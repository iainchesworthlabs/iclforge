#!/usr/bin/env python3
"""Holds the TrueHD and MLP decoders to FFmpeg's strict decode on the committed fixtures.

For every fixture in testdata/truehd/manifest.json:

  * the stream file is the one the manifest names (SHA-256);
  * FFmpeg's strict decode of it (-v error -xerror -err_detect crccheck+bitstream+buffer+explode)
    gives the PCM the manifest names (skipped with a notice when ffmpeg is not installed, unless
    --require-ffmpeg);
  * each decoder given with --decoder (a program run as `PROGRAM STREAM OUT.s32`, writing 32-bit
    little-endian PCM, the 24-bit sample shifted left by 8, as `ffmpeg -f s32le` does) gives the
    same PCM;
  * with --reference, tools/references/truehd_syntax.py gives it too.

The codec is lossless, so any difference is a decoder that is wrong. Exit status 1 on any
difference.
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"
REFERENCE = REPO / "tools" / "references" / "truehd_syntax.py"
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def ffmpeg_pcm(path, codec):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", *STRICT, "-f", codec, "-i", str(path), "-f", "s32le", "-"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return None, result.stderr.decode(errors="replace").strip()
    return result.stdout, ""


def run_decoder(command, path, scratch):
    out = scratch / "decoded.s32"
    out.unlink(missing_ok=True)
    result = subprocess.run([*command, str(path), str(out)], capture_output=True, check=False)
    if result.returncode != 0 or not out.exists():
        return None, result.stderr.decode(errors="replace").strip()
    return out.read_bytes(), ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=pathlib.Path, default=FIXTURES / "manifest.json")
    parser.add_argument(
        "--decoder",
        action="append",
        default=[],
        metavar="'PROGRAM [ARGS]'",
        help="a decoder to hold to the manifest: run as PROGRAM [ARGS] STREAM OUT.s32",
    )
    parser.add_argument("--reference", action="store_true", help="run the Python reference too")
    parser.add_argument("--require-ffmpeg", action="store_true")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    have_ffmpeg = shutil.which("ffmpeg") is not None
    if not have_ffmpeg:
        if args.require_ffmpeg:
            sys.exit("verify_truehd_interop: ffmpeg is required and not installed")
        print("notice: ffmpeg is not installed; its leg is skipped", file=sys.stderr)
    decoders = [(command, command.split()) for command in args.decoder]
    if args.reference:
        decoders.append(("truehd_syntax.py", [sys.executable, str(REFERENCE), "decode"]))

    failures = 0
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for fixture in manifest["fixtures"]:
            path = args.manifest.parent / fixture["file"]
            problems = []
            if sha256(path.read_bytes()) != fixture["stream_sha256"]:
                problems.append("the stream is not the one the manifest names")
            if have_ffmpeg:
                pcm, error = ffmpeg_pcm(path, fixture["codec"])
                if pcm is None:
                    problems.append(f"ffmpeg strict decode failed: {error}")
                elif sha256(pcm) != fixture["pcm_sha256"]:
                    problems.append("ffmpeg's strict decode is not the manifest's")
            for name, command in decoders:
                pcm, error = run_decoder(command, path, scratch)
                if pcm is None:
                    problems.append(f"{name} failed: {error}")
                elif sha256(pcm) != fixture["pcm_sha256"]:
                    problems.append(f"{name} differs from the manifest")
            failures += bool(problems)
            status = "FAIL" if problems else "ok  "
            print(f"{status} {fixture['name']}" + "".join(f"\n       {p}" for p in problems))
    print(f"{len(manifest['fixtures'])} fixtures, {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
