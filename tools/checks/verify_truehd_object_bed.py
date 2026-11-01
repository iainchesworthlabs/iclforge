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

Moving objects (EncoderConfig::object_paths; the notes in libs/truehd/notes) are held the same way.
For each of MOTION_CONFIGURATIONS (a slow path and a step path; the matrices'
coefficients held between updates, or sliding with the extension's slopes) the stream the encoder
tool writes must decode, by the reference, to the objects, and its bed must be within the
tolerance the configuration allows of the bed the paths ask for, computed here in real numbers. And
with --plan-tool the encoder's planner and
the Python reference writer (tools/references/truehd_writer.py, which plans the same way from the
same integers) are held to each other on streams the writer makes: the same sections, bit for bit,
the same rows the decoder starts from, and this library's decoder reads the writer's streams to the
objects they were made from.

Exit status 1 on any difference. The WAV is real audio (the DEE gold set's sources).
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCES = REPO / "testdata" / "truehd-dee" / "sources"
sys.path.insert(0, str(REPO / "tools" / "references"))
import truehd_syntax  # noqa: E402
import truehd_writer  # noqa: E402

# Object number, from 8 up, to the bed row (L R C LFE Ls Rs Lrs Rrs) it is mixed into
BED_ROW_OF_OBJECT = (0, 1, 2, 4, 5, 6, 7, 3)
MIX_RAW = -2896  # -1/sqrt(2) at frac_bits 12, the gain the encoder starts from

# (objects, noise shift, bypass)
CONFIGURATIONS = ((12, 0, 0), (12, 1, 0), (12, 3, 1), (14, 1, 1), (16, 2, 0))

# (objects, noise shift, bypass bits (0: none), motion, slopes, update units, stretch, drop idle,
# split): the encode tool's object-motion presets, with the stretch in samples (the file is 12,000
# frames)
MOTION_CONFIGURATIONS = (
    (12, 0, 0, "slow", False, 1, 1200, False, False),
    (12, 3, 2, "slow", True, 1, 1200, True, False),
    (12, 1, 1, "slow", True, 1, 2400, False, False),
    (14, 2, 0, "jump", False, 5, 1200, True, False),
    (14, 1, 1, "jump", True, 1, 1200, True, False),
    (16, 1, 3, "jump", False, 3, 1200, True, True),
)


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


def motion_tolerance(slopes, update_units, stretch, jump):
    """How far, in the last places of a coefficient at 2^-12, the bed's mix may be from the path's:
    half a place of rounding; with the slopes a little more (a coefficient is sent again where it
    is half a place off, a slope where it would end a quarter of a place off); with the
    coefficients held, how far the path moves in the samples they are held (none for a step that
    falls on an update)."""
    slope = truehd_writer.DEFAULT_GAIN / (1 << 16) * 4096 / stretch  # places a sample
    if slopes:
        return 0.75
    return 0.5625 if jump else 0.5625 + slope * 40 * update_units


def expected_motion_bed(rows, paths):
    """The 7.1 bed the paths ask for in real numbers, and for each sample the sum of the magnitudes
    of the objects that reach each row (what a coefficient's error is multiplied by)."""
    bed, reach = [], []
    for t, row in enumerate(rows):
        values, magnitudes = [float(v) for v in row[:8]], [0.0] * 8
        for channel, knots in paths.items():
            for r in range(8):
                if not any(gains[r] for _at, gains in knots):
                    continue
                numerator, denominator = truehd_writer.path_gain(knots, r, t)
                values[r] += numerator / denominator / (1 << 16) * row[channel]
                magnitudes[r] += abs(row[channel])
        bed.append(values)
        reach.append(magnitudes)
    return bed, reach


def check_motion_configuration(encoder, scratch, configuration):
    """The failures of one moving-object configuration, as sentences."""
    objects, noise, bypass_bits, motion, slopes, update, stretch, drop, split = configuration
    label = (
        f"{objects} objects moving {motion}, slopes {int(slopes)}, update {update}, noise {noise}, "
        f"bypass bits {bypass_bits}, drop {int(drop)}, split {int(split)}"
    )
    rows = object_rows(objects)
    source = scratch / f"motion{objects}.wav"
    write_wav(source, rows)
    stream = scratch / f"motion_{objects}_{motion}_{int(slopes)}_{update}_{noise}_{bypass_bits}.thd"
    options = [
        "object-bed=1",
        f"object-noise={noise}",
        f"object-bypass={int(bypass_bits > 0)}",
        f"object-bypass-bits={max(bypass_bits, 1)}",
        f"object-motion={motion}",
        f"object-stretch={stretch}",
        f"object-slopes={int(slopes)}",
        f"object-update={update}",
        f"object-drop={int(drop)}",
        f"object-split={int(split)}",
        "restart=8",
        "verify=1",
    ]
    run([encoder, source, stream, *options])
    data = stream.read_bytes()
    try:
        all_rows, _rate, channels = truehd_syntax.decode_stream(data, "all")
        best_rows, _rate, _channels = truehd_syntax.decode_stream(data, "best")
    except truehd_syntax.StreamError as error:
        return [f"{label}: the reference refuses the stream: {error}"]
    if channels != objects or [list(r) for r in all_rows] != rows:
        return [f"{label}: the reference's object channels are not the objects"]
    paths = truehd_writer.moving_gain_knots(objects, len(rows), stretch, motion == "jump")
    ideal, reach = expected_motion_bed(rows, paths)
    places = motion_tolerance(slopes, update, stretch, motion == "jump")
    extra = 127 * (1 << (noise + 8)) / 32768 if noise else 0.0
    off = sum(
        1
        for t, bed_row in enumerate(best_rows)
        for r in range(8)
        if abs(bed_row[r] - ideal[t][r]) > places / 4096 * reach[t][r] + extra + 1.01
    )
    if off:
        return [f"{label}: {off} bed samples are further from the paths' than {places:.2f} places"]
    return []


def check_plan_agreement(plan_tool, scratch):
    """The encoder's plan of moving objects against the Python reference writer's, on streams the
    writer makes (truehd_writer.MOTION_CASES): the sections bit for bit, the digests of the rows
    the decoder starts from and of the bypass bits, and this library's decoder and the Python
    reference both reading the stream to the objects."""
    directory = scratch / "moving"
    truehd_writer.write_moving(directory)
    failures = []
    for name in truehd_writer.MOTION_CASES:
        expect = (directory / f"{name}.expect").read_text().splitlines()
        planned = run(
            [plan_tool, "plan", directory / f"{name}.request", directory / f"{name}.objects"]
        ).stdout.splitlines()
        if planned != expect:
            first = next(
                (
                    f"line {i}: C++ {a[:60]!r} against Python {b[:60]!r}"
                    for i, (a, b) in enumerate(zip(planned, expect, strict=False))
                    if a != b
                ),
                f"{len(planned)} lines against {len(expect)}",
            )
            failures.append(f"{name}: the encoder's plan differs from the writer's ({first})")
        wanted = next(line for line in expect if line.startswith("pcm "))
        decoded = run([plan_tool, "decode", directory / f"{name}.thd"]).stdout.strip()
        if decoded != wanted:
            failures.append(f"{name}: this library's decoder does not read the writer's stream")
        rows, _rate, _channels = truehd_syntax.decode_stream(
            (directory / f"{name}.thd").read_bytes(), "all"
        )
        flat = b"".join(struct.pack("<i", v) for row in rows for v in row)
        if f"pcm {truehd_writer.fnv1a(flat):016x}" != wanted:
            failures.append(f"{name}: the Python reference does not read the writer's stream")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--encoder", required=True, type=pathlib.Path)
    parser.add_argument("--plan-tool", type=pathlib.Path)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for objects, noise, bypass in CONFIGURATIONS:
            failures += check_configuration(args.encoder, scratch, objects, noise, bypass)
        for configuration in MOTION_CONFIGURATIONS:
            failures += check_motion_configuration(args.encoder, scratch, configuration)
        if args.plan_tool:
            failures += check_plan_agreement(args.plan_tool, scratch)
    for failure in failures:
        print(f"FAIL {failure}")
    if not failures:
        print(f"ok   {len(CONFIGURATIONS)} object substreams decode to their objects")
        print(f"ok   {len(MOTION_CONFIGURATIONS)} moving-object substreams follow their paths")
        if args.plan_tool:
            print(f"ok   {len(truehd_writer.MOTION_CASES)} plans agree with the reference writer's")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
