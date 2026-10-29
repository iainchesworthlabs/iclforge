#!/usr/bin/env python3
"""Scores a TrueHD decoder against the Dolby Encoding Engine's TrueHD streams of known sources.

Run on the owner's machine, while the gold set exists: the set is not committed (DEE's licence
ends on 2026-11-06 and its streams stay with the owner; planning/truehd.md, "Oracles").
tools/generators/gen_dee_gold.py makes it, and this reads its manifest:

    DIR/dee-gold-manifest.json         the legs, each with its sources and its stream
    DIR/streams/<leg>/dee.mlp          the stream DEE wrote (TrueHD, whatever the extension)
    DIR/sources/<name>.wav             the PCM it was made from, 24 or 16 bit

For every TrueHD leg the stream is decoded and compared with the first source, sample for sample:
the codec is lossless, so the decode is the source, with the stream's channels in the stream's
order (each source channel is matched to the decoded channel that equals it) and, at most, a delay
of the first samples. The streams are test inputs only (the user's decision of 2026-09-26): they
are decoded and compared with the PCM that went in, and nothing here reads one to learn how the
encoder made it.

  PASS   the decoder succeeded and the decode is the source, bit for bit
  ALTER  the decoder succeeded, and the leg's DEE options alter the audio (loudness correction,
         the surround attenuation, --optimize-data-rate), so the decode is not expected to be
         the source; the figures say how far it is
  FAIL   the decoder failed (strict checks, a CRC, the lossless check), or a lossless leg
         decodes to something else
  SKIP   no stream or source on disk, or a source longer than --max-seconds

    score_truehd_decode.py --set DIR --forge PATH/TO/forge [--only NAME_PART ...] [--report FILE]
    score_truehd_decode.py --set DIR --decoder 'PROGRAM [ARGS]'   # PROGRAM [ARGS] STREAM OUT.wav

With no set, or one with no TrueHD legs, it says so and exits 0, so that a CI job without the set
passes. Exit status 1 if any leg FAILs.
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

MANIFEST = "dee-gold-manifest.json"
PROBE_SAMPLES = 1000
MAX_DELAY = 8192


def read_wav(path):
    """(rate, samples) of a PCM WAV, plain or WAVE_FORMAT_EXTENSIBLE, as a (frames, channels) int64
    array scaled to 24 bits. The standard library's wave module refuses the extensible form, which
    is what `forge decode` writes."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, payload = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos : pos + 4], int.from_bytes(data[pos + 4 : pos + 8], "little")
        body = pos + 8
        if tag == b"fmt ":
            fmt = data[body : body + size]
        elif tag == b"data":
            payload = data[body : body + size if size != 0xFFFFFFFF else len(data)]
            break
        pos = body + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError(f"{path}: no fmt or data chunk")
    tag = int.from_bytes(fmt[0:2], "little")
    channels = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = int.from_bytes(fmt[24:26], "little")
    if tag != 1 or bits not in (16, 24, 32) or channels == 0:
        raise ValueError(
            f"{path}: only integer PCM at 16, 24 or 32 bits is read (tag {tag}, {bits} bits)"
        )
    width = bits // 8
    frames = len(payload) // (width * channels)
    raw = np.frombuffer(payload[: frames * width * channels], dtype=np.uint8).reshape(-1, width)
    value = np.zeros(raw.shape[0], dtype=np.int64)
    for byte in range(width):
        value |= raw[:, byte].astype(np.int64) << (8 * byte)
    value = np.where(value >= 1 << (bits - 1), value - (1 << bits), value)
    scale = {16: 8, 24: 0, 32: -8}[bits]
    value = value << scale if scale >= 0 else value >> -scale
    return rate, value.reshape(frames, channels)


def compare(decoded, source):
    """How `decoded` relates to `source`: each source channel is matched to the first unused decoded
    channel equal to it over its first samples, at a delay of up to MAX_DELAY samples; then every
    sample is compared."""
    out = {
        "decoded_channels": int(decoded.shape[1]),
        "decoded_samples": len(decoded),
        "source_channels": int(source.shape[1]),
        "source_samples": len(source),
    }
    probe = min(PROBE_SAMPLES, len(source))
    for offset in range(MAX_DELAY):
        length = min(len(decoded) - offset, len(source))
        if length <= probe:
            break
        mapping, used = [], set()
        for c in range(source.shape[1]):
            hit = [
                d
                for d in range(decoded.shape[1])
                if d not in used
                and np.array_equal(decoded[offset : offset + probe, d], source[:probe, c])
            ]
            mapping.append(hit[0] if hit else None)
            used.update(hit[:1])
        if all(m is not None for m in mapping):
            diff = decoded[offset : offset + length][:, mapping] - source[:length]
            wrong = int(np.count_nonzero(diff))
            out.update(
                {
                    "lossless": wrong == 0 and length == len(source),
                    "delay": offset,
                    "channel_map": mapping,
                    "mismatched_values": wrong,
                    "compared_samples": int(length),
                }
            )
            if wrong:
                out["max_abs_error_24bit"] = int(np.max(np.abs(diff)))
            return out
    # No delay lines the channels up exactly: each source channel against its best correlated
    # decoded channel, for the figures.
    length = min(len(decoded), len(source))
    d = decoded[:length].astype(np.float64)
    norms = np.sqrt(np.sum(d * d, axis=0)) + 1e-30
    mapping, errors = [], []
    for c in range(source.shape[1]):
        r = source[:length, c].astype(np.float64)
        best = int(np.argmax(np.abs(r @ d) / norms))
        mapping.append(best)
        err = d[:, best] - r
        errors.append(
            round(10.0 * np.log10(max(float(err @ err), 1e-30) / (float(r @ r) or 1.0)), 1)
        )
    out.update({"lossless": False, "channel_map": mapping, "error_db_per_channel": errors})
    return out


def decode(command, stream, out):
    result = subprocess.run(
        [*command, str(stream), str(out)], capture_output=True, check=False, timeout=3600
    )
    text = (result.stderr + result.stdout).decode("utf-8", "replace").strip().splitlines()
    return result.returncode, (text[-1] if text else "")


def score_leg(name, entry, root, command, scratch, max_seconds, manifest):
    stream = root / "streams" / name / entry.get("stream", "dee.mlp")
    sources = entry.get("sources") or []
    source = root / "sources" / sources[0] if sources else None
    if not stream.is_file() or source is None or not source.is_file():
        return {"status": "SKIP", "why": "no stream or no source on disk"}
    seconds = manifest.get("sources", {}).get(source.stem, {}).get("seconds", 0)
    if max_seconds and seconds > max_seconds:
        return {"status": "SKIP", "why": f"the source is {seconds:g} s, over --max-seconds"}
    wav = scratch / f"{name}.wav"
    code, message = decode(command, stream, wav)
    if code != 0 or not wav.is_file():
        return {"status": "FAIL", "why": f"the decoder exited {code}: {message[:300]}"}
    try:
        _, decoded = read_wav(wav)
        _, reference = read_wav(source)
    except ValueError as error:
        return {"status": "FAIL", "why": str(error)}
    finally:
        wav.unlink(missing_ok=True)
    result = compare(decoded, reference)
    result["ffmpeg_lossless"] = (entry.get("ffmpeg") or {}).get("lossless")
    altered = entry.get("audio_altered")
    if altered:
        result.update({"status": "ALTER", "why": altered})
    elif result["lossless"]:
        result["status"] = "PASS"
    else:
        result["status"] = "FAIL"
        result["why"] = "the decode is not the source"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--set", type=pathlib.Path, help="the gold set's directory")
    parser.add_argument(
        "--forge",
        type=pathlib.Path,
        help="a forge executable: run as `forge decode STREAM OUT.wav`",
    )
    parser.add_argument("--decoder", help="PROGRAM [ARGS], run as PROGRAM [ARGS] STREAM OUT.wav")
    parser.add_argument(
        "--only",
        nargs="+",
        metavar="NAME_PART",
        help="score only the legs whose names contain one of these",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=120.0,
        help="skip legs whose source is longer (the 300 s soak programmes); 0 scores all",
    )
    parser.add_argument("--report", type=pathlib.Path, help="write the results here as JSON")
    args = parser.parse_args()

    manifest_path = args.set / MANIFEST if args.set else None
    if manifest_path is None or not manifest_path.is_file():
        print(
            "score_truehd_decode: no DEE gold set (--set DIR with dee-gold-manifest.json); "
            "nothing to score"
        )
        return 0
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    legs = {
        n: e
        for n, e in manifest.get("legs", {}).items()
        if e.get("codec") == "truehd"
        and "refused" not in e
        and "failed" not in e
        and (not args.only or any(part in n for part in args.only))
    }
    if not legs:
        print("score_truehd_decode: the set has no TrueHD legs to score")
        return 0
    if bool(args.forge) == bool(args.decoder):
        sys.exit(
            "score_truehd_decode: give exactly one of --forge and --decoder "
            "when there is something to score"
        )
    if np is None:
        sys.exit("score_truehd_decode: numpy is required")
    command = [str(args.forge), "decode"] if args.forge else args.decoder.split()

    results, counts = {}, {"PASS": 0, "ALTER": 0, "FAIL": 0, "SKIP": 0}
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for name in sorted(legs):
            result = score_leg(
                name, legs[name], args.set, command, scratch, args.max_seconds, manifest
            )
            results[name] = result
            counts[result["status"]] += 1
            detail = result.get("why", "")
            if result["status"] == "ALTER" and "mismatched_values" in result:
                detail += f" ({result['mismatched_values']} values differ)"
            if result["status"] == "FAIL" and result.get("ffmpeg_lossless") is False:
                detail += " (FFmpeg's decode is not the source either)"
            print(f"{result['status']:5} {name}" + (f": {detail}" if detail else ""), flush=True)
    print(
        f"{len(results)} legs: {counts['PASS']} pass, {counts['ALTER']} altered by DEE's options, "
        f"{counts['FAIL']} fail, {counts['SKIP']} skipped"
    )
    if args.report:
        args.report.write_text(
            json.dumps({"counts": counts, "legs": results}, indent=2) + "\n", encoding="utf-8"
        )
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
