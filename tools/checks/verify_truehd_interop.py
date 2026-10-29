#!/usr/bin/env python3
"""TrueHD (MLP) interoperability gate: forge's encoder and decoder against FFmpeg's.

TrueHD is lossless, so every check here is exact: a decoded sample either equals the source
sample or the gate fails. The legs, in the order they run:

  1. forge encodes a matrix of sources (1 to 8 channels, 44.1 to 192 kHz, 16 and 24 bit, the
     downmix on and off, the IIR filters on and off) and FFmpeg's decoder, under strict error
     detection, must give back each source exactly. Every lower presentation is checked too:
     FFmpeg's `-downmix` request for it must equal forge's own decode of it.
  2. forge decodes the committed golden masters FFmpeg's encoder wrote (testdata/truehd/ffmpeg,
     tools/generators/gen_truehd_ffmpeg_gold.py), each checked first against the SHA-256 its
     manifest pins, and must give back each source exactly.
  3. forge round trip: encode, then forge's decode, must give back each leg-1 source exactly,
     and probe must describe the stream it wrote.
  4. Atmos: forge encodes a 7.1 bed with eight objects and their object audio metadata into a
     16-channel substream; forge's decode must return the bed, the objects and the metadata;
     FFmpeg must decode the stream without error, and what it exposes of it (the presentation
     it decodes, its channel count) is recorded as a black-box finding, not judged.

Channel order: forge writes and reads a WAV in its channel mask's order, which for 6.1 and 7.1
is FFmpeg's native order too (planning/truehd.md, P38), so FFmpeg's decode is compared with the
source WAV as it stands.

Usage:
    python3 tools/checks/verify_truehd_interop.py --cli build/bin/forge [--ffmpeg ffmpeg]
        [--workdir DIR] [--quick]

    --quick   a smaller leg-1 matrix (one rate per family), for a local check

Exits non-zero when any check fails, after running every leg, so one run reports them all.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GOLD = REPO / "testdata" / "truehd" / "ffmpeg"

# FFmpeg's strict decode: -err_detect alone only changes what the decoder treats as an error
# internally; -xerror makes the first one fail the run (CONTRIBUTING.md, Oracles).
STRICT = ["-err_detect", "crccheck+bitstream+buffer+explode", "-xerror"]

# FFmpeg's names for the lower presentations it can be asked for (-downmix).
DOWNMIX_LAYOUT = {2: "stereo", 6: "5.1(side)"}

# SPEAKER_* masks for the WAVs this script writes, in the order forge reads them.
WAV_MASK = {1: 0x4, 2: 0x3, 3: 0x7, 4: 0x603, 5: 0x607, 6: 0x60F, 7: 0x70F, 8: 0x63F}


class Gate:
    """Collects check results so that every leg runs before the exit status is decided."""

    def __init__(self) -> None:
        self.failures: list[str] = []
        self.passed = 0
        self.findings: list[str] = []

    def check(self, ok: bool, what: str) -> None:
        if ok:
            self.passed += 1
            print(f"  ok    {what}")
        else:
            self.failures.append(what)
            print(f"  FAIL  {what}")

    def finding(self, text: str) -> None:
        self.findings.append(text)
        print(f"  note  {text}")


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, **kwargs)


def write_wav(path: Path, frames: list[list[int]], rate: int, bits: int) -> None:
    """A PCM WAV; WAVE_FORMAT_EXTENSIBLE with a channel mask above two channels."""
    channels = len(frames[0])
    width = bits // 8
    data = bytearray()
    for frame in frames:
        for v in frame:
            data += (v & ((1 << bits) - 1)).to_bytes(width, "little")
    if channels > 2:
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * channels * width,
                          channels * width, bits, 22, bits, WAV_MASK[channels])
        fmt += bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
    else:
        fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * width,
                          channels * width, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(data)) + bytes(data)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def synthesise(channels: int, rate: int, bits: int, frames: int, seed: int) -> list[list[int]]:
    """A different signal per channel, with silence, full-scale edges and a click."""
    rng = random.Random(seed)
    full = (1 << (bits - 1)) - 1
    low = -(1 << (bits - 1))
    out = []
    edge = frames // 2
    smooth = [0.0] * channels
    for n in range(frames):
        t = n / rate
        frame = []
        for c in range(channels):
            if edge <= n < edge + 40:
                v = full if (n + c) % 2 else low
            elif edge + 40 <= n < edge + 200:
                v = 0
            else:
                kind = c % 3
                if kind == 0:
                    x = 0.5 * math.sin(2 * math.pi * (300 + 170 * c) * t)
                elif kind == 1:
                    smooth[c] = 0.95 * smooth[c] + 0.05 * rng.uniform(-1, 1)
                    x = 3.0 * smooth[c]
                else:
                    x = 0.2 * rng.uniform(-1, 1) + 0.3 * math.sin(2 * math.pi * 60 * t)
                v = int(round(max(-1.0, min(1.0, x)) * full))
            frame.append(max(low, min(full, v)))
        out.append(frame)
    return out


def ffmpeg_s32(ffmpeg: str, path: Path, extra: list[str] | None = None,
               force_truehd: bool = False) -> tuple[int, bytes, str]:
    """FFmpeg's decode of `path` as raw 32-bit samples, its exit status and its messages.

    `force_truehd` passes -f truehd: FFmpeg's demuxer does not recognise its own 44.1 kHz
    output without it (P31).
    """
    cmd = [ffmpeg, "-hide_banner", "-v", "error"] + STRICT + (extra or [])
    if force_truehd:
        cmd += ["-f", "truehd"]
    cmd += ["-i", str(path), "-f", "s32le", "-"]
    r = run(cmd)
    return r.returncode, r.stdout, r.stderr.decode(errors="replace").strip()


def wav_s32(ffmpeg: str, path: Path) -> bytes:
    """A WAV's samples through FFmpeg's WAV reader, as raw 32-bit samples in the file's order."""
    r = run([ffmpeg, "-hide_banner", "-v", "error", "-i", str(path), "-f", "s32le", "-"])
    return r.stdout


def first_difference(a: bytes, b: bytes) -> str:
    if len(a) != len(b):
        return f"lengths differ ({len(a) // 4} against {len(b) // 4} samples)"
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            return f"first difference at sample {i // 4}"
    return "identical"


def leg_matrix(quick: bool) -> list[dict]:
    rates = [44100, 48000, 96000, 192000] if not quick else [48000, 96000]
    legs = []
    for channels in (1, 2, 3, 6, 7, 8):
        for rate in rates:
            if rate > 96000 and channels > 2:
                continue  # 9.6: one substream at 176.4 and 192 kHz
            for bits in (16, 24):
                legs.append({"channels": channels, "rate": rate, "bits": bits, "options": []})
    # The options that change the stream's structure, on the layouts they act on.
    legs.append({"channels": 6, "rate": 48000, "bits": 24, "options": ["truehd-downmix=0"]})
    legs.append({"channels": 8, "rate": 48000, "bits": 24, "options": ["truehd-downmix=0"]})
    legs.append({"channels": 2, "rate": 48000, "bits": 24, "options": ["truehd-iir=0"]})
    legs.append({"channels": 6, "rate": 96000, "bits": 24, "options": ["truehd-decorrelate=0"]})
    legs.append({"channels": 2, "rate": 48000, "bits": 16, "options": ["truehd-restart=128"]})
    return legs


def presentations(cli: str, path: Path) -> list[int]:
    r = run([cli, "probe", str(path), "json=1"])
    try:
        return list(json.loads(r.stdout.decode())["presentations"])
    except (ValueError, KeyError):
        return []


def legs_1_and_3(gate: Gate, cli: str, ffmpeg: str, work: Path, quick: bool) -> None:
    print("leg 1: forge encode, FFmpeg decode; leg 3: forge round trip")
    for index, leg in enumerate(leg_matrix(quick)):
        ch, rate, bits = leg["channels"], leg["rate"], leg["bits"]
        name = f"{ch}ch-{rate // 1000}k-{bits}" + "".join(
            "-" + o.split("=")[0].removeprefix("truehd-") + o.split("=")[1] for o in leg["options"])
        frames = int(0.12 * rate) + 23  # not a whole number of access units
        source = work / f"{name}.wav"
        stream = work / f"{name}.thd"
        write_wav(source, synthesise(ch, rate, bits, frames, 1000 + index), rate, bits)
        r = run([cli, "truehd-encode", str(source), str(stream)] + leg["options"])
        if r.returncode != 0:
            gate.check(False, f"{name}: forge encode ({r.stderr.decode().strip()})")
            continue
        want = wav_s32(ffmpeg, source)
        status, got, messages = ffmpeg_s32(ffmpeg, stream, force_truehd=True)
        gate.check(status == 0 and got == want,
                   f"{name}: FFmpeg's decode equals the source "
                   f"({first_difference(got, want)}{'; ' + messages if messages else ''})")
        decoded = work / f"{name}.forge.wav"
        r = run([cli, "decode", str(stream), str(decoded)])
        gate.check(r.returncode == 0 and wav_s32(ffmpeg, decoded) == want,
                   f"{name}: forge's decode equals the source")
        found = presentations(cli, stream)
        gate.check(bool(found) and found[-1] == ch, f"{name}: probe gives presentations {found}")
        for lower in found[:-1]:
            if lower not in DOWNMIX_LAYOUT:
                continue
            status, ff_lower, messages = ffmpeg_s32(
                ffmpeg, stream, ["-downmix", DOWNMIX_LAYOUT[lower]], force_truehd=True)
            ours = work / f"{name}.p{lower}.wav"
            r = run([cli, "decode", str(stream), str(ours), f"truehd-presentation={lower}"])
            gate.check(status == 0 and r.returncode == 0 and ff_lower == wav_s32(ffmpeg, ours),
                       f"{name}: the {lower}-channel presentation, FFmpeg's and forge's, agree "
                       f"({first_difference(ff_lower, wav_s32(ffmpeg, ours))})")


def leg_2(gate: Gate, cli: str, ffmpeg: str, work: Path) -> None:
    print("leg 2: forge decodes FFmpeg's golden masters")
    manifest = json.loads((GOLD / "manifest.json").read_text())
    for leg in manifest["legs"]:
        stream = GOLD / leg["stream"]
        source = GOLD / leg["source"]
        pinned = (hashlib.sha256(stream.read_bytes()).hexdigest() == leg["stream_sha256"] and
                  hashlib.sha256(source.read_bytes()).hexdigest() == leg["source_sha256"])
        gate.check(pinned, f"{leg['name']}: the committed files match the manifest's SHA-256")
        decoded = work / f"gold-{leg['name']}.wav"
        r = run([cli, "decode", str(stream), str(decoded)])
        gate.check(r.returncode == 0 and wav_s32(ffmpeg, decoded) == wav_s32(ffmpeg, source),
                   f"{leg['name']}: forge's decode equals the source")


def leg_4(gate: Gate, cli: str, ffmpeg: str, work: Path) -> None:
    print("leg 4: Atmos (16-channel substream with object audio metadata)")
    rate = 48000
    frames = int(0.25 * rate)
    bed = work / "atmos-bed.wav"
    objects = work / "atmos-objects.wav"
    stream = work / "atmos.thd"
    write_wav(bed, synthesise(8, rate, 24, frames, 77), rate, 24)
    write_wav(objects, synthesise(8, rate, 24, frames, 78), rate, 24)
    r = run([cli, "truehd-atmos-encode", str(bed), str(objects), str(stream)])
    if r.returncode != 0:
        gate.check(False, f"atmos: forge encode ({r.stderr.decode().strip()})")
        return
    out_bed = work / "atmos-bed.forge.wav"
    out_objects = work / "atmos-objects.forge.wav"
    out_oamd = work / "atmos-oamd.forge.json"
    r = run([cli, "truehd-atmos-decode", str(stream), str(out_bed), str(out_objects),
             str(out_oamd)])
    gate.check(r.returncode == 0, f"atmos: forge decode ({r.stderr.decode().strip()})")
    if r.returncode == 0:
        gate.check(wav_s32(ffmpeg, out_bed) == wav_s32(ffmpeg, bed),
                   "atmos: forge returns the 7.1 bed exactly")
        gate.check(wav_s32(ffmpeg, out_objects) == wav_s32(ffmpeg, objects),
                   "atmos: forge returns the eight objects exactly")
        try:
            oamd = json.loads(out_oamd.read_text())
            gate.check(len(oamd.get("frames", [])) > 0 and oamd.get("objects") == 8,
                       f"atmos: forge returns the object metadata ({len(oamd.get('frames', []))} "
                       "frames of 8 objects)")
        except ValueError:
            gate.check(False, "atmos: forge's object metadata is JSON")
    status, got, messages = ffmpeg_s32(ffmpeg, stream, force_truehd=True)
    gate.check(status == 0, f"atmos: FFmpeg decodes the stream without error"
                            f"{' (' + messages + ')' if messages else ''}")
    probe = run(["ffprobe", "-v", "error", "-f", "truehd", "-show_entries",
                 "stream=channels,channel_layout", "-of", "compact", str(stream)])
    exposed = probe.stdout.decode().strip()
    gate.finding(f"atmos: FFmpeg exposes {exposed or 'nothing it can describe'}, "
                 f"{len(got) // 4 // max(1, frames)} samples a frame decoded")
    bed_frames = wav_s32(ffmpeg, bed)
    if status == 0 and len(got) == len(bed_frames):
        same = got == bed_frames
        gate.finding("atmos: FFmpeg's decode is the bed's 7.1 fold of the scene"
                     if not same else "atmos: FFmpeg's decode equals the bed exactly")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", default=os.environ.get("ICLFORGE_CLI", ""),
                        help="forge (default: $ICLFORGE_CLI)")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    if not args.cli:
        parser.error("--cli or $ICLFORGE_CLI is required")
    with tempfile.TemporaryDirectory() as temp:
        work = args.workdir or Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        gate = Gate()
        legs_1_and_3(gate, args.cli, args.ffmpeg, work, args.quick)
        leg_2(gate, args.cli, args.ffmpeg, work)
        leg_4(gate, args.cli, args.ffmpeg, work)
    print(f"\n{gate.passed} checks passed, {len(gate.failures)} failed")
    for text in gate.findings:
        print(f"finding: {text}")
    for text in gate.failures:
        print(f"failed: {text}")
    return 1 if gate.failures else 0


if __name__ == "__main__":
    sys.exit(main())
