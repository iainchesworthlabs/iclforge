"""Unit tests for check_probe_ceilings.py, the AC-4 probe's per-fixture ceilings gate.

What it must catch: a fixture over its ceiling, a fixture with no entry in the table
(so a fixture added to the probe cannot go ungated), and a run that printed none of the
metric's lines. A fixture at its ceiling passes.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_probe_ceilings as cpc

TABLE = {
    "fixtures": {
        "ac4_20_music": {
            "peak_heap": 325000,
            "steady_allocs_per_frame": 58,
            "s3_internal_peak": 6000,
        },
        "ac4_51_music": {
            "peak_heap": 795000,
            "steady_allocs_per_frame": 168,
            "s3_internal_peak": 14000,
        },
    }
}
# A probe prints several key=value pairs on one line.
RUN = (
    "ac4_20_music.peak_bytes=295225 ac4_20_music.stack_bytes=19736\n"
    "ac4_20_music.steady_allocs_per_frame=55\n"
    "ac4_20_music.esp32s3.internal_peak_bytes=3188 ac4_20_music.esp32s3.psram_peak_bytes=286000\n"
    "ac4_51_music.peak_bytes=704311 ac4_51_music.stack_bytes=21080\n"
    "ac4_51_music.steady_allocs_per_frame=143\n"
    "ac4_51_music.esp32s3.internal_peak_bytes=12012\n"
    "heap.peak_bytes=704311\n"
)


class ProbeCeilings(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.table = self.dir / "ceilings.json"
        self.table.write_text(json.dumps(TABLE))

    def tearDown(self):
        self._tmp.cleanup()

    def run_check(self, metric, text):
        run = self.dir / "run.txt"
        run.write_text(text)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cpc.main(["--table", str(self.table), "--metric", metric, str(run)])
        return rc, out.getvalue(), err.getvalue()

    def test_each_metric_reads_its_own_lines_and_passes(self):
        for metric in ("peak_heap", "steady_allocs_per_frame", "s3_internal_peak"):
            rc, out, err = self.run_check(metric, RUN)
            self.assertEqual(rc, 0, (metric, err))
            self.assertIn("ac4_51_music", out)

    def test_the_global_heap_line_is_not_a_fixture(self):
        rc, out, _ = self.run_check("peak_heap", RUN)
        self.assertEqual(rc, 0)
        self.assertNotIn("heap = ", out)

    def test_a_value_over_its_ceiling_fails(self):
        over = RUN.replace("ac4_20_music.peak_bytes=295225", "ac4_20_music.peak_bytes=325001")
        rc, _, err = self.run_check("peak_heap", over)
        self.assertEqual(rc, 1)
        self.assertIn("ac4_20_music", err)
        self.assertIn("ceiling is 325000", err)

    def test_a_value_at_its_ceiling_passes(self):
        at = RUN.replace("steady_allocs_per_frame=55", "steady_allocs_per_frame=58")
        rc, _, _ = self.run_check("steady_allocs_per_frame", at)
        self.assertEqual(rc, 0)

    def test_a_fixture_without_an_entry_fails(self):
        rc, _, err = self.run_check("peak_heap", RUN + "ac4_71_new.peak_bytes=1000\n")
        self.assertEqual(rc, 1)
        self.assertIn("ac4_71_new has no peak_heap entry", err)

    def test_a_run_without_the_metric_fails(self):
        rc, _, err = self.run_check("s3_internal_peak", "ac4_20_music.peak_bytes=295225\n")
        self.assertEqual(rc, 1)
        self.assertIn("reported no", err)

    def test_the_title_is_the_callers(self):
        over = RUN.replace("704311 ac4_51_music.stack", "800000 ac4_51_music.stack")
        rc, _, err = self.run_check("peak_heap", over)
        self.assertEqual(rc, 1)
        self.assertIn("title=Footprint regression", err)
        run = self.dir / "run.txt"
        with contextlib.redirect_stderr(io.StringIO()) as e:
            cpc.main(
                [
                    "--table", str(self.table),
                    "--metric", "peak_heap",
                    "--title", "ESP32-S3 footprint regression",
                    str(run),
                ]
            )
        self.assertIn("title=ESP32-S3 footprint regression", e.getvalue())

    def test_the_committed_table_has_every_metric_for_every_fixture(self):
        table = Path(__file__).resolve().parents[2] / "testdata" / "ac4-probe-ceilings.json"
        fixtures = json.loads(table.read_text())["fixtures"]
        self.assertEqual(len(fixtures), 6)
        for name, entry in fixtures.items():
            for metric in cpc.METRICS:
                self.assertIn(metric, entry, (name, metric))


if __name__ == "__main__":
    unittest.main()
