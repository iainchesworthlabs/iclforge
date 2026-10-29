#!/usr/bin/env python3
"""Seeds for the TrueHD and MLP libFuzzer harnesses, made from the repository's own test data.

  truehd-seeds.py streams OUT_DIR   fuzz_truehd_parse's seeds (fuzz_truehd_decode and the
                                    differential harness read the same files): a cut of every stream
                                    of testdata/truehd and testdata/truehd/handbuilt, from its first
                                    access unit to the second major sync, or to a fixed number of
                                    units if that comes first
  truehd-seeds.py encoder OUT_DIR   fuzz_truehd_encode's seeds: the 8-byte configuration the harness
                                    reads, then 16-bit PCM, one file for each corner of the
                                    configuration space
  truehd-seeds.py mat OUT_DIR       fuzz_mat_unwrap's seeds, made by `forge spdif` (--forge) from
                                    cuts of three fixtures: a full burst of 24 access units, a
                                    burst of five (the rest of its slots empty) and the same in
                                    the other word order
  truehd-seeds.py containers OUT_DIR
                                    the seeds of fuzz_mp4_demux, fuzz_matroska_demux and
                                    fuzz_mpegts_demux (a directory each under OUT_DIR): a 5.1
                                    TrueHD stream, an MLP stream and a 96 kHz one, wrapped by
                                    `forge mp4`, `mkv` and `ts` (--forge)

tools/fuzz/generate-seeds.sh calls both. The streams are cut rather than copied so that a seed is a
few kilobytes: the engine mutates a small input at many more executions a second, and a stream's
tail is its head again. The cuts are then reduced by coverage with the engine's merge mode
(`tools/fuzz/run.sh merge`), which keeps one file for each set of branches. Nothing here is a stream
from a disc: every input is a committed fixture or a function of a constant.
"""

import argparse
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"
MAJOR_SYNC = bytes([0xF8, 0x72, 0x6F])
# A unit that has gone this far into a long restart gap is not worth its bytes.
MAX_UNITS = 40
LEAD_UNITS = 12
# The lengths below which a stream is taken whole.
WHOLE_BYTES = 6000


def access_units(data):
    """The (offset, length) of each access unit from the first one: the 12-bit length in 16-bit
    words that opens it (TH spec section 3.1)."""
    units = []
    offset = 0
    while offset + 4 <= len(data):
        length = (int.from_bytes(data[offset : offset + 2], "big") & 0xFFF) * 2
        if length < 4 or offset + length > len(data):
            break
        units.append((offset, length))
        offset += length
    return units


def has_major_sync(data, unit):
    offset, length = unit
    return length >= 8 and data[offset + 4 : offset + 7] == MAJOR_SYNC


def cut(data):
    """The head of a stream: whole if small, else up to and including the unit before its second
    major sync (so that the second restart header is not in it, but the interval that ends there
    is), at least LEAD_UNITS units and at most MAX_UNITS."""
    units = access_units(data)
    if not units:
        return b""
    if len(data) <= WHOLE_BYTES:
        return data[: units[-1][0] + units[-1][1]]
    count = min(len(units), MAX_UNITS)
    syncs = [i for i, unit in enumerate(units) if has_major_sync(data, unit)]
    if len(syncs) >= 2:
        count = min(count, max(syncs[1], LEAD_UNITS))
    count = max(count, min(len(units), LEAD_UNITS))
    end = units[count - 1][0] + units[count - 1][1]
    return data[:end]


def streams(out):
    out.mkdir(parents=True, exist_ok=True)
    sources = sorted(FIXTURES.glob("*.thd")) + sorted(FIXTURES.glob("*.mlp"))
    sources += sorted((FIXTURES / "handbuilt").glob("*.thd"))
    written = 0
    for path in sources:
        piece = cut(path.read_bytes())
        if not piece:
            continue
        (out / path.name).write_bytes(piece)
        written += len(piece)
    print(f"{len(sources)} seeds, {written} bytes in {out}")


# --- fuzz_truehd_encode ---


def header(rate, channels, width, restart, fir, search, downmix, stereo_mode, chunk):
    """The harness's first 8 bytes: libs/truehd/fuzz/fuzz_truehd_encode.cpp reads each modulo the
    number of choices it has. The rate is an index into 48, 44.1, 96, 88.2, 192 and 176.4 kHz, the
    width one into 24, 16 and 20 bits, the restart interval one into 8, 1, 9, 16, 50 and 128 units
    and the chunk one into 0 (all at once), 1, 77 and 4096 frames."""
    flags = (search & 1) | ((downmix & 1) << 1) | (stereo_mode << 2) | (chunk << 4)
    return bytes([rate, channels - 1, width, restart, fir, flags, 0, 0])


def pcm(kind, frames, channels):
    """16-bit little-endian interleaved samples of a signal that stresses one part of the coder."""
    out = bytearray()
    state = 0x2545F491
    for n in range(frames):
        for c in range(channels):
            if kind == "sine":
                value = int(12000 * math.sin(2 * math.pi * n * (3 + c) / 64))
            elif kind == "chord":
                value = int(
                    6000 * math.sin(2 * math.pi * n / 37)
                    + 5000 * math.sin(2 * math.pi * n * (c + 2) / 23)
                )
            elif kind == "noise":
                state = (state * 1103515245 + 12345) & 0x7FFFFFFF
                value = (state >> 8) % 65536 - 32768
            elif kind == "square":
                value = 32767 if (n // 16) % 2 == 0 else -32768
            elif kind == "dc":
                value = 20000 if c % 2 == 0 else -7
            elif kind == "burst":
                # harmonics, then digital silence inside one access unit
                value = int(16000 * math.sin(2 * math.pi * n / 11)) if n % 160 < 70 else 0
            elif kind == "silence":
                value = 0
            else:
                raise ValueError(kind)
            out += struct.pack("<h", max(-32768, min(32767, value)))
    return bytes(out)


# name: (configuration, signal, frames)
ENCODER_SEEDS = {
    "st48-s24-sine": (header(0, 2, 0, 0, 4, 1, 0, 0, 0), "sine", 1500),
    "st48-s24-matrix-search-noise": (header(0, 2, 0, 0, 8, 1, 0, 1, 0), "noise", 1200),
    "st48-s16-chord": (header(0, 2, 1, 0, 4, 0, 0, 2, 0), "chord", 1500),
    "st48-s20-square": (header(0, 2, 2, 0, 2, 1, 0, 0, 0), "square", 1200),
    "mono48-s24-dc": (header(0, 1, 0, 1, 0, 0, 0, 0, 1), "dc", 1500),
    "mono44-s16-sine-chunk77": (header(1, 1, 1, 2, 4, 0, 0, 0, 2), "sine", 1500),
    "st96-s24-chord": (header(2, 2, 0, 3, 4, 1, 0, 0, 0), "chord", 1500),
    "st88-s24-restart1": (header(3, 2, 0, 1, 6, 0, 0, 0, 0), "sine", 1500),
    "st192-s24-burst": (header(4, 2, 0, 4, 4, 1, 0, 0, 3), "burst", 1500),
    "st176-s16-noise": (header(5, 2, 1, 5, 3, 0, 0, 0, 0), "noise", 1200),
    "c30-48-s24-chord": (header(0, 3, 0, 0, 4, 1, 0, 0, 0), "chord", 1000),
    "c51-48-s24-downmix": (header(0, 6, 0, 0, 4, 1, 1, 0, 0), "chord", 700),
    "c51-48-s16-sine-chunk1": (header(0, 6, 1, 0, 4, 0, 0, 0, 1), "sine", 600),
    "c71-48-s24-downmix-noise": (header(0, 8, 0, 0, 2, 1, 1, 0, 0), "noise", 500),
    "c71-96-s24-square": (header(2, 8, 0, 0, 4, 0, 0, 0, 0), "square", 500),
    "c41-44-s20-burst": (header(1, 5, 2, 3, 4, 1, 1, 1, 2), "burst", 800),
    "st48-s24-silence": (header(0, 2, 0, 0, 4, 1, 0, 0, 0), "silence", 1200),
    "st48-s24-fir0": (header(0, 2, 0, 0, 0, 0, 0, 0, 0), "chord", 1500),
}


def encoder(out):
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for name, (config, kind, frames) in ENCODER_SEEDS.items():
        channels = config[1] + 1
        data = config + pcm(kind, frames, channels)
        (out / name).write_bytes(data)
        total += len(data)
    print(f"{len(ENCODER_SEEDS)} seeds, {total} bytes in {out}")


# --- forge's wrappers ---


def head_of(path, units):
    """The first `units` access units of a stream file."""
    data = path.read_bytes()
    found = access_units(data)[:units]
    return data[: found[-1][0] + found[-1][1]]


def run_forge(forge, *args):
    subprocess.run([forge, *map(str, args)], check=True, capture_output=True)


def swap_words(data):
    out = bytearray(data)
    out[0 : len(out) - 1 : 2], out[1 : len(out) : 2] = (
        out[1 : len(out) : 2],
        out[0 : len(out) - 1 : 2],
    )
    return bytes(out)


def mat(out, forge):
    out.mkdir(parents=True, exist_ok=True)
    inputs = {
        "truehd-m51-s16-full-burst": (FIXTURES / "truehd_m51_s16.thd", 24),
        "truehd-st96-s24-five-units": (FIXTURES / "truehd_st96_s24.thd", 5),
        "truehd-71-handbuilt-full-burst": (FIXTURES / "handbuilt" / "hb_eight_channels.thd", 24),
    }
    with tempfile.TemporaryDirectory() as scratch:
        for name, (path, units) in inputs.items():
            stream = pathlib.Path(scratch) / f"{name}.thd"
            stream.write_bytes(head_of(path, units))
            burst = out / f"{name}.mat"
            run_forge(forge, "spdif", stream, burst)
            if name.endswith("five-units"):
                (out / f"{name}-other-word-order.mat").write_bytes(swap_words(burst.read_bytes()))
    print(f"{len(list(out.iterdir()))} seeds in {out}")


def containers(out, forge):
    inputs = {
        "truehd-51-s16": (FIXTURES / "truehd_m51_s16.thd", 12),
        "mlp-st44-s16": (FIXTURES / "mlp_st44_s16.mlp", 12),
        "truehd-st96-s24": (FIXTURES / "truehd_st96_s24.thd", 12),
    }
    harness = {"mp4": "fuzz_mp4_demux", "mkv": "fuzz_matroska_demux", "ts": "fuzz_mpegts_demux"}
    with tempfile.TemporaryDirectory() as scratch:
        for name, (path, units) in inputs.items():
            suffix = path.suffix
            stream = pathlib.Path(scratch) / f"{name}{suffix}"
            stream.write_bytes(head_of(path, units))
            for command, directory in harness.items():
                target = out / directory
                target.mkdir(parents=True, exist_ok=True)
                run_forge(forge, command, stream, target / f"{name}.{command}")
    print(f"seeds in {out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("what", choices=["streams", "encoder", "mat", "containers"])
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--forge", help="a built forge, for `mat` and `containers`")
    args = parser.parse_args()
    if args.what in ("mat", "containers"):
        if not args.forge:
            parser.error(f"{args.what} needs --forge")
        {"mat": mat, "containers": containers}[args.what](args.out, args.forge)
    else:
        {"streams": streams, "encoder": encoder}[args.what](args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
