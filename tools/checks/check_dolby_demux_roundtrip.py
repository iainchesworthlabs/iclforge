#!/usr/bin/env python3
"""Dolby's stream demultiplexer against forge's containers, in both directions.

forge writes MP4, a fragmented-MP4 set with its DASH manifest and HLS playlists, and MPEG-2 TS for
AC-3, E-AC-3, E-AC-3 JOC and AC-4. Dolby's `streamdemuxer` (Online Delivery Kit) reads each, and the
elementary stream it writes must equal the stream forge started from. In the other direction it
reads Dolby's own sample files and forge's demultiplexer must produce the same elementary stream.
AC-4 streams are compared frame by frame: two writers frame the same raw frames differently (sync
word, length form, CRC) and both are right.

streamdemuxer lists Dolby Digital Plus, AC-4 and Dolby Vision as its codecs, so an AC-3 stream in a
transport stream gives no output; that leg is reported as a skip, not a failure.

    python3 tools/checks/check_dolby_demux_roundtrip.py --cli build/bin/forge [--samples DIR]

Needs the tool (tools/checks/dolby_odk.py explains how to get it and where it must be); exits 77
without it. --samples (or ICLFORGE_DOLBY_ODK_SAMPLES) is a directory of Dolby's own .mp4 test
signals, for the reverse direction. Nothing here is committed or fetched by CI. Exit status: 0
when every leg passed or was skipped, 1 when one disagreed or failed, 77 when the tool is absent.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

import dolby_odk as odk
import dolby_odk_corpus as corpus

CONTAINER_LEGS = ("mp4", "dash", "hls_media", "hls_master", "ts_dvb", "ts_atsc")


def dolby_elementary(result: odk.ToolResult) -> list[Path]:
    """The audio elementary streams the tool wrote (it also writes video streams)."""
    return [p for p in result.files if p.suffix in corpus.ES_SUFFIXES]


def forge_demux(cli: str, container: Path, out: Path) -> bytes | None:
    done = corpus.run([cli, "demux", container, out])
    return out.read_bytes() if done.returncode == 0 and out.exists() else None


def tool_failure(result: odk.ToolResult) -> str:
    tail = (result.text + result.err_text).strip().splitlines()
    if result.timed_out:
        return "timed out"
    if result.returncode < 0:
        return f"the tool crashed (signal {-result.returncode})"
    return f"exit {result.returncode}: {tail[-1][:80] if tail else 'no message'}"


def check_forge_written(cli: str, work: Path, root: str | None) -> list[odk.Result]:
    results: list[odk.Result] = []
    streams, notes = corpus.make_streams(cli, work / "streams")
    results += [odk.Result("corpus", odk.ERROR, note) for note in notes]
    for stream in streams:
        containers = corpus.make_containers(cli, stream, work / stream.name)
        expected = stream.path.read_bytes()
        for leg in CONTAINER_LEGS:
            subject = f"{stream.name} in {leg}"
            if leg not in containers:
                if not (stream.name == "ac4" and leg == "ts_atsc"):
                    results.append(odk.Result(subject, odk.ERROR, "forge did not write this container"))
                continue
            done = odk.streamdemuxer(containers[leg], work / "dolby" / f"{stream.name}_{leg}", root=root)
            files = dolby_elementary(done)
            if not files:
                if stream.name == "ac3" and leg.startswith("ts_") and done.returncode == 0:
                    results.append(odk.Result(subject, odk.SKIP, "streamdemuxer does not list AC-3"))
                else:
                    results.append(odk.Result(subject, odk.ERROR, tool_failure(done) + "; no stream written"))
                continue
            got = files[0].read_bytes()
            if corpus.same_stream(stream.name, got, expected):
                results.append(odk.Result(subject, odk.PASS))
            else:
                results.append(
                    odk.Result(subject, odk.DISAGREE, f"{len(got)} bytes from the tool, {len(expected)} from forge")
                )
    return results


def check_dolby_written(cli: str, samples: Path, work: Path, root: str | None) -> list[odk.Result]:
    results: list[odk.Result] = []
    for sample in sorted(samples.glob("*.mp4")):
        done = odk.streamdemuxer(sample, work / "dolby_samples" / sample.stem, root=root)
        files = dolby_elementary(done)
        if not files:
            results.append(odk.Result(sample.name, odk.SKIP, "the tool wrote no audio stream: " + tool_failure(done)))
            continue
        suffix = files[0].suffix
        mine = forge_demux(cli, sample, work / f"forge_{sample.stem}{suffix}")
        kind = "ac4" if suffix == ".ac4" else ("ac3" if suffix == ".ac3" else "eac3")
        if mine is None:
            results.append(odk.Result(sample.name, odk.DISAGREE, "forge could not demux a file Dolby's tool demuxes"))
        elif corpus.same_stream(kind, mine, files[0].read_bytes()):
            results.append(odk.Result(sample.name, odk.PASS))
        else:
            results.append(odk.Result(sample.name, odk.DISAGREE, "the two demultiplexers wrote different audio"))
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cli", default=corpus.default_cli(), help="forge binary (default $ICLFORGE_CLI)")
    parser.add_argument("--samples", default=os.environ.get("ICLFORGE_DOLBY_ODK_SAMPLES"))
    parser.add_argument("--work", help="working directory (default: a temporary one)")
    args = parser.parse_args(argv)
    reason = odk.skip_reason("streamdemuxer")
    if reason:
        print(f"skip: {reason}")
        return odk.SKIP_EXIT
    if not args.cli:
        parser.error("--cli or ICLFORGE_CLI is required")
    with tempfile.TemporaryDirectory(prefix="dolby-demux-") as tmp:
        work = Path(args.work or tmp)
        results = check_forge_written(args.cli, work, None)
        if args.samples:
            results += check_dolby_written(args.cli, Path(args.samples), work, None)
        else:
            results.append(odk.Result("Dolby sample files", odk.SKIP, "no --samples directory"))
        return odk.print_report("Dolby streamdemuxer against forge's containers", results)


if __name__ == "__main__":
    sys.exit(main())
