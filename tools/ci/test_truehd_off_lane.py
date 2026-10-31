"""Unit tests for the truehd-off job: the library built with ICLFORGE_BUILD_TRUEHD=OFF and the
tests on, the one configuration that builds the C API's TrueHD stand-ins
(libs/capi/src/truehd_absent.cpp).

stdlib only, like the other suites in tools/ci. They read the preset file, the workflow and the C
API's CMake rule, because what could rot unseen is the wiring: a preset that quietly turned the
tests off, or a job that stopped running the stand-ins' test, would stay green and prove nothing.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def preset(kind: str, name: str) -> dict:
    for entry in json.loads(text("CMakePresets.json"))[kind]:
        if entry["name"] == name:
            return entry
    raise AssertionError(f"no {kind} {name!r} in CMakePresets.json")


class Presets(unittest.TestCase):
    def test_the_configure_preset_turns_the_codec_off_and_leaves_the_tests_on(self):
        configure = preset("configurePresets", "config-linux-gcc-no-truehd")
        variables = configure["cacheVariables"]
        self.assertEqual(variables["ICLFORGE_BUILD_TRUEHD"], "OFF")
        # The root CMakeLists.txt refuses the option off beside these two, which use the codec.
        self.assertEqual(variables["ICLFORGE_BUILD_CLI"], "OFF")
        self.assertEqual(variables["ICLFORGE_BUILD_HEARTH"], "OFF")
        # The point of the preset: the tests stay on (the core fragment turns them on), and
        # nothing here turns them or the examples off.
        self.assertNotIn("ICLFORGE_BUILD_TESTS", variables)
        self.assertNotIn("ICLFORGE_BUILD_EXAMPLES", variables)
        core = preset("configurePresets", "core")["cacheVariables"]
        self.assertEqual(core["ICLFORGE_BUILD_TESTS"], "ON")

    def test_the_root_still_refuses_the_codec_off_beside_forge_and_hearth(self):
        """If it stops refusing, the preset's two OFF lines are no longer what lets it configure."""
        root = text("CMakeLists.txt")
        self.assertRegex(root, r"NOT ICLFORGE_BUILD_TRUEHD AND ICLFORGE_BUILD_CLI")
        self.assertRegex(root, r"NOT ICLFORGE_BUILD_TRUEHD AND _iclforge_hearth_apps")

    def test_the_build_and_test_presets_follow_it(self):
        self.assertEqual(
            preset("buildPresets", "build-linux-gcc-no-truehd")["configurePreset"],
            "config-linux-gcc-no-truehd",
        )
        self.assertEqual(
            preset("testPresets", "test-linux-gcc-no-truehd")["configurePreset"],
            "config-linux-gcc-no-truehd",
        )

    def test_the_capi_tests_pick_the_absent_file_exactly_when_the_codec_is_off(self):
        rule = text("libs/capi/tests/CMakeLists.txt")
        self.assertRegex(rule, r"if\(ICLFORGE_BUILD_TRUEHD\)[\s\S]*?test_capi_truehd\.cpp")
        self.assertIn("test_capi_truehd_absent.cpp", rule)


class TheJob(unittest.TestCase):
    def job(self) -> str:
        core = text(".github/workflows/_ci-core.yml")
        found = re.search(r"(?ms)^  truehd-off:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", core)
        self.assertIsNotNone(found)
        return found.group(1)

    def test_it_runs_the_presets_and_insists_the_standins_test_is_among_those_run(self):
        job = self.job()
        for needle in (
            "cmake --preset config-linux-gcc-no-truehd",
            "cmake --build --preset build-linux-gcc-no-truehd",
            "ctest --preset test-linux-gcc-no-truehd -N",
            "without ICLFORGE_BUILD_TRUEHD every TrueHD call is unsupported",
            "ICLFORGE_BUILD_TRUEHD:BOOL=OFF",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, job)

    def test_the_test_it_insists_on_exists(self):
        self.assertIn(
            "without ICLFORGE_BUILD_TRUEHD every TrueHD call is unsupported",
            text("libs/capi/tests/test_capi_truehd_absent.cpp"),
        )

    def test_its_result_reaches_ci_status(self):
        ci = text(".github/workflows/ci.yml")
        self.assertIn("R_TRUEHD_OFF: ${{ needs.core.outputs.truehd_off_result }}", ci)
        self.assertIn('"truehd-off:$R_TRUEHD_OFF"', ci)
        self.assertIn("truehd_off_result:", text(".github/workflows/_ci-core.yml"))


if __name__ == "__main__":
    unittest.main()
