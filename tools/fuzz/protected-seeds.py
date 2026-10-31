#!/usr/bin/env python3
"""Protected MP4s for fuzz_mp4_demux: TrueHD and MLP ('mlpa'), E-AC-3 and AC-4 in the layouts a
packager writes that the committed seeds do not already cover.

  protected-seeds.py OUT_DIR --forge PATH

The files are made by tools/generators/cenc_make_protected.py from a short clear MP4 (the same ones
tools/generators/gen_cenc_reader_fixtures.py makes for E-AC-3 and AC-4, and a cut of a TrueHD
fixture for 'mlpa'), under the keys that generator fixes for tests, which unlock nothing but these
files.
They carry the boxes of Common Encryption: `sinf` with `frma`, `schm` and `schi`/`tenc`, `senc`,
`saiz`, `saio`, `pssh` in `moov`, and for the leader and rotate variants a `sgpd`/`sbgp` pair with
`seig` entries. The reader parses `sinf`, `frma`, `schm` and `tenc` and refuses the track by name;
the others it walks over, so a mutation that turns another box into one of them reaches nothing
yet, and the seeds are there for the day a reader decrypts. (run.sh merge keeps the files that
reach different branches today.) The schemes and variants are the generator's: `cenc` and `cens`
(AES-CTR), `cbc1` and `cbcs` (AES-CBC); a clear lead-in entry in either order of `stsd`, a clear
first fragment marked by a `seig` group, and a second key rotated in by a `seig` group.
"""

import argparse
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "testdata" / "truehd"
GENERATORS = ROOT / "tools" / "generators"
SAMPLES = 6
MLPA_UNITS = 8

# (file name, codec, arguments after the scheme's position: scheme first)
SEEDS = (
    ("protected-mlpa-cenc.mp4", "mlpa", ("cenc",)),
    ("protected-mlpa-cbcs.mp4", "mlpa", ("cbcs",)),
    ("protected-mlpa-cbc1-leader-seig.mp4", "mlpa", ("cbc1", "--variant", "leader_seig")),
    ("protected-mlpa-cens-rotate.mp4", "mlpa", ("cens", "--variant", "rotate")),
    ("protected-ec3-cenc-leader-seig.mp4", "eac3", ("cenc", "--variant", "leader_seig")),
    ("protected-ec3-cens-rotate.mp4", "eac3", ("cens", "--variant", "rotate")),
    ("protected-ec3-cbc1-iv16.mp4", "eac3", ("cbc1", "--variant", "core")),
    (
        "protected-ec3-cenc-leader-entry-reversed.mp4",
        "eac3",
        ("cenc", "--variant", "leader_entry_rev"),
    ),
    ("protected-ac4-cenc-leader-seig.mp4", "ac4", ("cenc", "--variant", "leader_seig")),
    ("protected-ac4-cbcs-rotate.mp4", "ac4", ("cbcs", "--variant", "rotate")),
    ("protected-ac4-cens-iv16.mp4", "ac4", ("cens", "--iv-size", "16")),
)


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def clear_mlpa_mp4(forge, work, seeds):
    """A clear MP4 of MLPA_UNITS access units of a 5.1 TrueHD fixture."""
    data = (FIXTURES / "truehd_m51_s16.thd").read_bytes()
    units = seeds.access_units(data)[:MLPA_UNITS]
    stream = work / "stream.thd"
    stream.write_bytes(data[: units[-1][0] + units[-1][1]])
    mp4 = work / "clear-mlpa.mp4"
    subprocess.run([forge, "mp4", stream, mp4], check=True, capture_output=True)
    return mp4


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--forge", required=True)
    args = parser.parse_args()
    fixtures = load(GENERATORS / "gen_cenc_reader_fixtures.py", "gen_cenc_reader_fixtures")
    seeds = load(pathlib.Path(__file__).with_name("truehd-seeds.py"), "truehd_seeds")
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch)
        clear = {
            "eac3": fixtures.clear_eac3_mp4(pathlib.Path(args.forge), work),
            "ac4": fixtures.clear_ac4_mp4(pathlib.Path(args.forge), work),
            "mlpa": clear_mlpa_mp4(args.forge, work, seeds),
        }
        for name, codec, arguments in SEEDS:
            command = [
                sys.executable,
                str(GENERATORS / "cenc_make_protected.py"),
                str(clear[codec]),
                str(args.out / name),
                arguments[0],
                *arguments[1:],
                "--fragment",
                "2",
                "--max-samples",
                str(SAMPLES),
            ]
            subprocess.run(command, check=True, capture_output=True)
    print(f"{len(SEEDS)} files in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
