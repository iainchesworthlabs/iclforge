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

The first twelve cases are the original set and are kept byte for byte (their seeds follow their
order, so new cases go at the end). The later cases each exist for something the first twelve do
not reach, named in their `covers` field: the two sample rates between the others (88.2 and
176.4 kHz), a 20 bit source, the channel counts 3 to 5 and their layouts, the MLP layouts FFmpeg
adds (quad, 3.0 back), restart intervals of 8 and 128 units (the encoder's -max_interval), and
two streams joined end to end (`spliced` in the manifest, which the generic tests do not read:
the library's strict decode stops at the join, see ERRATA question 20). They are shorter than
the original set (0.08 s) to keep the committed bytes few.

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


def pick(*channels):
    """A pan filter taking these channels of reference_51.wav (L R C LFE Ls Rs), in this order."""
    return f"pan={len(channels)}c|" + "|".join(f"c{i}=c{c}" for i, c in enumerate(channels))


# The later cases: (name, codec, source, filter, sample rate, channels, bits, options). Options:
# seconds, layout (the FFmpeg channel layout the encoder is told), encoder (more encoder arguments)
# and covers (what the fixture is for).
SHORT = 0.08
MUSIC = "programme_music_stereo.flac"
MORE_CASES = [
    (
        "truehd_st88_s24",
        "truehd",
        MUSIC,
        None,
        88200,
        2,
        24,
        {"seconds": SHORT, "covers": "88.2 kHz, 80 samples to the access unit"},
    ),
    (
        "truehd_st176_s24",
        "truehd",
        MUSIC,
        None,
        176400,
        2,
        24,
        {"seconds": SHORT, "covers": "176.4 kHz, 160 samples to the access unit"},
    ),
    (
        "truehd_st48_s20",
        "truehd",
        MUSIC,
        None,
        48000,
        2,
        20,
        {
            "seconds": SHORT,
            "covers": "a 20 bit source: the low 4 bits of the 24 are an output shift",
        },
    ),
    (
        "truehd_c30_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 2),
        48000,
        3,
        16,
        {"seconds": SHORT, "layout": "3.0", "covers": "3 channels, L R C"},
    ),
    (
        "truehd_c21_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 3),
        48000,
        3,
        16,
        {"seconds": SHORT, "layout": "2.1", "covers": "3 channels with the LFE, L R LFE"},
    ),
    (
        "truehd_c40_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 2, 4),
        48000,
        4,
        16,
        {"seconds": SHORT, "layout": "4.0", "covers": "4 channels, L R C Cs"},
    ),
    (
        "truehd_c31_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 2, 3),
        48000,
        4,
        16,
        {"seconds": SHORT, "layout": "3.1", "covers": "4 channels with the LFE, L R C LFE"},
    ),
    (
        "truehd_c50_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 2, 4, 5),
        48000,
        5,
        16,
        {"seconds": SHORT, "layout": "5.0(side)", "covers": "5 channels, L R C Ls Rs"},
    ),
    (
        "truehd_c41_s16",
        "truehd",
        "reference_51.wav",
        pick(0, 1, 2, 3, 4),
        48000,
        5,
        16,
        {"seconds": SHORT, "layout": "4.1", "covers": "5 channels with the LFE, L R C LFE Cs"},
    ),
    (
        "mlp_quad_s16",
        "mlp",
        "reference_51.wav",
        pick(0, 1, 4, 5),
        48000,
        4,
        16,
        {"seconds": SHORT, "layout": "quad", "covers": "MLP, 4 channels, L R Ls Rs"},
    ),
    (
        "mlp_c30back_s16",
        "mlp",
        "reference_51.wav",
        pick(0, 1, 4),
        48000,
        3,
        16,
        {"seconds": SHORT, "layout": "3.0(back)", "covers": "MLP, 3 channels, L R Cs"},
    ),
    (
        "truehd_st48_s24_gap8",
        "truehd",
        MUSIC,
        None,
        48000,
        2,
        24,
        {
            "seconds": SHORT,
            "encoder": ["-max_interval", "8"],
            "covers": "a restart header every 8 access units, the least FFmpeg writes",
        },
    ),
    (
        "truehd_st48_s24_gap128",
        "truehd",
        MUSIC,
        None,
        48000,
        2,
        24,
        {
            "seconds": 0.14,
            "encoder": ["-max_interval", "128"],
            "covers": "a restart header every 128 access units, the most FFmpeg writes",
        },
    ),
]

# Two streams of one format joined end to end, as `cat a.thd b.thd` makes them: (name, source,
# filter, the start of each part in the source, seconds a part). The second stream's first
# restart header carries a lossless check that checks nothing (ERRATA 12), so the library's strict
# decode stops there; FFmpeg's decode warns and goes on.
SPLICED = [
    ("truehd_cat_st48_s24", MUSIC, None, (0.0, 3.0), SHORT),
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


def source_pcm(source, flt, channels, bits, seconds, seed, start=0.0):
    """The source as 32-bit PCM at 48 kHz: 16 bits of content, or 20 or 24 filled in."""
    args = ["-v", "error", *(["-ss", str(start)] if start else []), "-t", str(seconds)]
    args += ["-i", str(AUDIO / source)]
    if flt:
        args += ["-af", flt]
    args += ["-ar", "48000", "-ac", str(channels), "-f", "s16le", "-"]
    raw = ffmpeg(*args)
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    rng = random.Random(seed)
    if bits == 16:
        words = [s << 16 for s in samples]
    elif bits == 20:
        words = [(((s << 4) + rng.randint(-7, 8)) << 4) << 8 for s in samples]
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


def encode(path, codec, pcm_in, channels, bits, rate, layout=None, encoder=()):
    """Writes the stream FFmpeg's encoder makes from 48 kHz s32 PCM, at `rate`."""
    # The 48 kHz PCM goes in as raw s32; the encoder's own resampler makes the other rates.
    channel_args = ["-ch_layout", layout] if layout else ["-ac", str(channels)]
    ffmpeg(
        *["-y", "-v", "error", "-f", "s32le", "-ar", "48000", *channel_args, "-i", "-"],
        *["-ar", str(rate), "-c:a", codec, "-strict", "-2"],
        *["-sample_fmt", "s16p" if bits == 16 else "s32p", *encoder, "-f", codec, str(path)],
        data=pcm_in,
    )


def decode(path, codec):
    """FFmpeg's strict decode of the stream, as 32-bit little-endian samples."""
    return ffmpeg(*STRICT, "-f", codec, "-i", str(path), "-f", "s32le", "-")


def entry(name, path, codec, rate, channels, bits, pcm):
    stream = path.read_bytes()
    return {
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=pathlib.Path, default=REPO / "testdata" / "truehd")
    parser.add_argument("--seconds", type=float, default=0.25)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    entries = []
    cases = [(*case, {}) for case in CASES] + MORE_CASES
    for index, (name, codec, source, flt, rate, channels, bits, options) in enumerate(cases):
        suffix = "thd" if codec == "truehd" else "mlp"
        path = args.out / f"{name}.{suffix}"
        seconds = options.get("seconds", args.seconds)
        pcm_in = source_pcm(source, flt, channels, bits, seconds, seed=1000 + index)
        encode(
            path,
            codec,
            pcm_in,
            channels,
            bits,
            rate,
            options.get("layout"),
            options.get("encoder", ()),
        )
        found = entry(name, path, codec, rate, channels, bits, decode(path, codec))
        if "covers" in options:
            found["covers"] = options["covers"]
            found["seconds"] = seconds
            for key in ("layout", "encoder"):
                if key in options:
                    found["encoder_layout" if key == "layout" else "encoder_options"] = options[key]
        entries.append(found)
        print(f"{name:<24} {found['stream_bytes']:>7} bytes  {found['access_units']:>4} units")

    spliced = []
    for index, (name, source, flt, starts, seconds) in enumerate(SPLICED):
        path = args.out / f"{name}.thd"
        parts = []
        for part, start in enumerate(starts):
            pcm_in = source_pcm(
                source, flt, 2, 24, seconds, seed=2000 + 10 * index + part, start=start
            )
            piece = args.out / f".{name}.part{part}.thd"
            encode(piece, "truehd", pcm_in, 2, 24, 48000)
            parts.append(piece.read_bytes())
            piece.unlink()
        path.write_bytes(b"".join(parts))
        found = entry(name, path, "truehd", 48000, 2, 24, decode(path, "truehd"))
        found["parts"] = len(parts)
        found["join_access_units"] = [
            sum(access_units(part) for part in parts[: k + 1]) for k in range(len(parts) - 1)
        ]
        found["covers"] = "two streams of one format joined end to end"
        spliced.append(found)
        print(f"{name:<24} {found['stream_bytes']:>7} bytes  {found['access_units']:>4} units")

    manifest = {
        "_comment": (
            "Made by tools/generators/gen_truehd_ffmpeg.py. pcm_sha256 is the SHA-256 of what "
            "FFmpeg's strict decode gives, as 32-bit little-endian samples (the 24-bit value "
            "shifted left by 8), channels interleaved. `spliced` lists streams the generic "
            "tests do not decode: the library's strict decode stops at the join (ERRATA "
            "question 20), its lenient decode and FFmpeg's equal pcm_sha256."
        ),
        "ffmpeg": ffmpeg_version(),
        "seconds": args.seconds,
        "fixtures": entries,
        "spliced": spliced,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
