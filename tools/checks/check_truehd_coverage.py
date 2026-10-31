#!/usr/bin/env python3
"""Does the committed TrueHD and MLP fixture set still exercise every feature it is meant to?

A decoder test that passes over a stream which no longer carries the feature it was written for
proves nothing, and a fixture deleted or regenerated into something plainer takes its coverage
with it without any test failing. This reads every stream in testdata/truehd (the FFmpeg-made ones
and testdata/truehd/handbuilt) with tools/references/truehd_syntax.py (tools/checks/
truehd_census.py), and fails when a feature in REQUIRED appears in none of them: a sample rate, a
channel count, a restart interval, every FIR and IIR order, each residual codebook, matrices, a
permuted channel assignment, an output shift, LSB bypass, noise generators 0 and 1, the block data
check, the end-of-stream marker, extra data, the directory's DRC word, a 20 bit source, and so on.

KNOWN_GAPS lists what no committed stream carries and why; a gap that closes is reported so that
it moves to REQUIRED.

    check_truehd_coverage.py [--list]     # --list prints each feature and the streams that carry it

Needs nothing but the Python standard library; it takes about half a minute (the transcription is
slow and meant to be read).
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import truehd_census

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"

RATES = (44100, 48000, 88200, 96000, 176400, 192000)
# feature -> test on one stream's census record
REQUIRED = {}
for _rate in RATES:
    REQUIRED[f"sample rate {_rate} Hz"] = lambda r, v=_rate: r["sample_rate"] == v
for _kind in ("truehd", "mlp"):
    REQUIRED[f"stream type {_kind}"] = lambda r, v=_kind: r["kind"] == v
for _channels in (1, 2, 3, 4, 5, 6, 8, 16):
    REQUIRED[f"{_channels} channels"] = lambda r, v=_channels: r["channels"] == v
for _substreams in (1, 2, 3, 4):
    REQUIRED[f"{_substreams} substream(s)"] = lambda r, v=_substreams: r["substreams"] == v
for _gap in (8, 16, 128):
    REQUIRED[f"major sync every {_gap} access units"] = lambda r, v=_gap: v in r["major_sync_gaps"]
for _size in (40, 80, 160):
    REQUIRED[f"block of {_size} samples"] = lambda r, v=_size: v in r["block_sizes"]
for _order in range(1, 9):
    REQUIRED[f"FIR order {_order}"] = lambda r, v=_order: v in r["fir_orders"]
for _order in range(1, 5):
    REQUIRED[f"IIR order {_order}"] = lambda r, v=_order: v in r["iir_orders"]
for _fir, _iir in ((4, 4), (5, 3)):
    REQUIRED[f"FIR {_fir} with IIR {_iir} (8 together)"] = lambda r, v=(_fir, _iir): (
        v in [tuple(pair) for pair in r["combined_orders"]]
    )
for _type in (0, 1, 2, 3):
    REQUIRED[f"residual codebook {_type}"] = lambda r, v=_type: v in r["huff_types"]
for _shift, _label in ((0, "24 bit"), (4, "20 bit"), (7, "16 bit")):
    REQUIRED[f"{_label} source (restart header max_shift {_shift})"] = lambda r, v=_shift: (
        v in r["max_shifts"]
    )
REQUIRED.update(
    {
        "restart sync 0x31EA": lambda r: 0x31EA in r["restart_syncs"],
        "restart sync 0x31EB (noise_type 1)": lambda r: 0x31EB in r["restart_syncs"],
        "Huffman offset": lambda r: r["huff_offsets"],
        "one matrix": lambda r: 1 in r["matrix_counts"],
        "three matrices": lambda r: 3 in r["matrix_counts"],
        "LSB bypass": lambda r: r["lsb_bypass"],
        "LSB bypass under restart sync 0x31EB": lambda r: r["lsb_bypass_noise_type"],
        "a matrix noise shift (noise generator 1) at 48 kHz": lambda r: (
            r["matrix_noise_shift"] and r["sample_rate"] == 48000
        ),
        "a matrix noise shift (noise generator 1) at 96 kHz": lambda r: (
            r["matrix_noise_shift"] and r["sample_rate"] == 96000
        ),
        "a matrix noise shift (noise generator 1) at 192 kHz": lambda r: (
            r["matrix_noise_shift"] and r["sample_rate"] == 192000
        ),
        "a matrix coefficient on a noise channel": lambda r: r["noise_coefficients"],
        "an output shift": lambda r: r["output_shifts"],
        "a negative output shift": lambda r: r["output_shift_negative"],
        "an output shift on a matrix channel that ch_assign moves": lambda r: r[
            "output_shift_on_moved_channel"
        ],
        "IIR filter state (new_states) with a state shift": lambda r: r["iir_state_shift"],
        "a 16 bit MLP stream with bits set below bit 8": lambda r: r["mlp_word16_low_bits"],
        "a quantiser step": lambda r: bool(r["quant_steps"]),
        "a permuted ch_assign": lambda r: r["ch_assign_permuted"],
        "a ch_assign that changes between restart headers": lambda r: r["ch_assign_changes"],
        "a noise shift in the restart header": lambda r: r["noise_shift"],
        "a noise seed in the restart header": lambda r: r["noise_seed"],
        "the block data check": lambda r: r["data_check"],
        "the end-of-stream marker with padding": lambda r: (
            r["marker_units"] > 0 and r["padding_samples"] > 0
        ),
        "extra data": lambda r: r["extra_data_units"] > 0,
        "the directory's DRC word": lambda r: r["drc_word_units"] > 0,
    }
)

KNOWN_GAPS = {
    "FIR filter state": (
        lambda r: r["fir_state"],
        "the streams send the flag clear and the library refuses a FIR filter that sets it "
        "(ERRATA 9)",
    ),
}


def census_all(directory=FIXTURES):
    """{path: census record} for every stream under `directory`."""
    records = {}
    for path in sorted([*directory.glob("*.thd"), *directory.glob("*.mlp")]) + sorted(
        (directory / "handbuilt").glob("*.thd")
    ):
        try:
            records[path.relative_to(directory).as_posix()] = truehd_census.census_of(
                path.read_bytes()
            )
        except truehd_census.ref.StreamError:
            continue  # a stream the reference refuses has no census
    return records


def evaluate(records, required=None, gaps=None):
    """(missing features, closed gaps, {feature: [streams]}) for these records."""
    required = REQUIRED if required is None else required
    gaps = KNOWN_GAPS if gaps is None else gaps
    carriers = {
        name: [path for path, record in records.items() if test(record)]
        for name, test in required.items()
    }
    missing = [name for name, found in carriers.items() if not found]
    closed = [name for name, (test, _why) in gaps.items() if any(test(r) for r in records.values())]
    return missing, closed, carriers


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="print each feature and its streams")
    parser.add_argument("--dir", type=pathlib.Path, default=FIXTURES)
    args = parser.parse_args()
    records = census_all(args.dir)
    missing, closed, carriers = evaluate(records)
    if args.list:
        for name, found in carriers.items():
            shown = ", ".join(found[:4]) + (f" (+{len(found) - 4})" if len(found) > 4 else "")
            print(f"{name:<52} {len(found):>3}  {shown}")
    for name in missing:
        print(f"::error title=TrueHD fixture coverage::no committed stream carries {name}")
    for name in closed:
        print(f"notice: a stream now carries '{name}': move it from KNOWN_GAPS to REQUIRED")
    for name, (_test, why) in KNOWN_GAPS.items():
        if name not in closed:
            print(f"gap  {name}: {why}")
    print(
        f"{len(records)} streams, {len(REQUIRED)} features required, {len(missing)} missing, "
        f"{len(KNOWN_GAPS) - len(closed)} known gaps"
    )
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
