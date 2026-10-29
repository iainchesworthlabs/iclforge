#!/usr/bin/env python3
"""Export a small, hash-pinned subset of the DEE TrueHD golden masters into testdata/truehd/dee.

The Dolby Encoding Engine's golden set (tools/generators/gen_dee_gold.py) lives outside the
repository, on the machine DEE runs on, and DEE's licence ends on 2026-11-06. This script copies
chosen TrueHD legs into the repository so that they outlive it: for each leg, the first whole
access units of DEE's stream up to a size limit (a TrueHD stream cut at an access unit boundary
after a major sync is a stream in its own right, decodable from its start) and the matching
first samples of the leg's source WAV(s), with a manifest that pins every file's SHA-256 and
records the full stream's and source's SHA-256 from the DEE manifest for provenance.

DEE streams are test inputs only (gen_dee_gold.py's TRUEHD_NOTE): the cut is made by the access
unit framing alone, and nothing inside an access unit is read. The size limits keep the subset
inside the repository's budget for golden masters: each stream well under 1 MB, the whole well
under 10 MB.

Usage:
    python3 tools/generators/export_dee_truehd_gold.py --gold-set /path/to/dee-gold \
        [--legs thd-2ch-tones-48k24,thd-8ch-music-48k24-dmx6-dmx2] [--max-bytes 600000]

The default legs are one of each presentation and depth that DEE made losslessly. The owner then
reviews testdata/truehd/dee/manifest.json and commits it; tools/checks/verify_truehd_dee_gold.py
--committed checks it from then on.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "testdata" / "truehd" / "dee"

DEFAULT_LEGS = (
    "thd-2ch-tones-48k24",
    "thd-2ch-music-48k16",
    "thd-6ch-film-48k24",
    "thd-6ch-tones-96k24",
    "thd-8ch-music-48k24",
    "thd-8ch-music-48k24-dmx6-dmx2",
    "thd-8ch-music-48k24-indep2",
)
MAX_BYTES_PER_STREAM = 600_000
MAX_BYTES_TOTAL = 8_000_000


def samples_per_unit(rate: int) -> int:
    """40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96, 160 at 176.4 and 192
    (planning/truehd.md, P1)."""
    return 160 if rate > 96000 else 80 if rate > 48000 else 40


def cut_stream(data: bytes, max_bytes: int) -> tuple[bytes, int]:
    """The first whole access units of `data` within `max_bytes`, ending before an access unit
    that starts with a major sync so that every restart interval kept is whole; and how many
    access units that is. Reads only access_unit_length and the major sync word (3.1, 4.1)."""
    at = 0
    units = 0
    keep = 0
    keep_units = 0
    while at + 4 <= len(data):
        length = ((data[at] & 0x0F) << 8 | data[at + 1]) * 2
        if length < 4 or at + length > len(data):
            break
        major = data[at + 4:at + 8] in (b"\xf8\x72\x6f\xba", b"\xf8\x72\x6f\xbb")
        if major and at > 0:
            keep, keep_units = at, units
        if at + length > max_bytes:
            break
        at += length
        units += 1
    if at >= len(data):
        keep, keep_units = at, units
    return data[:keep], keep_units


def cut_wav(data: bytes, frames: int) -> bytes:
    """The first `frames` frames of a PCM WAV, its header rewritten to the new length."""
    at = 12
    fmt = None
    while at + 8 <= len(data):
        tag = data[at:at + 4]
        size = struct.unpack("<I", data[at + 4:at + 8])[0]
        if tag == b"fmt ":
            fmt = data[at:at + 8 + size]
        elif tag == b"data":
            channels, bits = struct.unpack("<H", fmt[10:12])[0], struct.unpack("<H", fmt[22:24])[0]
            body = data[at + 8:at + 8 + frames * channels * (bits // 8)]
            riff = b"WAVE" + fmt + b"data" + struct.pack("<I", len(body)) + body
            return b"RIFF" + struct.pack("<I", len(riff)) + riff
        at += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gold-set", type=Path, required=True,
                        help="the directory gen_dee_gold.py wrote (with dee-gold-manifest.json)")
    parser.add_argument("--legs", default=",".join(DEFAULT_LEGS))
    parser.add_argument("--max-bytes", type=int, default=MAX_BYTES_PER_STREAM)
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args()
    manifest = json.loads((args.gold_set / "dee-gold-manifest.json").read_text(encoding="utf-8"))
    sources = manifest.get("sources", {})
    out = {"_comment": ("A subset of the DEE TrueHD golden masters, cut at access unit "
                        "boundaries, made by tools/generators/export_dee_truehd_gold.py; test "
                        "inputs only (gen_dee_gold.py's TRUEHD_NOTE)."),
           "dee_version": manifest.get("dee_version"), "legs": {}}
    total = 0
    args.out.mkdir(parents=True, exist_ok=True)
    for name in [n for n in args.legs.split(",") if n]:
        entry = manifest.get("legs", {}).get(name)
        if not entry or entry.get("codec") != "truehd" or "refused" in entry or "failed" in entry:
            print(f"{name}: not a TrueHD leg DEE made; skipped", file=sys.stderr)
            continue
        stream_path = args.gold_set / "streams" / name / entry["stream"]
        data = stream_path.read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            print(f"{name}: {stream_path} does not match the manifest's SHA-256", file=sys.stderr)
            return 1
        cut, units = cut_stream(data, args.max_bytes)
        if not cut:
            print(f"{name}: no whole restart interval fits {args.max_bytes} bytes", file=sys.stderr)
            continue
        rate = sources[entry["sources"][0]]["sample_rate"]
        frames = units * samples_per_unit(rate)
        record = {"stream": f"{name}.thd", "access_units": units, "frames": frames,
                  "full_stream_sha256": entry["sha256"], "sources": [], "source_sha256": [],
                  "full_source_sha256": list(entry.get("source_sha256", [])),
                  "codec": "truehd"}
        if entry.get("audio_altered"):
            record["audio_altered"] = entry["audio_altered"]
        (args.out / record["stream"]).write_bytes(cut)
        record["stream_sha256"] = hashlib.sha256(cut).hexdigest()
        total += len(cut)
        for i, source in enumerate(entry.get("sources", [])):
            wav = cut_wav((args.gold_set / "sources" / f"{source}.wav").read_bytes(), frames)
            file_name = f"{name}.source{i + 1}.wav"
            (args.out / file_name).write_bytes(wav)
            record["sources"].append(file_name)
            record["source_sha256"].append(hashlib.sha256(wav).hexdigest())
            total += len(wav)
        out["legs"][name] = record
        print(f"{name}: {units} access units, {len(cut)} bytes")
    if total > MAX_BYTES_TOTAL:
        print(f"the subset is {total} bytes, over the {MAX_BYTES_TOTAL} budget; choose fewer "
              "legs or a smaller --max-bytes", file=sys.stderr)
        return 1
    (args.out / "manifest.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out / 'manifest.json'}: {len(out['legs'])} legs, {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
