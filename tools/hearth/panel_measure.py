#!/usr/bin/env python3
"""Measure what the screen costs on a booked board, from the board's own numbers.

The kit protocol of planning/p4x-meters-view.md ("Kit protocol"), as a program a session can run
unattended. A stream is played to the board over Sendspin by the repository's test server (the way
the product plays, and the only path the Meters view's tap listens to); while it plays, this
changes what the screen shows on a schedule (the speakers view in its glow and pulse styles, the
Meters view) and reads `GET /status` and `GET /panel-stats` once a second. What it takes from
them, per stretch of the schedule, is the audio path's side (underruns, late and dropped bursts,
the burst's time, the sync error), the panel's (refresh times, stack, heap, PSRAM) and the Meters
view's (the rate it holds, the time its analysis, widgets and render take, the tap's cost). It
judges the Meters stretches against the speakers stretches of the same stream and writes the
samples, the figures and a report in Markdown.

    panel_measure.py plan   [--views all|speakers] [--rows ID ...]
    panel_measure.py run    --board P4X --session ID --label TEXT --out DIR [--views ...]
                            [--rows ...]
    panel_measure.py report DIR

It changes the board's settings while it runs (the screen's view and speakers style, the layout, the
time to dim, the night schedule) and puts them back at the end, also when a row fails. It books the
board through the bench's lock like the board suite, never flashes it, and forgets only its own
pairing record. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import board_suite
import suite_sendspin

REPO = Path(__file__).resolve().parents[2]

# --- the schedule ----------------------------------------------------------------------------


@dataclass(frozen=True)
class Segment:
    name: str
    view: str  # a word of PUT /now-playing
    style: str  # a word of PUT /speakers-style
    seconds: int
    # Figures are taken from the samples after this many seconds of the segment: a view that has
    # just been built, a rate that has just settled, are not the view's steady state.
    warmup_s: int = 10
    counted: bool = (
        True  # False: the stream's settling, and the cycling, are not segments of the report
    )


SETTLE = Segment("settle", "speakers", "glow", 10, warmup_s=0, counted=False)


def segments(views: str, segment_s: int, cycles: int, cycle_s: int, warmup_s: int) -> list[Segment]:
    """The schedule of one row. `all`: speakers (glow), Meters, speakers (pulse), Meters again,
    then `cycles` times Meters and the speakers `cycle_s` seconds each, which is the leak test.
    `speakers`: the two styles only, for an image that has no Meters view (the speakers view as it
    was, to hold the others against). `baseline`: the same four, each between two stretches of the
    dark view (nothing drawn, the panel's task asleep), and no cycles: a burst's time creeps up
    over a few minutes of a stream whatever the screen shows, so what a view costs the audio path
    is its burst less the dark stretches either side of it, not less the stretch before."""
    base = [
        Segment("speakers-glow", "speakers", "glow", segment_s, warmup_s),
        Segment("speakers-pulse", "speakers", "pulse", segment_s, warmup_s),
    ]
    out = [SETTLE]
    if views == "speakers":
        return out + base
    if views == "baseline":
        meters = [Segment(f"meters-{k}", "meters", "glow", segment_s, warmup_s) for k in "ab"]
        shown = [base[0], meters[0], base[1], meters[1]]
        for k, segment in enumerate(shown):
            out.append(Segment(f"dark-{k}", "dark", "glow", segment_s, warmup_s))
            out.append(segment)
        out.append(Segment(f"dark-{len(shown)}", "dark", "glow", segment_s, warmup_s))
        return out
    out += [
        base[0],
        Segment("meters-a", "meters", "glow", segment_s, warmup_s),
        base[1],
        Segment("meters-b", "meters", "glow", segment_s, warmup_s),
    ]
    for k in range(cycles):
        out.append(Segment(f"cycle-meters-{k}", "meters", "glow", cycle_s, 0, counted=False))
        out.append(Segment(f"cycle-speakers-{k}", "speakers", "glow", cycle_s, 0, counted=False))
    return out


def total_seconds(plan: list[Segment], tail_s: int = 20) -> int:
    return sum(s.seconds for s in plan) + tail_s


def segment_at(plan: list[Segment], t: float) -> Segment | None:
    """The segment in force `t` seconds into the stream; the last stays in force after the end."""
    edge = 0.0
    for segment in plan:
        edge += segment.seconds
        if t < edge:
            return segment
    return plan[-1] if plan else None


# --- the rows --------------------------------------------------------------------------------


@dataclass(frozen=True)
class Row:
    id: str
    title: str
    layout: str
    outputs: int  # the outputs the room has: what the screen's grid draws
    file: str | None = (
        None  # relative to the repository; None with `tone`, or a file the person gives
    )
    tone: bool = False
    needs: str = ""  # "ac4": an image built with sdkconfig.ac4; "atmos": a file of the person's


EAC3_51 = "testdata/external-baseline/eac3-51-256/ffmpeg.ec3"
AC4_51 = "testdata/external-baseline/ac4-51-music-192/dee.ac4"

ROWS = (
    Row("pcm-2.0", "stereo PCM (two tones)", "2.0", 2, tone=True),
    Row("eac3-5.1", "E-AC-3 5.1", "5.1", 6, EAC3_51),
    Row("eac3-5.1-on-9.1.6", "E-AC-3 5.1 on a 9.1.6 room", "9.1.6", 16, EAC3_51),
    Row("joc-7.1.4", "E-AC-3 JOC (Atmos) on 7.1.4", "7.1.4", 12, needs="atmos"),
    Row("ac4-5.1", "AC-4 5.1", "5.1", 6, AC4_51, needs="ac4"),
)


def pick_rows(ids: list[str] | None) -> list[Row]:
    if not ids:
        return list(ROWS)
    known = {row.id: row for row in ROWS}
    missing = [i for i in ids if i not in known]
    if missing:
        raise board_suite.SuiteError(
            f"no such row: {', '.join(missing)} (rows: {', '.join(known)})"
        )
    return [known[i] for i in ids]


# --- the judgement --------------------------------------------------------------------------

# The thresholds are the plan's, written down before a measurement so that a result is held to
# them and not the other way round. Each is one number here and one sentence in the plan.
BURST_ALLOW_FRACTION = (
    0.15  # the Meters view's mean burst may exceed the speakers view's by this share ...
)
BURST_ALLOW_FLOOR_US = 250  # ... or by this many microseconds, whichever is more
TAP_BUDGET_US = {
    "small": 150,
    "large": 500,
}  # the tap's cost for a burst: up to 8 outputs, and more
TAP_LARGE_OUTPUTS = 8
COST_FRACTION_OF_STEP = 0.5  # the mean cost of an update, as a share of the step in force
SLOW_STEP_US = 100_000  # the step at ten updates a second
# The owner's wish (2026-10-26): twenty updates a second if possible, else a steady ten.
# A cell holds twenty with no fall to ten. It holds a steady ten when it fell (or never could),
# at most this share of its steps went by with no update, and an update costs at most half of
# the ten-a-second step.
STEADY_TEN_MISSED_FRACTION = 0.01
STACK_FREE_MIN = 4096  # the panel task's stack, bytes free at its least
INTERNAL_DROP_MAX = (
    8192  # the internal heap's low point under the Meters view, against the speakers view's
)
PSRAM_LEAK_MAX = 16384  # bytes of PSRAM lost over the open and close cycles
ERROR_ALLOW_US = 1000  # the sync error's largest, against the speakers view's


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    at = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[at]


def sendspin_of(sample: dict[str, Any]) -> dict[str, Any]:
    return sample.get("status") or {}


def delta(samples: list[dict[str, Any]], key: str) -> int:
    values = [
        sendspin_of(s).get(key)
        for s in samples
        if isinstance(sendspin_of(s).get(key), (int, float))
    ]
    return int(values[-1] - values[0]) if len(values) >= 2 else 0


def timing_mean_between(first: dict[str, Any] | None, last: dict[str, Any] | None) -> float | None:
    """The mean of a `timing` object ({last, mean, max, n}) over what happened between two readings
    of it, from the means and counts: (mean_b * n_b - mean_a * n_a) / (n_b - n_a)."""
    if not first or not last:
        return None
    added = last["n"] - first["n"]
    if added <= 0:
        return None
    return (last["mean"] * last["n"] - first["mean"] * first["n"]) / added


BURST_WINDOW_SAMPLES = 10  # readings (a second apart) in a window of the burst's tail


def running_mean_between(first: dict[str, Any], last: dict[str, Any]) -> float | None:
    """The mean burst time of the bursts between two readings of the board's running mean
    (`burst_us`, over `bursts` bursts since the stream began): (mean_b * n_b - mean_a * n_a) /
    (n_b - n_a). The board keeps whole microseconds, so the figure is good to about n_b / (n_b -
    n_a) of them: ten or so for a minute taken a quarter of an hour into a stream."""
    a, b = sendspin_of(first), sendspin_of(last)
    added = b["bursts"] - a["bursts"]
    if added <= 0:
        return None
    return (b["burst_us"] * b["bursts"] - a["burst_us"] * a["bursts"]) / added


def summarise(samples: list[dict[str, Any]], segment: Segment) -> dict[str, Any]:
    """The figures of one segment. Counters are read over the whole segment (an underrun in the
    first seconds is still an underrun), the steady-state figures after the warm-up."""
    out: dict[str, Any] = {
        "segment": segment.name,
        "view": segment.view,
        "style": segment.style,
        "samples": len(samples),
    }
    steady = [s for s in samples if s["t_in_segment"] >= segment.warmup_s]
    out["steady_samples"] = len(steady)
    if not samples:
        return out
    for key in ("underruns", "late", "dropped", "resyncs", "invalid", "bursts"):
        out[f"{key}_delta"] = delta(samples, key)
    # The board's burst_us is the mean of every burst since the stream began, not of the last few:
    # the mean of its readings is a mean of the whole history, in which a view that costs 500 us for
    # a minute moves it by a fraction of that. The segment's own mean is read between two readings.
    timed = [
        s
        for s in steady
        if isinstance(sendspin_of(s).get("burst_us"), (int, float))
        and isinstance(sendspin_of(s).get("bursts"), (int, float))
    ]
    if len(timed) >= 2:
        whole = running_mean_between(timed[0], timed[-1])
        if whole is not None:
            out["burst_us_mean"] = whole
        # And the tail, from windows of ten seconds (a shorter one is too few bursts for the
        # board's whole microseconds): the worst window and the 95th percentile of them.
        windows = [
            m
            for m in (
                running_mean_between(a, b)
                for a, b in zip(
                    timed[::BURST_WINDOW_SAMPLES],
                    timed[BURST_WINDOW_SAMPLES::BURST_WINDOW_SAMPLES],
                    strict=False,  # the last window has no end: it is not a window
                )
            )
            if m is not None
        ]
        if windows:
            out["burst_us_p95"] = percentile(windows, 0.95)
            out["burst_us_max"] = max(windows)
    errors = [
        abs(sendspin_of(s)["error_us"])
        for s in samples
        if isinstance(sendspin_of(s).get("error_us"), (int, float))
    ]
    if errors:
        out["error_us_max"] = max(errors)
    stacks = [
        sendspin_of(s)["decode_stack_free"]
        for s in samples
        if isinstance(sendspin_of(s).get("decode_stack_free"), (int, float))
    ]
    if stacks:
        out["decode_stack_free_min"] = min(stacks)
    panels = [s["panel"] for s in samples if s.get("panel")]
    if panels:
        first, last = panels[0]["panel"], panels[-1]["panel"]
        out["refresh_us_mean"] = timing_mean_between(
            first.get("refresh_us"), last.get("refresh_us")
        )
        out["refresh_us_max_seen"] = last["refresh_us"]["max"]
        for key in (
            "task_stack_free_min",
            "psram_free",
            "psram_free_min",
            "internal_free",
            "internal_free_min",
            "internal_largest",
        ):
            out[key] = last.get(key)
    meters = [
        s["panel"]["meters"]
        for s in samples
        if s.get("panel") and s["panel"].get("meters", {}).get("showing")
    ]
    if meters:
        last_m = meters[-1]
        rates = [m["rate_hz"] for m in meters]
        out["meters"] = {
            "representation": last_m["representation"],
            "outputs": last_m["outputs"],
            "rate_hz_min": min(rates),
            "rate_hz_last": last_m["rate_hz"],
            "falls": last_m["falls"],
            "rises": last_m["rises"],
            "updates": last_m["updates"],
            "late": last_m["late"],
            "dropped": last_m["dropped"],
            "mean_cost_us": last_m["mean_cost_us"],
            "analysis_us": last_m["analysis_us"],
            "widgets_us": last_m["widgets_us"],
            "render_us": last_m["render_us"],
            "tap": last_m["tap"],
        }
    return out


def group_samples(
    samples: list[dict[str, Any]], plan: list[Segment]
) -> dict[str, list[dict[str, Any]]]:
    """The samples by the segment they were taken in, each with its seconds into the segment."""
    starts: dict[str, float] = {}
    edge = 0.0
    for segment in plan:
        starts[segment.name] = edge
        edge += segment.seconds
    grouped: dict[str, list[dict[str, Any]]] = {}
    for sample in samples:
        name = sample["segment"]
        if name not in starts:
            continue
        grouped.setdefault(name, []).append({**sample, "t_in_segment": sample["t"] - starts[name]})
    return grouped


def judge_row(
    row: Row, figures: dict[str, dict[str, Any]], cycles: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """The criteria for one row: a list of {id, ok, text}. `ok` is True, False, or None where the
    row has no reading to judge (a missing segment is not a pass)."""
    verdicts: list[dict[str, Any]] = []

    def add(ident: str, ok: bool | None, text: str) -> None:
        verdicts.append({"id": ident, "ok": ok, "text": text})

    glow = figures.get("speakers-glow")
    meter_names = [n for n in ("meters-a", "meters-b") if n in figures and figures[n].get("meters")]

    # 1. The stream is whole in every segment, whatever the screen shows.
    bad = [
        f"{n}: "
        + ", ".join(
            f"{k.removesuffix('_delta')} {f[k]}"
            for k in (
                "underruns_delta",
                "late_delta",
                "dropped_delta",
                "resyncs_delta",
                "invalid_delta",
            )
            if f.get(k)
        )
        for n, f in figures.items()
        if any(
            f.get(k)
            for k in (
                "underruns_delta",
                "late_delta",
                "dropped_delta",
                "resyncs_delta",
                "invalid_delta",
            )
        )
    ]
    # The kit's first run had an underrun and a resync at the first opening of the view on a
    # stereo PCM stream and not at the second: whether the faults are the cold first build's is
    # worth a line of its own.
    cold = bool(bad) and all(b.startswith("meters-a:") for b in bad)
    add(
        "stream",
        None if not figures else not bad,
        "no underrun, late or dropped burst, resync or invalid burst in any segment"
        + (f" (found: {'; '.join(bad)})" if bad else "")
        + (
            "; only the first, cold opening of the view: repeat it before blaming the view"
            if cold
            else ""
        ),
    )

    # 2. The Meters view's burst is the speakers view's, give or take the tap.
    if glow and glow.get("burst_us_mean") is not None and meter_names:
        worst = max(
            figures[n]["burst_us_mean"]
            for n in meter_names
            if figures[n].get("burst_us_mean") is not None
        )
        allowed = max(BURST_ALLOW_FRACTION * glow["burst_us_mean"], BURST_ALLOW_FLOOR_US)
        add(
            "burst",
            worst - glow["burst_us_mean"] <= allowed,
            f"the Meters view's mean burst {worst:.0f} us against the speakers view's "
            f"{glow['burst_us_mean']:.0f} us (up to {allowed:.0f} us more is allowed)",
        )
    else:
        add("burst", None, "a mean burst under the speakers view and under the Meters view")

    # 3. The tap's own cost.
    if meter_names:
        tap = max(figures[n]["meters"]["tap"]["us_per_burst"] for n in meter_names)
        limit = TAP_BUDGET_US["large" if row.outputs > TAP_LARGE_OUTPUTS else "small"]
        add(
            "tap",
            tap <= limit,
            f"the tap costs the audio path {tap:.0f} us a burst "
            f"({limit} us allowed for {row.outputs} outputs)",
        )
        torn = sum(figures[n]["meters"]["tap"]["torn"] for n in meter_names)
        add("tap-torn", torn == 0, f"{torn} history reads were overrun by the writer")
    else:
        add("tap", None, "the Meters view showing")

    # 4. The rate each room holds, and whether it must hold twenty.
    if meter_names:
        rate = min(figures[n]["meters"]["rate_hz_min"] for n in meter_names)
        falls = sum(figures[n]["meters"]["falls"] for n in meter_names)
        cost = max(figures[n]["meters"]["mean_cost_us"] for n in meter_names)
        updates = sum(figures[n]["meters"]["updates"] for n in meter_names)
        missed = sum(figures[n]["meters"]["dropped"] for n in meter_names)
        missed_share = missed / max(updates + missed, 1)
        steady_ten = (
            missed_share <= STEADY_TEN_MISSED_FRACTION
            and cost <= COST_FRACTION_OF_STEP * SLOW_STEP_US
        )
        if rate == 20 and falls == 0:
            add("rate", True, "holds 20 Hz")
        elif steady_ten:
            add(
                "rate",
                True,
                f"holds a steady 10 Hz ({falls} falls from 20; an update costs "
                f"{cost / 1000:.1f} ms)",
            )
        else:
            add(
                "rate",
                False,
                f"holds neither 20 Hz nor a steady 10: {falls} falls, an update costs "
                f"{cost / 1000:.1f} ms, {100 * missed_share:.0f} % of the steps went by "
                "with no update",
            )
        step = 50_000 if rate == 20 else 100_000
        add(
            "cost",
            cost <= COST_FRACTION_OF_STEP * step,
            f"an update costs {cost / 1000:.1f} ms on average, {100 * cost / step:.0f} % of the "
            f"{step // 1000} ms step (at most {100 * COST_FRACTION_OF_STEP:.0f} %)",
        )
        add(
            "dropped",
            missed_share <= STEADY_TEN_MISSED_FRACTION,
            f"{missed} whole steps went by with no update ({100 * missed_share:.1f} % of the "
            f"steps; at most {100 * STEADY_TEN_MISSED_FRACTION:.0f} %)",
        )
    else:
        add("rate", None, "the Meters view showing")

    # 5. The panel task and the memory.
    stacks = [
        f["task_stack_free_min"]
        for f in figures.values()
        if f.get("task_stack_free_min") is not None
    ]
    if stacks:
        add(
            "stack",
            min(stacks) >= STACK_FREE_MIN,
            f"the panel task's stack had {min(stacks)} bytes free at its least "
            f"(at least {STACK_FREE_MIN})",
        )
    if glow and meter_names and glow.get("internal_free_min") is not None:
        low = min(figures[n]["internal_free_min"] for n in meter_names)
        add(
            "internal",
            glow["internal_free_min"] - low <= INTERNAL_DROP_MAX,
            f"internal heap low point {low} bytes under the Meters view, "
            f"{glow['internal_free_min']} under the speakers view",
        )
    if len(cycles) >= 2:
        first, last = cycles[0], cycles[-1]
        lost = first - last
        add(
            "leak",
            lost <= PSRAM_LEAK_MAX,
            f"PSRAM free after the speakers view: {first} bytes after the first of "
            f"{len(cycles)} openings, {last} after the last ({lost} lost)",
        )
    else:
        add("leak", None, "the open and close cycles")

    # 6. The sync error is not worse.
    if glow and meter_names and glow.get("error_us_max") is not None:
        worst_error = max(
            figures[n]["error_us_max"]
            for n in meter_names
            if figures[n].get("error_us_max") is not None
        )
        add(
            "sync",
            worst_error - glow["error_us_max"] <= ERROR_ALLOW_US,
            f"the largest sync error {worst_error:.0f} us under the Meters view, "
            f"{glow['error_us_max']:.0f} us under the speakers view",
        )
    return verdicts


def cycle_psram(samples: list[dict[str, Any]]) -> list[int]:
    """PSRAM free at the end of each cycling stretch of the speakers view: after the Meters view
    has been built and given back, so a leak is the difference between the first and the last."""
    last_in: dict[str, int] = {}
    for sample in samples:
        name = sample["segment"]
        if name.startswith("cycle-speakers-") and sample.get("panel"):
            last_in[name] = sample["panel"]["panel"]["psram_free"]
    return [last_in[k] for k in sorted(last_in, key=lambda n: int(n.rsplit("-", 1)[1]))]


def analyse(row: Row, samples: list[dict[str, Any]], plan: list[Segment]) -> dict[str, Any]:
    grouped = group_samples(samples, plan)
    figures = {s.name: summarise(grouped.get(s.name, []), s) for s in plan if s.counted}
    figures = {name: f for name, f in figures.items() if f["samples"]}
    cycles = cycle_psram(samples)
    return {
        "row": row.id,
        "title": row.title,
        "layout": row.layout,
        "outputs": row.outputs,
        "figures": figures,
        "dark_costs": dark_costs(figures),
        "cycles_psram_free": cycles,
        "verdicts": judge_row(row, figures, cycles),
    }


# --- the report -----------------------------------------------------------------------------


def cell(value: Any, spec: str = "") -> str:
    if value is None:
        return "-"
    return format(value, spec) if spec else str(value)


def dark_costs(figures: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    """What each view cost the audio path, in microseconds of burst: its mean burst less the mean
    of the dark stretches either side of it (the one there is, at the ends). Empty when the
    schedule has no dark stretch (`--views baseline` has them)."""
    order = [(name, f.get("burst_us_mean")) for name, f in figures.items()]
    out: list[dict[str, Any]] = []
    for i, (name, burst) in enumerate(order):
        if name.startswith("dark-") or burst is None:
            continue
        near: list[float] = []
        for side in (range(i - 1, -1, -1), range(i + 1, len(order))):
            for j in side:
                if order[j][0].startswith("dark-"):
                    if order[j][1] is not None:
                        near.append(order[j][1])
                    break
        if near:
            dark = sum(near) / len(near)
            out.append({"segment": name, "burst": burst, "dark": dark, "cost": burst - dark})
    return out


def render(run: dict[str, Any]) -> str:
    lines = [f"# Panel measurement: {run['label']}", ""]
    meta = run.get("board", {})
    lines.append(
        f"- board `{meta.get('host', '?')}`, {meta.get('chip', '?')}, "
        f"firmware `{meta.get('version', '?')}`, profile `{meta.get('profile', '?')}`"
    )
    lines.append(
        f"- recorded {run.get('recorded', '?')}; segments of {run.get('segment_s', '?')} s, "
        f"the first {run.get('warmup_s', '?')} s of each not counted"
    )
    lines.append("")
    for result in run["rows"]:
        lines += [f"## {result['title']} ({result['layout']}, {result['outputs']} outputs)", ""]
        if result.get("error"):
            lines += [f"**not measured:** {result['error']}", ""]
            continue
        header = (
            "| Segment | burst mean | burst p95 | late | dropped | underruns | refresh mean | rate "
            "| update cost | analysis | widgets | render | tap/burst |"
        )
        lines += [header, "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for name, f in result["figures"].items():
            m = f.get("meters") or {}
            lines.append(
                "| "
                + " | ".join(
                    [
                        name,
                        cell(f.get("burst_us_mean"), ".0f"),
                        cell(f.get("burst_us_p95"), ".0f"),
                        cell(f.get("late_delta")),
                        cell(f.get("dropped_delta")),
                        cell(f.get("underruns_delta")),
                        cell(f.get("refresh_us_mean"), ".0f"),
                        cell(m.get("rate_hz_min")),
                        cell(m.get("mean_cost_us")),
                        cell((m.get("analysis_us") or {}).get("mean"), ".0f"),
                        cell((m.get("widgets_us") or {}).get("mean"), ".0f"),
                        cell((m.get("render_us") or {}).get("mean"), ".0f"),
                        cell((m.get("tap") or {}).get("us_per_burst"), ".1f"),
                    ]
                )
                + " |"
            )
        lines += ["", "Times in microseconds.", ""]
        costs = result.get("dark_costs") or []
        if costs:
            darks = [
                f["burst_us_mean"]
                for n, f in result["figures"].items()
                if n.startswith("dark-") and f.get("burst_us_mean") is not None
            ]
            lines += [
                "| View | burst mean | dark either side | cost to the audio path |",
                "|---|---|---|---|",
            ]
            for c in costs:
                lines.append(
                    f"| {c['segment']} | {c['burst']:.0f} | {c['dark']:.0f} | {c['cost']:+.0f} |"
                )
            lines += [
                "",
                f"The dark stretches' means run from {min(darks):.0f} to {max(darks):.0f}: that "
                "spread is the stream's own drift, and what a view costs is read against the "
                "stretches either side of it.",
                "",
            ]
        for v in result["verdicts"]:
            mark = {True: "pass", False: "**FAIL**", None: "not judged"}[v["ok"]]
            lines.append(f"- {mark}: {v['id']}: {v['text']}")
        lines.append("")
    failed = [
        (r["row"], v["id"]) for r in run["rows"] for v in r.get("verdicts", []) if v["ok"] is False
    ]
    lines.append("## Verdict")
    lines.append("")
    lines.append(
        "All criteria that could be judged pass."
        if not failed
        else "Failed: " + ", ".join(f"{r} {i}" for r, i in failed) + "."
    )
    lines.append("")
    return "\n".join(lines)


# --- the board and the sampler -----------------------------------------------------------------


class MeasureBoard(board_suite.Board):
    """The suite's Board, with the few routes this program writes."""

    def put(self, path: str, text: str) -> int:
        status, _, _ = self._request("PUT", path, text.encode("utf-8"))
        return status

    def text(self, path: str) -> str | None:
        status, _, data = self._request("GET", path)
        return data.decode("utf-8", "replace").strip() if status == 200 else None

    def panel_stats(self) -> dict[str, Any] | None:
        status, _, data = self._request("GET", "/panel-stats")
        if status == 404:
            return None
        if status != 200:
            raise board_suite.SuiteError(f"{self.host}: GET /panel-stats answered {status}")
        return json.loads(data.decode("utf-8", "replace"))


STREAM_KEYS = (
    "playing",
    "bursts",
    "underruns",
    "late",
    "dropped",
    "invalid",
    "resyncs",
    "error_us",
    "worst_error_us",
    "burst_us",
    "worst_burst_us",
    "decode_stack_free",
)


@dataclass
class Sampler:
    board: Any
    plan: list[Segment]
    interval_s: float = 1.0
    start_wait_s: float = 120.0
    clock: Callable[[], float] = time.monotonic
    sleep: Callable[[float], None] = time.sleep
    samples: list[dict[str, Any]] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)
    stop: threading.Event = field(default_factory=threading.Event)

    def apply(self, segment: Segment) -> None:
        for path, word in (("/now-playing", segment.view), ("/speakers-style", segment.style)):
            code = self.board.put(path, word)
            if code != 200:
                self.problems.append(f"PUT {path} {word} answered {code}")

    def run(self) -> None:
        began = self.clock()
        first: float | None = None
        current: Segment | None = None
        failures = 0
        while not self.stop.is_set():
            now = self.clock()
            try:
                sendspin = (self.board.json("/status").get("sendspin")) or {}
                playing = sendspin.get("playing") in ("bursts", "pcm")
            except board_suite.SuiteError as error:
                failures += 1
                if failures > 5:
                    self.problems.append(f"the board stopped answering: {error}")
                    return
                self.sleep(self.interval_s)
                continue
            failures = 0
            if first is None:
                if not playing:
                    if now - began > self.start_wait_s:
                        self.problems.append(f"no stream began in {self.start_wait_s:.0f} s")
                        return
                    self.sleep(0.5)
                    continue
                first = now
            elif not playing and now - first > 5:
                return
            t = now - first
            segment = segment_at(self.plan, t)
            if segment is not None and segment is not current:
                self.apply(segment)
                current = segment
            try:
                panel = self.board.panel_stats()
            except (board_suite.SuiteError, ValueError) as error:
                panel = None
                self.problems.append(f"panel-stats: {error}")
            self.samples.append(
                {
                    "t": t,
                    "segment": current.name if current else "",
                    "status": {k: sendspin.get(k) for k in STREAM_KEYS},
                    "panel": panel,
                }
            )
            self.sleep(self.interval_s)


# --- running ----------------------------------------------------------------------------------


SETTINGS = ("/now-playing", "/speakers-style", "/dim-after", "/night-schedule", "/layout")


def read_settings(board: MeasureBoard) -> dict[str, str | None]:
    return {path: board.text(path) for path in SETTINGS}


def restore_settings(board: MeasureBoard, original: dict[str, str | None]) -> list[str]:
    notes: list[str] = []
    for path in ("/layout", "/night-schedule", "/dim-after", "/speakers-style", "/now-playing"):
        value = original.get(path)
        if value is None:
            continue
        try:
            code = board.put(path, value)
        except board_suite.SuiteError as error:
            notes.append(f"could not put {path} back: {error}")
            continue
        if code != 200:
            notes.append(f"PUT {path} {value!r} (putting it back) answered {code}")
    return notes


def measure_row(
    board: MeasureBoard,
    row: Row,
    plan: list[Segment],
    context: board_suite.SendspinContext,
    atmos: Path | None,
    out: Path,
) -> dict[str, Any]:
    programme: Path
    if row.tone:
        programme = Path("tone")
    elif row.needs == "atmos":
        if atmos is None:
            return {
                "row": row.id,
                "title": row.title,
                "layout": row.layout,
                "outputs": row.outputs,
                "error": "needs --atmos: an E-AC-3 JOC file (planning/p4x-meters-view.md)",
            }
        programme = atmos
    else:
        assert row.file is not None
        programme = REPO / row.file
    code = board.put("/layout", row.layout)
    if code != 200:
        return {
            "row": row.id,
            "title": row.title,
            "layout": row.layout,
            "outputs": row.outputs,
            "error": f"PUT /layout {row.layout} answered {code}: the board cannot carry this room",
        }
    sampler = Sampler(board, plan)
    sampler.apply(plan[0])
    thread = threading.Thread(target=sampler.run, daemon=True)
    thread.start()
    seconds = total_seconds(plan)
    try:
        run = suite_sendspin.play_row(
            board,
            context.testserver,
            context.state,
            programme,
            context.label,
            context.pairing,
            status_port=context.status_port,
            timeout_s=seconds + 150.0,
            seconds=seconds,
            tone=row.tone,
        )
    finally:
        sampler.stop.set()
        thread.join(timeout=15)
    (out / f"{row.id}.samples.json").write_text(json.dumps(sampler.samples), "utf-8")
    result = analyse(row, sampler.samples, plan)
    result["problems"] = sampler.problems + list(run.findings)
    if run.exit_code not in (0, None) and not sampler.samples:
        result["error"] = (
            f"the test server exited {run.exit_code}: {run.server_stderr.strip()[-300:]}"
        )
    return result


def cmd_plan(args: argparse.Namespace) -> int:
    plan = segments(args.views, args.segment_s, args.cycles, args.cycle_s, args.warmup_s)
    rows = pick_rows(args.rows)
    for row in rows:
        what = "tone" if row.tone else (row.file or "a JOC file (--atmos)")
        print(
            f"{row.id}\t{row.layout}\t{row.outputs} outputs\t{what}\t{row.needs or '-'}"
            f"\t{total_seconds(plan)} s"
        )
    print(
        f"segments: {', '.join(f'{s.name} {s.seconds}s' for s in plan if s.counted)}; "
        f"cycles: {args.cycles} of {args.cycle_s} s"
    )
    print(f"the whole run: about {len(rows) * (total_seconds(plan) + 60) // 60} minutes")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    rows = pick_rows(args.rows)
    plan = segments(args.views, args.segment_s, args.cycles, args.cycle_s, args.warmup_s)
    board_suite.resolve_board(args)
    if not args.host or not args.lock or not args.session:
        raise board_suite.SuiteError(
            "--board (or --host and --lock) and --session are required: "
            "a board is booked through its lock file first"
        )
    testserver = suite_sendspin.find_testserver(args.testserver, bench.load().locks)
    for line in board_suite.CHECKLIST:
        print(line)
    minutes = args.minutes or (len(rows) * (total_seconds(plan) + 90) // 60 + 15)
    took, ours = board_suite.hold_lock(
        Path(args.lock), args.session, args.purpose or f"panel measurement {args.label}", minutes
    )
    print(f"booked: {took}" if ours else f"already booked by this session: {took}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    board = MeasureBoard(args.host, args.port, args.timeout)
    original: dict[str, str | None] = {}
    pairing = None
    notes: list[str] = []
    results: list[dict[str, Any]] = []
    try:
        hardware = board.hardware()
        # An image from before the Meters PRs has no GET /panel-stats: --views speakers measures
        # the audio path's side of its speakers view, which is what the comparison with it needs.
        if board.panel_stats() is None and args.views != "speakers":
            raise board_suite.SuiteError(
                f"{args.host} has no screen, or an image with no GET /panel-stats "
                "(--views speakers measures the audio path alone)"
            )
        before = board.status()
        if before.get("state") != "stopped":
            raise board_suite.SuiteError(
                f"the board's /status is {before.get('state')!r}, not 'stopped'"
            )
        original = read_settings(board)
        print(f"settings found: {original}")
        for path, word in (("/dim-after", "never"), ("/night-schedule", "off")):
            code = board.put(path, word)
            if code != 200:
                print(f"note: PUT {path} {word} answered {code}")
        pairing = suite_sendspin.Pairing(board, keep=args.keep_pairing)
        context = board_suite.SendspinContext(
            testserver=testserver,
            state=Path(args.sendspin_state or suite_sendspin.default_state(bench.load().locks)),
            pairing=pairing,
            label=args.board or args.host,
            timeout=args.play_timeout,
            status_port=args.port,
        )
        for row in rows:
            print(f"== {row.id}: {row.title} on {row.layout}, {total_seconds(plan)} s")
            try:
                result = measure_row(
                    board, row, plan, context, Path(args.atmos) if args.atmos else None, out
                )
            except (board_suite.SuiteError, suite_sendspin.SendspinError) as error:
                result = {
                    "row": row.id,
                    "title": row.title,
                    "layout": row.layout,
                    "outputs": row.outputs,
                    "error": str(error),
                }
            results.append(result)
            for v in result.get("verdicts", []):
                print(
                    f"   {({True: 'pass', False: 'FAIL', None: '-'})[v['ok']]:>4} "
                    f"{v['id']}: {v['text']}"
                )
            if result.get("error"):
                print(f"   not measured: {result['error']}")
            board.stop()
    finally:
        if original:
            notes += restore_settings(board, original)
        if pairing is not None:
            try:
                print(pairing.close())
            except board_suite.SuiteError as error:
                notes.append(f"pairing: could not tidy the board's pairing table: {error}")
        if ours:
            print(f"lock: {bench.release_lock(Path(args.lock), args.session)}")
        else:
            print("lock: left booked, as it was found")
        for note in notes:
            print(f"note: {note}")
    run = {
        "label": args.label,
        "recorded": time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()),
        "board": {
            "host": args.host,
            "chip": hardware.get("chip"),
            "version": hardware.get("version"),
            "profile": hardware.get("profile"),
        },
        "segment_s": args.segment_s,
        "warmup_s": args.warmup_s,
        "views": args.views,
        "settings_found": original,
        "notes": notes,
        "rows": results,
    }
    (out / "results.json").write_text(json.dumps(run, indent=2) + "\n", "utf-8")
    (out / "report.md").write_text(render(run), "utf-8")
    print(f"wrote {out / 'report.md'}")
    failed = any(v["ok"] is False for r in results for v in r.get("verdicts", [])) or any(
        r.get("error") for r in results
    )
    return 1 if failed else 0


SPEAKERS_ALLOW_FRACTION = 0.05  # the speakers view of this image against the one before it:
SPEAKERS_ALLOW_FLOOR_US = 100  # its burst may be this share, or this many microseconds, longer


def compare_runs(before: dict[str, Any], after: dict[str, Any]) -> list[dict[str, Any]]:
    """The speakers view in two images, row by row: the image before the changes (the speakers
    view as it was) against the image with them (its glow and its pulse). The glow is held to the
    old view's burst; the pulse is only reported."""
    old_rows = {r["row"]: r for r in before.get("rows", [])}
    out: list[dict[str, Any]] = []
    for row in after.get("rows", []):
        old = old_rows.get(row["row"])
        if not old or old.get("error") or row.get("error"):
            continue
        for name in ("speakers-glow", "speakers-pulse"):
            a = old["figures"].get("speakers-glow")  # the old image has one style: the glow's slot
            b = row["figures"].get(name)
            if not a or not b or a.get("burst_us_mean") is None or b.get("burst_us_mean") is None:
                continue
            was, now = a["burst_us_mean"], b["burst_us_mean"]
            allowed = max(SPEAKERS_ALLOW_FRACTION * was, SPEAKERS_ALLOW_FLOOR_US)
            out.append(
                {
                    "row": row["row"],
                    "segment": name,
                    "burst_before": was,
                    "burst_after": now,
                    "change_pct": 100.0 * (now - was) / was if was else 0.0,
                    "refresh_before": a.get("refresh_us_mean"),
                    "refresh_after": b.get("refresh_us_mean"),
                    "ok": (now - was <= allowed) if name == "speakers-glow" else None,
                }
            )
    return out


def render_comparison(rows: list[dict[str, Any]]) -> str:
    lines = [
        "| Row | Segment | burst before | burst after | change | refresh before | refresh after |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        mark = {True: "", False: " **FAIL**", None: " (reported)"}[r["ok"]]
        lines.append(
            f"| {r['row']} | {r['segment']} | {cell(r['burst_before'], '.0f')} "
            f"| {cell(r['burst_after'], '.0f')} | {r['change_pct']:+.1f} %{mark} "
            f"| {cell(r['refresh_before'], '.0f')} | {cell(r['refresh_after'], '.0f')} |"
        )
    return "\n".join(lines) + "\n"


def cmd_compare(args: argparse.Namespace) -> int:
    before = json.loads((Path(args.before) / "results.json").read_text("utf-8"))
    after = json.loads((Path(args.after) / "results.json").read_text("utf-8"))
    rows = compare_runs(before, after)
    print(render_comparison(rows))
    return 1 if any(r["ok"] is False for r in rows) else 0


def cmd_report(args: argparse.Namespace) -> int:
    run = json.loads((Path(args.dir) / "results.json").read_text("utf-8"))
    text = render(run)
    (Path(args.dir) / "report.md").write_text(text, "utf-8")
    print(text)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument(
            "--views",
            choices=("all", "speakers", "baseline"),
            default="all",
            help="speakers: the speakers view only, for an image with no Meters view; "
            "baseline: each view between two stretches of the dark view, to say what it costs",
        )
        p.add_argument(
            "--rows",
            nargs="*",
            help=f"rows to measure (default all): {', '.join(r.id for r in ROWS)}",
        )
        p.add_argument("--segment-s", type=int, default=60)
        p.add_argument("--warmup-s", type=int, default=10)
        p.add_argument(
            "--cycles",
            type=int,
            default=20,
            help="open and close cycles of the Meters view, the leak test",
        )
        p.add_argument("--cycle-s", type=int, default=3)

    plan_p = sub.add_parser("plan", help="the rows and the schedule, and how long they take")
    common(plan_p)
    plan_p.set_defaults(func=cmd_plan)

    run_p = sub.add_parser("run", help="measure the rows on a booked board")
    common(run_p)
    run_p.add_argument(
        "--board", help="a board of tools/hearth/bench.json, which gives --host and --lock"
    )
    run_p.add_argument("--host")
    run_p.add_argument("--port", type=int, default=80)
    run_p.add_argument("--lock")
    run_p.add_argument("--session")
    run_p.add_argument("--purpose")
    run_p.add_argument(
        "--minutes",
        type=int,
        default=0,
        help="how long to book the board for (default: the run's length and a margin)",
    )
    run_p.add_argument(
        "--label", required=True, help="what the image is, for the report: its commit and build"
    )
    run_p.add_argument("--out", required=True)
    run_p.add_argument("--atmos", help="an E-AC-3 JOC (Atmos) file, for the 7.1.4 row")
    run_p.add_argument("--testserver")
    run_p.add_argument("--sendspin-state")
    run_p.add_argument("--keep-pairing", action="store_true")
    run_p.add_argument("--timeout", type=float, default=10.0)
    run_p.add_argument("--play-timeout", type=float, default=600.0)
    run_p.add_argument("--restart", action="store_true", default=False, help=argparse.SUPPRESS)
    run_p.add_argument("--reset-by", default="http", help=argparse.SUPPRESS)
    run_p.add_argument(
        "--accept-restart-fault", action="store_true", default=False, help=argparse.SUPPRESS
    )
    run_p.set_defaults(func=cmd_run)

    report_p = sub.add_parser("report", help="render a run's report again from its results.json")
    report_p.add_argument("dir")
    report_p.set_defaults(func=cmd_report)

    compare_p = sub.add_parser("compare", help="the speakers view of two images, row by row")
    compare_p.add_argument("before", help="the run directory of the image before the changes")
    compare_p.add_argument("after", help="the run directory of the image with them")
    compare_p.set_defaults(func=cmd_compare)

    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except (board_suite.SuiteError, suite_sendspin.SendspinError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
