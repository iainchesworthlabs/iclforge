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
) -> Path:
    build = root / "build"
    build.mkdir()
    write_config(build / "sdkconfig", final)
    if defaults is not None:
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_BUILD_TYPE:STRING=\nSDKCONFIG_DEFAULTS:UNINITIALIZED={defaults}\n", "utf-8"
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

    def test_the_workflow_builds_what_the_profiles_say(self) -> None:
        """_build.yml spells each published image's fragments by hand until it reads
        `sink_profile.py defaults`; until then they are held to the profiles here."""
        text = (REPO / ".github" / "workflows" / "_build.yml").read_text("utf-8")
        spelled = set(re.findall(r'"-DSDKCONFIG_DEFAULTS=([^"]+)"', text))
        for ident in ("s3-n16r8", "c6-16mb", "c6-4mb", "p4-rev1", "p4-rev1-ac4"):
            self.assertIn(
                sp.defaults_string(self.data, ident),
                spelled,
                f"_build.yml does not build {ident} as the profile says",
            )

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
            build = make_build(Path(tmp), dict(FINAL))
            data["profiles"]["a"]["locked"] = sp.snapshot(data, "a", sp.read_final(build), {})
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
