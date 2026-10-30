#!/usr/bin/env python3
"""Compares forge's TrueHD decoder with FFmpeg's: speed, and what each does with a damaged stream.

Streams come from the forge encoder, whose output both decoders read back exactly (the encoder
comparison, tools/checks/compare_truehd_encoders.py, checks that), so every figure below is against
a known source: the PCM that went in.

  speed     the wall time of `forge decode IN OUT.wav` and of
            `ffmpeg -f truehd -i IN -f null -`, the
            fastest of --runs, as times real time. Both include starting the process; forge also
            writes the WAV file, which only works against it.
  damage    one change to a stream, then each decoder on the result:
              drop-unit        an access unit removed from the first restart interval
              drop-unit-late   one removed after the first restart interval
              zero-unit        an access unit's payload overwritten with zeros
              flip-payload     one bit flipped in a unit's payload (a CRC or parity failure)
              flip-sync        one bit flipped in the major sync of the second restart interval
              insert-junk      100 bytes of noise between two units
              junk-prefix      3,000 bytes of noise before the first unit
              truncate         the stream cut in the middle of a unit
            for forge as it runs (stops at the first failed check) and with truehd-check=lenient
            (counts the failures and goes on), and for FFmpeg as it runs and with the strict flags
            of
            the encoder tests (-err_detect crccheck+bitstream+buffer+explode).
            The result is the exit status and the frames written, the frames equal to the source
            at the position they belong (a decoder that skips the lost unit is read at its own
            offset too), and the longest run of source frames missing or wrong.

    compare_truehd_decoders.py --forge PATH/TO/forge [--runs N] [--json FILE] [--markdown FILE]
                               [--speed-only | --damage-only] [--keep DIR]

Exit status 0; the report says what each decoder did and makes no claim. Needs ffmpeg and numpy.
"""

import argparse
import json
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import verify_truehd_encode_interop as interop  # noqa: E402
from score_truehd_decode import read_wav  # noqa: E402

PROGRAMMES = [
    ("music-20-48-24", ("flac", "programme_music_stereo.flac"), 48000, 2, 24, 10.0),
    ("music-20-96-24", ("flac", "programme_music_stereo.flac"), 96000, 2, 24, 10.0),
    ("ref-51-48-24", ("ref51",), 48000, 6, 24, 5.0),
    ("ref-51-96-24", ("ref51",), 96000, 6, 24, 5.0),
    ("music-20-192-24", ("flac", "programme_music_stereo.flac"), 192000, 2, 24, 5.0),
    ("ref-71-48-24", ("ref51",), 48000, 8, 24, 5.0),
]
DAMAGE_STREAM = "ref-51-48-24"
DAMAGE = (
    "drop-unit",
    "drop-unit-late",
    "zero-unit",
    "flip-payload",
    "flip-sync",
    "insert-junk",
    "junk-prefix",
    "truncate",
)


def units_of(data):
    """(offset, length) of each access unit of a clean stream: the first 12 bits are its length in
    16 bit words (TH spec §3.1)."""
    units, at = [], 0
    while at + 4 <= len(data):
        length = (((data[at] & 0x0F) << 8) | data[at + 1]) * 2
        if length < 4 or at + length > len(data):
            break
        units.append((at, length))
        at += length
    return units


def has_major_sync(data, offset):
    return data[offset + 4 : offset + 8] == bytes.fromhex("f8726fba")


def damaged(data, kind, seed=1):
    """The stream `data` with one change of `kind` (see the module text), or None if it has no unit
    to change."""
    rng = random.Random(seed)
    units = units_of(data)
    syncs = [i for i, (offset, _) in enumerate(units) if has_major_sync(data, offset)]
    if len(units) < 12 or len(syncs) < 2:
        return None
    first_interval_unit = max(2, syncs[1] // 2)  # inside the first restart interval
    late_unit = min(len(units) - 2, syncs[1] + 3)
    out = bytearray(data)
    if kind == "drop-unit":
        offset, length = units[first_interval_unit]
        del out[offset : offset + length]
    elif kind == "drop-unit-late":
        offset, length = units[late_unit]
        del out[offset : offset + length]
    elif kind == "zero-unit":
        offset, length = units[first_interval_unit]
        out[offset + 20 : offset + length] = bytes(length - 20)
    elif kind == "flip-payload":
        offset, length = units[first_interval_unit]
        out[offset + length // 2] ^= 0x10
    elif kind == "flip-sync":
        offset, _ = units[syncs[1]]
        out[offset + 6] ^= 0x01
    elif kind == "insert-junk":
        offset, _ = units[first_interval_unit]
        out[offset:offset] = bytes(rng.randrange(256) for _ in range(100))
    elif kind == "junk-prefix":
        out[0:0] = bytes(rng.randrange(256) for _ in range(3000))
    elif kind == "truncate":
        offset, length = units[late_unit]
        del out[offset + length // 2 :]
    else:
        raise ValueError(kind)
    return bytes(out)


def run_forge(forge, stream, scratch, lenient):
    out = scratch / "forge-out.wav"
    out.unlink(missing_ok=True)
    command = [forge, "decode", str(stream), str(out)]
    if lenient:
        command.append("truehd-check=lenient")
    result = subprocess.run(command, capture_output=True, check=False)
    pcm = None
    if out.exists() and out.stat().st_size > 44:
        try:
            pcm = read_wav(out)[1] >> 0
        except ValueError:
            pcm = None
    return result.returncode, pcm


def run_ffmpeg(stream, channels, strict):
    command = ["ffmpeg", "-hide_banner", "-v", "error"]
    if strict:
        command += interop.STRICT[2:]
    command += ["-f", "truehd", "-i", str(stream), "-f", "s32le", "-"]
    result = subprocess.run(command, capture_output=True, check=False)
    raw = np.frombuffer(result.stdout[: len(result.stdout) // (4 * channels) * 4 * channels], "<i4")
    pcm = (raw.reshape(-1, channels) >> 8).astype(np.int64) if raw.size else None
    return result.returncode, pcm


def judge(pcm, source):
    """How much of `source` the decoded frames reproduce at the place they belong, and the longest
    run of source frames that are missing or wrong. A decoder that loses units and then goes on
    writes fewer frames, so the decode is read twice: from the start, and aligned at its end with
    the end of the source (what follows the last unit it lost)."""
    if pcm is None or len(pcm) == 0:
        return {"frames": 0, "exact": 0, "longest_bad_run": len(source)}
    good = np.zeros(len(source), dtype=bool)
    n = min(len(pcm), len(source))
    good[:n] |= np.all(pcm[:n] == source[:n], axis=1)
    lost = len(source) - len(pcm)
    if lost > 0:
        good[lost:] |= np.all(pcm == source[lost:], axis=1)
    # the longest run of False
    padded = np.concatenate([[True], good, [True]])
    edges = np.flatnonzero(padded)
    longest = int(np.max(np.diff(edges)) - 1) if len(edges) > 1 else 0
    return {"frames": len(pcm), "exact": int(good.sum()), "longest_bad_run": longest}


def encode(forge, name, spec, rate, channels, bits, seconds, scratch):
    source = interop.build_source(spec, rate, channels, bits, seconds, 7)
    wav = scratch / f"{name}.wav"
    if bits == 16:
        interop.write_wav(wav, rate, source, 16)
    else:
        interop.write_wav(wav, rate, source << (24 - bits), 24)
    stream = scratch / f"{name}.thd"
    result = subprocess.run(
        [forge, "truehd-encode", str(wav), str(stream)], capture_output=True, check=False
    )
    if result.returncode != 0:
        sys.exit(f"forge truehd-encode failed for {name}: {result.stderr.decode(errors='replace')}")
    return source, stream, rate, channels


def timed(command, runs):
    best = None
    for _ in range(runs):
        started = time.perf_counter()
        subprocess.run(command, capture_output=True, check=False)
        elapsed = time.perf_counter() - started
        best = elapsed if best is None else min(best, elapsed)
    return best


def speed_rows(forge, scratch, runs):
    rows = []
    for name, spec, rate, channels, bits, seconds in PROGRAMMES:
        source, stream, _rate, _channels = encode(
            forge, name, spec, rate, channels, bits, seconds, scratch
        )
        audio = len(source) / rate
        out = scratch / "speed.wav"
        forge_s = timed([forge, "decode", str(stream), str(out)], runs)
        ffmpeg_s = timed(
            [
                "ffmpeg",
                "-hide_banner",
                "-v",
                "error",
                "-f",
                "truehd",
                "-i",
                str(stream),
                "-f",
                "null",
                "-",
            ],
            runs,
        )
        rows.append(
            {
                "name": name,
                "audio_s": round(audio, 2),
                "forge_ms": round(forge_s * 1000, 1),
                "ffmpeg_ms": round(ffmpeg_s * 1000, 1),
                "forge_x_realtime": round(audio / forge_s, 1),
                "ffmpeg_x_realtime": round(audio / ffmpeg_s, 1),
                "forge_over_ffmpeg": round(forge_s / ffmpeg_s, 3),
            }
        )
    return rows


def damage_rows(forge, scratch):
    name, spec, rate, channels, bits, seconds = next(p for p in PROGRAMMES if p[0] == DAMAGE_STREAM)
    source, stream, rate, channels = encode(
        forge, name, spec, rate, channels, bits, seconds, scratch
    )
    data = stream.read_bytes()
    rows = []
    for kind in DAMAGE:
        changed = damaged(data, kind)
        if changed is None:
            continue
        broken = scratch / f"{name}.{kind}.thd"
        broken.write_bytes(changed)
        row = {"damage": kind}
        runners = (
            ("forge", run_forge, (forge, broken, scratch, False)),
            ("forge_lenient", run_forge, (forge, broken, scratch, True)),
            ("ffmpeg", run_ffmpeg, (broken, channels, False)),
            ("ffmpeg_strict", run_ffmpeg, (broken, channels, True)),
        )
        for label, runner, arguments in runners:
            status, pcm = runner(*arguments)
            row[label] = {"exit": status, **judge(pcm, source)}
        rows.append(row)
    return {"stream": name, "frames": len(source), "rows": rows}


def markdown(report):
    lines = []
    if report.get("speed"):
        lines += [
            "| Stream | Audio s | forge ms | FFmpeg ms | forge x real time | FFmpeg x real time |",
            "|---|---:|---:|---:|---:|---:|",
        ]
        for r in report["speed"]:
            lines.append(
                f"| {r['name']} | {r['audio_s']:g} | {r['forge_ms']:g} | {r['ffmpeg_ms']:g} | "
                f"{r['forge_x_realtime']:g} | {r['ffmpeg_x_realtime']:g} |"
            )
        lines.append("")
    damage = report.get("damage")
    if damage:
        frames = damage["frames"]
        lines += [
            f"Damaged `{damage['stream']}` ({frames:,} frames). Each cell: exit status, frames "
            "written, frames equal to the source, longest run missing or wrong.",
            "",
            "| Damage | forge | forge lenient | FFmpeg | FFmpeg strict |",
            "|---|---|---|---|---|",
        ]
        for r in damage["rows"]:
            cells = [
                f"exit {r[k]['exit']}, {r[k]['frames']:,}, {r[k]['exact']:,}, "
                f"{r[k]['longest_bad_run']:,}"
                for k in ("forge", "forge_lenient", "ffmpeg", "ffmpeg_strict")
            ]
            lines.append(f"| {r['damage']} | " + " | ".join(cells) + " |")
        lines.append("")
    lines.append("Tools: " + "; ".join(f"{k} {v}" for k, v in report["versions"].items()))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--forge", required=True)
    parser.add_argument("--runs", type=int, default=7, help="runs per timing; the fastest is kept")
    parser.add_argument("--speed-only", action="store_true")
    parser.add_argument("--damage-only", action="store_true")
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    parser.add_argument("--keep", type=pathlib.Path)
    args = parser.parse_args()
    if np is None:
        sys.exit("compare_truehd_decoders: numpy is required")
    if shutil.which("ffmpeg") is None:
        sys.exit("compare_truehd_decoders: ffmpeg is required")

    def version(command):
        out = subprocess.run(command, capture_output=True, check=False)
        return (out.stdout or out.stderr).decode(errors="replace").splitlines()[0]

    report = {
        "versions": {
            "forge": version([args.forge, "--version"]),
            "ffmpeg": version(["ffmpeg", "-version"]),
        }
    }
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(args.keep or scratch_name)
        scratch.mkdir(parents=True, exist_ok=True)
        if not args.damage_only:
            report["speed"] = speed_rows(args.forge, scratch, args.runs)
        if not args.speed_only:
            report["damage"] = damage_rows(args.forge, scratch)
    text = markdown(report)
    print(text)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
