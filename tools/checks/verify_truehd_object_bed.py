#!/usr/bin/env python3
"""Holds the object substream of the encoder (ObjectLayout::kBedMatrices) to the Python reference.

The encoder writes substream 3 the way the owner's streams have it: restart sync 0x31EC, `min_chan`
8, and primitive matrices that rebuild every output channel from the 7.1 bed rows of substreams 0
to 2 and the channels substream 3 codes (libs/truehd/ERRATA.md entry 70). No decoder outside this
repository reads that sync, so the independent reading is tools/references/truehd_syntax.py, which
shares no code with libs/truehd. For each configuration (12, 14 and 16 objects; matrix noise shifts;
the LSB bypass) the stream the encoder tool writes must

  * decode, by the reference, to the objects it was made from, sample for sample, with the lossless
    check of every restart interval passing (the reference refuses a stream whose check fails);
  * give a 7.1 bed that, without noise, is the render ERRATA.md entry 70 defines, computed here from
    the definition and not from the encoder.

Exit status 1 on any difference. The WAV is real audio (the DEE gold set's sources).
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import wave

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCES = REPO / "testdata" / "truehd-dee" / "sources"
sys.path.insert(0, str(REPO / "tools" / "references"))
import truehd_syntax  # noqa: E402

# Object number, from 8 up, to the bed row (L R C LFE Ls Rs Lrs Rrs) it is mixed into
BED_ROW_OF_OBJECT = (0, 1, 2, 4, 5, 6, 7, 3)
MIX_RAW = -2896  # -1/sqrt(2) at frac_bits 12, the gain the encoder starts from

# (objects, noise shift, bypass)
CONFIGURATIONS = ((12, 0, 0), (12, 1, 0), (12, 3, 1), (14, 1, 1), (16, 2, 0))


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


def object_rows(objects):
    """`objects` channels of real audio, halved so that the bed made from them has room: the six of
    fx_music_51, the two of fx_music_20, and then the channels of fx_tones_71 from other places in
    it."""
    tones = read_wav(SOURCES / "fx_tones_71.wav")
    music = read_wav(SOURCES / "fx_music_51.wav")
    stereo = read_wav(SOURCES / "fx_music_20.wav")
    n = min(len(tones), len(music), len(stereo))
    rows = []
    for t in range(n):
        row = list(music[t]) + stereo[t]
        row += [tones[(t + 7 * c) % n][c - 8] for c in range(8, objects)]
        rows.append([v >> 1 for v in row])
    return rows


def expected_bed(rows):
    """The 7.1 bed the fixed render gives, from the definition: the first eight objects, and each
    object from 8 up taken into its bed row by the matrix whose decoder arithmetic is
    row + floor(raw * object / 2^12); the coded row is the object channel less that."""
    bed = []
    for row in rows:
        out = list(row[:8])
        for k, value in enumerate(row[8:]):
            out[BED_ROW_OF_OBJECT[k]] -= (MIX_RAW * value) >> 12
        bed.append(out)
    return bed


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{' '.join(map(str, cmd))}: exit {result.returncode}\n{result.stderr}")
    return result


def check_configuration(encoder, scratch, objects, noise, bypass):
    """The failures of one configuration, as sentences."""
    label = f"{objects} objects, noise shift {noise}, bypass {bypass}"
    rows = object_rows(objects)
    source = scratch / f"objects{objects}.wav"
    write_wav(source, rows)
    stream = scratch / f"bed{objects}_{noise}_{bypass}.thd"
    run(
        [
            encoder,
            source,
            stream,
            "object-bed=1",
            f"object-noise={noise}",
            f"object-bypass={bypass}",
            "restart=8",
            "verify=1",
        ]
    )
    data = stream.read_bytes()
    failures = []
    try:
        all_rows, _rate, channels = truehd_syntax.decode_stream(data, "all")
        best_rows, _rate, _channels = truehd_syntax.decode_stream(data, "best")
    except truehd_syntax.StreamError as error:
        return [f"{label}: the reference refuses the stream: {error}"]
    if channels != objects or [list(r) for r in all_rows] != rows:
        failures.append(f"{label}: the reference's object channels are not the objects")
    if len(best_rows) != len(rows) or len(best_rows[0]) != 8:
        failures.append(f"{label}: the reference's best presentation is not an 8 channel bed")
    elif noise == 0 and [list(r) for r in best_rows] != expected_bed(rows):
        failures.append(f"{label}: the reference's bed is not the render of the definition")
    elif noise != 0 and [list(r) for r in best_rows] == expected_bed(rows):
        failures.append(f"{label}: the matrices' noise is not in the bed")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--encoder", required=True, type=pathlib.Path)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for objects, noise, bypass in CONFIGURATIONS:
            failures += check_configuration(args.encoder, scratch, objects, noise, bypass)
    for failure in failures:
        print(f"FAIL {failure}")
    if not failures:
        print(f"ok   {len(CONFIGURATIONS)} object substreams decode to their objects")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
