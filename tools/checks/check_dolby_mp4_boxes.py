#!/usr/bin/env python3
"""Dolby's MP4 inspector against forge's sample entries, in both directions.

`mp4drop` (Online Delivery Kit) is Dolby's own parser of the boxes that describe a Dolby audio
track: `dac3` for AC-3, `dec3` for E-AC-3 (with the JOC extension flag and complexity index) and
`dac4` for AC-4. Two checks:

1. forge writes MP4 and the init segment of its fragmented MP4 for AC-3, E-AC-3, E-AC-3 JOC and AC-4;
   mp4drop's reading of the configuration box must equal what forge's probe says about the
   elementary stream the box was made from. That catches a bit-layout error in a box that forge's
   own reader, written by the same hand, would read back without complaint.
2. Dolby's own sample files (--samples): forge's reading of the configuration box (`forge probe`)
   must equal mp4drop's. That tests forge's reader on boxes it did not write. The same files give
   notes, not failures, where forge's writer chooses differently from Dolby's muxer (for example the
   channelcount of a 5.1 JOC entry).

    python3 tools/checks/check_dolby_mp4_boxes.py --cli build/bin/forge [--samples DIR]

Needs mp4drop (tools/checks/dolby_odk.py); exits 77 without it. Exit status: 0 when nothing
disagreed, 1 otherwise. Nothing from Dolby's kit is committed.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

import dolby_odk as odk
import dolby_odk_corpus as corpus

# The nominal AC-3 data rates of the frame size code table (ETSI TS 102 366); the AC3SpecificBox's
# bit_rate_code (Annex F) indexes them, which the agreement with mp4drop below confirms.
AC3_RATES_KBPS = (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640)
FSCOD_OF_RATE = {48000: 0, 44100: 1, 32000: 2}
# fs_index of the AC-4 specific box (ETSI TS 103 190-2 Annex E): 0 is 44.1 kHz and 1 is 48 kHz.
AC4_FS_INDEX_OF_RATE = {44100: 0, 48000: 1}


def forge_probe(cli: str, path: Path) -> dict | None:
    done = corpus.run([cli, "probe", path, "json=1"])
    if done.returncode != 0:
        return None
    try:
        return json.loads(done.stdout)
    except json.JSONDecodeError:
        return None


def expected_config(kind: str, probe: dict) -> dict[str, int]:
    """The configuration-box fields a stream implies, from forge's probe of the elementary stream."""
    stream = probe["stream"]
    if kind == "ac4":
        info = stream["ac4"]
        return {
            "bitstream_version": info["bitstream_version"],
            "fs_index": AC4_FS_INDEX_OF_RATE[info["sample_rate_hz"]],
            "frame_rate_index": info["frame_rate_index"],
            "n_presentations": info["n_presentations"],
        }
    fields = {
        "fscod": FSCOD_OF_RATE[stream["sample_rate_hz"]],
        "bsid": stream["bsid"],
        "bsmod": stream["bsmod"],
        "acmod": stream["acmod"],
        "lfeon": int(bool(stream["lfeon"])),
    }
    rate = round(stream.get("nominal_bitrate_kbps") or stream["bitrate_kbps"])
    if kind == "ac3":
        fields["bit_rate_code"] = AC3_RATES_KBPS.index(rate)
        return fields
    objects = stream.get("objects") or {}
    fields["data_rate"] = rate
    fields["num_ind_sub"] = stream["substreams_per_access_unit"] - 1
    fields["flag_ec3_extension_type_a"] = int(bool(objects.get("joc")))
    if objects.get("joc"):
        fields["complexity_index_type_a"] = objects["complexity_index"]
    return fields


def config_box(entry: odk.Box) -> odk.Box | None:
    for child in entry.children:
        if child.kind in ("dac3", "dec3", "dac4"):
            return child
    return None


def compare_config(subject: str, expected: dict[str, int], box: odk.Box) -> list[odk.Result]:
    wrong = []
    for name, want in expected.items():
        got = box.int_value(name)
        if got != want:
            wrong.append(f"{name}: mp4drop {got}, stream {want}")
    if wrong:
        return [odk.Result(subject, odk.DISAGREE, "; ".join(wrong))]
    return [odk.Result(subject, odk.PASS)]


def check_forge_written(cli: str, work: Path) -> tuple[list[odk.Result], dict[str, int]]:
    results: list[odk.Result] = []
    channelcounts: dict[str, int] = {}
    streams, notes = corpus.make_streams(cli, work / "streams")
    results += [odk.Result("corpus", odk.ERROR, note) for note in notes]
    for stream in streams:
        probe = forge_probe(cli, stream.path)
        if probe is None:
            results.append(odk.Result(stream.name, odk.ERROR, "forge probe did not read its own stream"))
            continue
        files = corpus.make_containers(cli, stream, work / stream.name)
        for leg in ("mp4", "fmp4_init"):
            subject = f"{stream.name} {leg}"
            if leg not in files:
                results.append(odk.Result(subject, odk.ERROR, "forge did not write this file"))
                continue
            try:
                boxes = odk.mp4drop_boxes(files[leg])
            except (RuntimeError, ValueError) as error:
                results.append(odk.Result(subject, odk.ERROR, str(error)))
                continue
            entries = odk.audio_sample_entries(boxes)
            if len(entries) != 1 or entries[0][1].kind != stream.entry:
                found = [e.kind for _, e in entries]
                results.append(odk.Result(subject, odk.DISAGREE, f"expected one {stream.entry} entry, found {found}"))
                continue
            entry = entries[0][1]
            box = config_box(entry)
            if box is None:
                results.append(odk.Result(subject, odk.DISAGREE, "no dac3/dec3/dac4 box in the sample entry"))
                continue
            results += compare_config(subject, expected_config(stream.name, probe), box)
            if leg == "mp4":
                channelcounts[stream.name] = entry.int_value("channelcount") or 0
    return results, channelcounts


def probe_codec_box(probe: dict) -> dict:
    return (probe.get("container") or {}).get("mp4", {}).get("codec_box") or {}


def check_dolby_written(cli: str, samples: Path, forge_counts: dict[str, int]) -> list[odk.Result]:
    results: list[odk.Result] = []
    dolby_counts: dict[tuple[str, bool], int] = {}
    for sample in sorted(samples.glob("*.mp4")):
        try:
            boxes = odk.mp4drop_boxes(sample)
        except (RuntimeError, ValueError) as error:
            results.append(odk.Result(sample.name, odk.ERROR, str(error)))
            continue
        probe = forge_probe(cli, sample)
        for _, entry in odk.audio_sample_entries(boxes):
            box = config_box(entry)
            if box is None or probe is None or box.kind != "dec3":
                continue
            theirs = probe_codec_box(probe)
            # ETSI TS 102 366 Annex F (EC3SpecificBox): num_ind_sub is the number of independent
            # substreams minus one, and forge's probe reports the count.
            mapping = {
                "fscod": "fscod", "bsid": "bsid", "bsmod": "bsmod", "acmod": "acmod",
                "data_rate": "data_rate_kbps", "num_dep_sub": "num_dep_sub",
            }
            wrong = []
            for dolby_name, forge_name in mapping.items():
                if box.int_value(dolby_name) != theirs.get(forge_name):
                    wrong.append(f"{dolby_name}: mp4drop {box.int_value(dolby_name)}, forge {theirs.get(forge_name)}")
            if (box.int_value("num_ind_sub") or 0) + 1 != theirs.get("independent_substreams"):
                wrong.append(
                    f"num_ind_sub: mp4drop {box.int_value('num_ind_sub')} (+1), "
                    f"forge {theirs.get('independent_substreams')} independent substreams"
                )
            if box.int_value("lfeon") != int(bool(theirs.get("lfeon"))):
                wrong.append(f"lfeon: mp4drop {box.int_value('lfeon')}, forge {theirs.get('lfeon')}")
            joc = bool(box.int_value("flag_ec3_extension_type_a"))
            if joc != (theirs.get("complexity_index") is not None):
                wrong.append(f"JOC flag: mp4drop {joc}, forge complexity {theirs.get('complexity_index')}")
            elif joc and box.int_value("complexity_index_type_a") != theirs.get("complexity_index"):
                wrong.append(
                    f"complexity: mp4drop {box.int_value('complexity_index_type_a')}, forge {theirs.get('complexity_index')}"
                )
            if wrong:
                results.append(odk.Result(sample.name, odk.DISAGREE, "; ".join(wrong)))
            else:
                results.append(odk.Result(sample.name, odk.PASS))
            dolby_counts[("ec-3", joc)] = entry.int_value("channelcount") or 0
    joc_count = dolby_counts.get(("ec-3", True))
    if joc_count is not None and forge_counts.get("joc") not in (None, joc_count):
        results.append(
            odk.Result(
                "channelcount of a JOC entry",
                odk.NOTE,
                f"forge writes {forge_counts['joc']}, Dolby's sample writes {joc_count} "
                "(single oracle: a Dolby sample and Dolby's muxer; owner to decide)",
            )
        )
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cli", default=corpus.default_cli(), help="forge binary (default $ICLFORGE_CLI)")
    parser.add_argument("--samples", default=os.environ.get("ICLFORGE_DOLBY_ODK_SAMPLES"))
    parser.add_argument("--work", help="working directory (default: a temporary one)")
    args = parser.parse_args(argv)
    reason = odk.skip_reason("mp4drop")
    if reason:
        print(f"skip: {reason}")
        return odk.SKIP_EXIT
    if not args.cli:
        parser.error("--cli or ICLFORGE_CLI is required")
    with tempfile.TemporaryDirectory(prefix="dolby-boxes-") as tmp:
        work = Path(args.work or tmp)
        results, counts = check_forge_written(args.cli, work)
        if args.samples:
            results += check_dolby_written(args.cli, Path(args.samples), counts)
        else:
            results.append(odk.Result("Dolby sample files", odk.SKIP, "no --samples directory"))
        return odk.print_report("Dolby mp4drop against forge's sample entries", results)


if __name__ == "__main__":
    sys.exit(main())
