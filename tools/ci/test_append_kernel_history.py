"""Unit tests for append_kernel_history.py, the per-kernel ns/call trend
writer.

This series is deliberately NON-GATING: even a doubling of one kernel must
only annotate (a ::warning:: tagged non-gating), never ::error:: and never
publish a failing output. What it must still do is append one record per
(leg, kernel), keyed so one kernel's trailing mean never mixes with another's,
nor one CPU model's with another's when the run names its own.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_kernel_history as akh


class KernelHistory(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.results = self.tmp / "results"
        self.hist_dir = self.tmp / "hist"
        self.history = self.hist_dir / "kernels-main.jsonl"

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, kernels, leg="linux", cpu_model=None):
        d = self.results / f"kernels-{leg}"
        d.mkdir(parents=True, exist_ok=True)
        (d / "k.json").write_text(json.dumps({"kernels": [
            {"name": n, "iters": 1000, "ns_per_call": v} for n, v in kernels.items()]}))
        if cpu_model is not None:
            (d / "environment.json").write_text(json.dumps(
                {"cpu_model": cpu_model, "runner_image": "unknown/unknown", "arch": "x86_64"}))

    def seed(self, rows):
        self.hist_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(r) + "\n" for r in rows) + "\n")

    def run_main(self):
        argv = ["x", "--results-dir", str(self.results), "--history-dir", str(self.hist_dir),
                "--branch", "main", "--commit", "c", "--commit-date", "d"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(buf):
            return akh.main(), buf.getvalue()

    def test_empty_results_append_nothing(self):
        self.results.mkdir()
        (self.results / "file.txt").write_text("")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertFalse(self.history.exists())

    def test_records_are_appended_per_kernel(self):
        self.write({"mdct": 100.0, "bitalloc": 50.0})
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        recs = [json.loads(line) for line in self.history.read_text().splitlines()]
        self.assertEqual([(r["leg"], r["kernel"], r["iters"]) for r in recs],
                         [("linux", "mdct", 1000), ("linux", "bitalloc", 1000)])
        self.assertIn("Appended 2 record(s)", out)

    def test_doubling_only_warns_non_gating(self):
        self.seed([{"commit": "o", "leg": "linux", "kernel": "mdct", "ns_per_call": 100.0}] * 3 +
                  [{"commit": "o", "leg": "linux", "kernel": "other", "ns_per_call": 1.0}])
        self.write({"mdct": 250.0})
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("Kernel trend hard regression (non-gating)::linux/mdct", out)
        self.assertNotIn("::error", out)

    def test_soft_tier_and_quiet_tier(self):
        self.seed([{"commit": "o", "leg": "linux", "kernel": "a", "ns_per_call": 100.0},
                   {"commit": "o", "leg": "linux", "kernel": "b", "ns_per_call": 100.0}])
        self.write({"a": 125.0, "b": 110.0})
        _, out = self.run_main()
        self.assertIn("::warning title=Kernel trend regression::linux/a", out)
        self.assertNotIn("linux/b", out)

    def test_a_commit_already_recorded_is_not_recorded_again(self):
        """A second run of a commit (the nightly, a dispatch on main) adds the kernels the
        first did not have, and is judged against what came before the commit."""
        self.write({"mdct": 100.0})
        self.run_main()
        self.write({"mdct": 250.0, "bitalloc": 50.0})
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        recs = [json.loads(line) for line in self.history.read_text().splitlines()]
        self.assertEqual([(r["kernel"], r["ns_per_call"]) for r in recs],
                         [("mdct", 100.0), ("bitalloc", 50.0)])
        self.assertIn("Appended 1 record(s)", out)
        self.assertIn("(1 already recorded for c)", out)
        # Nothing came before this commit, so 250 against its own 100 is no finding.
        self.assertNotIn("::warning", out)

    def test_trailing_mean_edge_cases(self):
        self.assertIsNone(akh.trailing_mean(self.history, "l", "k", 10))
        self.seed([{"leg": "l", "kernel": "k", "ns_per_call": v} for v in (9.0, 1.0, 3.0)])
        self.assertEqual(akh.trailing_mean(self.history, "l", "k", 2), 2.0)
        self.assertIsNone(akh.trailing_mean(self.history, "l", "zz", 2))

    def test_a_cpu_model_takes_only_its_own_records(self):
        self.seed([{"leg": "l", "kernel": "k", "ns_per_call": v, "cpu_model": cpu}
                   for cpu, v in (("slow", 40.0), ("slow", 60.0), ("fast", 20.0), ("fast", 20.0))] +
                  [{"leg": "l", "kernel": "k", "ns_per_call": 90.0}])
        self.assertEqual(akh.trailing_mean(self.history, "l", "k", 10, cpu_model="slow"), 50.0)
        self.assertIsNone(akh.trailing_mean(self.history, "l", "k", 10, cpu_model="other"))
        self.assertIsNone(akh.trailing_mean(self.history, "l", "k", 10, cpu_model="slow",
                                            min_records=3))

    def test_the_machine_is_read_from_the_leg_and_recorded(self):
        """environment.json sits beside the bench's file and is not one of its files: the
        loader used to read every *.json in the leg as kernel_bench.cpp's output."""
        self.write({"mdct": 100.0}, cpu_model="AMD EPYC 7763 64-Core Processor")
        self.write({"mdct": 300.0}, leg="bare")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        recs = [json.loads(line) for line in self.history.read_text().splitlines()]
        self.assertEqual([(r["leg"], r["kernel"], r["cpu_model"]) for r in recs],
                         [("bare", "mdct", ""),
                          ("linux", "mdct", "AMD EPYC 7763 64-Core Processor")])
        self.assertIn("bare (no CPU model recorded): 0 series judged against the same CPU "
                      "model's records, 0 against every machine's, 1 with no earlier record.", out)

    def test_a_slower_machine_is_not_a_slower_kernel(self):
        """Ten runs on the fast model, then the slow model's usual figure: +110% against
        them, no change against its own record before them."""
        self.seed([{"commit": "o", "leg": "linux", "kernel": "mdct", "ns_per_call": v,
                    "cpu_model": cpu} for cpu, v in [("slow", 440.0)] + [("fast", 210.0)] * 10])
        self.write({"mdct": 442.0}, cpu_model="slow")
        _, out = self.run_main()
        self.assertNotIn("::warning", out)
        self.assertIn("linux (measured on slow): 1 series judged against the same CPU model's "
                      "records, 0 against every machine's, 0 with no earlier record.", out)

    def test_a_slowdown_on_one_machine_names_that_machine(self):
        self.seed([{"commit": "o", "leg": "linux", "kernel": "mdct", "ns_per_call": v,
                    "cpu_model": cpu} for cpu, v in [("fast", 200.0)] * 2 + [("slow", 440.0)] * 10])
        self.write({"mdct": 250.0}, cpu_model="fast")
        _, out = self.run_main()
        self.assertIn("::warning title=Kernel trend regression::linux/mdct: 250.0 ns/call is "
                      "25% slower than the trailing 10-run mean on the same CPU model, fast "
                      "(200.0 ns/call) on main.", out)

    def test_the_history_from_before_the_machine_was_recorded_is_every_machines(self):
        """kernels-main.jsonl named no CPU model until 2026-10-13: a run that names one is
        judged against all of it, and says that it was."""
        self.seed([{"commit": "o", "leg": "linux", "kernel": "mdct", "ns_per_call": 100.0}] * 3)
        self.write({"mdct": 250.0}, cpu_model="AMD EPYC 7763 64-Core Processor")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("Kernel trend hard regression (non-gating)::linux/mdct: 250.0 ns/call is "
                      "150% slower than the trailing 10-run mean of every machine (100.0 "
                      "ns/call; too few earlier records are from this run's CPU model, AMD EPYC "
                      "7763 64-Core Processor, to compare with those alone, so a difference "
                      "between machines reads here as a change) on main.", out)
        self.assertNotIn("::error", out)
        self.assertIn("0 series judged against the same CPU model's records, 1 against every "
                      "machine's", out)


class TrendPageMirror(unittest.TestCase):
    """docs/performance-trend.md flags a kernel row in its own column by the rule this script
    annotates by, written again in JavaScript. There is no browser test, so what can be held
    is held here: the page's floor is the script's, and, where Node is installed, the page's
    function takes the mean the script takes for every row of a history of mixed machines."""

    PAGE = Path(__file__).resolve().parents[2] / "docs" / "performance-trend.md"

    def page_rule(self):
        text = self.PAGE.read_text(encoding="utf-8").replace("\r\n", "\n")
        found = [re.search(pattern, text, re.DOTALL | re.MULTILINE) for pattern in (
            r"^  function perfTrendCpu\(rec\) \{\n.*?\n  \}$",
            r"^  const PERF_TREND_MIN_SAME_CPU_RECORDS = \d+;$",
            r"^  function perfTrendBaselineRows\(rows, i, window\) \{\n.*?\n  \}$")]
        self.assertNotIn(None, found, f"{self.PAGE} no longer has the baseline rule")
        return "\n".join(m.group(0) for m in found)

    def test_the_page_has_the_scripts_floor(self):
        self.assertIn(f"const PERF_TREND_MIN_SAME_CPU_RECORDS = {akh.MIN_SAME_CPU_RECORDS};",
                      self.page_rule())

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_the_page_takes_the_mean_the_script_takes(self):
        # Two models that take turns unevenly, with records that name no model and the
        # capture script's "unknown" among them, and more of one model than the window holds.
        cpus = ["slow", None, "fast", "slow", "slow", "unknown", "fast", "slow"]
        rows = [{"commit": f"c{i}", "leg": "l", "kernel": "k", "ns_per_call": 100.0 + 7 * i,
                 **({} if cpus[i % len(cpus)] is None else {"cpu_model": cpus[i % len(cpus)]})}
                for i in range(40)]
        script = self.page_rule() + """
          const { rows, window } = JSON.parse(require("node:fs").readFileSync(0, "utf8"));
          console.log(JSON.stringify(rows.map((r, i) => {
            const basis = perfTrendBaselineRows(rows, i, window);
            const values = basis.rows.map((p) => p.ns_per_call);
            return [values.length ? values.reduce((a, b) => a + b, 0) / values.length : null,
                    basis.sameCpu];
          })));"""
        asked = json.dumps({"rows": rows, "window": akh.REGRESSION_TRAILING_WINDOW})
        done = subprocess.run(["node", "-e", script], input=asked,
                              capture_output=True, encoding="utf-8", check=False, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        page = json.loads(done.stdout)

        with tempfile.TemporaryDirectory() as tmp:
            history = Path(tmp) / "kernels-main.jsonl"
            for i, row in enumerate(rows):
                history.write_text("".join(json.dumps(r) + "\n" for r in rows[:i]))
                mean, same_cpu = akh.like_for_like_baseline(
                    history, "l", "k", row.get("cpu_model", ""))
                with self.subTest(row=i, cpu_model=row.get("cpu_model")):
                    self.assertEqual(page[i][1], same_cpu)
                    if mean is None:
                        self.assertIsNone(page[i][0])
                    else:
                        self.assertAlmostEqual(page[i][0], mean)
        # Both ways of taking it were met, or the comparison above held nothing.
        self.assertEqual({same for _, same in page}, {True, False})


if __name__ == "__main__":
    unittest.main()
