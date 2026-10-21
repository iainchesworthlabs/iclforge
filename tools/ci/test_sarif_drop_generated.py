"""Unit tests for sarif_drop_generated.py.

What must hold: a result only under the directory goes, in a relative URI and in
an absolute file:/// one, with either slash; a result with a location in our
source stays; a result with no location stays (the upload's problem, not this
script's); a directory name must match whole segments; and a file that is not
SARIF is refused rather than rewritten.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sarif_drop_generated as sdg

BUILD = "build/config-windows-msvc"


def result(*uris, rule="C6262"):
    return {
        "ruleId": rule,
        "locations": [{"physicalLocation": {"artifactLocation": {"uri": uri}}} for uri in uris],
    }


def run_with(*results):
    return {"runs": [{"results": list(results)}]}


class DropTest(unittest.TestCase):
    def kept(self, sarif):
        return [
            r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]
            for r in sarif["runs"][0]["results"]
            if r.get("locations")
        ]

    def test_generated_results_go_in_every_spelling(self):
        sarif = run_with(
            result(f"{BUILD}/apps/forge/gui/.rcc/qmlcache/Main_qml.cpp"),
            result(f"file:///D:/a/iclforge/iclforge/{BUILD}/apps/forge/gui/mocs_compilation.cpp"),
            result(r"D:\a\iclforge\iclforge\build\config-windows-msvc\x.cpp"),
            result("apps/shared/theme/src/system_motion.cpp", rule="C26813"),
        )
        self.assertEqual(sdg.drop(sarif, [BUILD]), 3)
        self.assertEqual(self.kept(sarif), ["apps/shared/theme/src/system_motion.cpp"])

    def test_a_result_with_a_location_in_our_source_stays(self):
        sarif = run_with(result(f"{BUILD}/gen.cpp", "libs/ac3/src/a.cpp"))
        self.assertEqual(sdg.drop(sarif, [BUILD]), 0)
        self.assertEqual(len(sarif["runs"][0]["results"]), 1)

    def test_a_result_with_no_location_stays(self):
        sarif = run_with({"ruleId": "X", "locations": []}, {"ruleId": "Y"})
        self.assertEqual(sdg.drop(sarif, [BUILD]), 0)
        self.assertEqual(len(sarif["runs"][0]["results"]), 2)

    def test_the_directory_matches_whole_segments(self):
        sarif = run_with(
            result("build/config-windows-msvc-debug/a.cpp"),
            result("rebuild/config-windows-msvc/a.cpp"),
        )
        self.assertEqual(sdg.drop(sarif, [BUILD]), 0)

    def test_main_rewrites_the_file_and_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "r.sarif"
            path.write_text(json.dumps(run_with(result(f"{BUILD}/a.cpp"), result("libs/a.cpp"))))
            self.assertEqual(sdg.main(["x", str(path), BUILD]), 0)
            left = json.loads(path.read_text())["runs"][0]["results"]
            self.assertEqual(
                [r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] for r in left],
                ["libs/a.cpp"],
            )

    def test_a_file_that_is_not_sarif_is_refused_and_left_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "r.sarif"
            path.write_text("not json")
            self.assertEqual(sdg.main(["x", str(path), BUILD]), 1)
            self.assertEqual(path.read_text(), "not json")


if __name__ == "__main__":
    unittest.main()
