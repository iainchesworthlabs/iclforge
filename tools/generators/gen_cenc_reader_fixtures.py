#!/usr/bin/env python3
"""Writes the three protected MP4s that the MP4 reader's tests and fuzz harness read.

The reader refuses a protected track and names it (libs/containers/src/mp4/reader.cpp). Its
tests build most of their boxes by hand; these three files add the layouts real packagers
write, made by tools/generators/cenc_make_protected.py from a short E-AC-3 stream that
`forge` encodes:

  cenc-clear-entry-then-enca.mp4   stsd holds the clear `ec-3` entry, then `enca`; the first
                                   fragment is clear and uses entry 1, the others are `cenc`
                                   encrypted and use entry 2 (the layout that used to be read
                                   as clear, with ciphertext in the output)
  cenc-enca-only.mp4               one `enca` entry, every fragment `cenc` encrypted
  cbcs-enca-only.mp4               the same with `cbcs` (a constant IV, so no `senc`)

Each is 6 access units of 2/0 stereo at 96 kbit/s (384 bytes each), 2 to a fragment, a few KB.
The keys are the generator's fixed test values (KID a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90); they
unlock nothing but these files, and the reader never reads a key. The files go in the MP4 fuzz
harness's seed directory, where libs/containers/tests and apps/forge/cli/tests read them too.

    python3 tools/generators/gen_cenc_reader_fixtures.py --forge build/bin/forge
    python3 tools/generators/gen_cenc_reader_fixtures.py --forge build/bin/forge --check

--check writes nothing and exits 1 if a committed file differs from what the script makes. The
output depends on `forge`'s E-AC-3 encoder, so a change to the encoder moves these bytes; the
reader's tests read the box structure and the KID and do not depend on the audio. Needs the
`cryptography` package (for cenc_make_protected.py).
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "libs" / "containers" / "fuzz" / "seeds" / "fuzz_mp4_demux"
MAKE_PROTECTED = Path(__file__).resolve().with_name("cenc_make_protected.py")

FRAMES = 6
FRAME_BYTES = 384  # 96 kbit/s stereo E-AC-3 at 48 kHz: 1536 samples take 384 bytes
SYNC_WORD = b"\x0b\x77"

# (output name, scheme, variant)
FIXTURES = (
    ("cenc-clear-entry-then-enca.mp4", "cenc", "leader_entry"),
    ("cenc-enca-only.mp4", "cenc", "core"),
    ("cbcs-enca-only.mp4", "cbcs", "core"),
)


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")


def clear_mp4(forge: Path, work: Path) -> Path:
    """A clear MP4 of FRAMES access units, made by forge."""
    stream = work / "stream.ec3"
    run([str(forge), "eac3-sine", str(stream), "1", "96", "440", "50", "stereo", "quiet"])
    data = stream.read_bytes()
    cut = FRAMES * FRAME_BYTES
    if len(data) < cut or data[:2] != SYNC_WORD or data[cut : cut + 2] != SYNC_WORD:
        sys.exit("forge's E-AC-3 stream is not 384-byte frames; the constants here are stale")
    stream.write_bytes(data[:cut])
    mp4 = work / "clear.mp4"
    run([str(forge), "mp4", str(stream), str(mp4), "quiet"])
    return mp4


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    parser.add_argument("--forge", required=True, type=Path, help="a built forge")
    parser.add_argument("--out", type=Path, default=SEEDS, help="directory to write")
    parser.add_argument("--check", action="store_true", help="compare, write nothing")
    args = parser.parse_args()

    differing = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        clear = clear_mp4(args.forge, work)
        for name, scheme, variant in FIXTURES:
            made = work / name
            run(
                [
                    sys.executable,
                    "-I",
                    str(MAKE_PROTECTED),
                    str(clear),
                    str(made),
                    scheme,
                    "--variant",
                    variant,
                    "--fragment",
                    "2",
                ]
            )
            target = args.out / name
            if args.check:
                if not target.exists() or target.read_bytes() != made.read_bytes():
                    differing.append(name)
                continue
            args.out.mkdir(parents=True, exist_ok=True)
            target.write_bytes(made.read_bytes())
            print(f"wrote {target.relative_to(ROOT)}: {target.stat().st_size} bytes")
    if differing:
        print("differ from the committed files: " + ", ".join(differing))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
