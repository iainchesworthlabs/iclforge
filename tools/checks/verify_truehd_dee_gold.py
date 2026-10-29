#!/usr/bin/env python3
"""Decode the Dolby Encoding Engine's TrueHD golden masters with forge and with FFmpeg.

The DEE set is made on the owner's machine by tools/generators/gen_dee_gold.py and kept outside
the repository (its manifest is dee-gold-manifest.json; DEE's licence ends on 2026-11-06). DEE
streams are test inputs only (gen_dee_gold.py's TRUEHD_NOTE): this script decodes them and
compares the PCM with the source each was made from; it does not look inside them.

For every TrueHD leg of the manifest that DEE made:
  - the stream's and the sources' SHA-256 must be the manifest's;
  - forge's decode and FFmpeg's (strict) must both succeed;
  - where the manifest names no audio_altered, both decodes of the top presentation must equal
    the leg's first source sample for sample, and a leg with an independent 2-channel
    presentation (a second source) must give that source as its 2-channel presentation, from
    both decoders;
  - an Atmos leg (made from an ADM master, so with no WAV to compare) must decode in both.

The same checks run over the committed subset, testdata/truehd/dee (made by
tools/generators/export_dee_truehd_gold.py), with --committed.

Usage:
    ICLFORGE_DEE_GOLD_DIR=/path/to/dee-gold python3 tools/checks/verify_truehd_dee_gold.py \
        --cli build/bin/forge
    python3 tools/checks/verify_truehd_dee_gold.py --cli build/bin/forge --committed

Without ICLFORGE_DEE_GOLD_DIR (and without --committed) it prints why it skipped and exits 0:
the set exists only where DEE ran. With --committed and no committed subset, likewise.
Exits 1 when any check fails.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
COMMITTED = REPO / "testdata" / "truehd" / "dee"
STRICT = ["-err_detect", "crccheck+bitstream+buffer+explode", "-xerror"]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def s32(ffmpeg: str, path: Path, extra: list[str] | None = None,
        input_format: list[str] | None = None) -> tuple[int, bytes, str]:
    """A file's samples through FFmpeg as raw 32-bit integers: its exit status, the samples and
    its messages."""
    cmd = [ffmpeg, "-hide_banner", "-v", "error", *STRICT, *(extra or []),
           *(input_format or []), "-i", str(path), "-f", "s32le", "-"]
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode, r.stdout, r.stderr.decode(errors="replace").strip()


def wav_samples(ffmpeg: str, path: Path) -> bytes:
    r = subprocess.run([ffmpeg, "-hide_banner", "-v", "error", "-i", str(path), "-f", "s32le",
                        "-"], capture_output=True)
    return r.stdout


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.passed = 0

    def check(self, ok: bool, what: str) -> None:
        if ok:
            self.passed += 1
            print(f"  ok    {what}")
        else:
            self.failures.append(what)
            print(f"  FAIL  {what}")


def legs_of(manifest: dict, root: Path, committed: bool) -> list[dict]:
    """The TrueHD legs to check: name, stream paths and their SHA-256, sources, altered."""
    legs = []
    for name, entry in sorted(manifest.get("legs", {}).items()):
        if entry.get("codec") != "truehd" or "refused" in entry or "failed" in entry:
            continue
        if committed:
            streams = [(root / entry["stream"], entry["stream_sha256"])]
            sources = [(root / s, h) for s, h in zip(entry.get("sources", []),
                                                     entry.get("source_sha256", []))]
        else:
            described = entry.get("streams") or [entry]
            streams = [(root / "streams" / name / d["stream"], d["sha256"]) for d in described]
            sources = [(root / "sources" / f"{s}.wav", h)
                       for s, h in zip(entry.get("sources", []), entry.get("source_sha256", []))]
        legs.append({"name": name, "streams": streams, "sources": sources,
                     "altered": entry.get("audio_altered"),
                     "master": entry.get("master") is not None or name.startswith("obj-")})
    return legs


def check_leg(report: Report, leg: dict, cli: str, ffmpeg: str, work: Path) -> None:
    name = leg["name"]
    for path, digest in leg["streams"] + leg["sources"]:
        if not path.is_file():
            report.check(False, f"{name}: {path.name} is present")
            return
        report.check(sha256(path) == digest, f"{name}: {path.name} matches its SHA-256")
    stream = leg["streams"][0][0]
    ours = work / f"{name}.forge.wav"
    r = subprocess.run([cli, "decode", str(stream), str(ours)], capture_output=True)
    report.check(r.returncode == 0,
                 f"{name}: forge decodes it ({r.stderr.decode(errors='replace').strip()[-160:]})")
    status, theirs, messages = s32(ffmpeg, stream, input_format=["-f", "truehd"])
    report.check(status == 0, f"{name}: FFmpeg decodes it{' (' + messages + ')' if messages else ''}")
    if leg["master"] or not leg["sources"] or r.returncode != 0 or status != 0:
        return
    if leg["altered"]:
        print(f"  skip  {name}: audio altered ({leg['altered']}), no sample comparison")
        return
    source = wav_samples(ffmpeg, leg["sources"][0][0])
    report.check(wav_samples(ffmpeg, ours) == source, f"{name}: forge's decode equals the source")
    report.check(theirs == source, f"{name}: FFmpeg's decode equals the source")
    if len(leg["sources"]) > 1:
        # An independent 2-channel presentation: its own source, from each decoder.
        two = work / f"{name}.forge.p2.wav"
        r2 = subprocess.run([cli, "decode", str(stream), str(two), "truehd-presentation=2"],
                            capture_output=True)
        second = wav_samples(ffmpeg, leg["sources"][1][0])
        report.check(r2.returncode == 0 and wav_samples(ffmpeg, two) == second,
                     f"{name}: forge's 2-channel presentation equals the second source")
        status2, theirs2, _ = s32(ffmpeg, stream, ["-downmix", "stereo"], ["-f", "truehd"])
        report.check(status2 == 0 and theirs2 == second,
                     f"{name}: FFmpeg's 2-channel presentation equals the second source")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", default=os.environ.get("ICLFORGE_CLI", ""))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--committed", nargs="?", const=str(COMMITTED), default=None,
                        metavar="DIR",
                        help="check the committed subset instead (testdata/truehd/dee, or DIR)")
    args = parser.parse_args()
    if not args.cli:
        parser.error("--cli or $ICLFORGE_CLI is required")
    if args.committed:
        root = Path(args.committed)
        manifest_path = root / "manifest.json"
        if not manifest_path.is_file():
            print(f"skipped: no committed DEE TrueHD subset ({manifest_path} is absent); "
                  "tools/generators/export_dee_truehd_gold.py makes one from the local set")
            return 0
    else:
        value = os.environ.get("ICLFORGE_DEE_GOLD_DIR", "")
        if not value:
            print("skipped: ICLFORGE_DEE_GOLD_DIR is not set. The DEE golden set is made by "
                  "tools/generators/gen_dee_gold.py where DEE runs and kept outside the "
                  "repository; point the variable at it to check its TrueHD legs.")
            return 0
        root = Path(value)
        manifest_path = root / "dee-gold-manifest.json"
        if not manifest_path.is_file():
            print(f"error: {manifest_path} is absent; ICLFORGE_DEE_GOLD_DIR must be the "
                  "directory gen_dee_gold.py wrote", file=sys.stderr)
            return 1
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    legs = legs_of(manifest, root, args.committed is not None)
    print(f"{len(legs)} TrueHD legs in {manifest_path}")
    report = Report()
    with tempfile.TemporaryDirectory() as temp:
        for leg in legs:
            check_leg(report, leg, args.cli, args.ffmpeg, Path(temp))
    print(f"\n{report.passed} checks passed, {len(report.failures)} failed")
    for text in report.failures:
        print(f"failed: {text}")
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
