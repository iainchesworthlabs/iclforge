"""Unit tests for sink_profile.py, the reader of firmware/hearth-sink/profiles.json
(planning/esp32-platform-profiles.md).

stdlib unittest, as ci.yml's script-lint job runs it. The builds are made from
nothing: an sdkconfig, a CMakeCache.txt and, where a budget is tested, two
files of the right size.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ci"))

import check_firmware_package as release_rules  # noqa: E402
import sink_profile as sp  # noqa: E402

REPO = HERE.parents[1]

# Images the release rules know that no profile describes yet. Naming one here is a
# decision, not a default: the rule lists in tools/ci/check_firmware_package.py and the
# profiles file are meant to be the same list.
IMAGES_WITHOUT_A_PROFILE: set[str] = {"hearth-sink-esp32p4-rev3"}


def small_data() -> dict:
    return {
        "format": 1,
        "watch": ["*STACK*", "SPIRAM*", "ICLFORGE_EXAMPLE_*RING*"],
        "free": {"ICLFORGE_EXAMPLE_WIFI_*": "the board's own"},
        "overlays": {
            "wifi-credentials": {"touches": ["ICLFORGE_EXAMPLE_WIFI_*"]},
            "measure": {"touches": ["ICLFORGE_EXAMPLE_REPORT_EVERY_FRAMES"]},
            "twin": {"touches": ["SPIRAM_MODE_*"], "fragment": "twin"},
        },
        "profiles": {
            "a": {
                "target": "esp32s3",
                "fragments": ["hw", "psram"],
                "boards": ["S3a"],
                "image": {
                    "name": "hearth-sink-esp32s3",
                    "flash_size": "16MB",
                    "psram": True,
                    "lowest": 0,
                    "highest": 99,
                    "refuse_above": None,
                },
                "budget": {"app_bytes_max": 100, "bootloader_bytes_max": 50},
                "locked": {
                    "CONFIG_SPIRAM": "y",
                    "CONFIG_SPIRAM_MODE_OCT": "y",
                    "CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM": "1",
                    "CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES": "32768",
                    "CONFIG_ICLFORGE_EXAMPLE_REPORT_EVERY_FRAMES": "125",
                },
            }
        },
    }


FINAL = {
    "CONFIG_SPIRAM": "y",
    "CONFIG_SPIRAM_MODE_OCT": "y",
    "CONFIG_SPIRAM_MODE_QUAD": "n",
    "CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM": "1",
    "CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES": "32768",
    "CONFIG_ICLFORGE_EXAMPLE_REPORT_EVERY_FRAMES": "125",
    "CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID": "somewhere",
}


def write_config(path: Path, values: dict[str, str]) -> None:
    lines = []
    for key, value in values.items():
        if value == "n":
            lines.append(f"# {key} is not set")
        elif re.fullmatch(r"-?[0-9]+|y", value):
            lines.append(f"{key}={value}")
        else:
            lines.append(f'{key}="{value}"')
    path.write_text("\n".join(lines) + "\n", "utf-8")


def make_build(
    root: Path,
    final: dict[str, str],
    defaults: str | None = "sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram",
    identity: tuple[str, str] | bool | None = None,
) -> Path:
    build = root / "build"
    build.mkdir()
    write_config(build / "sdkconfig", final)
    if defaults is not None:
        who = ""
        if identity is not False:
            who = (
                f"ICLFORGE_PROFILE:STRING={'a' if identity is None else identity[0]}\n"
                "ICLFORGE_PROFILE_HASH:STRING="
                f"{sp.profile_hash(small_data(), 'a') if identity is None else identity[1]}\n"
            )
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_BUILD_TYPE:STRING=\nSDKCONFIG_DEFAULTS:UNINITIALIZED={defaults}\n{who}", "utf-8"
        )
    return build


class TheFile(unittest.TestCase):
    """profiles.json against the tree it describes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = sp.load()

    def test_every_fragment_a_profile_names_is_in_the_project(self) -> None:
        for ident in self.data["profiles"]:
            for name in sp.fragment_names(self.data, ident, ("measure",)):
                self.assertTrue((sp.PROJECT / name).is_file(), f"{ident}: {name} is not there")

    def test_every_overlay_fragment_is_in_the_project(self) -> None:
        for name, spec in self.data["overlays"].items():
            if spec.get("fragment"):
                self.assertTrue(
                    (sp.PROJECT / f"sdkconfig.{spec['fragment']}").is_file(), f"overlay {name}"
                )

    def test_a_profile_names_a_target_the_packager_knows(self) -> None:
        for ident, spec in self.data["profiles"].items():
            self.assertIn(spec["target"], sp.TARGETS, ident)

    def test_published_images_are_the_release_rules(self) -> None:
        """tools/ci/check_firmware_package.py holds each published image to a rule; the
        profiles say the same thing, so a change to one without the other fails here."""
        rules = release_rules.IMAGES
        named = set()
        for ident, spec in self.data["profiles"].items():
            image = spec.get("image")
            if not image:
                continue
            named.add(image["name"])
            rule = rules.get(image["name"])
            self.assertIsNotNone(rule, f"{ident}: the release rules have no {image['name']}")
            wanted = sp.image_rule(self.data, image["name"])
            self.assertEqual(
                (
                    wanted["target"],
                    wanted["chip_id"],
                    wanted["flash_size"],
                    wanted["psram"],
                    wanted["lowest"],
                    wanted["highest"],
                    wanted["refuse_above"],
                ),
                (
                    rule.target,
                    rule.chip_id,
                    rule.flash_size,
                    rule.psram,
                    rule.lowest,
                    rule.highest,
                    rule.refuse_above,
                ),
                ident,
            )
        for name in rules:
            if name not in named:
                self.assertIn(
                    name,
                    IMAGES_WITHOUT_A_PROFILE,
                    f"the release rules publish {name} and no profile describes it",
                )

    def test_the_panel_s_stack_keys_are_free_and_not_unplaced_in_a_profile_without_a_screen(
        self,
    ) -> None:
        """Main's P4X work (#1400) put ICLFORGE_EXAMPLE_PANEL_STACK_BYTES in every target's
        Kconfig and LVGL's LV_SPAN_SNIPPET_STACK_SIZE in every P4 build. Both match `*STACK*`,
        and a build of any profile had two findings for keys that place nothing there."""
        data = sp.load()
        for key in (
            "CONFIG_ICLFORGE_EXAMPLE_PANEL_STACK_BYTES",
            "CONFIG_LV_SPAN_SNIPPET_STACK_SIZE",
        ):
            self.assertTrue(sp.is_watched(data, key), key)
            self.assertTrue(sp.is_free(data, key), key)
        # a real stack key that is not one of these is still not free
        self.assertFalse(sp.is_free(data, "CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"))

    def test_the_workflow_builds_each_profile_from_the_file_and_verifies_it(self) -> None:
        """_build.yml takes each board's fragments from `sink_profile.py defaults` and holds
        the build to the profile with `verify`; no board build spells its own list."""
        text = (REPO / ".github" / "workflows" / "_build.yml").read_text("utf-8")
        read = set(re.findall(r'"\$profile" defaults ([a-z0-9-]+)', text))
        checked = set(re.findall(r'"\$profile" verify ([a-z0-9-]+)', text))
        board_builds = {"s3-n16r8", "s3-n16r8-ac4", "c6-16mb", "c6-4mb", "p4-rev1", "p4-rev1-ac4"}
        self.assertEqual(read, board_builds)
        self.assertEqual(checked, board_builds)
        spelled = re.findall(
            r'"-DSDKCONFIG_DEFAULTS=(sdkconfig\.defaults;sdkconfig\.hw[^"]*)"', text
        )
        # The P4X panel's two builds (#1400) are not profiles yet: p4-rev3 is its own change.
        # Each is an `other_stacks` entry, so a list that is not one of these still fails.
        unprofiled = {
            "sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.p4-rev3;sdkconfig.sendspin;"
            "sdkconfig.panel;sdkconfig.panel-dsi",
            "sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.sendspin;sdkconfig.panel;"
            "sdkconfig.panel-null",
        }
        listed = set(sp.load()["other_stacks"])
        self.assertLessEqual(unprofiled, listed)
        self.assertEqual(
            [name for name in spelled if name not in unprofiled],
            [],
            "a board build in _build.yml spells its own fragment list",
        )

    def test_the_qemu_overlay_moves_no_stack_ring_cache_or_feature(self) -> None:
        """The twin is the release shape because the overlay touches only what QEMU has no
        hardware for. A pattern that could move a stack, a ring or a cache is a different
        shape, which CI would then be running in place of the board's."""
        touches = self.data["overlays"]["qemu-net"]["touches"]
        locked = self.data["profiles"]["s3-n16r8"]["locked"]
        moved = [key for key in locked if sp._matches(key, touches)]
        self.assertTrue(moved, "the overlay touches nothing the profile locks")
        for key in moved:
            for word in ("STACK", "RING", "CACHE", "IN_PSRAM", "DMA", "AC4", "SENDSPIN", "LANE"):
                self.assertNotIn(word, key, f"qemu-net may move {key}")
        # What it does move is each one of the four things QEMU has no hardware for.
        for key in (
            "CONFIG_ICLFORGE_EXAMPLE_NETWORK_OPENETH",
            "CONFIG_ICLFORGE_EXAMPLE_SINK_CAPTURE",
            "CONFIG_SPIRAM_MODE_QUAD",
            "CONFIG_ESP_CONSOLE_UART_DEFAULT",
        ):
            self.assertTrue(sp._matches(key, touches), f"qemu-net does not declare {key}")

    def test_the_profiles_are_distinct_builds(self) -> None:
        seen: dict[str, str] = {}
        for ident in self.data["profiles"]:
            key = (
                self.data["profiles"][ident]["target"] + "/" + sp.defaults_string(self.data, ident)
            )
            self.assertNotIn(key, seen, f"{ident} is {seen.get(key)} under another name")
            seen[key] = ident

    def test_a_locked_profile_locks_the_placement_of_its_stacks(self) -> None:
        for ident, spec in self.data["profiles"].items():
            locked = spec.get("locked", {})
            if not locked:
                continue
            self.assertIn("CONFIG_IDF_TARGET", locked, ident)
            self.assertTrue(
                any(key.endswith("_STACK_BYTES") for key in locked),
                f"{ident} locks no task stack's size",
            )


class Config(unittest.TestCase):
    def test_a_value_a_string_and_an_unset_key(self) -> None:
        values = sp.parse_config(
            'CONFIG_A=y\nCONFIG_B=240\nCONFIG_C="x y"\n# CONFIG_D is not set\n# a comment\n'
        )
        self.assertEqual(
            values, {"CONFIG_A": "y", "CONFIG_B": "240", "CONFIG_C": "x y", "CONFIG_D": "n"}
        )

    def test_the_later_fragment_wins(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            project = Path(tmp)
            (project / "sdkconfig.defaults").write_text("CONFIG_A=1\nCONFIG_B=1\n", "utf-8")
            (project / "sdkconfig.hw").write_text("CONFIG_B=2\n", "utf-8")
            (project / "sdkconfig.psram").write_text("# CONFIG_A is not set\n", "utf-8")
            data = small_data()
            self.assertEqual(
                sp.requested(data, "a", project=project), {"CONFIG_A": "n", "CONFIG_B": "2"}
            )

    def test_defaults_in_order_with_an_overlay_and_a_network(self) -> None:
        data = small_data()
        self.assertEqual(
            sp.defaults_string(data, "a"), "sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram"
        )
        self.assertEqual(
            sp.defaults_string(data, "a", ("twin",), "D:/net.sdkconfig"),
            "sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram;sdkconfig.twin;D:/net.sdkconfig",
        )

    def test_an_unknown_profile_or_overlay_is_named(self) -> None:
        data = small_data()
        with self.assertRaisesRegex(sp.ProfileError, "no profile 'z'"):
            sp.profile(data, "z")
        with self.assertRaisesRegex(sp.ProfileError, "no overlay 'z'"):
            sp.fragment_names(data, "a", ("z",))

    def test_the_rule_of_an_image_is_the_profiles(self) -> None:
        rule = sp.image_rule(small_data(), "hearth-sink-esp32s3")
        self.assertEqual(rule["chip_id"], 0x0009)
        self.assertEqual(rule["profile"], "a")
        with self.assertRaises(sp.ProfileError):
            sp.image_rule(small_data(), "hearth-sink-none")


class Verify(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.data = small_data()

    def tearDown(self) -> None:
        self.dir.cleanup()

    def rules(self, findings: list[sp.Finding]) -> list[str]:
        return sorted({finding.rule for finding in findings})

    def test_a_build_that_is_the_profile_passes(self) -> None:
        build = make_build(self.root, dict(FINAL))
        self.assertEqual(sp.verify(self.data, "a", build), [])

    def test_a_locked_key_that_moved_fails_and_says_which(self) -> None:
        final = dict(FINAL)
        final["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM"] = "0"
        build = make_build(self.root, final)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["locked"])
        self.assertIn("DECODE_STACK_IN_PSRAM is 0", findings[0].message)
        self.assertIn("locks 1", findings[0].message)

    def test_a_key_the_build_does_not_have_fails_unless_locked_off(self) -> None:
        final = dict(FINAL)
        del final["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"]
        build = make_build(self.root, final)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["locked"])
        self.assertIn(sp.ABSENT, findings[0].message)

    def test_an_overlay_may_move_only_the_keys_it_declares(self) -> None:
        final = dict(FINAL)
        final["CONFIG_ICLFORGE_EXAMPLE_REPORT_EVERY_FRAMES"] = "50"
        build = make_build(self.root, final)
        self.assertEqual(self.rules(sp.verify(self.data, "a", build)), ["locked"])
        self.assertEqual(sp.verify(self.data, "a", build, ("measure",)), [])
        final["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"] = "24576"
        second = self.root / "two"
        second.mkdir()
        findings = sp.verify(self.data, "a", make_build(second, final), ("measure",))
        self.assertEqual(self.rules(findings), ["locked"])
        self.assertIn("DECODE_STACK_BYTES", findings[0].message)

    def test_a_new_stack_nobody_placed_is_found(self) -> None:
        final = dict(FINAL)
        final["CONFIG_ICLFORGE_EXAMPLE_NEW_TASK_STACK_BYTES"] = "8192"
        build = make_build(self.root, final)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["unplaced"])
        self.assertIn("NEW_TASK_STACK_BYTES", findings[0].message)

    def test_a_watched_key_that_is_off_or_free_is_not_a_finding(self) -> None:
        final = dict(FINAL)
        final["CONFIG_SPIRAM_RODATA"] = "n"
        final["CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID"] = "elsewhere"
        build = make_build(self.root, final)
        self.assertEqual(sp.verify(self.data, "a", build), [])

    def test_a_watched_key_that_turned_on_is_found(self) -> None:
        final = dict(FINAL)
        final["CONFIG_SPIRAM_RODATA"] = "y"
        build = make_build(self.root, final)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["unplaced"])

    def test_fragments_in_another_order_or_missing_are_found(self) -> None:
        build = make_build(
            self.root, dict(FINAL), "sdkconfig.defaults;sdkconfig.psram;sdkconfig.hw"
        )
        self.assertEqual(self.rules(sp.verify(self.data, "a", build)), ["fragments"])

    def test_a_build_with_no_cache_cannot_say_what_it_was_given(self) -> None:
        build = make_build(self.root, dict(FINAL), None)
        self.assertEqual(self.rules(sp.verify(self.data, "a", build)), ["fragments"])

    def test_a_network_file_is_an_overlay_that_has_to_be_named(self) -> None:
        given = "sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram;D:/esp/build/wifi.sdkconfig"
        build = make_build(self.root, dict(FINAL), given)
        self.assertEqual(self.rules(sp.verify(self.data, "a", build)), ["fragments"])
        self.assertEqual(sp.verify(self.data, "a", build, ("wifi-credentials",)), [])

    def test_a_build_that_does_not_say_which_profile_it_is_found(self) -> None:
        build = make_build(self.root, dict(FINAL), identity=False)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["identity"])
        self.assertIn("identity a --cmake", findings[0].message)

    def test_a_build_of_another_profile_is_found(self) -> None:
        build = make_build(self.root, dict(FINAL), identity=("b", "000000000000"))
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["identity"])
        self.assertIn("is b, not a", findings[0].message)

    def test_a_build_from_an_older_definition_is_found(self) -> None:
        build = make_build(self.root, dict(FINAL), identity=("a", "000000000000"))
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["identity"])
        self.assertIn("another definition", findings[0].message)

    def test_the_hash_follows_what_the_profile_locks(self) -> None:
        before = sp.profile_hash(self.data, "a")
        self.data["profiles"]["a"]["locked"]["CONFIG_SPIRAM"] = "n"
        self.assertNotEqual(before, sp.profile_hash(self.data, "a"))
        self.assertEqual(len(before), 12)

    def test_an_image_over_its_budget_is_found(self) -> None:
        build = make_build(self.root, dict(FINAL))
        (build / sp.APP_NAME).write_bytes(b"\0" * 101)
        (build / "bootloader").mkdir()
        (build / "bootloader" / "bootloader.bin").write_bytes(b"\0" * 51)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["budget"])
        self.assertEqual(len(findings), 2)

    def test_an_image_inside_its_budget_passes(self) -> None:
        build = make_build(self.root, dict(FINAL))
        (build / sp.APP_NAME).write_bytes(b"\0" * 100)
        self.assertEqual(sp.verify(self.data, "a", build), [])

    def test_a_profile_that_locks_nothing_is_said_so(self) -> None:
        self.data["profiles"]["a"]["locked"] = {}
        build = make_build(self.root, dict(FINAL))
        self.assertIn("locked", self.rules(sp.verify(self.data, "a", build)))


def size_report(diram_used: int) -> str:
    """`idf.py size --format json2`, with the CMake chatter a re-run puts before it."""
    report = {
        "version": "1.2",
        "total_size": 1,
        "layout": [
            {"name": "Flash Code", "total": 0, "used": 5, "free": 0, "parts": {}},
            {"name": "DIRAM", "total": 341760, "used": diram_used, "free": 1, "parts": {}},
        ],
    }
    return "[0/1] Re-running CMake...\n-- {note}\n" + json.dumps(report, indent=4) + "\nERROR: x\n"


class StaticRam(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.data = small_data()
        self.data["profiles"]["a"]["budget"]["internal_static_bytes_max"] = 1000
        self.build = make_build(self.root, dict(FINAL))

    def tearDown(self) -> None:
        self.dir.cleanup()

    def test_the_report_is_found_after_other_output(self) -> None:
        path = self.root / "size.json"
        path.write_text(size_report(900), "utf-8")
        self.assertEqual(sp.read_size_report(path)["DIRAM"], 900)

    def test_static_internal_ram_inside_the_budget_passes(self) -> None:
        path = self.root / "size.json"
        path.write_text(size_report(1000), "utf-8")
        self.assertEqual(sp.verify(self.data, "a", self.build, size_json=path), [])

    def test_static_internal_ram_over_the_budget_is_found(self) -> None:
        path = self.root / "size.json"
        path.write_text(size_report(1001), "utf-8")
        findings = sp.verify(self.data, "a", self.build, size_json=path)
        self.assertEqual([finding.rule for finding in findings], ["budget"])
        self.assertIn("1,001", findings[0].message)

    def test_without_a_report_the_budget_is_not_judged(self) -> None:
        self.assertEqual(sp.verify(self.data, "a", self.build), [])

    def test_a_file_with_no_report_is_an_error(self) -> None:
        path = self.root / "size.json"
        path.write_text("nothing here\n", "utf-8")
        with self.assertRaises(sp.ProfileError):
            sp.read_size_report(path)


class Docs(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.data = small_data()
        self.data["other_stacks"] = {"sdkconfig.defaults;sdkconfig.hw": "the probe's"}

    def tearDown(self) -> None:
        self.dir.cleanup()

    def doc(self, text: str) -> Path:
        path = self.root / "page.md"
        path.write_text(text, "utf-8")
        return path

    def test_a_profile_a_listed_stack_and_a_ci_shape_are_known(self) -> None:
        path = self.doc(
            "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram\n"
            "and sdkconfig.defaults;sdkconfig.hw for the probe,\n"
            "and sdkconfig.defaults;sdkconfig.ci-http for CI.\n"
        )
        self.assertEqual(sp.check_docs(self.data, self.root, [path]), [])

    def test_a_stack_nobody_listed_is_found_with_its_line(self) -> None:
        path = self.doc("fine\nbuild with sdkconfig.defaults;sdkconfig.psram;sdkconfig.hw now\n")
        findings = sp.check_docs(self.data, self.root, [path])
        self.assertEqual(len(findings), 1)
        self.assertIn("page.md:2", findings[0].message)

    def test_a_stale_generated_table_is_found_and_can_be_written(self) -> None:
        path = self.doc(f"{sp.TABLE_BEGIN}\n\nold\n\n{sp.TABLE_END}\n")
        findings = sp.check_docs(self.data, self.root, [path])
        self.assertEqual([finding.rule for finding in findings], ["docs"])
        self.assertEqual(sp.check_docs(self.data, self.root, [path], write=True), [])
        self.assertEqual(sp.check_docs(self.data, self.root, [path]), [])
        self.assertIn("`hearth-sink-esp32s3`", path.read_text("utf-8"))

    def test_a_table_with_no_end_is_found(self) -> None:
        path = self.doc(f"{sp.TABLE_BEGIN}\nnever closed\n")
        findings = sp.check_docs(self.data, self.root, [path])
        self.assertIn(sp.TABLE_END, findings[0].message)

    def test_the_real_tree_holds(self) -> None:
        self.assertEqual(sp.check_docs(sp.load()), [])


class ExpectedConsole(unittest.TestCase):
    """The lines that say where a profile put its rings and stacks (the QEMU twin's checks)."""

    def data(self, **locked: str) -> dict:
        data = small_data()
        data["profiles"]["a"]["locked"] = {f"CONFIG_{key}": value for key, value in locked.items()}
        return data

    def test_a_profile_with_psram_expects_its_stacks_and_rings_there(self) -> None:
        data = self.data(
            SPIRAM="y",
            ICLFORGE_SENDSPIN="y",
            ICLFORGE_EXAMPLE_RING_IN_PSRAM="1",
            ICLFORGE_EXAMPLE_RING_BYTES="32768",
            ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM="1",
            ICLFORGE_EXAMPLE_DECODE_STACK_BYTES="40960",
            ICLFORGE_EXAMPLE_SENDSPIN_RING_BYTES="262144",
            ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_IN_PSRAM="1",
            ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES="32768",
            ICLFORGE_EXAMPLE_DECODE_LANE="1",
            ICLFORGE_EXAMPLE_DECODE_CORE="1",
        )
        self.assertEqual(
            sp.expected_console(data, "a"),
            [
                "player: ring 32768 bytes in PSRAM",
                "player: decode stack 40960 bytes in PSRAM",
                "burst player: ring 262144 bytes and buffers in PSRAM",
                "burst player: decode stack 32768 bytes in PSRAM",
                "player: decode stages on cores 1 and 0",
            ],
        )

    def test_a_stack_the_profile_leaves_internal_is_not_expected_in_psram(self) -> None:
        data = self.data(
            SPIRAM="y",
            ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM="0",
            ICLFORGE_EXAMPLE_DECODE_STACK_BYTES="32768",
        )
        self.assertEqual(sp.expected_console(data, "a"), [])

    def test_a_profile_with_no_psram_expects_none_of_the_psram_lines(self) -> None:
        data = self.data(
            ICLFORGE_EXAMPLE_RING_IN_PSRAM="1",
            ICLFORGE_EXAMPLE_RING_BYTES="32768",
            ICLFORGE_EXAMPLE_DECODE_CORE="-1",
        )
        self.assertEqual(sp.expected_console(data, "a"), [])

    def test_the_release_profile_expects_the_http_stack_in_psram(self) -> None:
        """The 2026-10-12 failure, as a line a build has to print."""
        lines = sp.expected_console(sp.load(), "s3-n16r8")
        self.assertIn("player: decode stack 32768 bytes in PSRAM", lines)


class Snapshot(unittest.TestCase):
    def test_the_fragment_keys_and_the_watched_ones_less_the_free(self) -> None:
        data = small_data()
        final = dict(FINAL)
        final["CONFIG_UNRELATED"] = "y"
        asked = {
            "CONFIG_SPIRAM": "y",
            "CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID": "x",
            "CONFIG_NOT_IN_THE_BUILD": "1",
        }
        locked = sp.snapshot(data, "a", final, asked)
        self.assertIn("CONFIG_SPIRAM", locked)
        self.assertEqual(locked["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"], "32768")
        self.assertEqual(locked["CONFIG_NOT_IN_THE_BUILD"], sp.ABSENT)
        self.assertNotIn("CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID", locked)
        self.assertNotIn("CONFIG_UNRELATED", locked)
        # A watched member of a choice that was not taken is not a decision.
        self.assertNotIn("CONFIG_SPIRAM_MODE_QUAD", locked)

    def test_a_snapshot_of_a_build_verifies_it(self) -> None:
        data = small_data()
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "first"
            first.mkdir()
            build = make_build(first, dict(FINAL))
            data["profiles"]["a"]["locked"] = sp.snapshot(data, "a", sp.read_final(build), {})
            second = Path(tmp) / "second"
            second.mkdir()
            build = make_build(second, dict(FINAL), identity=("a", sp.profile_hash(data, "a")))
            self.assertEqual(sp.verify(data, "a", build), [])


class CommandLine(unittest.TestCase):
    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = sp.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_list_defaults_and_table_of_the_real_file(self) -> None:
        code, out, _ = self.run_cli("list")
        self.assertEqual(code, 0)
        self.assertIn("s3-n16r8\t", out)
        code, out, _ = self.run_cli("defaults", "c6-4mb")
        self.assertEqual(
            out.strip(),
            "sdkconfig.defaults;sdkconfig.hw;sdkconfig.sendspin;sdkconfig.c6;sdkconfig.sendspin-c6",
        )
        code, out, _ = self.run_cli("table", "s3-n16r8")
        self.assertIn("`hearth-sink-esp32s3`", out)

    def test_identity_is_the_id_and_a_hash_for_a_configure(self) -> None:
        code, out, _ = self.run_cli("identity", "s3-n16r8", "--cmake")
        self.assertEqual(code, 0)
        self.assertRegex(
            out.strip(), r"^-DICLFORGE_PROFILE=s3-n16r8 -DICLFORGE_PROFILE_HASH=[0-9a-f]{12}$"
        )

    def test_an_unknown_profile_is_exit_2(self) -> None:
        code, _, err = self.run_cli("defaults", "no-such")
        self.assertEqual(code, 2)
        self.assertIn("no profile", err)

    def test_verify_exits_1_on_a_finding_and_0_on_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            profiles = Path(tmp) / "profiles.json"
            profiles.write_text(json.dumps(small_data()), "utf-8")
            build = make_build(Path(tmp), dict(FINAL))
            code, out, _ = self.run_cli(
                "--profiles", str(profiles), "verify", "a", "--build-dir", str(build)
            )
            self.assertEqual((code, "holds to its profile" in out), (0, True))
            final = dict(FINAL)
            final["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM"] = "0"
            write_config(build / "sdkconfig", final)
            code, out, _ = self.run_cli(
                "--profiles", str(profiles), "verify", "a", "--build-dir", str(build), "--github"
            )
            self.assertEqual(code, 1)
            self.assertIn("::error title=profile a, locked::", out)


if __name__ == "__main__":
    unittest.main()
