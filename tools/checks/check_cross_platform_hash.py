"""Cross-platform bitstream-hash gate (cross-platform bitstream reproducibility).

verify_gold_reference.sh's own SNR checks compare two DECODES of the same
bitstream, so they cannot see a divergence in the bitstream itself - and one
exists: `linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` have measured
~6.02 dB (one AC-3 exponent step) below every x86 leg on this same gate since
those legs were added, unexplained (see docs/building.md's "Floating-point
contraction" section for what has been ruled out and what has not).

This does not explain that gap - it pins it, so a change to any leg's output,
in either direction, is caught immediately instead of silently reshaping an
already-mysterious number. `testdata/bitstream-hashes.json` records one
SHA-256 per (kernel, transform mode) pair, over the three streams this
project's OWN encoder produces inside verify_gold_reference.sh: the gold-
reference WAV through three tool sets (gold.ac3, gold.ec3, gold_cpl.ec3) and,
since the encoder's floating-point decisions are taken on a signal's own
statistics, the CC0 music and speech programmes through the tools and
searches that have such a decision (the prog_* streams). The external
third-party fixtures never change since this project does not re-encode
them, so hashing them would only restate their own file's hash.

A key with no pinned entry is reported, not failed: this file cannot be
hand-updated for a leg nobody has run it on. Run this once on a leg, read the
hash it prints, add it to the JSON, and the NEXT run on that leg is a real
gate. A key whose pinned hash does not match is a hard failure - every other
bit-exactness gate in this project (libs/ac3/tests/core/test_simd_kernels.cpp, the
codec matrix's byte-identical checks) works the same way for the same
reason: a silent change to a number nobody is watching is worse than a loud,
possibly-still-mysterious one.

Usage: check_cross_platform_hash.py --cli <forge> --workdir <dir>
           [--label-suffix <suffix>] [--pins <path>]
Exit 0 if every kernel/mode key present in the pin file matches (and prints
any unpinned key found this run); exit 1 on a real mismatch.
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_PINS = REPO_ROOT / "testdata" / "bitstream-hashes.json"

# label -> the file verify_gold_reference.sh's own encode step wrote it as.
# Only this project's own encoder output: the external-baseline fixtures
# (ext_*) are committed, static, and never re-encoded on any leg, so their
# hash is just their own file's hash, not a signal about this run.
STREAMS = {
    "ac3": "gold.ac3",
    "eac3": "gold.ec3",
    "eac3_cpl": "gold_cpl.ec3",
    # The programme streams verify_gold_reference.sh encodes for this check
    # alone (hash only; the block "Real programme material" in it has why).
    "prog_ac3_music_192": "prog_ac3_music_192.ac3",
    "prog_ac3_music_192_couple": "prog_ac3_music_192_couple.ac3",
    "prog_ac3_music_192_distortion": "prog_ac3_music_192_distortion.ac3",
    "prog_ac3_speech_128": "prog_ac3_speech_128.ac3",
    "prog_eac3_music_128_auto": "prog_eac3_music_128_auto.ec3",
    "prog_eac3_music_128_cpl": "prog_eac3_music_128_cpl.ec3",
    "prog_eac3_music_128_spx": "prog_eac3_music_128_spx.ec3",
    "prog_eac3_music_128_aht": "prog_eac3_music_128_aht.ec3",
    "prog_eac3_music_128_all": "prog_eac3_music_128_all.ec3",
    "prog_eac3_music_128_cpl_ecpl": "prog_eac3_music_128_cpl_ecpl.ec3",
    "prog_eac3_music_128_tpn": "prog_eac3_music_128_tpn.ec3",
    "prog_eac3_speech_96_auto": "prog_eac3_speech_96_auto.ec3",
    "prog_eac3_music_128_vbr": "prog_eac3_music_128_vbr.ec3",
    "prog_eac3_gold51_256_spx": "prog_eac3_gold51_256_spx.ec3",
    "prog_eac3_gold51_256_aht": "prog_eac3_gold51_256_aht.ec3",
    "prog_eac3_gold51_256_all": "prog_eac3_gold51_256_all.ec3",
}


def kernel_name(cli: Path) -> str:
    out = subprocess.run([str(cli), "--version"], capture_output=True, text=True, check=True).stdout
    match = re.search(r"^\s*kernels:\s*(\S+)\s*$", out, re.MULTILINE)
    if not match:
        raise RuntimeError(f"'{cli} --version' has no 'kernels:' line - got:\n{out}")
    return match.group(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--cli", required=True, type=Path, help="forge binary this run used")
    parser.add_argument("--workdir", required=True, type=Path, help="gate script's own workdir")
    parser.add_argument("--label-suffix", default="", help="TRANSFORM_MODE suffix, e.g. _reference")
    parser.add_argument("--pins", default=DEFAULT_PINS, type=Path, help="pinned-hash JSON file")
    args = parser.parse_args()

    kernel = kernel_name(args.cli)
    mode = args.label_suffix.lstrip("_") or "fast"
    pins = json.loads(args.pins.read_text()) if args.pins.exists() else {}

    unpinned = {}
    failed = False
    for stream_label, filename in STREAMS.items():
        path = args.workdir / filename
        if not path.exists():
            print(
                f"::error::check_cross_platform_hash: {path} missing - "
                "run verify_gold_reference.sh first",
                file=sys.stderr,
            )
            return 1
        digest = sha256_file(path)
        key = f"{kernel}/{mode}/{stream_label}"
        pinned = pins.get(key)
        if pinned is None:
            unpinned[key] = digest
            print(f"[unpinned] {key} = {digest}")
        elif pinned == digest:
            print(f"[ok]       {key} = {digest}")
        else:
            print(
                f"::error::[MISMATCH] {key}: pinned {pinned}, this run produced {digest}",
                file=sys.stderr,
            )
            failed = True

    if unpinned:
        print(
            "\nNo pinned hash yet for: " + ", ".join(unpinned) + ".\n"
            f"Add the value(s) printed above to {args.pins} to gate this leg "
            "in future runs - not a failure by itself.",
            file=sys.stderr,
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
