"""Tests for panel_measure.py: the schedule, the figures taken from samples, the judgement, and the
sampler against a board that is a script. Nothing here touches a board."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import board_suite
import panel_measure as pm


def timing(last: float, mean: float, maximum: float, n: int) -> dict[str, Any]:
    return {"last": last, "mean": mean, "max": maximum, "n": n}


def panel_stats(
    *,
    psram_free: int = 20_000_000,
    refreshes: int = 100,
    meters: dict[str, Any] | None = None,
    stack: int = 9000,
    internal_low: int = 60_000,
) -> dict[str, Any]:
    out: dict[str, Any] = {
        "panel": {
            "width": 1024,
            "height": 600,
            "refreshes": refreshes,
            "refresh_us": timing(8000, 8000.0, 20000, refreshes),
            "task_stack_free_min": stack,
            "psram_free": psram_free,
            "psram_free_min": psram_free - 1000,
            "psram_largest": 1_000_000,
            "internal_free": 100_000,
            "internal_free_min": internal_low,
            "internal_largest": 50_000,
        }
    }
    if meters is not None:
        out["meters"] = meters
    return out


def meters(
    *,
    rate: int = 20,
    cost: int = 20_000,
    falls: int = 0,
    dropped: int = 0,
    tap: float = 25.0,
    outputs: int = 6,
    torn: int = 0,
) -> dict[str, Any]:
    return {
        "showing": True,
        "representation": "grid",
        "outputs": outputs,
        "width": 1024,
        "height": 600,
        "opens": 1,
        "open_ms": 50_000,
        "rate_hz": rate,
        "mean_cost_us": cost,
        "updates": 1000,
        "late": 0,
        "dropped": dropped,
        "falls": falls,
        "rises": 0,
        "analysis_us": timing(8000, 8000.0, 9000, 1000),
        "widgets_us": timing(1000, 1000.0, 2000, 1000),
        "render_us": timing(9000, 9000.0, 20000, 1000),
        "tap": {
            "blocks": 1000,
            "us_per_burst": tap,
            "us_max_block": 30,
            "reads": 1000,
            "torn": torn,
            "short_reads": 0,
        },
    }


def status(
    bursts: int,
    *,
    burst_us: int = 4000,
    underruns: int = 0,
    late: int = 0,
    error: int = 200,
    playing: str = "bursts",
) -> dict[str, Any]:
    return {
        "playing": playing,
        "bursts": bursts,
        "underruns": underruns,
        "late": late,
        "dropped": 0,
        "invalid": 0,
        "resyncs": 0,
        "error_us": error,
        "worst_error_us": error,
        "burst_us": burst_us,
        "worst_burst_us": burst_us + 500,
        "decode_stack_free": 18_000,
    }


def stretch(
    plan: list[pm.Segment],
    name: str,
    *,
    start: float,
    burst_us: int,
    with_meters: dict[str, Any] | None = None,
    stack: int = 9000,
    **kw: Any,
) -> list[dict[str, Any]]:
    """One sample a second through the segment `name`."""
    segment = next(s for s in plan if s.name == name)
    out = []
    for k in range(segment.seconds):
        t = start + k
        out.append(
            {
                "t": t,
                "segment": name,
                "status": status(int(t) * 30, burst_us=burst_us, **kw),
                "panel": panel_stats(refreshes=100 + int(t), meters=with_meters, stack=stack),
            }
        )
    return out


PLAN = pm.segments("all", 30, 3, 3, 5)


def full_run(
    *,
    meters_burst: int = 4100,
    meter: dict[str, Any] | None = None,
    glow_burst: int = 4000,
    **kw: Any,
) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    t = 0.0
    for segment in PLAN:
        use = meter if segment.view == "meters" else None
        burst = meters_burst if segment.view == "meters" else glow_burst
        samples += stretch(
            PLAN,
            segment.name,
            start=t,
            burst_us=burst,
            with_meters=use if segment.view == "meters" else None,
            **kw,
        )
        t += segment.seconds
    return samples


class Schedule(unittest.TestCase):
    def test_the_full_schedule_is_the_two_styles_and_the_view_twice_then_the_cycles(self) -> None:
        names = [s.name for s in pm.segments("all", 60, 2, 3, 10)]
        self.assertEqual(
            names[:5], ["settle", "speakers-glow", "meters-a", "speakers-pulse", "meters-b"]
        )
        self.assertEqual(
            names[5:], ["cycle-meters-0", "cycle-speakers-0", "cycle-meters-1", "cycle-speakers-1"]
        )

    def test_an_image_with_no_meters_view_is_measured_on_the_speakers_view_only(self) -> None:
        plan = pm.segments("speakers", 60, 20, 3, 10)
        self.assertEqual([s.name for s in plan], ["settle", "speakers-glow", "speakers-pulse"])
        self.assertTrue(all(s.view == "speakers" for s in plan))

    def test_the_run_is_the_segments_and_a_tail(self) -> None:
        plan = pm.segments("all", 60, 20, 3, 10)
        self.assertEqual(pm.total_seconds(plan), 10 + 4 * 60 + 20 * 6 + 20)

    def test_the_baseline_schedule_puts_the_dark_view_either_side_of_every_other_view(self) -> None:
        plan = pm.segments("baseline", 40, 20, 3, 10)
        self.assertEqual(
            [s.name for s in plan],
            [
                "settle",
                "dark-0",
                "speakers-glow",
                "dark-1",
                "meters-a",
                "dark-2",
                "speakers-pulse",
                "dark-3",
                "meters-b",
                "dark-4",
            ],
        )
        self.assertTrue(all(s.view == "dark" for s in plan if s.name.startswith("dark-")))
        # No cycles, however many are asked for: the leak test is the other schedule's.
        self.assertEqual(pm.total_seconds(plan), 10 + 9 * 40 + 20)

    def test_a_views_cost_is_its_burst_less_the_dark_stretches_either_side(self) -> None:
        figures = {
            "dark-0": {"burst_us_mean": 4000.0},
            "speakers-glow": {"burst_us_mean": 4100.0},
            "dark-1": {"burst_us_mean": 4040.0},
            "meters-a": {"burst_us_mean": 4300.0},
            "dark-2": {"burst_us_mean": 4080.0},
            "speakers-pulse": {"burst_us_mean": None},
        }
        costs = {c["segment"]: c for c in pm.dark_costs(figures)}
        self.assertEqual(set(costs), {"speakers-glow", "meters-a"})
        self.assertAlmostEqual(costs["speakers-glow"]["dark"], 4020.0)
        self.assertAlmostEqual(costs["speakers-glow"]["cost"], 80.0)
        self.assertAlmostEqual(costs["meters-a"]["cost"], 4300.0 - 4060.0)

    def test_a_view_at_the_end_has_the_one_dark_stretch_there_is_and_a_run_with_none_has_no_costs(
        self,
    ) -> None:
        self.assertEqual(
            pm.dark_costs({"meters-a": {"burst_us_mean": 5.0}, "dark-0": {"burst_us_mean": 3.0}})[
                0
            ]["cost"],
            2.0,
        )
        self.assertEqual(pm.dark_costs({"speakers-glow": {"burst_us_mean": 1.0}}), [])

    def test_the_segment_in_force_is_found_by_time_and_the_last_stays_after_the_end(self) -> None:
        plan = pm.segments("all", 60, 1, 3, 10)
        self.assertEqual(pm.segment_at(plan, 0).name, "settle")
        self.assertEqual(pm.segment_at(plan, 9.9).name, "settle")
        self.assertEqual(pm.segment_at(plan, 10).name, "speakers-glow")
        self.assertEqual(pm.segment_at(plan, 70).name, "meters-a")
        self.assertEqual(pm.segment_at(plan, 10_000).name, "cycle-speakers-0")

    def test_rows_are_picked_by_id_and_an_unknown_one_is_refused(self) -> None:
        self.assertEqual([r.id for r in pm.pick_rows(["eac3-5.1"])], ["eac3-5.1"])
        self.assertEqual(len(pm.pick_rows(None)), len(pm.ROWS))
        with self.assertRaises(board_suite.SuiteError):
            pm.pick_rows(["nope"])


class Figures(unittest.TestCase):
    def test_a_timing_mean_between_two_readings_is_the_mean_of_what_came_between(self) -> None:
        first = timing(0, 10.0, 0, 10)  # total 100
        last = timing(0, 16.0, 0, 20)  # total 320
        self.assertAlmostEqual(pm.timing_mean_between(first, last), 22.0)
        self.assertIsNone(pm.timing_mean_between(first, first))
        self.assertIsNone(pm.timing_mean_between(None, last))

    def test_a_segments_burst_is_read_between_two_readings_of_the_boards_running_mean(self) -> None:
        # The board's burst_us is the mean of every burst since the stream began: a minute at
        # 4,000 us and then a minute at 4,500 reads 4,250 at the end; the second minute's is 4,500.
        plan = [
            pm.Segment("a", "speakers", "glow", 60, 5),
            pm.Segment("b", "meters", "glow", 60, 5),
        ]
        samples: list[dict[str, Any]] = []
        total = 0
        bursts = 0
        for second in range(120):
            cost = 4000 if second < 60 else 4500
            for _ in range(30):
                bursts += 1
                total += cost
            samples.append(
                {
                    "t": float(second),
                    "segment": "a" if second < 60 else "b",
                    "status": status(bursts, burst_us=total // bursts),
                    "panel": None,
                }
            )
        grouped = pm.group_samples(samples, plan)
        first = pm.summarise(grouped["a"], plan[0])
        second = pm.summarise(grouped["b"], plan[1])
        self.assertAlmostEqual(first["burst_us_mean"], 4000, delta=1)
        self.assertAlmostEqual(second["burst_us_mean"], 4500, delta=15)
        self.assertAlmostEqual(second["burst_us_max"], 4500, delta=150)

    def test_the_mean_between_two_readings_needs_bursts_between_them(self) -> None:
        a = {"status": status(100, burst_us=4000)}
        b = {"status": status(160, burst_us=4100)}
        self.assertAlmostEqual(pm.running_mean_between(a, b), (4100 * 160 - 4000 * 100) / 60)
        self.assertIsNone(pm.running_mean_between(a, a))

    def test_percentile_takes_the_value_at_the_rank(self) -> None:
        self.assertEqual(pm.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.95), 10)
        self.assertEqual(pm.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.5), 5)
        self.assertEqual(pm.percentile([], 0.95), 0.0)

    def test_a_segment_is_summarised_over_the_warm_up_for_counters_and_after_it_for_the_bursts(
        self,
    ) -> None:
        samples = full_run()
        grouped = pm.group_samples(samples, PLAN)
        glow = pm.summarise(
            grouped["speakers-glow"], next(s for s in PLAN if s.name == "speakers-glow")
        )
        self.assertEqual(glow["samples"], 30)
        self.assertEqual(glow["steady_samples"], 25)
        self.assertEqual(glow["burst_us_mean"], 4000)
        self.assertEqual(glow["underruns_delta"], 0)
        self.assertEqual(glow["bursts_delta"], 29 * 30)
        self.assertEqual(glow["decode_stack_free_min"], 18_000)
        self.assertNotIn("meters", glow)

    def test_an_underrun_in_the_warm_up_still_counts(self) -> None:
        samples = full_run()
        for s in samples:
            if s["segment"] == "meters-a" and s["t"] > 40:
                s["status"]["underruns"] = 1
        grouped = pm.group_samples(samples, PLAN)
        figures = pm.summarise(grouped["meters-a"], next(s for s in PLAN if s.name == "meters-a"))
        self.assertEqual(figures["underruns_delta"], 1)

    def test_the_meters_figures_are_the_last_readings_of_the_segment(self) -> None:
        samples = full_run(meter=meters(rate=20, cost=18_000, tap=30.0))
        grouped = pm.group_samples(samples, PLAN)
        figures = pm.summarise(grouped["meters-a"], next(s for s in PLAN if s.name == "meters-a"))
        self.assertEqual(figures["meters"]["rate_hz_min"], 20)
        self.assertEqual(figures["meters"]["mean_cost_us"], 18_000)
        self.assertEqual(figures["meters"]["tap"]["us_per_burst"], 30.0)

    def test_the_psram_free_after_each_cycle_is_read_from_the_speakers_stretches(self) -> None:
        samples = []
        for k, free in enumerate([20_000_000, 19_990_000, 19_980_000]):
            samples.append(
                {
                    "t": 1.0 + k,
                    "segment": f"cycle-speakers-{k}",
                    "status": {},
                    "panel": panel_stats(psram_free=free),
                }
            )
            samples.append(
                {
                    "t": 1.5 + k,
                    "segment": f"cycle-meters-{k}",
                    "status": {},
                    "panel": panel_stats(psram_free=1),
                }
            )
        self.assertEqual(pm.cycle_psram(samples), [20_000_000, 19_990_000, 19_980_000])


class Judgement(unittest.TestCase):
    def verdicts(self, row: pm.Row, samples: list[dict[str, Any]]) -> dict[str, Any]:
        result = pm.analyse(row, samples, PLAN)
        return {v["id"]: v for v in result["verdicts"]}

    def test_a_clean_run_passes_every_criterion(self) -> None:
        row = pm.ROWS[1]  # E-AC-3 5.1
        verdicts = self.verdicts(row, full_run(meter=meters()))
        for ident in (
            "stream",
            "burst",
            "tap",
            "tap-torn",
            "rate",
            "cost",
            "dropped",
            "stack",
            "internal",
            "sync",
        ):
            self.assertTrue(verdicts[ident]["ok"], (ident, verdicts[ident]["text"]))
        # the cycle stretches here carry the same PSRAM free figure: no leak
        self.assertTrue(verdicts["leak"]["ok"], verdicts["leak"]["text"])

    def test_a_burst_over_the_allowance_fails(self) -> None:
        row = pm.ROWS[1]
        verdicts = self.verdicts(
            row, full_run(meters_burst=4000 + 700, meter=meters())
        )  # 15 % of 4000 is 600
        self.assertFalse(verdicts["burst"]["ok"])
        verdicts = self.verdicts(row, full_run(meters_burst=4000 + 500, meter=meters()))
        self.assertTrue(verdicts["burst"]["ok"])

    def test_a_small_burst_is_allowed_the_floor(self) -> None:
        verdicts = self.verdicts(
            pm.ROWS[0], full_run(glow_burst=300, meters_burst=500, meter=meters(outputs=2))
        )
        self.assertTrue(verdicts["burst"]["ok"])  # +200 us is under the 250 us floor
        verdicts = self.verdicts(
            pm.ROWS[0], full_run(glow_burst=300, meters_burst=600, meter=meters(outputs=2))
        )
        self.assertFalse(verdicts["burst"]["ok"])

    def test_an_underrun_anywhere_fails_the_stream(self) -> None:
        samples = full_run(meter=meters())
        for s in samples:
            if s["segment"] == "speakers-pulse" and s["t"] > 90:
                s["status"]["underruns"] = 2
        verdicts = self.verdicts(pm.ROWS[1], samples)
        self.assertFalse(verdicts["stream"]["ok"])
        self.assertIn("speakers-pulse", verdicts["stream"]["text"])

    def test_the_tap_has_a_larger_budget_for_a_room_of_more_than_eight_outputs(self) -> None:
        row = pm.ROWS[2]  # 16 outputs
        self.assertTrue(
            self.verdicts(row, full_run(meter=meters(tap=400.0, outputs=16)))["tap"]["ok"]
        )
        self.assertFalse(
            self.verdicts(row, full_run(meter=meters(tap=600.0, outputs=16)))["tap"]["ok"]
        )
        small = pm.ROWS[1]
        self.assertFalse(self.verdicts(small, full_run(meter=meters(tap=200.0)))["tap"]["ok"])

    def test_a_torn_history_read_fails(self) -> None:
        self.assertFalse(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(torn=1)))["tap-torn"]["ok"]
        )

    def test_a_cell_holds_twenty_a_second_or_else_a_steady_ten_and_which_is_said(self) -> None:
        twenty = self.verdicts(pm.ROWS[1], full_run(meter=meters()))["rate"]
        self.assertTrue(twenty["ok"])
        self.assertIn("holds 20 Hz", twenty["text"])
        # Fell to ten and held it: an update costs under half of a 100 ms step, no step missed.
        steady = self.verdicts(pm.ROWS[1], full_run(meter=meters(rate=10, falls=1, cost=30_000)))
        self.assertTrue(steady["rate"]["ok"])
        self.assertIn("steady 10 Hz", steady["rate"]["text"])
        # Fell to ten and still did not keep up: the kit's first run (an update cost 130 to 440 ms).
        slow = self.verdicts(
            pm.ROWS[2],
            full_run(meter=meters(rate=10, falls=1, cost=131_000, dropped=205, outputs=16)),
        )["rate"]
        self.assertFalse(slow["ok"])
        self.assertIn("neither 20 Hz nor a steady 10", slow["text"])
        # Cheap but missing steps: not steady.
        missing = self.verdicts(
            pm.ROWS[1], full_run(meter=meters(rate=10, falls=1, cost=30_000, dropped=50))
        )["rate"]
        self.assertFalse(missing["ok"])
        # A fall to ten and back is a fall: twenty is held only with none.
        fell = self.verdicts(pm.ROWS[1], full_run(meter=meters(rate=20, falls=1)))["rate"]
        self.assertTrue(fell["ok"])
        self.assertIn("steady 10 Hz", fell["text"])

    def test_faults_only_in_the_first_opening_say_so(self) -> None:
        samples = full_run(meter=meters())
        for s in samples:
            if s["segment"] == "meters-a" and s["t"] > 45:
                s["status"]["underruns"] = 1
                s["status"]["resyncs"] = 1
        text = self.verdicts(pm.ROWS[1], samples)["stream"]["text"]
        self.assertIn("only the first, cold opening", text)
        for s in samples:
            if s["segment"] == "meters-b" and s["t"] > 105:
                s["status"]["underruns"] = 1
        text = self.verdicts(pm.ROWS[1], samples)["stream"]["text"]
        self.assertNotIn("only the first, cold opening", text)

    def test_the_cost_is_held_to_half_of_the_step_in_force(self) -> None:
        self.assertTrue(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(cost=25_000)))["cost"]["ok"]
        )
        self.assertFalse(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(cost=25_001)))["cost"]["ok"]
        )
        ten = meters(rate=10, cost=49_000, falls=1, outputs=16)
        self.assertTrue(self.verdicts(pm.ROWS[2], full_run(meter=ten))["cost"]["ok"])

    def test_a_dropped_step_is_allowed_to_a_hundredth_of_them_and_no_more(self) -> None:
        # Two segments of 1,000 updates each.
        self.assertTrue(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(dropped=2)))["dropped"]["ok"]
        )
        self.assertFalse(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(dropped=60)))["dropped"]["ok"]
        )

    def test_a_panel_stack_nearly_used_up_fails(self) -> None:
        self.assertFalse(
            self.verdicts(pm.ROWS[1], full_run(meter=meters(), stack=3000))["stack"]["ok"]
        )

    def test_a_psram_leak_over_the_cycles_fails(self) -> None:
        samples = full_run(meter=meters())
        free = 20_000_000
        for s in samples:
            if s["segment"].startswith("cycle-speakers-"):
                k = int(s["segment"].rsplit("-", 1)[1])
                s["panel"]["panel"]["psram_free"] = free - k * 20_000
        verdicts = self.verdicts(pm.ROWS[1], samples)
        self.assertFalse(verdicts["leak"]["ok"])  # 40,000 lost over three openings
        for s in samples:
            if s["segment"].startswith("cycle-speakers-"):
                s["panel"]["panel"]["psram_free"] = (
                    free - int(s["segment"].rsplit("-", 1)[1]) * 4_000
                )
        self.assertTrue(self.verdicts(pm.ROWS[1], samples)["leak"]["ok"])

    def test_a_run_with_no_meters_segment_is_not_judged_on_them(self) -> None:
        verdicts = self.verdicts(pm.ROWS[1], full_run(meter=None))
        self.assertIsNone(verdicts["tap"]["ok"])
        self.assertIsNone(verdicts["rate"]["ok"])
        self.assertIsNone(verdicts["burst"]["ok"])

    def test_a_larger_sync_error_under_the_meters_view_fails(self) -> None:
        samples = full_run(meter=meters())
        for s in samples:
            if s["segment"] == "meters-b":
                s["status"]["error_us"] = 2500
        self.assertFalse(self.verdicts(pm.ROWS[1], samples)["sync"]["ok"])


class Report(unittest.TestCase):
    def test_the_report_has_a_table_a_line_a_criterion_and_a_verdict(self) -> None:
        result = pm.analyse(pm.ROWS[1], full_run(meter=meters()), PLAN)
        run = {
            "label": "abc1234 p4-rev3",
            "board": {
                "host": "192.168.1.251",
                "chip": "ESP32-P4",
                "version": "v1",
                "profile": "p4-rev3",
            },
            "recorded": "2026-10-26T12:00Z",
            "segment_s": 30,
            "warmup_s": 5,
            "rows": [result],
        }
        text = pm.render(run)
        self.assertIn("# Panel measurement: abc1234 p4-rev3", text)
        self.assertIn("| meters-a |", text)
        self.assertIn("- pass: rate:", text)
        self.assertIn("All criteria that could be judged pass.", text)

    def test_a_failure_is_named_in_the_verdict(self) -> None:
        result = pm.analyse(pm.ROWS[1], full_run(meter=meters(dropped=60)), PLAN)
        text = pm.render({"label": "x", "rows": [result]})
        self.assertIn("**FAIL**", text)
        self.assertIn("Failed: eac3-5.1 dropped.", text)

    def test_a_row_that_could_not_be_measured_says_why(self) -> None:
        text = pm.render(
            {
                "label": "x",
                "rows": [
                    {
                        "row": "joc-7.1.4",
                        "title": "JOC",
                        "layout": "7.1.4",
                        "outputs": 12,
                        "error": "needs --atmos",
                    }
                ],
            }
        )
        self.assertIn("**not measured:** needs --atmos", text)


class FakeBoard:
    """A board that is a script: it is idle until `start`, then plays until `end`, and the views the
    sampler puts are recorded."""

    def __init__(self, clock: FakeClock, start: float, end: float) -> None:
        self.clock = clock
        self.start = start
        self.end = end
        self.puts: list[tuple[str, str, float]] = []
        self.panel_404 = False

    def json(self, path: str) -> dict[str, Any]:
        now = self.clock.now
        playing = "bursts" if self.start <= now < self.end else "idle"
        return {"state": "playing", "sendspin": status(int(now) * 30, playing=playing)}

    def put(self, path: str, text: str) -> int:
        self.puts.append((path, text, self.clock.now))
        return 200

    def panel_stats(self) -> dict[str, Any] | None:
        return None if self.panel_404 else panel_stats(meters=meters())


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class SamplerTests(unittest.TestCase):
    def run_sampler(
        self, start: float, end: float, plan: list[pm.Segment], **kw: Any
    ) -> tuple[pm.Sampler, FakeBoard]:
        clock = FakeClock()
        board = FakeBoard(clock, start, end)
        sampler = pm.Sampler(board, plan, clock=clock, sleep=clock.sleep, **kw)
        sampler.run()
        return sampler, board

    def test_it_starts_its_clock_when_the_stream_starts_and_puts_each_view_at_its_time(
        self,
    ) -> None:
        plan = pm.segments("all", 10, 1, 2, 2)
        sampler, board = self.run_sampler(start=7.0, end=7.0 + pm.total_seconds(plan), plan=plan)
        puts = [(p, w, round(t - 7.0)) for p, w, t in board.puts]
        self.assertIn(("/now-playing", "speakers", 0), puts)
        self.assertIn(("/now-playing", "meters", 20), puts)  # settle 10 + glow 10
        self.assertIn(("/speakers-style", "pulse", 30), puts)
        self.assertTrue(sampler.samples)
        self.assertEqual(sampler.samples[0]["t"], 0.0)
        self.assertEqual(sampler.samples[0]["segment"], "settle")
        self.assertEqual({s["segment"] for s in sampler.samples}, {s.name for s in plan})

    def test_it_ends_when_the_stream_does(self) -> None:
        plan = pm.segments("speakers", 10, 0, 2, 2)
        sampler, _ = self.run_sampler(start=0.0, end=15.0, plan=plan)
        self.assertLess(sampler.samples[-1]["t"], 21.0)
        self.assertEqual(sampler.problems, [])

    def test_it_gives_up_on_a_stream_that_never_begins(self) -> None:
        plan = pm.segments("speakers", 10, 0, 2, 2)
        sampler, _ = self.run_sampler(start=10_000.0, end=20_000.0, plan=plan, start_wait_s=30.0)
        self.assertEqual(sampler.samples, [])
        self.assertIn("no stream began", sampler.problems[0])

    def test_a_board_without_panel_stats_is_sampled_with_none(self) -> None:
        plan = pm.segments("speakers", 10, 0, 2, 2)
        clock = FakeClock()
        board = FakeBoard(clock, 0.0, 12.0)
        board.panel_404 = True
        sampler = pm.Sampler(board, plan, clock=clock, sleep=clock.sleep)
        sampler.run()
        self.assertTrue(all(s["panel"] is None for s in sampler.samples))


class Settings(unittest.TestCase):
    def test_the_settings_found_are_put_back_and_a_refusal_is_a_note(self) -> None:
        class Putter:
            def __init__(self) -> None:
                self.log: list[tuple[str, str]] = []

            def put(self, path: str, text: str) -> int:
                self.log.append((path, text))
                return 409 if path == "/layout" else 200

        board = Putter()
        notes = pm.restore_settings(
            board,
            {
                "/now-playing": "speakers",
                "/speakers-style": "glow",
                "/dim-after": "5",
                "/night-schedule": "22:00-07:00 on",
                "/layout": "2.0",
            },
        )  # type: ignore[arg-type]
        self.assertEqual(
            [p for p, _ in board.log],
            ["/layout", "/night-schedule", "/dim-after", "/speakers-style", "/now-playing"],
        )
        self.assertEqual(len(notes), 1)
        self.assertIn("/layout", notes[0])

    def test_a_setting_the_board_did_not_report_is_left_alone(self) -> None:
        class Putter:
            def __init__(self) -> None:
                self.log: list[str] = []

            def put(self, path: str, text: str) -> int:
                self.log.append(path)
                return 200

        board = Putter()
        pm.restore_settings(board, {"/now-playing": "speakers", "/night-schedule": None})  # type: ignore[arg-type]
        self.assertEqual(board.log, ["/now-playing"])


class Comparison(unittest.TestCase):
    def run_of(self, glow: float, pulse: float | None = None) -> dict[str, Any]:
        figures: dict[str, Any] = {
            "speakers-glow": {"burst_us_mean": glow, "refresh_us_mean": 5000.0}
        }
        if pulse is not None:
            figures["speakers-pulse"] = {"burst_us_mean": pulse, "refresh_us_mean": 5200.0}
        return {"rows": [{"row": "eac3-5.1", "figures": figures}]}

    def test_the_glow_is_held_to_the_old_view_and_the_pulse_is_only_reported(self) -> None:
        rows = pm.compare_runs(self.run_of(4000.0), self.run_of(4150.0, 4600.0))
        self.assertEqual([r["segment"] for r in rows], ["speakers-glow", "speakers-pulse"])
        self.assertTrue(rows[0]["ok"])  # +150 us is under 5 % of 4000 (200)
        self.assertIsNone(rows[1]["ok"])
        self.assertAlmostEqual(rows[1]["change_pct"], 15.0)

    def test_a_glow_slower_than_the_allowance_fails(self) -> None:
        rows = pm.compare_runs(self.run_of(4000.0), self.run_of(4300.0))
        self.assertFalse(rows[0]["ok"])
        self.assertIn("**FAIL**", pm.render_comparison(rows))

    def test_a_short_burst_is_allowed_the_floor(self) -> None:
        self.assertTrue(pm.compare_runs(self.run_of(300.0), self.run_of(390.0))[0]["ok"])
        self.assertFalse(pm.compare_runs(self.run_of(300.0), self.run_of(410.0))[0]["ok"])

    def test_a_row_either_image_could_not_measure_is_left_out(self) -> None:
        after = {"rows": [{"row": "eac3-5.1", "error": "needs --atmos"}]}
        self.assertEqual(pm.compare_runs(self.run_of(4000.0), after), [])
        self.assertEqual(pm.compare_runs({"rows": []}, self.run_of(4000.0)), [])


class Cli(unittest.TestCase):
    def test_plan_lists_the_rows_and_the_length(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = pm.main(["plan", "--rows", "eac3-5.1", "pcm-2.0"])
        self.assertEqual(code, 0)
        text = out.getvalue()
        self.assertIn("eac3-5.1\t5.1\t6 outputs", text)
        self.assertIn("pcm-2.0\t2.0\t2 outputs\ttone", text)
        self.assertIn("about", text)

    def test_run_refuses_without_a_booking(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = pm.main(["run", "--label", "x", "--out", "unused", "--rows", "pcm-2.0"])
        self.assertEqual(code, 2)
        self.assertIn("refused", err.getvalue())


if __name__ == "__main__":
    unittest.main()
