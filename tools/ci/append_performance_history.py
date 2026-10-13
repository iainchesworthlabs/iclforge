"""Append one CI run's encoder throughput results to the quality-history
branch's per-branch performance JSONL file, and flag a trailing-baseline
slowdown at two tiers: a soft one (::warning::, never fails this script) and
a hard one (::error::, signalled via $GITHUB_OUTPUT's hard_regression so the
caller can fail the job *after* still committing and pushing the data - see
.github/workflows/ci.yml's "persist-performance-trend" job, which fails
after the push so a big regression is never silently un-recorded just
because it also failed the run).

This is the trend-tracking HALF of the performance suite, not the hard
real-time gate - that is tests/performance/test_performance.cpp's iclforge-perf
target, a separate CI-blocking ctest run on every push/PR. This script only
ever runs on a run of main that records the trend (after a merge, nightly,
or a dispatch on main - ci.yml's record_trend),
records numbers iclforge-perf's pass/fail already implicitly bounds, and
raises a softer, trend-relative signal on top: not "did this exceed the
absolute real-time budget" (iclforge-perf's job) but "is this drifting slower over
time even while still passing that gate".

Reads one JSON file per leg - tests/performance/bench_encoder.cpp's
--json-out schema ({"real_time_budget_ms_per_frame": ..., "results": [{name,
frames, total_ms, ms_per_frame}, ...]}) - collected under
--results-dir/<leg>/*.json by CI's download-artifact step, and appends one
JSONL record per (leg, config) to <history-dir>/performance-<branch>.jsonl.
Reuses the quality-history branch (a different file, not a new branch) -
see docs/performance-trend.md.

Mirrors tools/ci/append_quality_history.py closely by design (same shape,
same reasoning) - the one real difference is direction: for quality, LOWER
dB is worse; for performance, HIGHER ms/frame is worse. Percentage-based
thresholds are used here rather than quality-trend's fixed dB deltas,
because ms/frame's absolute scale varies a lot across CI runner hardware in
a way dB (already a log-relative unit) does not.

A percentage does not make two machines comparable, though, and the hosted
runner a leg is measured on is not one machine: `ubuntu-latest` was six CPU
models in the six weeks to 2026-10-12, and in the last two days of them
plain_51 read 2.07, 3.74 and 4.38 ms/frame on three, with nothing in the code
to account for it. So the trailing mean is taken over the records measured on
the SAME CPU model as the run being judged (`cpu_model`, which each record
carries, from .github/scripts/capture-bench-environment.sh). When the history
holds none from that model, or the run recorded no model, the mean is of
every record, as it was before, and the annotation and the log say so - see
MIN_SAME_CPU_RECORDS below for what that was measured to change.

A (leg, config) already recorded for this commit is not recorded again, as
in append_quality_history.py: the nightly run and a dispatch on main measure
a commit the run after its merge may have measured already, and the series
holds one point a commit. A repeated run is still judged, against the history
from before its commit, so a hard regression keeps failing the job it is in.

stdlib-only (json/argparse/pathlib), matching compare_wav.py's/
append_quality_history.py's own no-new-CI-provisioning reasoning. Takes
commit and commit-date as arguments rather than reading the clock itself -
see this project's general rule against Date.now()-style nondeterminism in
tooling; the caller sources commit-date from `git show -s --format=%cI`.
"""

import argparse
import json
import os
import sys
from pathlib import Path

from append_quality_history import write_recent_window

# How many trailing same-(branch,leg,config) entries the regression check
# averages over - same window append_quality_history.py uses, for the same
# reason (enough to smooth ordinary run-to-run noise without going stale).
REGRESSION_TRAILING_WINDOW = 10
# A run more than 20% slower than its own trailing mean is worth a table
# annotation, even though it may still comfortably pass iclforge-perf's absolute
# real-time gate.
REGRESSION_SLOWDOWN_FRACTION = 0.20
# A run at least DOUBLE its trailing mean is worth failing the job over
# outright, not just annotating - this is the threshold that would have
# caught the ~8-28x regression this whole suite exists because of, with
# enormous margin to spare.
HARD_REGRESSION_SLOWDOWN_FRACTION = 1.0
# How many earlier records of a series must be from the run's own CPU model
# for the run to be judged against those alone; with fewer it is judged
# against every machine's records, which is the comparison this rule replaces.
#
# One, the smallest number that is a comparison at all. Replayed over
# performance-main.jsonl as it stood on 2026-10-13 (2 098 linux-gcc records
# that name a CPU model, six models): the mean of every machine raised 118
# soft warnings and no hard one. Like for like raised 11, every one of them
# on "AMD EPYC 9V74 80-Core Processor", which runs at two speeds under one
# name (3.74 and 4.82 ms/frame for plain_51). What is left is the fallback,
# and it is the noisy path: 7 warnings with this at one, 9 at two, 13 at
# three, 27 at five, while the 152 records that had only one or two earlier
# ones from their own model raised none when judged against those. Timing
# noise on a shared runner is one-sided (see load_leg_results), so a mean of
# one sample can sit too slow and hide a regression for a run, but it does not
# invent one; a mean across machines does both, by the 2.1x between them.
MIN_SAME_CPU_RECORDS = 1
# What a record carries when its machine was not named: no environment.json
# for the leg (an older workflow, a local run), or a runner the capture script
# could read no model name from.
UNNAMED_CPU_MODELS = ("", "unknown")


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (an artifact named
    'performance-<preset>', downloaded with the prefix stripped back off by
    the caller - see the workflow step), each holding one or more of
    bench_encoder.cpp's --json-out files.

    MULTIPLE files per leg are reduced to ONE record per (leg, config), not
    appended as several. The workflow runs iclforge-bench more than once so this
    reduction has something to work on; one file still works and simply
    reduces to itself.

    The reduction is the MINIMUM run, and the whole record is taken from that
    run rather than assembled from per-field minima across different runs -
    so mean, p95 and max in a record always describe the same measurement
    rather than a composite that never happened.

    Minimum, not mean, for the reason compare_performance.py already documents
    for the PR-time comparison: timing noise on a shared runner is one-sided.
    A co-tenant, a thermal event or a page-cache miss can only ever make a run
    slower, never faster, so the fastest of N is the closest estimate of what
    the machine can actually do and the mean mostly measures the neighbours.
    This script previously recorded a SINGLE unrepeated sample straight into
    the permanent history, which is a weaker signal than the PR comparison
    feeding into the same merge already used - the trailing-mean tiers were
    being fed a noisier number than the reviewer saw.

    `runs` and `spread` are recorded alongside so a reader (and
    docs/performance-trend.md) can tell a real move from a noisy one: spread
    is this workload's own (max-min)/min across the repetitions of THIS run,
    the same definition compare_performance.py uses.
    """
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("performance-")
        # {config: [per-run record]}, in file order.
        by_config: dict[str, list[dict]] = {}
        budget = None
        environment = {}
        environment_path = leg_dir / "environment.json"
        if environment_path.exists():
            environment = json.loads(environment_path.read_text())
        for json_file in sorted(leg_dir.glob("bench*.json")):
            payload = json.loads(json_file.read_text())
            budget = payload["real_time_budget_ms_per_frame"]
            for result in payload["results"]:
                by_config.setdefault(result["name"], []).append(result)

        for config, runs in by_config.items():
            best = min(runs, key=lambda r: r["ms_per_frame"])
            values = [r["ms_per_frame"] for r in runs]
            low = min(values)
            spread = (max(values) - low) / low if low > 0 else 0.0
            yield {
                "leg": leg,
                "config": config,
                "frames": best["frames"],
                "total_ms": best["total_ms"],
                "ms_per_frame": best["ms_per_frame"],
                # .get(), not [...]: iclforge-bench only started emitting the
                # per-frame distribution later than this script, so any JSON
                # produced by an older binary - a rebuilt merge base, a re-run
                # of an old artifact - simply has no tail to record. None is
                # written rather than a zero, so a reader can tell "not
                # measured" from "measured as fast".
                "p95_ms_per_frame": best.get("p95_ms_per_frame"),
                "max_ms_per_frame": best.get("max_ms_per_frame"),
                "runs": len(runs),
                "spread": spread,
                # Per LEG, not per invocation: with more than one leg the
                # measurements come from different machines, so a single
                # command-line value could not describe them both. Each leg's
                # job drops an environment.json beside its bench JSON (see
                # .github/scripts/capture-bench-environment.sh); a leg without
                # one records empty strings.
                "cpu_model": environment.get("cpu_model", ""),
                "runner_image": environment.get("runner_image", ""),
                # Per result where iclforge-bench wrote one: AC-4's frame is 2 048
                # samples against A/52's 1 536, so one file-level budget cannot
                # describe both codecs' rows.
                "real_time_budget_ms_per_frame": best.get("real_time_budget_ms_per_frame", budget),
            }


def recorded_keys(history_path: Path, commit: str):
    """The (leg, config) of every record the file already holds for `commit`."""
    if not history_path.exists():
        return set()
    keys = set()
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("commit") == commit:
            keys.add((rec.get("leg"), rec.get("config")))
    return keys


def trailing_mean(history_path: Path, leg: str, config: str, window: int,
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
        if rec.get("leg") == leg and rec.get("config") == config:
            matches.append(rec["ms_per_frame"])
    if len(matches) < max(min_records, 1):
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def like_for_like_baseline(history_path: Path, leg: str, config: str, cpu_model: str,
                           exclude_commit=None):
    """(the trailing mean a run is judged against, whether it is like for like).

    Like for like is the mean of the series' records from the run's own CPU
    model. Without MIN_SAME_CPU_RECORDS of those, or without a model to match,
    it is the mean of every record of the series and the second value is
    False; (None, False) when the series has no earlier record at all."""
    if cpu_model not in UNNAMED_CPU_MODELS:
        same = trailing_mean(history_path, leg, config, REGRESSION_TRAILING_WINDOW,
                             exclude_commit=exclude_commit, cpu_model=cpu_model,
                             min_records=MIN_SAME_CPU_RECORDS)
        if same is not None:
            return same, True
    return trailing_mean(history_path, leg, config, REGRESSION_TRAILING_WINDOW,
                         exclude_commit=exclude_commit), False


def baseline_words(baseline: str, same_cpu: bool, cpu_model: str) -> str:
    """What a slowdown is measured against, for an annotation: `baseline` is
    the mean with its unit. A mean across machines says that it is one, and
    why, because a slower machine reads against it as a slower build."""
    if same_cpu:
        return (f"the trailing {REGRESSION_TRAILING_WINDOW}-run mean on the same CPU model, "
                f"{cpu_model} ({baseline})")
    why = ("this run recorded no CPU model" if cpu_model in UNNAMED_CPU_MODELS else
           f"too few earlier records are from this run's CPU model, {cpu_model}, to compare "
           "with those alone")
    return (f"the trailing {REGRESSION_TRAILING_WINDOW}-run mean of every machine ({baseline}; "
            f"{why}, so a difference between machines reads here as a change)")


def note_judged(judged: dict, leg: str, cpu_model: str, baseline, same_cpu: bool) -> None:
    """Counts one series of a leg under what it was judged against."""
    counts = judged.setdefault((leg, cpu_model), {"same": 0, "every": 0, "none": 0})
    counts["none" if baseline is None else "same" if same_cpu else "every"] += 1


def report_judged(judged: dict) -> None:
    """One log line a leg, flagged or not: a run judged against every machine's
    records is one whose silence means less, and the log is where that shows."""
    for (leg, cpu_model), counts in sorted(judged.items()):
        machine = ("no CPU model recorded" if cpu_model in UNNAMED_CPU_MODELS
                   else f"measured on {cpu_model}")
        print(f"{leg} ({machine}): {counts['same']} series judged against the same CPU "
              f"model's records, {counts['every']} against every machine's, "
              f"{counts['none']} with no earlier record.")


def emit_github_output(name: str, value: str) -> None:
    """No-op outside GitHub Actions (GITHUB_OUTPUT unset) - safe to call from
    a plain local run of this script."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a") as f:
        f.write(f"{name}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"performance-{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no performance JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    already = recorded_keys(history_path, args.commit)
    lines = []
    hard_regression = False
    # (leg, cpu_model) -> how many of its series were judged against what.
    judged = {}
    for rec in records:
        baseline, same_cpu = like_for_like_baseline(
            history_path, rec["leg"], rec["config"], rec["cpu_model"],
            exclude_commit=args.commit)
        note_judged(judged, rec["leg"], rec["cpu_model"], baseline, same_cpu)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "config": rec["config"],
            "frames": rec["frames"],
            "total_ms": rec["total_ms"],
            "ms_per_frame": rec["ms_per_frame"],
            "p95_ms_per_frame": rec["p95_ms_per_frame"],
            "max_ms_per_frame": rec["max_ms_per_frame"],
            "runs": rec["runs"],
            "spread": rec["spread"],
            # What the number was measured ON, not just what it was. ms/frame
            # is hardware-relative in a way the quality series' dB is not (see
            # this module's own header on why the thresholds here are
            # percentages rather than fixed deltas), so a hosted-image bump or
            # a differently-specced runner shows up as an unattributable step
            # in the series unless the environment is recorded beside it.
            # cpu_model is also what the next run's baseline is chosen by
            # (like_for_like_baseline). Empty string on a leg whose job wrote
            # no environment.json - an older workflow, or a local run.
            "cpu_model": rec["cpu_model"],
            "runner_image": rec["runner_image"],
            "real_time_budget_ms_per_frame": rec["real_time_budget_ms_per_frame"],
        }
        if (rec["leg"], rec["config"]) not in already:
            lines.append(json.dumps(entry, sort_keys=True))

        slowdown = None if baseline is None or baseline <= 0 else (
            (rec["ms_per_frame"] - baseline) / baseline)
        than = None if slowdown is None else baseline_words(
            f"{baseline:.3f} ms/frame", same_cpu, rec["cpu_model"])
        if slowdown is not None and slowdown >= HARD_REGRESSION_SLOWDOWN_FRACTION:
            hard_regression = True
            print(f"::error title=Performance trend hard regression::{rec['leg']}/"
                  f"{rec['config']}: {rec['ms_per_frame']:.3f} ms/frame is "
                  f"{slowdown * 100:.0f}% slower than {than} on "
                  f"{args.branch} - past the {HARD_REGRESSION_SLOWDOWN_FRACTION * 100:.0f}% "
                  "hard-regression threshold. Still recorded below; the run this came from is "
                  "failed separately so this doesn't go unnoticed.")
        elif slowdown is not None and slowdown >= REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Performance trend regression::{rec['leg']}/"
                  f"{rec['config']}: {rec['ms_per_frame']:.3f} ms/frame is "
                  f"{slowdown * 100:.0f}% slower than {than} on "
                  f"{args.branch}. This is a trend warning, not a failure - see iclforge-perf for "
                  "the absolute real-time gate.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    skipped = len(records) - len(lines)
    print(f"Appended {len(lines)} record(s) to {history_path}"
          + (f" ({skipped} already recorded for {args.commit[:8]})" if skipped else ""))
    report_judged(judged)
    write_recent_window(history_path)
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
