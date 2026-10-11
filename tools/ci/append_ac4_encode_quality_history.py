"""Append one CI run's AC-4 encoder scores to the quality-history branch.

Reads the JSON tools/checks/score_ac4_encode.py --json-out writes (one entry per programme leg
the encoder was scored on: the minimum per-channel SNR, the LSD and the MOS-LQO of its stream,
decoded by this project's decoder, against its source) and appends one JSONL record per leg to
<history-dir>/ac4-encode-quality-<branch>.jsonl. The Listening quality tile on the home page, the
Predicted listening quality card of docs/performance-quality.md and docs/quality-trend.md's AC-4
encode section read it.

It is the encoder's counterpart of the series append_ac4_quality_history.py keeps for the decoder
(ac4-quality-*.jsonl, Dolby's streams decoded by this project's decoder), and a separate file for a
reason: that file's row is a stream Dolby's encoder wrote, this one's is a stream this project's
did, and "which stream" is what a reader of either is asking. Its rows carry `codec: "ac4"` as
the external-comparison series' carry "ac3" and "eac3", which is how the tile files them.

Only the MOS has a trend tier here, and it is soft: a ::warning::, never a failure.

- score_ac4_encode.py's pinned floors already fail CI on every leg it scores, and a pin is the
  better gate for a deterministic pipeline: SNR and LSD are recorded here and read, not judged.
- ViSQOL's MOS-LQO is a model's prediction of a listening test and says less about a window of a
  5 s fixture than SNR does about the whole of it, so an unexplained move is a reason to look and
  not one to refuse a merge (append_external_comparison_history.py's MOS tier makes the same
  call).
- MOS_REGRESSION_DROP is the scorer's own MOS_MARGIN: its floors sit that far under the first
  measurement, so a leg that has drifted that far under its trailing mean is a leg that has
  used up a pin's whole margin since a run when it did better. The external-comparison tier is
  0.15, since the tool sets there differ by that much between themselves; this pipeline has one
  encoder and a fixed set of sources.

A leg already recorded for this commit is not recorded again. FFmpeg Validate runs in the nightly
run, and a nightly on a tip that has not moved scores the same legs to the same numbers: the series
would otherwise carry every leg once per run, and a reader counting rows would count them all (the
decode series did, until append_ac4_quality_history.py learned the same guard). A repeated leg is
still judged, against its history from before this commit, so a run says again what the first said.

stdlib-only, like every tools/ci/append_*.py script.
"""

import argparse
import json
import sys
from pathlib import Path

from append_ac4_quality_history import read_history, trailing_mean, verdict
from append_quality_history import write_recent_window

REGRESSION_TRAILING_WINDOW = 10
# score_ac4_encode.py's MOS_MARGIN: how far under the first measurement a floor is pinned.
MOS_REGRESSION_DROP = 0.1


def branch_slug(branch: str) -> str:
    """A single flat filename component for `branch`: every branch here is <type>/<name>, and an
    un-sanitised '/' interpolated into a Path joins as an extra path component."""
    return branch.replace("/", "_").replace("\\", "_")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json-in", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True)
    args = parser.parse_args(argv)

    legs = json.loads(args.json_in.read_text()).get("legs", [])
    if not legs:
        print(f"::warning title=AC-4 encode quality trend::{args.json_in} scored no legs; "
              "nothing appended")
        return 0

    history_path = args.history_dir / f"ac4-encode-quality-{branch_slug(args.branch)}.jsonl"
    history = read_history(history_path)
    recorded = {rec.get("leg") for rec in history if rec.get("commit") == args.commit}
    earlier = [rec for rec in history if rec.get("commit") != args.commit]
    lines = []
    for leg in legs:
        rec = {
            "branch": args.branch,
            "commit": args.commit,
            "commit_date": args.commit_date,
            "codec": "ac4",
            "leg": leg["leg"],
            "bitrate_kbps": leg.get("bitrate_kbps"),
            "min_snr_db": leg.get("min_snr_db"),
            "lsd_db": leg.get("lsd_db"),
            "mos_lqo": leg.get("mos_lqo"),
        }
        baseline = trailing_mean(earlier, rec["leg"], "mos_lqo", REGRESSION_TRAILING_WINDOW)
        if verdict(rec["mos_lqo"], baseline, MOS_REGRESSION_DROP, float("inf"), False):
            print(f"::warning title=AC-4 encode quality soft regression::{rec['leg']} MOS-LQO "
                  f"{rec['mos_lqo']:.2f} is {baseline - rec['mos_lqo']:.2f} below its trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean of {baseline:.2f} on {args.branch}. "
                  "Soft tier only: the scorer's own pins fail the run when a floor is crossed.")
        if rec["leg"] not in recorded:
            lines.append(json.dumps(rec, sort_keys=True))

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as out:
        for line in lines:
            out.write(line + "\n")
    skipped = len(legs) - len(lines)
    print(f"Appended {len(lines)} record(s) to {history_path}"
          + (f" ({skipped} already recorded for {args.commit[:8]})" if skipped else ""))
    write_recent_window(history_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
