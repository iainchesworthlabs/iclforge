#!/usr/bin/env python3
"""Makes the TrueHD and MLP fixtures of testdata/truehd/ with FFmpeg's encoders.

Each fixture is a short stream FFmpeg's `truehd` or `mlp` encoder wrote from the programme
material in testdata/audio/, with the PCM FFmpeg's strict decode of it gives (32-bit little-
endian, the 24-bit sample shifted left by 8) recorded as a SHA-256 in
testdata/truehd/manifest.json. The library's decoder has to give the same samples
(libs/truehd/tests/decoder/test_ffmpeg_fixtures.cpp), and so does `forge decode`
(tools/checks/verify_truehd_interop.py). The codec is lossless, so a decode that differs is wrong.

The programme material is 16 bits. A 24-bit fixture is made from it with its 16-bit samples placed
in the upper bits of a 24-bit word and a small pseudo-random value (from Python's seeded
generator) in the 8 low bits, so that the encoder has 24 bits of content to code and not 16 with
zeros under them.

FFmpeg's encoders write 1 to 6 channels at 44.1 to 192 kHz in 16 or 24 bits, with one substream
(two for 5.1 TrueHD). They do not write the Atmos substream, 7.1, noise generation, an IIR filter,
or LSB bypass, so those features have no fixture here (libs/truehd/ERRATA.md says which).

Usage: gen_truehd_ffmpeg.py [--out DIR] [--seconds S]
Needs ffmpeg with the experimental truehd and mlp encoders (6.1 or later).
"""

import argparse
import hashlib
import json
import pathlib
import random
import struct
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
AUDIO = REPO / "testdata" / "audio"
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]

# name, codec, source, filter on the source, sample rate, channels, bits
CASES = [
    (
        "truehd_mono48_s24",
        "truehd",
        "programme_music_stereo.flac",
        "pan=mono|c0=0.5*c0+0.5*c1",
        48000,
        1,
        24,
    ),
    ("truehd_st48_s16", "truehd", "programme_music_stereo.flac", None, 48000, 2, 16),
    ("truehd_st48_s24", "truehd", "programme_music_stereo.flac", None, 48000, 2, 24),
    ("truehd_st44_s24", "truehd", "programme_music_stereo.flac", None, 44100, 2, 24),
    ("truehd_st96_s24", "truehd", "programme_music_stereo.flac", None, 96000, 2, 24),
    ("truehd_st192_s24", "truehd", "programme_music_stereo.flac", None, 192000, 2, 24),
    ("truehd_m51_s16", "truehd", "reference_51.wav", None, 48000, 6, 16),
    ("truehd_m51_s24", "truehd", "reference_51.wav", None, 48000, 6, 24),
    ("mlp_st48_s24", "mlp", "programme_music_stereo.flac", None, 48000, 2, 24),
    ("mlp_st96_s24", "mlp", "programme_music_stereo.flac", None, 96000, 2, 24),
    ("mlp_st44_s16", "mlp", "programme_music_stereo.flac", None, 44100, 2, 16),
    ("mlp_m51_s24", "mlp", "reference_51.wav", None, 48000, 6, 24),
]


def ffmpeg(*args, data=None):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", *args], input=data, capture_output=True, check=False
    )
    if result.returncode != 0:
        sys.exit(f"ffmpeg {' '.join(args)} failed:\n{result.stderr.decode(errors='replace')}")
    return result.stdout


def ffmpeg_version():
    out = subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True, check=False).stdout
    return out.splitlines()[0].split(" Copyright")[0]


def source_pcm(source, flt, channels, bits, seconds, seed):
    """The source as 32-bit little-endian PCM at 48 kHz: 16 bits of content, or 24 filled in."""
    args = ["-v", "error", "-t", str(seconds), "-i", str(AUDIO / source)]
    if flt:
        args += ["-af", flt]
    args += ["-ar", "48000", "-ac", str(channels), "-f", "s16le", "-"]
    raw = ffmpeg(*args)
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    rng = random.Random(seed)
    if bits == 16:
        words = [s << 16 for s in samples]
    else:
        words = [((s << 8) + rng.randint(-127, 127)) << 8 for s in samples]
    return struct.pack(f"<{len(words)}i", *words)


def access_units(data):
    count, offset = 0, 0
    while offset + 2 <= len(data):
        length = (int.from_bytes(data[offset : offset + 2], "big") & 0xFFF) * 2
        if length < 4 or offset + length > len(data):
            break
        count += 1
        offset += length
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=pathlib.Path, default=REPO / "testdata" / "truehd")
    parser.add_argument("--seconds", type=float, default=0.25)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    entries = []
    for index, (name, codec, source, flt, rate, channels, bits) in enumerate(CASES):
        suffix = "thd" if codec == "truehd" else "mlp"
        path = args.out / f"{name}.{suffix}"
        pcm_in = source_pcm(source, flt, channels, bits, args.seconds, seed=1000 + index)
        # The 48 kHz PCM goes in as raw s32; the encoder's own resampler makes the other rates.
        encode = [
            "-y",
            "-v",
            "error",
            "-f",
            "s32le",
            "-ar",
            "48000",
            "-ac",
            str(channels),
            "-i",
            "-",
            "-ar",
            str(rate),
            "-c:a",
            codec,
            "-strict",
            "-2",
            "-sample_fmt",
            "s16p" if bits == 16 else "s32p",
            "-f",
            codec,
            str(path),
        ]
        ffmpeg(*encode, data=pcm_in)
        pcm = ffmpeg(*STRICT, "-f", codec, "-i", str(path), "-f", "s32le", "-")
        stream = path.read_bytes()
        entries.append(
            {
                "name": name,
                "file": path.name,
                "codec": codec,
                "sample_rate": rate,
                "channels": channels,
                "bits": bits,
                "access_units": access_units(stream),
                "stream_bytes": len(stream),
                "stream_sha256": hashlib.sha256(stream).hexdigest(),
                "pcm_samples": len(pcm) // 4 // channels,
                "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
            }
        )
        print(f"{name:<20} {len(stream):>7} bytes  {entries[-1]['access_units']:>4} units")

    manifest = {
        "_comment": (
            "Made by tools/generators/gen_truehd_ffmpeg.py. pcm_sha256 is the SHA-256 of what "
            "FFmpeg's strict decode gives, as 32-bit little-endian samples (the 24-bit value "
            "shifted left by 8), channels interleaved."
        ),
        "ffmpeg": ffmpeg_version(),
        "seconds": args.seconds,
        "fixtures": entries,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
