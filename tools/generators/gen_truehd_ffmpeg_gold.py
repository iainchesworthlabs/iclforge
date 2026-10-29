#!/usr/bin/env python3
"""Local-only generator of the TrueHD (MLP) golden masters FFmpeg's encoder writes.

This is the one-time (or occasional) step that produces the committed streams under
testdata/truehd/ffmpeg/ which libs/mlp's tests and tools/checks/verify_truehd_interop.py decode.
It is modelled on tools/generators/gen_external_baseline.py: it runs a third-party encoder, so it
is never run in CI (a build must never depend on FFmpeg's encoder being present or on its exact
version); CI reads the committed files and their manifest. Run it locally, review the manifest
diff, commit the result.

Why FFmpeg: the specification (truehd-spec.md, kept outside the repository) contradicts itself in
many places, and planning/truehd.md settles each contradiction against FFmpeg used as a black box.
These streams are the decoder half of that evidence: libs/mlp's decoder must reproduce each
source sample for sample. FFmpeg's `truehd` encoder is experimental (`-strict -2`) and takes at
most 5.1 (planning/truehd.md, P18: asked for 7.1 it writes 5.0), so the legs are 2 and 6
channels; 8-channel and 16-channel interoperability rests on our encoder and FFmpeg's decoder
instead (verify_truehd_interop.py).

The sources are synthesised here, not by FFmpeg's lavfi generators, so that they depend on no
FFmpeg version: a fixed-seed random generator, a different signal per channel (so that a channel
order or substream mix-up cannot pass), and the edge cases a lossless coder must survive: a run of
digital silence, full-scale positive and negative samples, a square wave at full scale, and noise
at the bit depth's full range. Each source is written as a canonical PCM WAV.

Legs (name: channels, sample rate, bits):

  stereo-48k-16     2,  48000, 16      stereo-48k-24     2,  48000, 24
  stereo-96k-16     2,  96000, 16      stereo-96k-24     2,  96000, 24
  51-48k-16         6,  48000, 16      51-48k-24         6,  48000, 24
  51-96k-16         6,  96000, 16      51-96k-24         6,  96000, 24
  stereo-44k-16     2,  44100, 16      stereo-192k-24    2, 192000, 24
  stereo-48k-24-odd 2,  48000, 24, a length that is not a whole number of access units

Writes testdata/truehd/ffmpeg/<leg>.wav, <leg>.thd and manifest.json (SHA-256 of each file, the
FFmpeg version and command line, and the source's shape).

Usage: python3 tools/generators/gen_truehd_ffmpeg_gold.py [--ffmpeg PATH] [--out DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "testdata" / "truehd" / "ffmpeg"

# Bump by hand when the legs or the sources change, so that a regenerated manifest says so.
GOLD_VERSION = 1

# Source length in seconds: long enough for many restart intervals (FFmpeg restarts every 16
# access units by default, 13.3 ms at 48 kHz) and short enough that every stream is well under
# 1 MB.
DURATION_SECONDS = 0.2

LEGS = [
    # name, channels, rate, bits, extra samples past a whole number of access units
    ("stereo-48k-16", 2, 48000, 16, 0),
    ("stereo-48k-24", 2, 48000, 24, 0),
    ("stereo-96k-16", 2, 96000, 16, 0),
    ("stereo-96k-24", 2, 96000, 24, 0),
    ("51-48k-16", 6, 48000, 16, 0),
    ("51-48k-24", 6, 48000, 24, 0),
    ("51-96k-16", 6, 96000, 16, 0),
    ("51-96k-24", 6, 96000, 24, 0),
    ("stereo-44k-16", 2, 44100, 16, 0),
    ("stereo-192k-24", 2, 192000, 24, 0),
    ("stereo-48k-24-odd", 2, 48000, 24, 17),
]

# FFmpeg's channel layout names for the channel counts above: 5.1(side) is the layout its
# TrueHD encoder takes for six channels (`ffmpeg -h encoder=truehd`).
LAYOUTS = {2: "stereo", 6: "5.1(side)"}


def samples_per_access_unit(rate: int) -> int:
    """40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96, 160 at 176.4 and 192 (P1)."""
    return 160 if rate > 96000 else 80 if rate > 48000 else 40


def synthesise(channels: int, rate: int, bits: int, frames: int, seed: int) -> list[list[int]]:
    """Return frames x channels integer samples at `bits` bits.

    Each channel gets its own signal so that channel order and substream assignment are
    observable: a sine at its own frequency, a chirp, band-limited noise, and so on. The middle of
    the file carries the edge cases every channel shares: 300 samples of digital silence, then a
    run alternating between the most positive and most negative values, then a full-scale square
    wave.
    """
    rng = random.Random(seed)
    full = (1 << (bits - 1)) - 1
    low = -(1 << (bits - 1))
    out = [[0] * channels for _ in range(frames)]
    silence_start = frames // 3
    edge_start = silence_start + 300
    square_start = edge_start + 64
    square_end = square_start + 256
    smoothed = [0.0] * channels
    for n in range(frames):
        t = n / rate
        for c in range(channels):
            if silence_start <= n < edge_start:
                v = 0
            elif edge_start <= n < square_start:
                v = full if (n + c) % 2 == 0 else low
            elif square_start <= n < square_end:
                v = full if ((n - square_start) // (8 + c)) % 2 == 0 else low
            else:
                kind = c % 4
                if kind == 0:
                    x = 0.6 * math.sin(2 * math.pi * (220 + 110 * c) * t)
                elif kind == 1:
                    x = 0.5 * math.sin(2 * math.pi * (200 + 4000 * t) * t)
                elif kind == 2:
                    smoothed[c] = 0.9 * smoothed[c] + 0.1 * rng.uniform(-1.0, 1.0)
                    x = 2.5 * smoothed[c]
                else:
                    x = 0.3 * rng.uniform(-1.0, 1.0) + 0.2 * math.sin(2 * math.pi * 50 * t)
                v = int(round(max(-1.0, min(1.0, x)) * full))
            out[n][c] = max(low, min(full, v))
    return out


def write_wav(path: Path, samples: list[list[int]], rate: int, bits: int) -> None:
    """Write a canonical PCM WAV (WAVE_FORMAT_PCM, little endian)."""
    channels = len(samples[0])
    width = bits // 8
    data = bytearray()
    for frame in samples:
        for v in frame:
            data += (v & ((1 << bits) - 1)).to_bytes(width, "little")
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * width, channels * width, bits)
    header += b"fmt " + struct.pack("<I", len(fmt)) + fmt
    header += b"data" + struct.pack("<I", len(data))
    path.write_bytes(header + data)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ffmpeg_version(ffmpeg: str) -> str:
    out = subprocess.run([ffmpeg, "-hide_banner", "-version"], capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()[0] if out else "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg", default="ffmpeg", help="the ffmpeg binary to encode with")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    manifest = {
        "_comment": ("TrueHD golden masters written by FFmpeg's experimental truehd encoder, "
                     "made by tools/generators/gen_truehd_ffmpeg_gold.py (never run in CI). "
                     "Each stream must decode to its source WAV sample for sample."),
        "gold_version": GOLD_VERSION,
        "ffmpeg": ffmpeg_version(args.ffmpeg),
        "legs": [],
    }
    for index, (name, channels, rate, bits, extra) in enumerate(LEGS):
        spa = samples_per_access_unit(rate)
        frames = int(DURATION_SECONDS * rate) // spa * spa + extra
        samples = synthesise(channels, rate, bits, frames, seed=20261029 + index)
        wav = args.out / f"{name}.wav"
        thd = args.out / f"{name}.thd"
        write_wav(wav, samples, rate, bits)
        # s16 for 16-bit sources and s32 for 24-bit ones: the sample formats the encoder takes
        # (`ffmpeg -h encoder=truehd`). FFmpeg reads a 24-bit WAV as s32 with the low byte zero.
        sample_fmt = "s16" if bits == 16 else "s32"
        command = [args.ffmpeg, "-hide_banner", "-v", "error", "-y", "-i", str(wav),
                   "-af", f"aformat=sample_fmts={sample_fmt}:channel_layouts={LAYOUTS[channels]}",
                   "-c:a", "truehd", "-strict", "-2", "-f", "truehd", str(thd)]
        subprocess.run(command, check=True)
        size = thd.stat().st_size
        if size >= 1_000_000:
            print(f"{name}: {size} bytes, over the 1 MB a golden master may take", file=sys.stderr)
            return 1
        manifest["legs"].append({
            "name": name,
            "channels": channels,
            "sample_rate": rate,
            "bits": bits,
            "frames": frames,
            "source": wav.name,
            "source_sha256": sha256(wav),
            "stream": thd.name,
            "stream_sha256": sha256(thd),
            "command": " ".join(["ffmpeg"] + command[1:-1] + [thd.name]).replace(str(wav), wav.name),
        })
        print(f"{name}: {frames} frames, {size} bytes")
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
