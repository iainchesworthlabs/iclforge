#!/usr/bin/env python3
"""Crucible's TrueHD 5.1 leg, held against FFmpeg's strict decode.

The leg (apps/crucible/engine/src/output_stage.cpp, OutputMode::kTrueHd51) takes the encoder's
bed, rounds it to 24-bit integers, encodes it as TrueHD and hands a sink MAT bursts. The device-free
tests already hold those bursts to this library's own decoder (iclforge-crucible-tests); this holds
the same bursts to a decoder written by someone else:

  1. run the hidden test case [truehd_dump] of iclforge-crucible-tests, which feeds the leg the
     repository's 5.1 programme material (testdata/audio/reference_51.wav) and writes the bursts
     the sink received (leg.mat) and the integers the leg was given (leg.s32)
  2. unwrap the bursts with iclforge-truehd-container-tool (`demux mat`), the code the MAT tests
     hold against FFmpeg's `spdif` muxer
  3. decode the TrueHD stream with FFmpeg's strict flags and require its PCM to equal the
     integers the leg was given, sample for sample, over everything the bursts carry

FFmpeg cannot read MAT itself (libs/containers/ERRATA.md, M7), hence step 2. What this does not
show: that a receiver locks on the stream, or that the audio is right once the hardware has it.

Usage: verify_crucible_truehd_leg.py --tests <iclforge-crucible-tests> --tool
           <iclforge-truehd-container-tool> [--ffmpeg ffmpeg] [--keep DIR]
Exit status 1 when the check fails.
"""

from __future__ import annotations

import argparse
import array
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# ffmpeg's strict decode flags (CONTRIBUTING.md, "Oracles"): without them FFmpeg conceals what
# it cannot read.
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]

CHANNELS = 6
# One MAT burst, 24 access units of 40 samples at 48 kHz: the least the bursts can carry.
MIN_FRAMES = 24 * 40


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, check=False, **kwargs)


def tail(data: bytes) -> str:
    return data.decode(errors="replace").strip()[-400:]


def samples(data: bytes) -> array.array:
    values = array.array("i")
    if values.itemsize != 4:
        raise SystemExit("this check needs a 4-byte int")
    values.frombytes(data[: len(data) - len(data) % 4])
    if sys.byteorder == "big":
        values.byteswap()
    return values


def check(tests: str, tool: str, ffmpeg: str, work: Path) -> bool:
    env = dict(os.environ, ICLFORGE_CRUCIBLE_TRUEHD_DUMP_DIR=str(work))
    done = run([tests, "[truehd_dump]"], env=env)
    if done.returncode != 0:
        print(f"FAIL  dump: {tests} [truehd_dump] exited {done.returncode}: {tail(done.stdout + done.stderr)}")
        return False
    mat, expected_file = work / "leg.mat", work / "leg.s32"
    if not mat.exists() or not expected_file.exists():
        print(f"FAIL  dump: the test case wrote no leg.mat and leg.s32 ({tail(done.stdout)})")
        return False
    print(f"PASS  dump ({mat.stat().st_size // 61440} bursts, {mat.stat().st_size} bytes)")

    thd = work / "leg.thd"
    done = run([tool, "demux", "mat", str(mat), str(thd)])
    if done.returncode != 0 or not thd.exists() or thd.stat().st_size == 0:
        print(f"FAIL  unwrap: {tail(done.stderr)}")
        return False
    print(f"PASS  unwrap ({thd.stat().st_size} bytes of TrueHD)")

    # FFmpeg's decoder gives 24-bit samples left-justified in 32 bits.
    decoded = run([ffmpeg, *STRICT, "-i", str(thd), "-map", "0:a:0", "-f", "s32le", "-"])
    if decoded.returncode != 0:
        print(f"FAIL  ffmpeg strict decode: {tail(decoded.stderr)}")
        return False
    got = samples(decoded.stdout)
    want = samples(expected_file.read_bytes())
    frames = len(got) // CHANNELS
    if len(got) % CHANNELS or frames < MIN_FRAMES:
        print(f"FAIL  ffmpeg decode: {len(got)} samples is not a whole number of 6-channel frames, or too few")
        return False
    if len(got) > len(want):
        print(f"FAIL  ffmpeg decode: {len(got)} samples, more than the {len(want)} the leg was given")
        return False
    for i, value in enumerate(got):
        if value != want[i] << 8:
            print(
                f"FAIL  ffmpeg decode differs from the input at frame {i // CHANNELS}, "
                f"channel {i % CHANNELS}: {value >> 8} against {want[i]}"
            )
            return False
    print(
        f"PASS  ffmpeg strict decode equals the integers the leg was given "
        f"({frames} of {len(want) // CHANNELS} frames; the rest is the encoder's last restart interval)"
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tests", required=True, help="iclforge-crucible-tests")
    parser.add_argument("--tool", required=True, help="iclforge-truehd-container-tool")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--keep", help="write the files here and leave them")
    args = parser.parse_args()

    if shutil.which(args.ffmpeg) is None:
        print(f"SKIP  {args.ffmpeg} is not on the path; there is no oracle to hold the leg to")
        return 0 if os.environ.get("ICLFORGE_ALLOW_SKIP") else 1
    if args.keep:
        work = Path(args.keep)
        work.mkdir(parents=True, exist_ok=True)
        ok = check(args.tests, args.tool, args.ffmpeg, work)
    else:
        with tempfile.TemporaryDirectory(prefix="crucible-truehd-") as tmp:
            ok = check(args.tests, args.tool, args.ffmpeg, Path(tmp))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
