"""Unit tests for append_performance_history.py, the post-merge throughput
trend writer.

What it must do: append exactly one record per (leg, config) - the FASTEST
repetition's whole record, not per-field minima - to the published history,
and flag a slowdown against the trailing mean: >=20% warns, >=100% emits an
::error:: and publishes hard_regression=true (while STILL writing the data,
so a big regression is never un-recorded). The trailing mean is of the records
measured on the run's own CPU model: a slower machine is not a slower build,
and a slower build on a fast machine is not hidden by the slow ones before it.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_performance_history as aph


def write_leg(results: Path, leg: str, runs: list[list[dict]], environment=None):
    d = results / f"performance-{leg}"
    d.mkdir(parents=True, exist_ok=True)
    for i, results_list in enumerate(runs):
        (d / f"bench{i}.json").write_text(json.dumps({
            "real_time_budget_ms_per_frame": 32.0, "results": results_list}))
    if environment is not None:
        (d / "environment.json").write_text(json.dumps(environment))


def row(name, ms, **extra):
    return {"name": name, "frames": 100, "total_ms": ms * 100, "ms_per_frame": ms, **extra}


class LoadLegResults(unittest.TestCase):
    def test_minimum_run_record_is_taken_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = Path(tmp)
            (res / "stray.txt").write_text("not a leg")
            write_leg(res, "linux", [
                [row("ac3", 2.0, p95_ms_per_frame=9.0, max_ms_per_frame=10.0)],
                [row("ac3", 1.0, p95_ms_per_frame=1.5, max_ms_per_frame=20.0)],
            ], environment={"cpu_model": "EPYC", "runner_image": "ubuntu-24.04"})
            write_leg(res, "old", [[row("ac3", 0.0)]])
            recs = list(aph.load_leg_results(res))
        self.assertEqual(len(recs), 2)
        linux = recs[0]
        self.assertEqual(linux["leg"], "linux")
        self.assertEqual((linux["ms_per_frame"], linux["p95_ms_per_frame"],
                          linux["max_ms_per_frame"]), (1.0, 1.5, 20.0))
        self.assertEqual(linux["runs"], 2)
        self.assertAlmostEqual(linux["spread"], 1.0)
        self.assertEqual(linux["cpu_model"], "EPYC")
        old = recs[1]
        # Older binaries emit no tail: recorded as None, not zero.
        self.assertIsNone(old["p95_ms_per_frame"])
        self.assertEqual((old["spread"], old["cpu_model"]), (0.0, ""))

    def test_a_result_budget_overrides_the_file_budget(self):
        """AC-4 rows carry their own 2 048-sample budget; rows without one
        keep the file's A/52 budget."""
        with tempfile.TemporaryDirectory() as tmp:
            res = Path(tmp)
            write_leg(res, "linux", [[
                row("plain_51", 1.0),
                row("ac4_51_encode", 2.0, real_time_budget_ms_per_frame=42.667),
            ]])
            recs = {r["config"]: r for r in aph.load_leg_results(res)}
        self.assertEqual(recs["plain_51"]["real_time_budget_ms_per_frame"], 32.0)
        self.assertEqual(recs["ac4_51_encode"]["real_time_budget_ms_per_frame"], 42.667)


class TrailingMean(unittest.TestCase):
    def test_window_and_matching(self):
        with tempfile.TemporaryDirectory() as tmp:
            h = Path(tmp) / "h.jsonl"
            self.assertIsNone(aph.trailing_mean(h, "l", "c", 3))
            lines = [json.dumps({"leg": "l", "config": "c", "ms_per_frame": v})
                     for v in (100.0, 1.0, 2.0, 3.0)]
            lines.append(json.dumps({"leg": "other", "config": "c", "ms_per_frame": 50}))
            h.write_text("\n".join(lines) + "\n\n")
            self.assertEqual(aph.trailing_mean(h, "l", "c", 3), 2.0)
            self.assertIsNone(aph.trailing_mean(h, "l", "nope", 3))

    def test_a_cpu_model_takes_only_its_own_records(self):
        """However far back they are: the window counts that model's records, not the
        series'. A record that names no model matches none."""
        with tempfile.TemporaryDirectory() as tmp:
            h = Path(tmp) / "h.jsonl"
            rows = [("slow", 4.0), ("slow", 6.0), (None, 9.0)] + [("fast", 2.0)] * 12
            h.write_text("".join(json.dumps(
                {"leg": "l", "config": "c", "ms_per_frame": ms,
                 **({} if cpu is None else {"cpu_model": cpu})}) + "\n" for cpu, ms in rows))
            self.assertEqual(aph.trailing_mean(h, "l", "c", 10), 2.0)
            self.assertEqual(aph.trailing_mean(h, "l", "c", 10, cpu_model="slow"), 5.0)
            self.assertEqual(aph.trailing_mean(h, "l", "c", 1, cpu_model="slow"), 6.0)
            self.assertIsNone(aph.trailing_mean(h, "l", "c", 10, cpu_model="other"))
            # Too few of a model's records is no mean of them, not a mean of fewer.
            slow = {"cpu_model": "slow"}
            self.assertIsNone(aph.trailing_mean(h, "l", "c", 10, min_records=3, **slow))
            self.assertEqual(aph.trailing_mean(h, "l", "c", 10, min_records=2, **slow), 5.0)


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.results = self.tmp / "results"
        self.history_dir = self.tmp / "history"
        self.history = self.history_dir / "performance-main.jsonl"
        self.output = self.tmp / "gh_output"

    def tearDown(self):
        self._tmp.cleanup()

    def seed(self, ms_values, leg="linux", config="ac3"):
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(
            {"commit": f"c{i}", "leg": leg, "config": config, "ms_per_frame": v}) + "\n"
            for i, v in enumerate(ms_values)))

    def run_main(self):
        argv = ["x", "--results-dir", str(self.results), "--history-dir", str(self.history_dir),
                "--branch", "main", "--commit", "abc", "--commit-date", "2026-01-01T00:00:00Z"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aph.main()
        return rc, buf.getvalue(), (self.output.read_text() if self.output.exists() else "")

    def test_no_results_is_a_warning_not_an_append(self):
        self.results.mkdir()
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertFalse(self.history.exists())
        self.assertEqual(verdict, "")

    def test_first_record_is_appended_with_no_verdict_trip(self):
        write_leg(self.results, "linux", [[row("ac3", 1.0)]])
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        rec = json.loads(self.history.read_text())
        self.assertEqual((rec["commit"], rec["branch"], rec["leg"], rec["config"]),
                         ("abc", "main", "linux", "ac3"))
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertNotIn("::warning", out)

    def test_doubling_is_hard_but_still_recorded(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 2.0)]])
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("::error title=Performance trend hard regression::linux/ac3", out)
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertEqual(len(self.history.read_text().splitlines()), 6)

    def test_soft_tier_only_warns(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 1.25)]])
        _, out, verdict = self.run_main()
        self.assertIn("::warning title=Performance trend regression::", out)
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_below_soft_tier_is_quiet(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 1.19)]])
        _, out, _verdict = self.run_main()
        self.assertNotIn("::warning", out)
        self.assertNotIn("::error", out)

    def test_a_commit_already_recorded_is_not_recorded_again(self):
        """The nightly run and a dispatch on main measure a commit the run after its merge
        may have recorded already: the series keeps one point a commit, and a second run
        adds only the leg the first did not have."""
        write_leg(self.results, "linux", [[row("ac3", 1.0)]])
        self.run_main()
        write_leg(self.results, "linux", [[row("ac3", 1.1)]])
        write_leg(self.results, "arm", [[row("ac3", 3.0)]])
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        recs = [json.loads(line) for line in self.history.read_text().splitlines()]
        self.assertEqual([(r["leg"], r["ms_per_frame"]) for r in recs],
                         [("linux", 1.0), ("arm", 3.0)])
        self.assertIn("Appended 1 record(s)", out)
        self.assertIn("(1 already recorded for abc)", out)
        # The output file holds a line for each of the two runs: the last is this one's.
        self.assertEqual(verdict.splitlines()[-1], "hard_regression=false")

    def test_a_repeated_run_is_judged_against_what_came_before_its_commit(self):
        """Its own earlier record is not part of its baseline: with it the mean would be
        1.17 and a doubling would read as +71%, a warning, the second time."""
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 2.0)]])
        self.run_main()
        _, out, verdict = self.run_main()
        self.assertIn("::error title=Performance trend hard regression::linux/ac3", out)
        self.assertEqual(verdict.splitlines()[-1], "hard_regression=true")
        self.assertEqual(len(self.history.read_text().splitlines()), 6)

    def test_emit_output_is_a_noop_locally(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            aph.emit_github_output("x", "y")   # must not raise


class LikeForLike(unittest.TestCase):
    """The hosted runner is one of several CPU models from run to run, twice as fast on one
    as on another. The figures are the shape of main's on 2026-10-12: 4.4 ms/frame on one
    model, 2.1 on another, the same commit."""

    SLOW, FAST = "AMD EPYC 7763 64-Core Processor", "AMD EPYC 9V45 96-Core Processor"

    setUp, tearDown, run_main = Main.setUp, Main.tearDown, Main.run_main

    def seed(self, rows):
        """`rows` is (cpu_model, ms_per_frame), oldest first; None is a record with no model."""
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(
            {"commit": f"c{i}", "leg": "linux", "config": "ac3", "ms_per_frame": ms,
             **({} if cpu is None else {"cpu_model": cpu})}) + "\n"
            for i, (cpu, ms) in enumerate(rows)))

    def measure(self, ms, cpu_model=None):
        write_leg(self.results, "linux", [[row("ac3", ms)]],
                  environment=None if cpu_model is None else {"cpu_model": cpu_model})
        return self.run_main()

    def test_a_slower_machine_is_not_a_slower_build(self):
        """The trailing ten are all the fast model. Against them the slow model's usual
        figure is +110%, which failed the job; against its own last record it is no change."""
        self.seed([(self.SLOW, 4.4)] + [(self.FAST, 2.1)] * 10)
        rc, out, verdict = self.measure(4.42, self.SLOW)
        self.assertEqual(rc, 0)
        self.assertNotIn("::error", out)
        self.assertNotIn("::warning", out)
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertIn(f"linux (measured on {self.SLOW}): 1 series judged against the same CPU "
                      "model's records, 0 against every machine's, 0 with no earlier record.", out)

    def test_a_slowdown_on_a_fast_machine_is_not_hidden_by_the_slow_ones(self):
        """The other half: 2.1 to 2.7 on the fast model is +29%, and 4.4 is its doubling.
        Against the slow model's 4.4 before it, one reads as faster and one as no change."""
        history = [(self.FAST, 2.1)] * 3 + [(self.SLOW, 4.4)] * 10
        self.seed(history)
        _, out, verdict = self.measure(2.7, self.FAST)
        self.assertIn("::warning title=Performance trend regression::linux/ac3: 2.700 ms/frame "
                      "is 29% slower than the trailing 10-run mean on the same CPU model, "
                      f"{self.FAST} (2.100 ms/frame) on main.", out)
        self.assertEqual(verdict, "hard_regression=false\n")

        self.seed(history)
        _, out, verdict = self.measure(4.4, self.FAST)
        self.assertIn("::error title=Performance trend hard regression::linux/ac3: 4.400 "
                      "ms/frame is 110% slower than the trailing 10-run mean on the same CPU "
                      f"model, {self.FAST} (2.100 ms/frame) on main", out)
        self.assertEqual(verdict.splitlines()[-1], "hard_regression=true")

    def test_a_cpu_model_the_history_has_not_met_is_judged_against_every_machine(self):
        """There is nothing like it to compare with, so the old comparison is made, both
        tiers of it, and the annotation says what it is a comparison with."""
        self.seed([(self.FAST, 2.0)] * 5)
        _, out, verdict = self.measure(2.5, "Neoverse-N2")
        self.assertIn("::warning title=Performance trend regression::linux/ac3: 2.500 ms/frame "
                      "is 25% slower than the trailing 10-run mean of every machine (2.000 "
                      "ms/frame; too few earlier records are from this run's CPU model, "
                      "Neoverse-N2, to compare with those alone, so a difference between "
                      "machines reads here as a change) on main.", out)
        self.assertIn("linux (measured on Neoverse-N2): 0 series judged against the same CPU "
                      "model's records, 1 against every machine's, 0 with no earlier record.", out)
        self.assertEqual(verdict, "hard_regression=false\n")

        self.seed([(self.FAST, 2.0)] * 5)
        _, out, verdict = self.measure(4.0, "Neoverse-N2")
        self.assertIn("::error title=Performance trend hard regression::linux/ac3", out)
        self.assertIn("mean of every machine", out)
        self.assertEqual(verdict.splitlines()[-1], "hard_regression=true")

    def test_a_run_that_names_no_cpu_model_is_judged_against_every_record(self):
        """No environment.json, and the capture script's "unknown": neither is a machine,
        so "unknown" is not matched with the "unknown" records before it."""
        for cpu_model in (None, "unknown"):
            with self.subTest(cpu_model=cpu_model):
                self.seed([("unknown", 1.0)] * 3 + [(self.SLOW, 3.0)] * 3)
                _, out, _ = self.measure(2.5, cpu_model)
                self.assertIn("is 25% slower than the trailing 10-run mean of every machine "
                              "(2.000 ms/frame; this run recorded no CPU model, so a difference "
                              "between machines reads here as a change)", out)
                self.assertIn("linux (no CPU model recorded): 0 series judged against the same "
                              "CPU model's records, 1 against every machine's", out)

    def test_records_that_name_no_cpu_model_are_no_ones_like(self):
        """The history from before the model was recorded: a run that names one has no
        record like it there, and is judged against all of them."""
        self.seed([(None, 1.0)] * 5)
        _, out, _ = self.measure(1.25, self.SLOW)
        self.assertIn("mean of every machine (1.000 ms/frame; too few earlier records", out)

    def test_a_repeated_run_is_not_its_own_like(self):
        """The first run of a commit on a model the history has not met records it. The
        second is still judged against what came before the commit, which has none."""
        self.seed([(self.FAST, 2.0)] * 5)
        self.measure(4.0, self.SLOW)
        _, out, verdict = self.measure(4.0, self.SLOW)
        self.assertIn("mean of every machine", out)
        self.assertEqual(verdict.splitlines()[-1], "hard_regression=true")

    def test_the_floor_on_a_models_records_is_one_constant(self):
        """With a higher floor, a model with fewer records than it falls back."""
        self.seed([(self.SLOW, 4.4)] * 2 + [(self.FAST, 2.1)] * 10)
        with mock.patch.object(aph, "MIN_SAME_CPU_RECORDS", 3):
            _, out, verdict = self.measure(4.4, self.SLOW)
        self.assertIn("::error title=Performance trend hard regression::linux/ac3", out)
        self.assertIn("mean of every machine (2.100 ms/frame; too few earlier records", out)
        self.assertEqual(verdict, "hard_regression=true\n")


if __name__ == "__main__":
    unittest.main()
