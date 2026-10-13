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
ns_per_call}, ...]}) from --results-dir/<leg>/*.json and appends one JSONL
record per (leg, kernel) to <history-dir>/kernels-<branch>.jsonl. Each
series is keyed by its own kernel name end to end - the same
one-series-per-check separation tools/ci/append_performance_history.py keeps
per (leg, config), and for the same reason the quality-trend history had to
stop conflating unrelated checks: a trailing mean over mixed kernels is a
number with no owner.

stdlib-only, and commit/commit-date arrive as arguments rather than reading
the clock - same reasoning as append_performance_history.py, whose shape
this mirrors deliberately.
"""

import argparse
import json
import sys
from pathlib import Path

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


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (named 'kernels-<leg>'),
    each holding kernel_bench.cpp's --json-out file. A separate tree from
    append_performance_history.py's --results-dir on purpose: that script
    globs every *.json under its legs and requires iclforge-bench's schema, so the
    two producers' outputs must never share a directory."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("kernels-")
        for json_file in sorted(leg_dir.glob("*.json")):
            payload = json.loads(json_file.read_text())
            for kernel in payload["kernels"]:
                yield {
                    "leg": leg,
                    "kernel": kernel["name"],
                    "iters": kernel["iters"],
                    "ns_per_call": kernel["ns_per_call"],
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
                  exclude_commit=None):
    """The records of `exclude_commit` are left out: a run is judged against
    what came before its commit, not against an earlier run of the same one."""
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if exclude_commit is not None and rec.get("commit") == exclude_commit:
            continue
        if rec.get("leg") == leg and rec.get("kernel") == kernel:
            matches.append(rec["ns_per_call"])
    if not matches:
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


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
    for rec in records:
        baseline = trailing_mean(history_path, rec["leg"], rec["kernel"],
                                 REGRESSION_TRAILING_WINDOW, exclude_commit=args.commit)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "kernel": rec["kernel"],
            "iters": rec["iters"],
            "ns_per_call": rec["ns_per_call"],
        }
        if (rec["leg"], rec["kernel"]) not in already:
            lines.append(json.dumps(entry, sort_keys=True))

        slowdown = None if baseline is None or baseline <= 0 else (
            (rec["ns_per_call"] - baseline) / baseline)
        if slowdown is not None and slowdown >= HARD_REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Kernel trend hard regression (non-gating)::{rec['leg']}/"
                  f"{rec['kernel']}: {rec['ns_per_call']:.1f} ns/call is "
                  f"{slowdown * 100:.0f}% slower than the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.1f} ns/call) on "
                  f"{args.branch}. Recorded; the kernel trend never fails the job - "
                  "iclforge-perf and the whole-frame series are the gates.")
        elif slowdown is not None and slowdown >= REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Kernel trend regression::{rec['leg']}/"
                  f"{rec['kernel']}: {rec['ns_per_call']:.1f} ns/call is "
                  f"{slowdown * 100:.0f}% slower than the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.1f} ns/call) on "
                  f"{args.branch}.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    skipped = len(records) - len(lines)
    print(f"Appended {len(lines)} record(s) to {history_path}"
          + (f" ({skipped} already recorded for {args.commit[:8]})" if skipped else ""))
    write_recent_window(history_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
