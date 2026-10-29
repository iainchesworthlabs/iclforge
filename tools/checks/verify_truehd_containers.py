#!/usr/bin/env python3
"""TrueHD in containers, held against FFmpeg: MP4, fragmented MP4, Matroska, MPEG-TS and MAT.

The containers carry TrueHD with no document to read (libs/containers/ERRATA.md), so FFmpeg is the oracle, in
both directions and bit-exactly, over the committed fixtures (testdata/truehd/manifest.json):

  ours -> FFmpeg    the container iclforge-truehd-container-tool writes is read by FFmpeg, whose strict decode of
                    it equals its strict decode of the raw stream
  FFmpeg -> ours    the container FFmpeg writes is read by the tool, and the elementary stream it gives back is the
                    raw stream, byte for byte
  round trip        mux then demux with the tool alone gives the raw stream back
  MAT               the tool's MAT bursts equal the bursts of FFmpeg's `spdif` muxer for every whole frame

A leg whose container FFmpeg cannot write or read is reported as SKIP with the reason, never as a pass. The MLP
fixtures are not carried (FFmpeg's muxers take TrueHD only for these containers).

Usage: verify_truehd_containers.py --tool <iclforge-truehd-container-tool> [--ffmpeg ffmpeg] [--only mp4,mkv,ts,mat]
                                   [--fixtures testdata/truehd] [--keep DIR]
Exit status 1 when any leg fails.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# ffmpeg's strict decode flags (CONTRIBUTING.md, "Oracles"): without them FFmpeg conceals what it cannot read.
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]

# format -> (the tool's name for it, ffmpeg's muxer, file extension, extra ffmpeg output options)
FORMATS = {
    "mp4": ("mp4", "mp4", "mp4", ["-strict", "-2"]),
    "mp4frag": ("mp4frag", "mp4", "frag.mp4", []),
    "mkv": ("mkv", "matroska", "mkv", []),
    "ts": ("ts", "mpegts", "ts", []),
}


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, **kwargs)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Report:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def ok(self, leg: str) -> None:
        self.passed += 1
        print(f"PASS  {leg}")

    def fail(self, leg: str, why: str) -> None:
        self.failed += 1
        print(f"FAIL  {leg}: {why}")

    def skip(self, leg: str, why: str) -> None:
        self.skipped += 1
        print(f"SKIP  {leg}: {why}")


def ffmpeg_pcm(ffmpeg: str, source: Path, extra_in: list[str] | None = None) -> tuple[bytes | None, str]:
    """Strict decode of the first audio stream to s32le, or (None, stderr)."""
    cmd = [ffmpeg, *STRICT, *(extra_in or []), "-i", str(source), "-map", "0:a:0", "-f", "s32le", "-"]
    done = run(cmd)
    if done.returncode != 0:
        return None, done.stderr.decode(errors="replace").strip()[-300:]
    return done.stdout, ""


def check_format(report: Report, tool: str, ffmpeg: str, name: str, raw: Path, work: Path) -> None:
    tool_name, muxer, ext, out_opts = FORMATS[name]
    leg = f"{raw.name} {name}"
    reference, why = ffmpeg_pcm(ffmpeg, raw)
    if reference is None:
        report.fail(f"{leg} reference", f"ffmpeg cannot decode the raw stream: {why}")
        return
    stream = raw.read_bytes()

    ours = work / f"{raw.stem}.ours.{ext}"
    made = run([tool, "mux", tool_name, str(raw), str(ours)])
    if made.returncode != 0:
        report.fail(f"{leg} mux", made.stderr.decode(errors="replace").strip())
        return

    # round trip through the tool alone
    back = work / f"{raw.stem}.ours.{ext}.thd"
    done = run([tool, "demux", "mp4" if name == "mp4frag" else tool_name, str(ours), str(back)])
    if done.returncode != 0:
        report.fail(f"{leg} round trip", done.stderr.decode(errors="replace").strip())
    elif back.read_bytes() != stream:
        report.fail(f"{leg} round trip", "the demuxed stream is not the raw stream")
    else:
        report.ok(f"{leg} round trip")

    # ours -> FFmpeg
    pcm, why = ffmpeg_pcm(ffmpeg, ours)
    if pcm is None:
        report.fail(f"{leg} ours -> ffmpeg", why)
    elif pcm != reference:
        report.fail(f"{leg} ours -> ffmpeg", f"decode differs ({sha256(pcm)[:12]} vs {sha256(reference)[:12]})")
    else:
        report.ok(f"{leg} ours -> ffmpeg")

    # FFmpeg -> ours
    theirs = work / f"{raw.stem}.ffmpeg.{ext}"
    if name == "mp4frag":
        made = run([ffmpeg, "-v", "error", "-y", "-i", str(raw), "-c", "copy", "-strict", "-2", "-f", "mp4",
                    "-movflags", "frag_keyframe+delay_moov+default_base_moof", str(theirs)])
    else:
        made = run([ffmpeg, "-v", "error", "-y", "-i", str(raw), "-c", "copy", *out_opts, "-f", muxer, str(theirs)])
    if made.returncode != 0:
        report.skip(f"{leg} ffmpeg -> ours", "ffmpeg cannot write it: " + made.stderr.decode(errors="replace").strip()[-160:])
        return
    back = work / f"{raw.stem}.ffmpeg.{ext}.thd"
    done = run([tool, "demux", "mp4" if name == "mp4frag" else tool_name, str(theirs), str(back)])
    if done.returncode != 0:
        report.fail(f"{leg} ffmpeg -> ours", done.stderr.decode(errors="replace").strip())
    elif back.read_bytes() != stream:
        report.fail(f"{leg} ffmpeg -> ours", "the demuxed stream is not the raw stream")
    else:
        report.ok(f"{leg} ffmpeg -> ours")


def check_mat(report: Report, tool: str, ffmpeg: str, raw: Path, work: Path) -> None:
    leg = f"{raw.name} mat"
    ours = work / f"{raw.stem}.ours.mat"
    made = run([tool, "mux", "mat", str(raw), str(ours)])
    if made.returncode != 0:
        report.fail(f"{leg} mux", made.stderr.decode(errors="replace").strip())
        return
    theirs = work / f"{raw.stem}.ffmpeg.spdif"
    made = run([ffmpeg, "-v", "error", "-y", "-i", str(raw), "-c", "copy", "-f", "spdif", str(theirs)])
    if made.returncode != 0:
        report.skip(f"{leg} bytes", "ffmpeg cannot write it: " + made.stderr.decode(errors="replace").strip()[-160:])
    else:
        a, b = ours.read_bytes(), theirs.read_bytes()
        # FFmpeg drops a last frame that is not full; the tool pads it (ERRATA M5), so only the whole frames compare.
        if a[: len(b)] == b and len(a) >= len(b):
            report.ok(f"{leg} bytes ({len(b) // 61440} whole frames equal FFmpeg's, {len(a) // 61440} written)")
        else:
            first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            report.fail(f"{leg} bytes", f"differs from ffmpeg's spdif output at byte {first}")

        # FFmpeg's own MAT -> ours: the access units of the whole frames come back, byte for byte.
        back = work / f"{raw.stem}.ffmpeg.spdif.thd"
        done = run([tool, "demux", "mat", str(theirs), str(back)])
        stream = raw.read_bytes()
        if done.returncode != 0:
            report.fail(f"{leg} ffmpeg -> ours", done.stderr.decode(errors="replace").strip())
        elif not stream.startswith(back.read_bytes()) or not back.stat().st_size:
            report.fail(f"{leg} ffmpeg -> ours", "the unwrapped access units are not a prefix of the raw stream")
        else:
            report.ok(f"{leg} ffmpeg -> ours")

    back = work / f"{raw.stem}.ours.mat.thd"
    done = run([tool, "demux", "mat", str(ours), str(back)])
    if done.returncode != 0:
        report.fail(f"{leg} round trip", done.stderr.decode(errors="replace").strip())
    elif back.read_bytes() != raw.read_bytes():
        report.fail(f"{leg} round trip", "the unwrapped access units are not the raw stream")
    else:
        report.ok(f"{leg} round trip")

    # ours -> FFmpeg: only if its spdif demuxer reads MAT at all. Probe it on its own output first: if it finds no
    # stream there, the leg has no oracle (ERRATA M7) and is skipped, not passed.
    out = work / f"{raw.stem}.ours.mat.ffmpeg.thd"
    own = run([ffmpeg, "-v", "error", "-y", "-f", "spdif", "-i", str(theirs), "-c", "copy", "-f", "truehd",
               str(work / f"{raw.stem}.ffmpeg.spdif.ffmpeg.thd")])
    if own.returncode != 0:
        report.skip(f"{leg} ours -> ffmpeg",
                    "ffmpeg's spdif demuxer reads no stream from its own TrueHD MAT output, so FFmpeg cannot read MAT")
        return
    done = run([ffmpeg, "-v", "error", "-y", "-f", "spdif", "-i", str(ours), "-c", "copy", "-f", "truehd", str(out)])
    if done.returncode != 0:
        report.fail(f"{leg} ours -> ffmpeg", "ffmpeg reads its own MAT but not ours: " + done.stderr.decode(errors="replace").strip()[-160:])
    elif not raw.read_bytes().startswith(out.read_bytes()) or not out.stat().st_size:
        report.fail(f"{leg} ours -> ffmpeg", "the access units ffmpeg reads are not a prefix of the raw stream")
    else:
        report.ok(f"{leg} ours -> ffmpeg")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True, help="iclforge-truehd-container-tool")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--only", default="mp4,mp4frag,mkv,ts,mat")
    parser.add_argument("--fixtures", default=str(REPO / "testdata" / "truehd"))
    parser.add_argument("--keep", help="keep the files written here instead of a temporary directory")
    args = parser.parse_args()

    if shutil.which(args.ffmpeg) is None:
        print(f"error: {args.ffmpeg} not found", file=sys.stderr)
        return 2
    fixtures = Path(args.fixtures)
    manifest = json.loads((fixtures / "manifest.json").read_text())
    raws = [fixtures / f["file"] for f in manifest["fixtures"] if f["codec"] == "truehd"]
    wanted = args.only.split(",")
    report = Report()
    work_root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="truehd-containers-"))
    work_root.mkdir(parents=True, exist_ok=True)
    try:
        for raw in raws:
            for name in FORMATS:
                if name in wanted:
                    check_format(report, args.tool, args.ffmpeg, name, raw, work_root)
            if "mat" in wanted:
                check_mat(report, args.tool, args.ffmpeg, raw, work_root)
    finally:
        if not args.keep:
            shutil.rmtree(work_root, ignore_errors=True)
    print(f"\n{report.passed} passed, {report.failed} failed, {report.skipped} skipped")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
