#!/usr/bin/env python3
"""Holds `forge truehd-atmos` to the decoders that can read what it writes.

The stream has a 7.1 bed in substreams 0 to 2 and 12 object channels in substream 3 (restart sync
0x31EC, which no decoder outside this repository reads: libs/truehd/ERRATA.md entries 61 to 64). So:

  * forge's own decode of the object channels (truehd-presentation=all) must give back the WAV the
    stream was made from, sample for sample, and its decode of the best presentation must be the
    stream's 7.1 bed;
  * tools/references/truehd_syntax.py, an independent transcription, must decode both
    presentations to the same samples;
  * FFmpeg's strict decode, which reads substreams 0 to 2 and leaves substream 3 alone, must give
    the same 7.1 bed, in its own channel order.

The objects are real audio (the DEE gold set's sources). Exit status 1 on any difference. FFmpeg is
skipped with a notice when it is not installed, unless --require-ffmpeg.
"""

import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCES = REPO / "testdata" / "truehd-dee" / "sources"
sys.path.insert(0, str(REPO / "tools" / "references"))
import truehd_syntax  # noqa: E402

STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]
FFMPEG_ORDER = [0, 1, 2, 3, 6, 7, 4, 5]  # ERRATA.md entry 33


def read_wav(path):
    """Interleaved 24 bit samples of a PCM WAV as rows of channels."""
    with wave.open(str(path)) as w:
        channels = w.getnchannels()
        raw = w.readframes(w.getnframes())
    values = [int.from_bytes(raw[i : i + 3], "little", signed=True) for i in range(0, len(raw), 3)]
    return [values[i : i + channels] for i in range(0, len(values), channels)]


def write_wav(path, rows, rate=48000):
    """A 24 bit PCM WAV from rows of channels."""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(rows[0]))
        w.setsampwidth(3)
        w.setframerate(rate)
        w.writeframes(b"".join(v.to_bytes(3, "little", signed=True) for row in rows for v in row))


def object_rows():
    """Twelve channels, the LFE first: the six of fx_music_51, the two of fx_music_20 and four of
    fx_tones_71 from another place in it."""
    tones = read_wav(SOURCES / "fx_tones_71.wav")
    music = read_wav(SOURCES / "fx_music_51.wav")
    stereo = read_wav(SOURCES / "fx_music_20.wav")
    n = min(len(tones), len(music), len(stereo))
    rows = []
    for t in range(n):
        row = list(music[t])
        row += stereo[t]
        row += [tones[(t + 7 * c) % n][c - 8] for c in range(8, 12)]
        rows.append(row)
    return rows


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{' '.join(map(str, cmd))}: exit {result.returncode}\n{result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--forge", required=True, type=pathlib.Path)
    parser.add_argument("--require-ffmpeg", action="store_true")
    args = parser.parse_args()
    have_ffmpeg = shutil.which("ffmpeg") is not None
    if not have_ffmpeg:
        if args.require_ffmpeg:
            sys.exit("verify_truehd_atmos_interop: ffmpeg is required and not installed")
        print("notice: ffmpeg is not installed; its leg is skipped")

    failures = []
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(scratch_name)
        rows = object_rows()
        source = scratch / "objects.wav"
        write_wav(source, rows)
        stream = scratch / "atmos.thd"
        run([args.forge, "truehd-atmos", source, stream, "verify"])
        data = stream.read_bytes()

        all_wav = scratch / "all.wav"
        best_wav = scratch / "best.wav"
        run([args.forge, "decode", stream, all_wav, "truehd-presentation=all"])
        run([args.forge, "decode", stream, best_wav])
        forge_all = read_wav(all_wav)
        forge_best = read_wav(best_wav)
        if forge_all != rows:
            failures.append("forge's decode of the object channels is not the WAV it was made from")
        if len(forge_best) != len(rows) or len(forge_best[0]) != 8:
            failures.append("forge's best presentation is not an 8 channel bed")

        reference_best, _rate, _channels = truehd_syntax.decode_stream(data, "best")
        reference_all, _rate, _channels = truehd_syntax.decode_stream(data, "all")
        if [list(r) for r in reference_all] != rows:
            failures.append(
                "the reference's object channels are not the WAV the stream was made from"
            )
        if [list(r) for r in reference_best] != forge_best:
            failures.append("the reference's bed is not forge's")

        if have_ffmpeg:
            raw = scratch / "ffmpeg.s32"
            result = subprocess.run(
                ["ffmpeg", *STRICT, "-i", str(stream), "-f", "s32le", str(raw)],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                failures.append(f"ffmpeg's strict decode failed: {result.stderr.strip()}")
            else:
                words = struct.unpack(f"<{raw.stat().st_size // 4}i", raw.read_bytes())
                ffmpeg_rows = [words[i : i + 8] for i in range(0, len(words), 8)]
                in_order = [
                    [row[FFMPEG_ORDER.index(c)] >> 8 for c in range(8)] for row in ffmpeg_rows
                ]
                if in_order != forge_best:
                    failures.append("ffmpeg's decode is not the bed forge decodes")
        digest = hashlib.sha256(data).hexdigest()[:12]
    for failure in failures:
        print(f"FAIL {failure}")
    if not failures:
        print(
            f"ok   forge truehd-atmos: {len(rows)} samples of a 7.1 bed and 12 objects ({digest})"
        )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
