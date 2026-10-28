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
import kconfig_scan  # noqa: E402
import sink_profile as sp  # noqa: E402

REPO = HERE.parents[1]

# Images the release rules know that no profile describes yet. Naming one here is a
# decision, not a default: the rule lists in tools/ci/check_firmware_package.py and the
# profiles file are meant to be the same list. None is left: the P4's v3.x image has its profile.
IMAGES_WITHOUT_A_PROFILE: set[str] = set()


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

    def test_every_profile_has_a_layout_evidence_row(self) -> None:
        """layout_options.hpp says what a kind of board has been measured to play, by profile id,
        and answers "not verified" for an id it has no row for. A new profile is a new kind of
        board: it names its measurements there, or says in the row that it has none."""
        header = (REPO / "libs/device/include/iclforge/layout_options.hpp").read_text("utf-8")
        table = header.split("kLayoutEvidence{{", 1)[1].split("}};", 1)[0]
        rows = set(re.findall(r'^\s*\{"([a-z0-9-]+)",', table, re.MULTILINE))
        self.assertEqual(
            rows, set(self.data["profiles"]), "kLayoutEvidence and profiles.json differ"
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

    def test_the_measure_overlay_sets_only_the_keys_it_declares(self) -> None:
        """An image built with `measure` is the profile's, not a scratch build, because the
        overlay moves nothing the guard holds: its fragment sets only observation keys, each of
        them declared, and none of them a stack, a ring, a cache or a placement."""
        spec = self.data["overlays"]["measure"]
        self.assertEqual(spec.get("fragment"), "measure")
        text = (sp.PROJECT / "sdkconfig.measure").read_text("utf-8")
        keys = [
            line.split("=", 1)[0]
            for line in text.splitlines()
            if line.startswith("CONFIG_") and "=" in line
        ]
        self.assertTrue(keys, "sdkconfig.measure sets nothing")
        for key in keys:
            self.assertTrue(sp._matches(key, spec["touches"]), f"{key} is not declared")
            for word in ("STACK", "RING", "CACHE", "IN_PSRAM", "DMA", "LANE", "CORE"):
                self.assertNotIn(word, key, f"measure may not move {key}")

    def test_a_measure_image_is_the_profile_s_and_its_hash_key_may_differ_from_the_locked_value(
        self,
    ) -> None:
        locked = self.data["profiles"]["p4-rev1"]["locked"]
        self.assertEqual(locked["CONFIG_ICLFORGE_EXAMPLE_AC4_PCM_HASH"], "0")
        moved = sp.touched(self.data, ("measure",))
        self.assertTrue(sp._matches("CONFIG_ICLFORGE_EXAMPLE_AC4_PCM_HASH", moved))
        names = sp.fragment_names(self.data, "p4-rev1", ("measure",))
        self.assertEqual(names[-1], "sdkconfig.measure")

    def test_a_published_image_does_not_serve_its_screen_over_http(self) -> None:
        """The screen's read-back and touch server has no authentication: anyone on the network
        reads the picture and presses the buttons. A profile that names a published image has the
        port at 0 in what its fragments ask for and in what it locks, so that the image the
        release builds is the one with no server; a bench that wants it builds with the
        `panel-capture` overlay, which a release does not."""
        key = "CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT"
        checked = []
        for ident, spec in self.data["profiles"].items():
            if not spec.get("image"):
                continue
            checked.append(ident)
            self.assertEqual(sp.requested(self.data, ident).get(key, "0"), "0", ident)
            self.assertEqual(spec.get("locked", {}).get(key, "0"), "0", ident)
        self.assertIn("p4-rev3", checked)
        self.assertEqual(self.data["profiles"]["p4-rev3"]["locked"][key], "0")

    def test_the_kconfig_default_of_the_screen_server_is_off(self) -> None:
        text = (sp.PROJECT / "main" / "Kconfig.projbuild").read_text("utf-8")
        match = re.search(
            r"^config ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT\b.*?^\s+default (\d+)$",
            text,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(match, "the symbol is not in main/Kconfig.projbuild")
        self.assertEqual(match.group(1), "0")

    def test_the_panel_capture_overlay_turns_on_the_server_and_nothing_else(self) -> None:
        key = "CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT"
        spec = self.data["overlays"]["panel-capture"]
        self.assertEqual(spec.get("fragment"), "panel-capture")
        asked = sp.parse_config((sp.PROJECT / "sdkconfig.panel-capture").read_text("utf-8"))
        self.assertEqual(asked, {key: "8081"})
        self.assertEqual(
            sp.touched(self.data, ("panel-capture",)), ["ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT"]
        )
        # Given after the profile's own fragments, so that it wins over the profile's 0.
        names = sp.fragment_names(self.data, "p4-rev3", ("panel-capture",))
        self.assertEqual(names[-1], "sdkconfig.panel-capture")
        self.assertEqual(sp.requested(self.data, "p4-rev3")[key], "0")
        self.assertEqual(sp.requested(self.data, "p4-rev3", ("panel-capture",))[key], "8081")

    def test_the_v3_profile_has_the_screen_s_fragments_in_order_and_the_kit_as_its_board(
        self,
    ) -> None:
        spec = self.data["profiles"]["p4-rev3"]
        self.assertEqual(
            spec["fragments"], ["hw", "p4", "p4-rev3", "sendspin", "ac4", "panel", "panel-dsi"]
        )
        self.assertEqual(spec["target"], "esp32p4")
        # The kit (chip v3.2) ran this image on 2026-10-20; it is the only board of that silicon.
        self.assertEqual(spec["boards"], ["P4X"], "a board is named when one has run the image")
        self.assertEqual(spec["image"]["name"], "hearth-sink-esp32p4-rev3")

    def test_the_two_p4_images_refuse_each_other_s_chips(self) -> None:
        """v1.x and v3.x firmware are not interchangeable (Espressif's v3.x guide, 1.1): the
        revision windows of the two profiles do not overlap, so neither takes the other's chip."""
        old, new = (self.data["profiles"][i]["image"] for i in ("p4-rev1", "p4-rev3"))
        self.assertLess(old["highest"], new["lowest"])
        self.assertEqual(old["refuse_above"], old["highest"])
        self.assertEqual(new["refuse_above"], new["highest"])

    def test_the_v3_profile_locks_the_screen_task_s_stack_which_is_free_for_every_other(
        self,
    ) -> None:
        key = "CONFIG_ICLFORGE_EXAMPLE_PANEL_STACK_BYTES"
        self.assertTrue(sp.is_free(self.data, key))
        self.assertEqual(self.data["profiles"]["p4-rev3"]["locked"][key], "16384")
        for ident, spec in self.data["profiles"].items():
            if ident != "p4-rev3":
                self.assertNotIn(key, spec["locked"], f"{ident} has no screen")

    def test_the_v3_profile_is_on_400_mhz_with_the_screen_on_and_no_other_display(self) -> None:
        locked = self.data["profiles"]["p4-rev3"]["locked"]
        self.assertEqual(locked["CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400"], "y")
        self.assertEqual(locked["CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI"], "y")
        self.assertEqual(locked["CONFIG_LV_USE_CUSTOM_MALLOC"], "y")
        self.assertEqual(locked["CONFIG_ESP32P4_REV_MIN_FULL"], "301")
        self.assertEqual(locked["CONFIG_ESP32P4_REV_MAX_FULL"], "399")
        self.assertNotIn("CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL", locked)

    def test_the_v3_profile_has_budgets_a_build_can_be_held_to(self) -> None:
        budget = self.data["profiles"]["p4-rev3"]["budget"]
        self.assertGreater(budget["app_bytes_max"], 2_400_000)
        self.assertGreater(budget["internal_static_bytes_max"], 150_000)
        self.assertEqual(budget["bootloader_bytes_max"], 24576)

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

    def test_every_profile_locks_the_sendspin_ac4_listing_keys(self) -> None:
        """#1446's two keys and the frame rate one are declared with no dependency, so they are
        in every profile's configuration and match the watch pattern: a profile that does not
        lock them is reported [unplaced] by the first build after main is merged in."""
        for ident, spec in self.data["profiles"].items():
            locked = spec["locked"]
            self.assertIn("CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4", locked, ident)
            self.assertIn("CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_CODED_CHANNELS", locked, ident)
            self.assertIn("CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_FRAME_RATE", locked, ident)
            self.assertTrue(
                sp.is_watched(self.data, "CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4"), "the pattern"
            )

    def test_the_boards_that_list_ac4_state_what_keeps_up(self) -> None:
        """An S3 with the decoder and the data cache lists AC-4 at 2 channels and 25 frames a
        second (an S3 over Sendspin falls behind at 29.97), a P4 with the decoder at 6 channels
        and no rate limit; no other profile lists it."""
        listed = {
            "s3-n16r8-ac4": ("1", "2", "25"),
            "p4-rev1": ("1", "6", "0"),
            "p4-rev3": ("1", "6", "0"),
        }
        for ident, spec in self.data["profiles"].items():
            locked = spec["locked"]
            got = (
                locked["CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4"],
                locked["CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_CODED_CHANNELS"],
                locked["CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_FRAME_RATE"],
            )
            if ident in listed:
                self.assertEqual(got, listed[ident], ident)
            else:
                self.assertEqual(got[0], "0", ident)
                self.assertEqual(got[2], "0", ident)

    def test_the_ac4_profiles_lock_the_burst_player_s_40_kb_decode_stack(self) -> None:
        for ident in ("s3-n16r8-ac4", "p4-rev1", "p4-rev3"):
            locked = self.data["profiles"][ident]["locked"]
            self.assertEqual(
                locked["CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES"], "40960", ident
            )

    def test_the_workflow_builds_each_profile_from_the_file_and_verifies_it(self) -> None:
        """_build.yml takes each board's fragments from `sink_profile.py defaults` and holds
        the build to the profile with `verify`; no board build spells its own list."""
        text = (REPO / ".github" / "workflows" / "_build.yml").read_text("utf-8")
        read = set(re.findall(r'"\$profile" defaults ([a-z0-9-]+)', text))
        checked = set(re.findall(r'"\$profile" verify ([a-z0-9-]+)', text))
        board_builds = {
            "s3-n16r8",
            "s3-n16r8-ac4",
            "c6-16mb",
            "c6-4mb",
            "p4-rev1",
            "p4-rev3",
        }
        self.assertEqual(read, board_builds)
        self.assertEqual(checked, board_builds)
        spelled = re.findall(
            r'"-DSDKCONFIG_DEFAULTS=(sdkconfig\.defaults;sdkconfig\.hw[^"]*)"', text
        )
        # The P4X panel's null-display build (#1400) is a development build, not a board's
        # image, and is not a profile. It is an `other_stacks` entry, so a list that is not it
        # still fails. The v3.x image is the profile p4-rev3 and is built from the file.
        unprofiled = {
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

    def test_the_guard_steps_run_after_an_earlier_failure(self) -> None:
        """A step after a failed step is skipped by default, and on main the minimum-footprint
        probes failed first, so no sink build was held to its profile (the C6's static RAM budget
        was wrong for hours). Every step that runs the guard or carries an image to the packager
        is `!cancelled()`, and so is the packaging job."""
        text = (REPO / ".github" / "workflows" / "_build.yml").read_text("utf-8")
        steps = re.split(r"(?m)^      - (?=name:|uses:)", text)[1:]
        guarded = []
        for step in steps:
            runs_guard = re.search(r'"\$profile" (defaults|verify|expect-console) ', step)
            carries_image = "actions/upload-artifact" in step and re.search(
                r"name: esp32-firmware-(esp32s3|riscv)\b", step
            )
            if not (runs_guard or carries_image):
                continue
            head = step.split("\n")[0]
            guarded.append(head)
            self.assertIn("if: ${{ !cancelled() }}", step.split("run: |")[0], head)
        # the S3 profile builds, the QEMU twin and the S3 image upload; the C6 and P4 builds
        # and their upload
        self.assertGreaterEqual(len(guarded), 10, guarded)
        package = text[text.index("\n  package-esp32-firmware:") :]
        self.assertIn("if: ${{ !cancelled() && inputs.run_esp }}", package.split("steps:")[0])

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

    def test_a_key_that_is_free_and_locked_is_held_to_the_locked_value(self) -> None:
        """The screen task's stack is `free` for every profile with no screen and locked by hand
        in the one that has one: locked is read first."""
        self.data["free"]["ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"] = "inert elsewhere"
        final = dict(FINAL)
        final["CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_BYTES"] = "16384"
        build = make_build(self.root, final)
        findings = sp.verify(self.data, "a", build)
        self.assertEqual(self.rules(findings), ["locked"])
        self.assertIn("DECODE_STACK_BYTES is 16384", findings[0].message)

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

    def test_an_image_with_less_room_than_a_change_must_leave_is_found(self) -> None:
        build = make_build(self.root, dict(FINAL))
        (build / sp.APP_NAME).write_bytes(b"\0" * 91)
        # Without the ask it is inside its budget, as before.
        self.assertEqual(sp.verify(self.data, "a", build), [])
        findings = sp.verify(self.data, "a", build, app_headroom=10)
        self.assertEqual(self.rules(findings), ["budget"])
        self.assertIn("9 under the profile's 100", findings[0].message)
        self.assertIn("leave 10 free", findings[0].message)

    def test_an_image_with_exactly_the_room_asked_for_passes(self) -> None:
        build = make_build(self.root, dict(FINAL))
        (build / sp.APP_NAME).write_bytes(b"\0" * 90)
        self.assertEqual(sp.verify(self.data, "a", build, app_headroom=10), [])

    def test_an_image_over_its_budget_is_one_finding_with_or_without_the_room(self) -> None:
        build = make_build(self.root, dict(FINAL))
        (build / sp.APP_NAME).write_bytes(b"\0" * 101)
        findings = sp.verify(self.data, "a", build, app_headroom=10)
        self.assertEqual(len(findings), 1)
        self.assertIn("over the profile's 100", findings[0].message)

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

    def test_verify_with_app_headroom_fails_an_image_too_close_to_its_budget(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            profiles = Path(tmp) / "profiles.json"
            profiles.write_text(json.dumps(small_data()), "utf-8")
            build = make_build(Path(tmp), dict(FINAL))
            (build / sp.APP_NAME).write_bytes(b"\0" * 95)
            argv = ("--profiles", str(profiles), "verify", "a", "--build-dir", str(build))
            self.assertEqual(self.run_cli(*argv)[0], 0)
            code, out, _ = self.run_cli(*argv, "--app-headroom", "10", "--github")
            self.assertEqual(code, 1)
            self.assertIn("::error title=profile a, budget::", out)
            self.assertIn("a change must leave 10 free", out)
            code, _, _ = self.run_cli(*argv, "--app-headroom", "5")
            self.assertEqual(code, 0)


class KconfigChange(unittest.TestCase):
    """`check-change`: what a pull request adds to, or takes from, the Kconfig, held to the
    profiles with no ESP-IDF (planning/esp32-platform-profiles.md, 'Where each check runs')."""

    KCONFIG = """
config ICLFORGE_SENDSPIN
    bool "Sendspin"
    default y

config ICLFORGE_EXAMPLE_NEW_RING_BYTES
    int "A ring"
    default 4096
    help
        Text that mentions config SOMETHING_ELSE and default 1 in its help,
        which is not a declaration.

config ICLFORGE_EXAMPLE_NEW_SWITCH
    bool "A switch"
    default n

menu "Sendspin"
    depends on ICLFORGE_SENDSPIN

config ICLFORGE_EXAMPLE_NEW_S3_STACK_BYTES
    int "A stack on the S3 only"
    depends on IDF_TARGET_ESP32S3
    default 8192

config ICLFORGE_EXAMPLE_NEW_UNKNOWN_STACK_BYTES
    int "A stack under a symbol the profile does not state"
    depends on SOME_IDF_SYMBOL
    default 8192

endmenu
"""

    def data(self) -> dict:
        def profile(target: str, **locked: str) -> dict:
            return {"target": target, "fragments": ["a"], "locked": dict(locked)}

        return {
            "format": 1,
            "watch": ["ICLFORGE_EXAMPLE_*"],
            "free": {"ICLFORGE_EXAMPLE_FREE_*": "named"},
            "profiles": {
                "s3": profile("esp32s3", CONFIG_ICLFORGE_SENDSPIN="y"),
                "c6": profile("esp32c6", CONFIG_ICLFORGE_SENDSPIN="y"),
            },
        }

    def run_check(self, data: dict, head: str, base: str | None = "") -> list[sp.Finding]:
        path = sp.KCONFIG_FILES[0]
        return sp.check_change(data, {path: head}, None if base is None else {path: base})

    def test_the_scanner_reads_declarations_and_steps_over_help(self) -> None:
        found = kconfig_scan.scan(self.KCONFIG)
        self.assertEqual(
            sorted(found),
            [
                "ICLFORGE_EXAMPLE_NEW_RING_BYTES",
                "ICLFORGE_EXAMPLE_NEW_S3_STACK_BYTES",
                "ICLFORGE_EXAMPLE_NEW_SWITCH",
                "ICLFORGE_EXAMPLE_NEW_UNKNOWN_STACK_BYTES",
                "ICLFORGE_SENDSPIN",
            ],
        )
        ring = found["ICLFORGE_EXAMPLE_NEW_RING_BYTES"]
        self.assertEqual((ring.type, ring.defaults, ring.conditional), ("int", ["4096"], False))
        self.assertTrue(found["ICLFORGE_EXAMPLE_NEW_SWITCH"].plain_n)
        s3 = found["ICLFORGE_EXAMPLE_NEW_S3_STACK_BYTES"]
        self.assertEqual(s3.depends, ["IDF_TARGET_ESP32S3"])
        self.assertEqual(s3.enclosing, ["menu ICLFORGE_SENDSPIN"])

    def test_the_conditions_are_three_valued(self) -> None:
        data = self.data()
        data["profiles"]["s3"]["locked"]["CONFIG_FEATURE"] = "n"
        check = sp.evaluate
        self.assertIs(check(data, "s3", "IDF_TARGET_ESP32S3"), True)
        self.assertIs(check(data, "c6", "IDF_TARGET_ESP32S3"), False)
        self.assertIs(check(data, "s3", "ICLFORGE_SENDSPIN && IDF_TARGET_ESP32S3"), True)
        self.assertIs(check(data, "s3", "!FEATURE"), True)
        self.assertIs(check(data, "s3", "(FEATURE || IDF_TARGET_ESP32S3) && !FEATURE"), True)
        # a symbol the profile does not state is unknown, and False && unknown is False
        self.assertIsNone(check(data, "s3", "SOME_IDF_SYMBOL"))
        self.assertIs(check(data, "c6", "IDF_TARGET_ESP32S3 && SOME_IDF_SYMBOL"), False)
        self.assertIsNone(check(data, "s3", "SOME_IDF_SYMBOL = 3"))

    def test_a_new_int_key_nobody_placed_is_reported_for_every_profile_it_is_in(self) -> None:
        found = self.run_check(self.data(), self.KCONFIG)
        text = " | ".join(f.message for f in found)
        self.assertIn("CONFIG_ICLFORGE_EXAMPLE_NEW_RING_BYTES is new", text)
        self.assertIn("in c6, s3", text)
        # on the S3 only: the C6's target is not it
        self.assertRegex(text, r"CONFIG_ICLFORGE_EXAMPLE_NEW_S3_STACK_BYTES is new .* in s3,")
        self.assertNotIn(
            "NEW_S3_STACK_BYTES is new and the guard watches it, and it is in c6", text
        )
        # a bool off by default, and a key whose condition the profile does not state, are the
        # configured build's to judge
        self.assertNotIn("NEW_SWITCH", text)
        self.assertNotIn("NEW_UNKNOWN_STACK_BYTES", text)

    def test_a_key_locked_in_a_profile_or_named_free_is_placed(self) -> None:
        data = self.data()
        for ident in data["profiles"]:
            data["profiles"][ident]["locked"]["CONFIG_ICLFORGE_EXAMPLE_NEW_RING_BYTES"] = "4096"
        data["profiles"]["s3"]["locked"]["CONFIG_ICLFORGE_EXAMPLE_NEW_S3_STACK_BYTES"] = "8192"
        self.assertEqual(self.run_check(data, self.KCONFIG), [])
        data = self.data()
        data["free"]["ICLFORGE_EXAMPLE_NEW_*"] = "not a placement"
        self.assertEqual(self.run_check(data, self.KCONFIG), [])

    def test_only_what_the_change_adds_is_reported_with_a_base(self) -> None:
        # the same file at the base: nothing is new
        self.assertEqual(self.run_check(self.data(), self.KCONFIG, base=self.KCONFIG), [])
        # one key more than the base
        more = self.KCONFIG + "\nconfig ICLFORGE_EXAMPLE_ONE_MORE_BYTES\n    int\n    default 1\n"
        found = self.run_check(self.data(), more, base=self.KCONFIG)
        self.assertEqual(len(found), 1)
        self.assertIn("ICLFORGE_EXAMPLE_ONE_MORE_BYTES", found[0].message)

    def test_a_key_the_guard_does_not_watch_is_not_its_business(self) -> None:
        text = (
            "config ICLFORGE_SENDSPIN\n    bool\n    default y\n\n"
            "config SOME_OTHER_THING\n    int\n    default 1\n"
        )
        self.assertEqual(self.run_check(self.data(), text), [])

    def test_a_lock_on_a_key_no_kconfig_declares_is_reported(self) -> None:
        data = self.data()
        data["profiles"]["s3"]["locked"]["CONFIG_ICLFORGE_EXAMPLE_GONE_BYTES"] = "4096"
        data["profiles"]["s3"]["locked"]["CONFIG_ICLFORGE_EXAMPLE_OFF_BYTES"] = "n"
        data["profiles"]["s3"]["locked"]["CONFIG_SPIRAM"] = "y"  # ESP-IDF's, not ours
        text = (
            "config ICLFORGE_SENDSPIN\n    bool\n    default y\n\n"
            "config ICLFORGE_EXAMPLE_KEPT_BYTES\n    int\n    default 1\n"
        )
        data["profiles"]["s3"]["locked"]["CONFIG_ICLFORGE_EXAMPLE_KEPT_BYTES"] = "1"
        data["profiles"]["c6"]["locked"]["CONFIG_ICLFORGE_EXAMPLE_KEPT_BYTES"] = "1"
        found = self.run_check(data, text)
        self.assertEqual(len(found), 1, [f.message for f in found])
        self.assertIn("profile s3 locks CONFIG_ICLFORGE_EXAMPLE_GONE_BYTES=4096", found[0].message)

    def test_the_tree_as_it_is_holds_to_its_profiles(self) -> None:
        head = {path: (REPO / path).read_text("utf-8") for path in sp.KCONFIG_FILES}
        found = sp.check_change(sp.load(), head, None)
        self.assertEqual([f.message for f in found], [])

    def test_the_command_reads_the_tree_and_says_so(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = sp.main(["check-change", "--all"])
        self.assertEqual((code, err.getvalue()), (0, ""), out.getvalue())
        self.assertIn("placed in the profiles", out.getvalue())

    def test_the_command_needs_a_base_or_all(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = sp.main(["check-change"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
