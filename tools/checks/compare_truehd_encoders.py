#!/usr/bin/env python3
"""Compares forge's TrueHD encoder with FFmpeg's and the Dolby Encoding Engine's, by stream size.

For every source of the corpus:

  * `forge truehd-encode` writes a stream (with its own verification on), `forge decode` reads it
    back and FFmpeg's strict decode reads it back too: both must give the source sample for sample
    (the codec is lossless, so this is a comparison of bytes);
  * FFmpeg's TrueHD encoder (`-c:a truehd -strict -2`) writes a stream of the same source, where it
    writes that layout at all;
  * where a gold set is given (--set), the stream the Dolby Encoding Engine wrote of the same source
    is read from it. Its bytes are counted and `forge probe` prints its summary fields; nothing else
    about it is read (README rule 4 of planning/work-packages: a Dolby stream is a test input).

The corpus is the legs of the gold set's manifest (the 13 committed in testdata/truehd-dee are the
default set, and the part CI can run: DEE's licence ends on 2026-11-06 and the full set stays on the
owner's machine) and programme material cut from testdata/audio at 2.0, 5.1 and 7.1, 44.1 to 192 kHz
and 16 and 24 bits (--programme), and any WAV files in the directories given with --corpus.

    compare_truehd_encoders.py --forge PATH/TO/forge [--set DIR] [--programme] [--corpus DIR ...]
                               [--only NAME_PART ...] [--json FILE] [--markdown FILE]

The report is a table of bytes and ratios (forge over FFmpeg, forge over DEE), the totals over the
rows that have both sides, and the tool versions. Exit status 1 if a forge stream is not bit-exact
in either decoder, 2 if --max-ratio-ffmpeg or --max-ratio-dee is exceeded by a row that has the
other side. Without ffmpeg only forge's own decode is checked.
"""

import argparse
import json
import pathlib
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
from score_truehd_decode import compare, read_wav  # noqa: E402

REPO = HERE.parents[1]
DEFAULT_SET = REPO / "testdata" / "truehd-dee"
MANIFEST = "dee-gold-manifest.json"
PROBE_FIELDS = (
    "codec",
    "sample_rate",
    "substreams",
    "channels",
    "access_units",
    "variable_rate",
    "peak_data_rate",
    "major_syncs",
)
# What each programme row is cut from, then (name, source, rate, channels, bits, seconds).
PROGRAMME = [
    ("music-20-48-24", ("flac", "programme_music_stereo.flac"), 48000, 2, 24, 10.0),
    ("music-20-48-16", ("flac", "programme_music_stereo.flac"), 48000, 2, 16, 10.0),
    ("music-20-44-24", ("flac", "programme_music_stereo.flac"), 44100, 2, 24, 10.0),
    ("music-20-96-24", ("flac", "programme_music_stereo.flac"), 96000, 2, 24, 10.0),
    ("music-20-192-24", ("flac", "programme_music_stereo.flac"), 192000, 2, 24, 5.0),
    ("speech-10-48-24", ("flac", "programme_speech_stereo.flac"), 48000, 1, 24, 10.0),
    ("speech-20-48-24", ("flac", "programme_speech_stereo.flac"), 48000, 2, 24, 10.0),
    ("ref-51-48-24", ("ref51",), 48000, 6, 24, 5.0),
    ("ref-51-48-16", ("ref51",), 48000, 6, 16, 5.0),
    ("ref-51-96-24", ("ref51",), 96000, 6, 24, 5.0),
    ("ref-51-192-24", ("ref51",), 192000, 6, 24, 2.5),
    ("ref-71-48-24", ("ref51",), 48000, 8, 24, 5.0),
    ("ref-71-96-24", ("ref51",), 96000, 8, 24, 5.0),
]


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, check=False, **kwargs)


def tool_version(command):
    out = run(command)
    text = (out.stdout or out.stderr).decode(errors="replace").strip()
    return text.splitlines()[0] if text else "unknown"


def probe_summary(forge, stream):
    """The summary fields `forge probe json=1` prints for a stream (PROBE_FIELDS), as a dict."""
    out = run([forge, "probe", str(stream), "json=1"])
    try:
        info = json.loads(out.stdout)["truehd"]
    except (ValueError, KeyError):
        return {"error": "forge probe printed no TrueHD summary"}
    return {k: info[k] for k in PROBE_FIELDS if k in info}


def gold_legs(root):
    """The TrueHD legs of a gold set with one source each: (name, wav, stream, options)."""
    manifest_path = root / MANIFEST
    if not manifest_path.exists():
        return []
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    legs = []
    for name, leg in manifest.get("legs", {}).items():
        if leg.get("codec") != "truehd" or len(leg.get("sources", [])) != 1:
            continue
        wav = root / "sources" / leg["sources"][0]
        stream = root / "streams" / name / leg.get("stream", "dee.mlp")
        if wav.exists() and stream.exists():
            legs.append((name, wav, stream, leg.get("options", [])))
    return legs


def dee_note(options):
    """A short label for what a DEE leg asks for beyond the plain stream, from its command line."""
    words = [o for o in options if o.startswith("--") and o not in ("--input-format",)]
    presentations = options.count("--presentation")
    notes = []
    if presentations > 1:
        notes.append(f"{presentations} presentations")
    notes += [w[2:] for w in words if w not in ("--presentation",)]
    return ", ".join(notes)


def wav_facts(path):
    data = path.read_bytes()
    rate, samples = read_wav(path)
    bits = int.from_bytes(data[34:36], "little") if data[12:16] == b"fmt " else 24
    return rate, samples, bits


def forge_encode(forge, wav, stream, extra):
    started = time.perf_counter()
    out = run([forge, "truehd-encode", str(wav), str(stream), "verify", *extra])
    elapsed = time.perf_counter() - started
    return out.returncode, out.stderr.decode(errors="replace").strip(), elapsed


def ffmpeg_encode(wav, out):
    result = run(
        [
            "ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(wav),
            "-c:a", "truehd", "-strict", "-2", "-f", "truehd", str(out),
        ]
    )  # fmt: skip
    return out.stat().st_size if result.returncode == 0 and out.exists() else None


def forge_decode_exact(forge, stream, wav, scratch):
    """True if `forge decode` gives the source, sample for sample, else the reason."""
    decoded = scratch / (stream.stem + ".decoded.wav")
    out = run([forge, "decode", str(stream), str(decoded)])
    if out.returncode != 0:
        return "forge decode failed: " + out.stderr.decode(errors="replace").strip()[:200]
    _rate, source = read_wav(wav)
    _rate, got = read_wav(decoded)
    result = compare(got, source)
    if result.get("lossless") and result.get("delay") == 0:
        if result.get("channel_map") == list(range(source.shape[1])):
            return True
        return f"channels reordered: {result.get('channel_map')}"
    return f"differs from the source: {result}"


def ffmpeg_decode_exact(stream, source_pcm24, channels, bits):
    pcm, error = interop.ffmpeg_decode(stream)
    if pcm is None:
        return "ffmpeg strict decode failed: " + error[:200]
    columns = interop.FFMPEG_ORDER.get(channels)
    ordered = source_pcm24[:, columns] if columns else source_pcm24
    expected = (ordered.astype(np.int64) << 8).astype("<i4").tobytes()
    if pcm == expected:
        return True
    order = interop.find_permutation(expected, pcm, channels)
    return f"channels reordered by ffmpeg: {order}" if order else "differs from the source"


def programme_cases(scratch, seed):
    """The programme rows: (name, wav path, rate, channels, bits)."""
    cases = []
    for name, spec, rate, channels, bits, seconds in PROGRAMME:
        pcm = interop.build_source(spec, rate, channels, bits, seconds, seed)
        wav = scratch / f"{name}.wav"
        if bits == 16:
            interop.write_wav(wav, rate, pcm, 16)
        else:
            interop.write_wav(wav, rate, pcm << (24 - bits), 24)
        cases.append((name, wav))
    return cases


def row_for(args, scratch, name, wav, dee_stream, note):
    rate, samples, bits = wav_facts(wav)
    channels = samples.shape[1]
    stream = scratch / f"{name}.thd"
    code, error, seconds = forge_encode(args.forge, wav, stream, args.forge_arg)
    row = {
        "name": name,
        "rate": rate,
        "channels": channels,
        "bits": bits,
        "seconds": round(len(samples) / rate, 3),
        "note": note,
        "forge_bytes": None,
        "ffmpeg_bytes": None,
        "dee_bytes": None,
        "forge_encode_x_realtime": None,
        "forge_decode_exact": None,
        "ffmpeg_decode_exact": None,
        "dee_probe": None,
    }
    if code != 0:
        row["forge_decode_exact"] = "forge encode failed: " + error[:200]
        return row
    row["forge_bytes"] = stream.stat().st_size
    row["forge_encode_x_realtime"] = round(len(samples) / rate / seconds, 1) if seconds else None
    row["forge_decode_exact"] = forge_decode_exact(args.forge, stream, wav, scratch)
    if shutil.which("ffmpeg"):
        row["ffmpeg_decode_exact"] = ffmpeg_decode_exact(stream, samples, channels, bits)
        row["ffmpeg_bytes"] = ffmpeg_encode(wav, scratch / f"{name}.ffmpeg.thd")
    if dee_stream is not None:
        row["dee_bytes"] = dee_stream.stat().st_size
        row["dee_probe"] = probe_summary(args.forge, dee_stream)
    return row


def ratio(a, b):
    return round(a / b, 3) if a and b else None


def totals(rows, other):
    pairs = [(r["forge_bytes"], r[other]) for r in rows if r["forge_bytes"] and r[other]]
    if not pairs:
        return None
    return {
        "rows": len(pairs),
        "forge_bytes": sum(a for a, _ in pairs),
        f"{other}": sum(b for _, b in pairs),
        "ratio": ratio(sum(a for a, _ in pairs), sum(b for _, b in pairs)),
    }


def markdown(rows, versions, args):
    lines = [
        "| Source | Rate | Ch | Bits | s | DEE | FFmpeg | forge | forge/FFmpeg | forge/DEE "
        "| bit-exact |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]

    def cell(value):
        return f"{value:,}" if isinstance(value, int) else "-"

    for r in rows:
        exact = (
            "yes"
            if r["forge_decode_exact"] is True and r["ffmpeg_decode_exact"] in (True, None)
            else "NO"
        )
        label = r["name"] + (f" ({r['note']})" if r["note"] else "")
        lines.append(
            f"| {label} | {r['rate']} | {r['channels']} | {r['bits']} | {r['seconds']:g} | "
            f"{cell(r['dee_bytes'])} | {cell(r['ffmpeg_bytes'])} | {cell(r['forge_bytes'])} | "
            f"{r['ratio_ffmpeg'] or '-'} | {r['ratio_dee'] or '-'} | {exact} |"
        )
    lines.append("")
    for other, label in (("ffmpeg_bytes", "FFmpeg"), ("dee_bytes", "DEE")):
        t = totals(rows, other)
        if t:
            lines.append(
                f"Over the {t['rows']} rows with {label}'s stream: forge "
                f"{t['forge_bytes']:,} bytes, {label} {t[other]:,}, ratio {t['ratio']}."
            )
    lines.append("")
    lines.append("Tools: " + "; ".join(f"{k} {v}" for k, v in versions.items()))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--forge", required=True, help="the forge executable")
    parser.add_argument(
        "--set",
        type=pathlib.Path,
        default=DEFAULT_SET,
        help=f"a gold set with {MANIFEST} (default: the 13 legs committed in testdata/truehd-dee); "
        "'none' reads no DEE stream",
    )
    parser.add_argument("--programme", action="store_true", help="add the programme rows")
    parser.add_argument(
        "--corpus", type=pathlib.Path, action="append", default=[],
        help="a directory of WAV files to add as rows (no DEE stream)",
    )  # fmt: skip
    parser.add_argument("--only", action="append", default=[], help="rows whose name contains this")
    parser.add_argument(
        "--forge-arg", action="append", default=[], help="extra argument for truehd-encode"
    )
    parser.add_argument("--max-ratio-ffmpeg", type=float, default=0.0)
    parser.add_argument("--max-ratio-dee", type=float, default=0.0)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    parser.add_argument("--keep", type=pathlib.Path, help="keep the WAV files and streams here")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    if np is None:
        sys.exit("compare_truehd_encoders: numpy is required")

    versions = {"forge": tool_version([args.forge, "--version"])}
    if shutil.which("ffmpeg"):
        versions["ffmpeg"] = tool_version(["ffmpeg", "-version"])
    rows = []
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(args.keep or scratch_name)
        scratch.mkdir(parents=True, exist_ok=True)
        plan = []
        if str(args.set) != "none":
            for name, wav, stream, options in gold_legs(args.set):
                plan.append((name, wav, stream, dee_note(options)))
        else:
            args.set = None
        if args.programme:
            for name, wav in programme_cases(scratch, args.seed):
                plan.append((name, wav, None, ""))
        for directory in args.corpus:
            for wav in sorted(directory.glob("*.wav")):
                plan.append((wav.stem, wav, None, ""))
        for name, wav, dee_stream, note in plan:
            if args.only and not any(token in name for token in args.only):
                continue
            row = row_for(args, scratch, name, wav, dee_stream, note)
            row["ratio_ffmpeg"] = ratio(row["forge_bytes"], row["ffmpeg_bytes"])
            row["ratio_dee"] = ratio(row["forge_bytes"], row["dee_bytes"])
            rows.append(row)
            print(
                f"{name}: forge {row['forge_bytes']} ffmpeg {row['ffmpeg_bytes']} "
                f"dee {row['dee_bytes']} exact {row['forge_decode_exact'] is True}/"
                f"{row['ffmpeg_decode_exact']}",
                file=sys.stderr,
            )

    report = {"versions": versions, "rows": rows, "totals": {
        "ffmpeg": totals(rows, "ffmpeg_bytes"), "dee": totals(rows, "dee_bytes")}}  # fmt: skip
    text = markdown(rows, versions, args)
    print(text)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(text + "\n", encoding="utf-8")

    inexact = [
        r["name"]
        for r in rows
        if r["forge_decode_exact"] is not True or r["ffmpeg_decode_exact"] not in (True, None)
    ]
    if inexact:
        print("not bit-exact: " + ", ".join(inexact), file=sys.stderr)
        return 1
    over = [
        r["name"]
        for r in rows
        if (
            args.max_ratio_ffmpeg
            and r["ratio_ffmpeg"]
            and r["ratio_ffmpeg"] > args.max_ratio_ffmpeg
        )
        or (args.max_ratio_dee and r["ratio_dee"] and r["ratio_dee"] > args.max_ratio_dee)
    ]
    if over:
        print("over the size limit: " + ", ".join(over), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
