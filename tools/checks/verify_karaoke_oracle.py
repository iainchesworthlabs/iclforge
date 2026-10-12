#!/usr/bin/env python3
"""Check this project's karaoke reproductions against the Dolby Reference Player's decoder.

Annex C reproduces a karaoke stream (bsmod 7 above 1/0) as L, C and R with the melody in the
centre and the vocals at the stream's surround level. A/52 defines that reproduction for AC-3 and
none for E-AC-3, and Dolby's own decoder applies it to both (planning/karaoke-eac3-findings.md).
This script is the repeatable form of that measurement: it writes a tone-per-channel WAV, encodes
it with `forge` as AC-3 and as E-AC-3 flagged karaoke, decodes each with `forge decode karaoke`
and with the Reference Player's `dlbac3dec` in line mode, and compares what each output channel
carries as RATIOS to the left channel's own tone. Ratios, because the scale-down that keeps a sum
under 1 is an implementation's choice (Dolby's line mode applies none).

Local only, like quality_race.py's Dolby leg: the Reference Player is licensed and never runs in
CI. Without it the script says so and exits 0. The measured ratios are also constants in
libs/ac3/tests/decoder/test_eac3_karaoke.cpp, so the numbers outlive the installed player.

    python tools/checks/verify_karaoke_oracle.py --forge build/bin/forge
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

RATE = 48000
SECONDS = 3.0
AMPLITUDE = 0.2
# One tone per coded location; the LFE's stays under its 120 Hz band limit.
TONE_HZ = {"L": 300.0, "R": 500.0, "C": 700.0, "Ls": 900.0, "Rs": 1100.0, "Cs": 900.0, "LFE": 60.0}
MASK_BITS = {"L": 0x1, "R": 0x2, "C": 0x4, "LFE": 0x8, "Ls": 0x10, "Rs": 0x20, "Cs": 0x100}
PLAYER_DEFAULT = r"C:\Program Files\Dolby\Dolby Reference Player"
PCM_SUBFORMAT = bytes.fromhex("0100000000001000800000aa00389b71")
# The ratio of a tone in an output channel to L's own tone must agree within this (linear).
TOLERANCE = 0.03


@dataclass(frozen=True)
class Case:
    name: str
    codec: str  # "ac3" or "eac3"
    locations: tuple[str, ...]  # WAV order, which is also the order forge encodes them in
    layout: str  # forge's layout argument
    options: str  # appended to the encode command
    out_ch_config: int  # dlbac3dec out-ch-config: 6 is 5.0, 7 is 5.1


CASES = (
    Case("ac3 3/2 + LFE, surmixlev -6", "ac3", ("L", "R", "C", "LFE", "Ls", "Rs"), "51",
         "bsmod=7 surmixlev=-6 cmixlev=-3", 7),
    # bsmod=7 alone writes the mixing group at the encoder's defaults (centre -4.5, surround -6):
    # an E-AC-3 stream flagged karaoke always carries the levels its reproduction uses.
    Case("e-ac-3 3/2 + LFE, bsmod=7 alone", "eac3", ("L", "R", "C", "LFE", "Ls", "Rs"), "51",
         "bsmod=7", 7),
    Case("e-ac-3 3/2 + LFE, lorosurmixlev -3", "eac3", ("L", "R", "C", "LFE", "Ls", "Rs"), "51",
         "bsmod=7 cmixlev=-3 surmixlev=-3", 7),
    Case("e-ac-3 3/2 + LFE, lorocmixlev -6, lorosurmixlev -6", "eac3",
         ("L", "R", "C", "LFE", "Ls", "Rs"), "51", "bsmod=7 cmixlev=-6 surmixlev=-6", 7),
    Case("e-ac-3 3/1, bsmod=7 alone", "eac3", ("L", "R", "C", "Cs"), "L,C,R,Cs", "bsmod=7", 6),
    Case("e-ac-3 2/1, bsmod=7 alone", "eac3", ("L", "R", "Cs"), "L,R,Cs", "bsmod=7", 6),
)


def write_tone_wav(path: Path, locations: tuple[str, ...]) -> None:
    """A WAVE_FORMAT_EXTENSIBLE 16-bit WAV with one tone per location, mask from the locations."""
    mask = 0
    for name in locations:
        mask |= MASK_BITS[name]
    count = len(locations)
    frames = int(RATE * SECONDS)
    data = bytearray()
    for i in range(frames):
        for name in locations:
            value = AMPLITUDE * math.sin(2.0 * math.pi * TONE_HZ[name] * i / RATE)
            data += struct.pack("<h", round(value * 32767))
    fmt = struct.pack("<HHIIHHHHI16s", 0xFFFE, count, RATE, RATE * count * 2, count * 2, 16, 22,
                      16, mask, PCM_SUBFORMAT)
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data"
            + struct.pack("<I", len(data)) + bytes(data))
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_wav(path: Path) -> tuple[int, list[list[float]]]:
    """(rate, channels) of a PCM16, PCM24, PCM32 or float32 WAV, plain or extensible."""
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    position = 12
    fmt = None
    data = b""
    while position + 8 <= len(raw):
        chunk = raw[position:position + 4]
        size = struct.unpack("<I", raw[position + 4:position + 8])[0]
        body = raw[position + 8:position + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            data = body
            break
        position += 8 + size + (size & 1)
    if fmt is None:
        raise ValueError(f"{path} has no fmt chunk")
    tag, channels, rate, bits = fmt
    width = bits // 8
    frames = len(data) // (channels * width)
    count = frames * channels
    if tag == 3 and bits == 32:
        values = struct.unpack(f"<{count}f", data[:count * 4])
    elif tag == 1 and bits == 16:
        values = [v / 32768.0 for v in struct.unpack(f"<{count}h", data[:count * 2])]
    elif tag == 1 and bits == 32:
        values = [v / 2147483648.0 for v in struct.unpack(f"<{count}i", data[:count * 4])]
    elif tag == 1 and bits == 24:
        values = [int.from_bytes(data[i:i + 3], "little", signed=True) / 8388608.0
                  for i in range(0, count * 3, 3)]
    else:
        raise ValueError(f"{path}: unsupported format {fmt}")
    return rate, [list(values[c::channels]) for c in range(channels)]


def tone_level(samples: list[float], hz: float, rate: int) -> float:
    """The amplitude of a sine at `hz`, by correlation over one second from 0.5 s in."""
    start = rate // 2
    window = samples[start:start + rate]
    step = 2.0 * math.pi * hz / rate
    real = sum(v * math.cos(step * i) for i, v in enumerate(window))
    imag = sum(v * math.sin(step * i) for i, v in enumerate(window))
    return 2.0 * math.hypot(real, imag) / max(len(window), 1)


def tone_matrix(
    channels: list[list[float]], rate: int, tones: dict[str, float]
) -> list[dict[str, float]]:
    """For each output channel, each tone's amplitude."""
    return [
        {name: tone_level(channel, hz, rate) for name, hz in tones.items()}
        for channel in channels
    ]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=False, capture_output=True, text=True, **kwargs)


def dolby_decode(player: Path, source: Path, target: Path, out_ch_config: int) -> bool:
    gst = player / "gst-launch-1.0.exe"
    env = dict(os.environ, GST_PLUGIN_PATH=str(player / "gst-plugins"))
    pipeline = (
        f"filesrc location={source.as_posix()} ! dlbac3parse ! dlbac3dec "
        f"out-ch-config={out_ch_config} drc-mode=line drop-delay=false ! audioconvert ! "
        f"audio/x-raw,format=F32LE ! wavenc ! filesink location={target.as_posix()}"
    )
    done = run([str(gst), "-q", *pipeline.split(" ")], env=env)
    return done.returncode == 0 and target.exists() and target.stat().st_size > 44


def compare(case: Case, ours: list[dict[str, float]], theirs: list[dict[str, float]]) -> list[str]:
    """Each channel's tone ratios to L's own tone, both decoders; the lines that disagree."""
    problems: list[str] = []
    if len(ours) != len(theirs):
        return [f"{case.name}: {len(ours)} channels here, {len(theirs)} in Dolby's decoder"]
    ours_l = ours[0]["L"]
    theirs_l = theirs[0]["L"]
    if ours_l <= 0.0 or theirs_l <= 0.0:
        return [f"{case.name}: no L tone in an output"]
    for channel, (mine, yours) in enumerate(zip(ours, theirs, strict=True)):
        for name in TONE_HZ:
            ratio_mine = mine[name] / ours_l
            ratio_yours = yours[name] / theirs_l
            if abs(ratio_mine - ratio_yours) > TOLERANCE:
                problems.append(
                    f"{case.name}: channel {channel} tone {name}: {ratio_mine:.3f} here, "
                    f"{ratio_yours:.3f} in Dolby's decoder"
                )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--forge", required=True, type=Path, help="the forge executable")
    parser.add_argument("--player", type=Path, default=Path(PLAYER_DEFAULT),
                        help="the Dolby Reference Player's install directory")
    parser.add_argument("--keep", type=Path, help="keep the streams and WAVs in this directory")
    args = parser.parse_args()

    if not (args.player / "gst-launch-1.0.exe").exists():
        print(f"skipped: no Dolby Reference Player at {args.player}")
        return 0
    if not args.forge.exists():
        print(f"error: no forge at {args.forge}", file=sys.stderr)
        return 2

    problems: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        work = args.keep or Path(scratch)
        work.mkdir(parents=True, exist_ok=True)
        for index, case in enumerate(CASES):
            tag = f"case{index}"
            source = work / f"{tag}.wav"
            stream = work / f"{tag}.{'ac3' if case.codec == 'ac3' else 'ec3'}"
            write_tone_wav(source, case.locations)
            encode = [str(args.forge), "encode" if case.codec == "ac3" else "eac3-encode",
                      str(source), str(stream), "384"]
            if case.codec == "eac3":
                encode.append("none")
            encode += [case.layout, *case.options.split(" ")]
            if run(encode).returncode != 0:
                problems.append(f"{case.name}: encoding failed")
                continue
            mine_wav = work / f"{tag}_forge.wav"
            decode = run([str(args.forge), "decode", str(stream), str(mine_wav), "karaoke"])
            if decode.returncode != 0:
                problems.append(f"{case.name}: forge decode failed: {decode.stderr.strip()}")
                continue
            theirs_wav = work / f"{tag}_dolby.wav"
            if not dolby_decode(args.player, stream, theirs_wav, case.out_ch_config):
                problems.append(f"{case.name}: Dolby's decoder produced nothing")
                continue
            rate_mine, mine = read_wav(mine_wav)
            rate_theirs, theirs = read_wav(theirs_wav)
            found = compare(case, tone_matrix(mine, rate_mine, TONE_HZ),
                            tone_matrix(theirs, rate_theirs, TONE_HZ))
            print(f"{'ok  ' if not found else 'FAIL'} {case.name} ({len(mine)} channels)")
            problems += found
    for line in problems:
        print(f"  {line}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
