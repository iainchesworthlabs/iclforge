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
  * with --forge PATH, `forge decode` gives it too (its 24-bit WAV, read back as 32-bit words);
  * with --reference, tools/references/truehd_syntax.py gives it too.

The codec is lossless, so any difference is a decoder that is wrong. Exit status 1 on any
difference.

With --handbuilt the same is done for the hand-built streams of testdata/truehd/handbuilt
(tools/references/truehd_writer.py). FFmpeg's decode of those is held to the outcome their manifest
names for each stream: the whole signal it was built from (equal), fewer samples of which the
first are the signal's (short), a failure (refused), a success on a stream the library refuses
(decodes), or other samples than the signal (differs). --forge and --reference
are held to the signals (or to refusing the stream).
"""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"
HANDBUILT = FIXTURES / "handbuilt"
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


def wav_as_s32le(data):
    """A 24-bit PCM WAV's samples as 32-bit little-endian words (the sample in the top 3 bytes)."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    offset, bits = 12, 0
    while offset + 8 <= len(data):
        tag, size = (
            data[offset : offset + 4],
            int.from_bytes(data[offset + 4 : offset + 8], "little"),
        )
        body = offset + 8
        if tag == b"fmt ":
            bits = int.from_bytes(data[body + 14 : body + 16], "little")
        elif tag == b"data":
            if bits != 24:
                raise ValueError(f"{bits}-bit WAV, expected 24")
            size = min(size, len(data) - body)
            out = bytearray()
            for at in range(body, body + size - 2, 3):
                out += b"\x00" + data[at : at + 3]
            return bytes(out)
        offset = body + size + (size & 1)
    raise ValueError("no data chunk")


def run_forge(forge, path, scratch, options=()):
    out = scratch / "forge.wav"
    out.unlink(missing_ok=True)
    result = subprocess.run(
        [forge, "decode", str(path), str(out), *options], capture_output=True, check=False
    )
    if result.returncode != 0 or not out.exists():
        return None, (result.stderr or result.stdout).decode(errors="replace").strip()
    try:
        return wav_as_s32le(out.read_bytes()), ""
    except ValueError as error:
        return None, str(error)


def reference_pcm(path, presentation):
    """The Python reference decoder's PCM for the stream, or an error (a refusal)."""
    sys.path.insert(0, str(REFERENCE.parent))
    import truehd_syntax  # noqa: PLC0415

    try:
        rows, _rate, _channels = truehd_syntax.decode_stream(path.read_bytes(), presentation)
    except truehd_syntax.StreamError as error:
        return None, str(error)
    return truehd_syntax.to_s32le(rows), ""


def ffmpeg_major():
    """The major version of the installed ffmpeg, None for a build that does not say."""
    result = subprocess.run(["ffmpeg", "-version"], capture_output=True, check=False)
    match = re.match(r"ffmpeg version n?(\d+)\.", result.stdout.decode(errors="replace"))
    return int(match.group(1)) if match else None


def in_stream_order(pcm, channels, columns):
    """32-bit interleaved PCM whose column k holds the stream's channel columns[k], put back in the
    stream's order (FFmpeg returns the channels of a 7 or 8 channel stream in its own order)."""
    words = [pcm[i : i + 4] for i in range(0, len(pcm), 4)]
    out = [b""] * len(words)
    for frame in range(0, len(words), channels):
        for k, column in enumerate(columns):
            out[frame + column] = words[frame + k]
    return b"".join(out)


def check_handbuilt_ffmpeg(path, fixture, cache, held=True):
    """The problems with FFmpeg's strict decode of a hand-built stream against the outcome the
    manifest names for it (equal: the whole signal; short: fewer samples, the first of them the
    signal's; refused: it fails; decodes: it succeeds, the library refusing the stream; differs: it
    succeeds and returns other samples, whose hash the manifest holds). The
    outcomes other than equal were seen with one FFmpeg version and are held only if `held`."""
    if path not in cache:
        cache[path] = ffmpeg_pcm(path, fixture.get("codec", "truehd"))
    pcm, error = cache[path]
    outcome = fixture["ffmpeg"]
    kind = outcome["outcome"]
    if kind != "equal" and not held:
        return []
    if kind == "refused":
        return [] if pcm is None else ["ffmpeg decoded a stream the manifest says it refuses"]
    if pcm is None:
        return [f"ffmpeg strict decode failed: {error}"]
    if kind == "decodes":
        return []
    if kind == "differs":
        if sha256(pcm) == fixture["pcm_sha256"]:
            return ["ffmpeg returned the signal, where the manifest says it differs"]
        if sha256(pcm) != outcome["pcm_sha256"]:
            return ["ffmpeg did not return the samples the manifest says it returns"]
        return []
    if kind == "short":
        width = fixture["channels"] * 4
        head = pcm[: outcome["equal_samples"] * width]
        if len(pcm) != outcome["returned"] * width or sha256(head) != outcome["pcm_sha256"]:
            return [
                f"ffmpeg did not return {outcome['returned']} samples, the first "
                f"{outcome['equal_samples']} of them the signal's"
            ]
        return []
    if kind == "equal":
        if "columns" in outcome:
            pcm = in_stream_order(pcm, fixture["channels"], outcome["columns"])
        if sha256(pcm) != fixture["pcm_sha256"]:
            return ["ffmpeg's strict decode is not the signal the stream was built from"]
        return []
    return [f"unknown ffmpeg outcome {kind}"]


def check_handbuilt(args, have_ffmpeg, scratch):
    """The hand-built set: FFmpeg held to the outcome the manifest names for each stream, and the
    library (forge, the Python reference) to the signal each was built from, or to refusing it."""
    manifest = json.loads(args.manifest.read_text())
    cache = {}
    failures = 0
    held = True
    if have_ffmpeg:
        seen = int(manifest["ffmpeg_version"].split(".")[0])
        held = ffmpeg_major() == seen
        if not held:
            print(
                f"notice: the outcomes were seen with FFmpeg {manifest['ffmpeg_version']}, "
                f"this is {ffmpeg_major()}.x: only the equal ones are held"
            )
    entries = [(f, False) for f in manifest["fixtures"]] + [
        (f, True) for f in manifest.get("refused", [])
    ]
    for fixture, refused in entries:
        path = args.manifest.parent / fixture["file"]
        problems = []
        if sha256(path.read_bytes()) != fixture["stream_sha256"]:
            problems.append("the stream is not the one the manifest names")
        if have_ffmpeg:
            problems += check_handbuilt_ffmpeg(path, fixture, cache, held)
        presentation = fixture.get("presentation", "best")
        if args.forge:
            options = [f"truehd-presentation={presentation}"]
            pcm, error = run_forge(args.forge, path, scratch, options)
            if refused:
                if pcm is not None:
                    problems.append("forge decoded a stream the manifest says it refuses")
            elif pcm is None:
                problems.append(f"forge decode failed: {error}")
            elif sha256(pcm) != fixture["pcm_sha256"]:
                problems.append("forge decode differs from the signal the stream was built from")
        if args.reference:
            pcm, error = reference_pcm(path, presentation)
            if refused:
                if pcm is not None:
                    problems.append("the reference decoded a stream the manifest says it refuses")
            elif pcm is None:
                problems.append(f"the reference failed: {error}")
            elif sha256(pcm) != fixture["pcm_sha256"]:
                problems.append("the reference differs from the signal the stream was built from")
        failures += bool(problems)
        status = "FAIL" if problems else "ok  "
        label = f"{fixture['name']} (ffmpeg: {fixture['ffmpeg']['outcome']})"
        print(f"{status} {label}" + "".join(f"\n       {p}" for p in problems))
    print(f"{len(entries)} hand-built streams, {failures} failing")
    return 1 if failures else 0


def check_spliced(manifest_path, manifest, args, have_ffmpeg, scratch):
    """The streams of two encodes joined end to end. FFmpeg's strict decode and the library's
    lenient decode return the same samples; the library's strict decode stops at the join, where
    the second stream's first restart header carries a lossless check that checks nothing
    (libs/truehd/ERRATA.md, entry 12 and question 20)."""
    failures = 0
    for fixture in manifest.get("spliced", []):
        path = manifest_path.parent / fixture["file"]
        problems = []
        if sha256(path.read_bytes()) != fixture["stream_sha256"]:
            problems.append("the stream is not the one the manifest names")
        if have_ffmpeg:
            pcm, error = ffmpeg_pcm(path, "truehd")
            if pcm is None:
                problems.append(f"ffmpeg strict decode failed: {error}")
            elif sha256(pcm) != fixture["pcm_sha256"]:
                problems.append("ffmpeg's strict decode is not the manifest's")
        if args.forge:
            pcm, error = run_forge(args.forge, path, scratch, ["truehd-check=lenient"])
            if pcm is None:
                problems.append(f"forge lenient decode failed: {error}")
            elif sha256(pcm) != fixture["pcm_sha256"]:
                problems.append("forge lenient decode differs from the manifest")
            pcm, error = run_forge(args.forge, path, scratch)
            if pcm is not None:
                problems.append("forge's strict decode did not stop at the join")
            elif "lossless check" not in error:
                problems.append(f"forge's strict decode stopped for another reason: {error}")
        failures += bool(problems)
        status = "FAIL" if problems else "ok  "
        print(f"{status} {fixture['name']} (spliced)" + "".join(f"\n       {p}" for p in problems))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=pathlib.Path, default=None)
    parser.add_argument(
        "--handbuilt",
        action="store_true",
        help="the hand-built set (testdata/truehd/handbuilt): FFmpeg held to the outcome its "
        "manifest names for each stream, and --forge / --reference to the signals",
    )
    parser.add_argument(
        "--decoder",
        action="append",
        default=[],
        metavar="'PROGRAM [ARGS]'",
        help="a decoder to hold to the manifest: run as PROGRAM [ARGS] STREAM OUT.s32",
    )
    parser.add_argument("--forge", metavar="PATH", help="the forge executable: `forge decode`")
    parser.add_argument("--reference", action="store_true", help="run the Python reference too")
    parser.add_argument("--require-ffmpeg", action="store_true")
    args = parser.parse_args()
    if args.manifest is None:
        args.manifest = (HANDBUILT if args.handbuilt else FIXTURES) / "manifest.json"

    have_ffmpeg = shutil.which("ffmpeg") is not None
    if not have_ffmpeg:
        if args.require_ffmpeg:
            sys.exit("verify_truehd_interop: ffmpeg is required and not installed")
        print("notice: ffmpeg is not installed; its leg is skipped", file=sys.stderr)
    if args.handbuilt:
        if args.decoder:
            sys.exit("verify_truehd_interop: --decoder does not apply to --handbuilt")
        with tempfile.TemporaryDirectory() as scratch_name:
            return check_handbuilt(args, have_ffmpeg, pathlib.Path(scratch_name))

    manifest = json.loads(args.manifest.read_text())
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
            if args.forge:
                pcm, error = run_forge(args.forge, path, scratch)
                if pcm is None:
                    problems.append(f"forge decode failed: {error}")
                elif sha256(pcm) != fixture["pcm_sha256"]:
                    problems.append("forge decode differs from the manifest")
            for name, command in decoders:
                pcm, error = run_decoder(command, path, scratch)
                if pcm is None:
                    problems.append(f"{name} failed: {error}")
                elif sha256(pcm) != fixture["pcm_sha256"]:
                    problems.append(f"{name} differs from the manifest")
            failures += bool(problems)
            status = "FAIL" if problems else "ok  "
            print(f"{status} {fixture['name']}" + "".join(f"\n       {p}" for p in problems))
        failures += check_spliced(args.manifest, manifest, args, have_ffmpeg, scratch)
    print(f"{len(manifest['fixtures'])} fixtures, {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
