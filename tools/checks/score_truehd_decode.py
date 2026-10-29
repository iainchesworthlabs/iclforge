#!/usr/bin/env python3
"""Scores a TrueHD decoder against the DEE streams of known sources, on the owner's machine.

The set is not committed (DEE's licence ends on 2026-11-06 and its streams stay with the owner:
planning/truehd.md, "Oracles"). tools/generators/gen_dee_gold.py makes it. The layout this reads
is a directory of `NAME.thd` or `NAME.mlp` streams, each beside `NAME.wav`, the 24-bit (or 16-bit)
PCM source it was made from. The streams are decoded and compared with the source sample for
sample: the codec is lossless. Nothing here reads a stream to learn the format.

    score_truehd_decode.py --set DIR --decoder 'PROGRAM [ARGS]'

The decoder is run as `PROGRAM [ARGS] STREAM OUT.s32` and writes 32-bit little-endian PCM, the
24-bit sample shifted left by 8 (the form `ffmpeg -f s32le` and `forge decode` give). With no set,
or an empty one, the script says so and exits 0, so that a CI job without the set passes."""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import wave


def source_samples(path):
    """The source's samples as 32-bit words, the sample in the top bytes (as `-f s32le` gives)."""
    with wave.open(str(path), "rb") as source:
        width = source.getsampwidth()
        raw = source.readframes(source.getnframes())
    out = bytearray()
    for i in range(0, len(raw), width):
        out += b"\x00" * (4 - width) + raw[i : i + width]
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--set", type=pathlib.Path, help="the directory of DEE streams and sources")
    parser.add_argument("--decoder", help="PROGRAM [ARGS] to run as PROGRAM [ARGS] STREAM OUT.s32")
    args = parser.parse_args()

    streams = sorted([*args.set.glob("*.thd"), *args.set.glob("*.mlp")]) if args.set else []
    if not streams:
        print("score_truehd_decode: no DEE streams (--set DIR); nothing to score")
        return 0
    if not args.decoder:
        sys.exit("score_truehd_decode: --decoder is required when there is a set")

    failures = 0
    with tempfile.TemporaryDirectory() as scratch_name:
        out = pathlib.Path(scratch_name) / "decoded.s32"
        for stream in streams:
            source = stream.with_suffix(".wav")
            if not source.exists():
                print(f"SKIP {stream.name}: no {source.name} beside it")
                continue
            result = subprocess.run(
                [*args.decoder.split(), str(stream), str(out)], capture_output=True, check=False
            )
            if result.returncode != 0:
                failures += 1
                print(
                    f"FAIL {stream.name}: decoder failed: {result.stderr.decode(errors='replace')}"
                )
                continue
            expected, actual = source_samples(source), out.read_bytes()
            good = expected == actual
            failures += not good
            print(f"{'ok  ' if good else 'FAIL'} {stream.name}: {len(actual) // 4} samples")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
