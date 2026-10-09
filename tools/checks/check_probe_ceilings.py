"""Hold the AC-4 footprint probe's per-fixture figures to their ceilings
(testdata/ac4-probe-ceilings.json).

The probe prints each figure as `<fixture>.<key>=<bytes or count>`. This reads one
metric from a run and compares every fixture's value with the table's ceiling for that
fixture, so the Cortex-M3 and host leg (tools/checks/run_baremetal_probe.sh --ac4) and
the ESP32-S3's QEMU leg (tools/checks/run_esp32s3_probe.sh --ac4) state each figure once.

Metrics, and the probe line each reads:
    peak_heap                 <fixture>.peak_bytes
    steady_allocs_per_frame   <fixture>.steady_allocs_per_frame
    s3_internal_peak          <fixture>.esp32s3.internal_peak_bytes

Usage:
    check_probe_ceilings.py --table <ceilings.json> --metric <metric>
                            [--title <annotation title>] <run.txt>

Exits non-zero when a fixture is over its ceiling, when a fixture has no entry for the
metric, or when the run printed none of the metric's lines.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# metric -> (the key the probe prints after the fixture's name, what the value is called, its unit)
METRICS = {
    "peak_heap": ("peak_bytes", "peak heap", "bytes"),
    "steady_allocs_per_frame": ("steady_allocs_per_frame", "churn", "allocations/frame"),
    "s3_internal_peak": ("esp32s3.internal_peak_bytes", "internal RAM", "bytes"),
}


def values_of_run(text: str, metric: str) -> dict[str, int]:
    """Each fixture's value of the metric, in the order the run printed them."""
    key = re.escape(METRICS[metric][0])
    found: dict[str, int] = {}
    for fixture, value in re.findall(rf"\b(ac4_[a-z0-9_]+)\.{key}=(\d+)", text):
        found[fixture] = int(value)
    return found


def ceilings_of_table(path: Path, metric: str) -> dict[str, int]:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {
        str(fixture): int(entry[metric])
        for fixture, entry in document["fixtures"].items()
        if metric in entry
    }


def check(values: dict[str, int], ceilings: dict[str, int], metric: str, title: str,
          table_name: str) -> bool:
    label, unit = METRICS[metric][1], METRICS[metric][2]
    ok = True
    for fixture, value in values.items():
        ceiling = ceilings.get(fixture)
        if ceiling is None:
            print(f"::error title=No {label} ceiling::{fixture} has no {metric} entry in "
                  f"{table_name} - add one from a measured run", file=sys.stderr)
            ok = False
            continue
        print(f"{label}: {fixture} = {value} {unit} (ceiling {ceiling})")
        if value > ceiling:
            print(
                f"::error title={title}::{fixture}'s {label} is {value} {unit}, "
                f"ceiling is {ceiling}",
                file=sys.stderr,
            )
            ok = False
    return ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--table", required=True, type=Path)
    parser.add_argument("--metric", required=True, choices=sorted(METRICS))
    parser.add_argument("--title", default="Footprint regression")
    parser.add_argument("run", type=Path)
    args = parser.parse_args(argv)

    values = values_of_run(args.run.read_text(encoding="utf-8", errors="replace"), args.metric)
    if not values:
        print(
            f"error: the probe reported no <fixture>.{METRICS[args.metric][0]} line in {args.run}",
            file=sys.stderr,
        )
        return 1
    ceilings = ceilings_of_table(args.table, args.metric)
    return 0 if check(values, ceilings, args.metric, args.title, args.table.name) else 1


if __name__ == "__main__":
    sys.exit(main())
