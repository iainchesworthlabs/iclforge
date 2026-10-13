"""Append one CI run's per-kernel micro-benchmark results to the
quality-history branch's per-branch kernel JSONL file, and annotate a
trailing-baseline slowdown - as a ::warning:: at both tiers, never a job
failure.

This is the third, finest-grained member of the performance suite
(docs/performance-trend.md): iclforge-perf gates absolute real-time throughput,
iclforge-bench trends whole-frame ms/frame, and iclforge-kernelbench
(tests/performance/kernel_bench.cpp) trends each hot kernel's ns/call in
isolation, so a whole-frame drift can be attributed to the kernel that
caused it without re-running a profiler.

Deliberately NON-GATING, unlike append_performance_history.py's
hard-regression tier: a micro-kernel's ns/call on a shared CI runner is far
noisier than a 200-frame whole-frame average (some kernels here run in tens
of nanoseconds, where one scheduler hiccup moves the number a lot), and the
whole-frame series plus iclforge-perf already gate anything a user would feel.
Kernel regressions surface on docs/performance-trend.md's per-kernel tables
and as workflow annotations; they do not fail the run at any threshold.

Reads kernel_bench.cpp's --json-out schema ({"kernels": [{name, iters,
ns_per_call}, ...]}) from --results-dir/<leg>/*.json, and the machine it ran
on from that directory's environment.json when the job wrote one, and appends
one JSONL record per (leg, kernel) to <history-dir>/kernels-<branch>.jsonl. Each
series is keyed by its own kernel name end to end - the same
one-series-per-check separation tools/ci/append_performance_history.py keeps
per (leg, config), and for the same reason the quality-trend history had to
stop conflating unrelated checks: a trailing mean over mixed kernels is a
number with no owner.

So is a trailing mean over mixed machines, and the hosted runner this leg is
measured on is one of several CPU models from run to run. A run is judged
against the records of its own CPU model, exactly as
append_performance_history.py judges a whole-frame run (see its
MIN_SAME_CPU_RECORDS), and against every record, saying so, when the history
has none from that model. The records written before 2026-10-13 name no
model, so each model's own comparison starts with its first record after.

stdlib-only, and commit/commit-date arrive as arguments rather than reading
the clock - same reasoning as append_performance_history.py, whose shape
this mirrors deliberately.
"""

import argparse
import json
import sys
from pathlib import Path

# One rule and one wording for both speed series: what is too few records of a
# CPU model to compare with alone, which models are no model, and how an
# annotation says what it compared against.
from append_performance_history import (
    MIN_SAME_CPU_RECORDS,
    UNNAMED_CPU_MODELS,
    baseline_words,
    note_judged,
    report_judged,
)
from append_quality_history import write_recent_window

# Same trailing window append_performance_history.py uses, for the same
# reason (smooths run-to-run noise without going stale).
REGRESSION_TRAILING_WINDOW = 10
# Same two annotation tiers as the whole-frame series - but here BOTH are
# ::warning::, because this script never fails the job (see module
# docstring). The tier names are kept so a reader can line the annotations
# up against the whole-frame series' identically-derived ones.
REGRESSION_SLOWDOWN_FRACTION = 0.20
HARD_REGRESSION_SLOWDOWN_FRACTION = 1.0
# MIN_SAME_CPU_RECORDS is append_performance_history.py's, which says how it
# was chosen. Replayed over kernels-main.jsonl as it stood on 2026-10-13, with
# each record given the CPU model of the whole-frame record of its commit
# (5 612 records): the mean of every machine raised 441 warnings, like for
# like 63, and the fallback 19 more with it at one, 48 at three.


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (named 'kernels-<leg>'),
    each holding kernel_bench.cpp's --json-out file. A separate tree from
    append_performance_history.py's --results-dir on purpose: that script
    globs every *.json under its legs and requires iclforge-bench's schema, so the
    two producers' outputs must never share a directory.

    environment.json is the one file here that is not the bench's: the leg's
    job writes it (.github/scripts/capture-bench-environment.sh) to say which
    machine the kernels ran on. A leg without one records an empty string."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("kernels-")
        environment = {}
        environment_path = leg_dir / "environment.json"
        if environment_path.exists():
            environment = json.loads(environment_path.read_text())
        for json_file in sorted(leg_dir.glob("*.json")):
            if json_file == environment_path:
                continue
            payload = json.loads(json_file.read_text())
            for kernel in payload["kernels"]:
                yield {
                    "leg": leg,
                    "kernel": kernel["name"],
                    "iters": kernel["iters"],
                    "ns_per_call": kernel["ns_per_call"],
                    "cpu_model": environment.get("cpu_model", ""),
                }


def recorded_keys(history_path: Path, commit: str):
    """The (leg, kernel) of every record the file already holds for `commit`."""
    if not history_path.exists():
        return set()
    keys = set()
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("commit") == commit:
            keys.add((rec.get("leg"), rec.get("kernel")))
    return keys


def trailing_mean(history_path: Path, leg: str, kernel: str, window: int,
                  exclude_commit=None, cpu_model=None, min_records=1):
    """The records of `exclude_commit` are left out: a run is judged against
    what came before its commit, not against an earlier run of the same one.

    Given a `cpu_model`, only the records measured on that model are taken,
    however far back the last `window` of them reach. None when the series
    has fewer than `min_records` to take."""
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if exclude_commit is not None and rec.get("commit") == exclude_commit:
            continue
        if cpu_model is not None and rec.get("cpu_model") != cpu_model:
            continue
        if rec.get("leg") == leg and rec.get("kernel") == kernel:
            matches.append(rec["ns_per_call"])
    if len(matches) < max(min_records, 1):
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def like_for_like_baseline(history_path: Path, leg: str, kernel: str, cpu_model: str,
                           exclude_commit=None):
    """(the trailing mean a run is judged against, whether it is like for like)
    - append_performance_history.py's rule of the same name, on this series."""
    if cpu_model not in UNNAMED_CPU_MODELS:
        same = trailing_mean(history_path, leg, kernel, REGRESSION_TRAILING_WINDOW,
                             exclude_commit=exclude_commit, cpu_model=cpu_model,
                             min_records=MIN_SAME_CPU_RECORDS)
        if same is not None:
            return same, True
    return trailing_mean(history_path, leg, kernel, REGRESSION_TRAILING_WINDOW,
                         exclude_commit=exclude_commit), False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"kernels-{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no kernel JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    # A (leg, kernel) already recorded for this commit is not recorded again, as in
    # append_performance_history.py beside this: the nightly run and a dispatch on main
    # measure a commit the run after its merge may have measured already.
    already = recorded_keys(history_path, args.commit)
    lines = []
    # (leg, cpu_model) -> how many of its series were judged against what.
    judged = {}
    for rec in records:
        baseline, same_cpu = like_for_like_baseline(
            history_path, rec["leg"], rec["kernel"], rec["cpu_model"],
            exclude_commit=args.commit)
        note_judged(judged, rec["leg"], rec["cpu_model"], baseline, same_cpu)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "kernel": rec["kernel"],
            "iters": rec["iters"],
            "ns_per_call": rec["ns_per_call"],
            # The machine, as the whole-frame records name it and for the same
            # use: the next run's baseline is the records that match it.
            "cpu_model": rec["cpu_model"],
        }
        if (rec["leg"], rec["kernel"]) not in already:
            lines.append(json.dumps(entry, sort_keys=True))

        slowdown = None if baseline is None or baseline <= 0 else (
            (rec["ns_per_call"] - baseline) / baseline)
        than = None if slowdown is None else baseline_words(
            f"{baseline:.1f} ns/call", same_cpu, rec["cpu_model"])
        if slowdown is not None and slowdown >= HARD_REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Kernel trend hard regression (non-gating)::{rec['leg']}/"
                  f"{rec['kernel']}: {rec['ns_per_call']:.1f} ns/call is "
                  f"{slowdown * 100:.0f}% slower than {than} on "
                  f"{args.branch}. Recorded; the kernel trend never fails the job - "
                  "iclforge-perf and the whole-frame series are the gates.")
        elif slowdown is not None and slowdown >= REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Kernel trend regression::{rec['leg']}/"
                  f"{rec['kernel']}: {rec['ns_per_call']:.1f} ns/call is "
                  f"{slowdown * 100:.0f}% slower than {than} on "
                  f"{args.branch}.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    skipped = len(records) - len(lines)
    print(f"Appended {len(lines)} record(s) to {history_path}"
          + (f" ({skipped} already recorded for {args.commit[:8]})" if skipped else ""))
    report_judged(judged)
    write_recent_window(history_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
