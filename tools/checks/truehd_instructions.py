#!/usr/bin/env python3
"""Count the x86-64 instructions a TrueHD decode or encode takes per access unit.

DD+ has one deterministic cost figure, the instructions a frame takes on a soft-float Cortex-M3
under QEMU's -icount (tools/checks/run_baremetal_probe.sh --icount, docs/performance-trend.md
"Instructions per frame"). That needs QEMU and an arm-none-eabi compiler. This is the figure that
can be taken on a host with valgrind: the instructions callgrind counts inside the codec's entry
points (Decoder::decode, Encoder::encode and Encoder::flush) while iclforge-bench runs one TrueHD
workload, divided by the number of calls, and by the audio an access unit holds. It is x86-64
instructions, GCC -O3 as the bench is built, and not Thumb-2 or RV32 ones: the count is a proxy
for the work, not for any part's cycles, and a move of one per cent in it is real because two runs
of the same binary agree to the instruction. (callgrind counts every iteration of a rep-prefixed
instruction, so the library's memset and memcpy of a few hundred bytes weigh more here than they
cost on hardware; a figure is comparable with another taken by the same libc.)

    truehd_instructions.py --bench build/bin/iclforge-bench
    truehd_instructions.py --bench build/bin/iclforge-bench --workload truehd_51_decode \\
        --json-out instructions.json --ceilings testdata/truehd-instruction-ceilings.json

With --ceilings, a workload over its ceiling is a failure (exit 1), as the Cortex-M3 leg's rows
are: the file maps a workload name to the most instructions per access unit it may take.
Valgrind is not in the CI images, so this runs where it is installed; nothing calls it from a
workflow.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# The workloads iclforge-bench writes for TrueHD, and the functions whose calls and whose inclusive
# cost are the codec's.
WORKLOADS = {
    "truehd_stereo_decode": ("iclforge::truehd::Decoder::decode*",),
    "truehd_51_decode": ("iclforge::truehd::Decoder::decode*",),
    "truehd_stereo_encode": ("iclforge::truehd::Encoder::encode*", "iclforge::truehd::Encoder::flush*"),
    "truehd_51_encode": ("iclforge::truehd::Encoder::encode*", "iclforge::truehd::Encoder::flush*"),
}
# The function whose calls are the units: one per access unit in both directions.
UNIT_FUNCTION = {
    "decode": "iclforge::truehd::Decoder::decode",
    "encode": "iclforge::truehd::Encoder::encode",
}


def parse_callgrind(text: str, unit_function: str) -> tuple[int, int]:
    """(the instructions collected, the number of calls of `unit_function`) from a callgrind
    output written with --compress-strings=no.

    The collected total is the file's `totals:` line, which with --toggle-collect counts only what
    ran inside the toggled functions. The calls are the sum of the `calls=` lines that follow a
    `cfn=` naming the function (with its argument list cut off), over every caller."""
    total = 0
    calls = 0
    callee = ""
    for line in text.splitlines():
        if line.startswith("totals:"):
            total = int(line.split(":", 1)[1].split()[0])
        elif line.startswith("cfn="):
            callee = line[4:]
        elif line.startswith("calls=") and callee:
            name = callee.split("(", 1)[0]
            if name == unit_function:
                calls += int(line[6:].split()[0])
            callee = ""
    return total, calls


def run_workload(bench: Path, workload: str, valgrind: str) -> dict:
    direction = "decode" if workload.endswith("_decode") else "encode"
    with tempfile.TemporaryDirectory(prefix="truehd-icount-") as scratch:
        out = Path(scratch) / "callgrind.out"
        result_json = Path(scratch) / "bench.json"
        command = [valgrind, "--tool=callgrind", "--compress-strings=no", "--compress-pos=no",
                   f"--callgrind-out-file={out}"]
        command += [f"--toggle-collect={pattern}" for pattern in WORKLOADS[workload]]
        command += [str(bench), "--only", workload, "--json-out", str(result_json)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        total, calls = parse_callgrind(out.read_text(encoding="utf-8"), UNIT_FUNCTION[direction])
        budget_ms = json.loads(result_json.read_text())["results"][0]["real_time_budget_ms_per_frame"]
    if calls == 0:
        raise SystemExit(f"{workload}: callgrind saw no call of {UNIT_FUNCTION[direction]}")
    per_unit = total / calls
    return {
        "workload": workload,
        "calls": calls,
        "instructions_per_unit": round(per_unit),
        "unit_ms": budget_ms,
        # instructions per second of audio: a unit is budget_ms of it.
        "instructions_per_second_of_audio": round(per_unit * 1000.0 / budget_ms),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--bench", type=Path, required=True, help="the iclforge-bench binary")
    parser.add_argument("--workload", action="append", choices=sorted(WORKLOADS),
                        help="a workload to count; repeatable, default all four")
    parser.add_argument("--valgrind", default="valgrind")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--ceilings", type=Path,
                        help="a JSON file of {workload: most instructions per access unit}")
    args = parser.parse_args()

    rows = [run_workload(args.bench, name, args.valgrind) for name in (args.workload or sorted(WORKLOADS))]
    ceilings = json.loads(args.ceilings.read_text())["ceilings"] if args.ceilings else {}

    failed = False
    print(f"{'workload':<24} {'calls':>8} {'instr/unit':>12} {'unit ms':>8} {'Minstr/s audio':>15}  ceiling")
    for row in rows:
        ceiling = ceilings.get(row["workload"])
        over = ceiling is not None and row["instructions_per_unit"] > ceiling
        failed = failed or over
        print(f"{row['workload']:<24} {row['calls']:>8} {row['instructions_per_unit']:>12,} "
              f"{row['unit_ms']:>8.3f} {row['instructions_per_second_of_audio'] / 1e6:>15.1f}  "
              f"{'-' if ceiling is None else format(ceiling, ',')}{' OVER' if over else ''}")
    if args.json_out:
        args.json_out.write_text(json.dumps({"rows": rows}, indent=2) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
