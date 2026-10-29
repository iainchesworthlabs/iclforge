#!/usr/bin/env python3
"""Writes the protected MP4s that the MP4 reader's tests and fuzz harness read.

The reader refuses a protected track and names it (libs/containers/src/mp4/reader.cpp). Its
tests build most of their boxes by hand; these files add the layouts real packagers write, made
by tools/generators/cenc_make_protected.py from a short stream that `forge` encodes. Two sets,
chosen with --set (the default is both):

--set refusal: three files of E-AC-3, from Shaka Packager's and GPAC's layouts

  cenc-clear-entry-then-enca.mp4   stsd holds the clear `ec-3` entry, then `enca`; the first
                                   fragment is clear and uses entry 1, the others are `cenc`
                                   encrypted and use entry 2 (the layout that used to be read
                                   as clear, with ciphertext in the output)
  cenc-enca-only.mp4               one `enca` entry, every fragment `cenc` encrypted
  cbcs-enca-only.mp4               the same with `cbcs` (a constant IV, so no `senc`)

--set dolby-ott: four files with the protection layout of Dolby's public OTT reference audio
(planning/cenc-conformance.md, M13), the `dolby-ott` profile of cenc_make_protected.py

  dolby-ddp-cenc-iv8-piff.mp4          E-AC-3, `cenc`, whole sample, 8-byte IVs, `saiz` default 8,
                                       `senc` flags 0 and its PIFF uuid copy
  dolby-ddp-cbcs-const-iv-empty-senc.mp4   E-AC-3, `cbcs` 0:0 with a constant IV, an empty
                                       `senc`, `saiz` 0 and 0
  dolby-ac4-cenc-subsample-piff.mp4    AC-4, `cenc`, a clear lead per sample (`senc` flags 0x2),
                                       a `saiz` size table, the uuid copy
  dolby-ac4-cbcs-1-9-subsample.mp4     AC-4, `cbcs` 1:9 over the protected range, a constant IV,
                                       a `senc` that holds only the subsample map

All have PlayReady and Widevine `pssh` boxes in `moov` only. Each is 6 access units, 2 to a
fragment, a few KB: E-AC-3 stereo at 96 kbit/s (384 bytes each); AC-4 stereo at 96 kbit/s from
`forge ac4-encode` of a generated 0.5 s WAV.

The keys are the generator's fixed test values (KID a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90); they
unlock nothing but these files, and the reader never reads a key. The files go in the MP4 fuzz
harness's seed directory, where libs/containers/tests and apps/forge/cli/tests read them too.

    python3 tools/generators/gen_cenc_reader_fixtures.py --forge build/bin/forge
    python3 tools/generators/gen_cenc_reader_fixtures.py --forge build/bin/forge --check
    python3 tools/generators/gen_cenc_reader_fixtures.py --forge build/bin/forge --set dolby-ott

--check writes nothing and exits 1 if a committed file differs from what the script makes. The
output depends on `forge`'s E-AC-3 and AC-4 encoders, so a change to an encoder moves these bytes;
the reader's tests read the box structure and the KID and do not depend on the audio. Needs the
`cryptography` package (for cenc_make_protected.py).
"""

import argparse
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "libs" / "containers" / "fuzz" / "seeds" / "fuzz_mp4_demux"
MAKE_PROTECTED = Path(__file__).resolve().with_name("cenc_make_protected.py")

FRAMES = 6
FRAME_BYTES = 384  # 96 kbit/s stereo E-AC-3 at 48 kHz: 1536 samples take 384 bytes
SYNC_WORD = b"\x0b\x77"
AC4_SECONDS = 0.5  # enough AC-4 frames to keep the first FRAMES of

# (output name, source codec, extra cenc_make_protected.py arguments); the source codec is the
# clear MP4 the file is made from
REFUSAL_FIXTURES = (
    ("cenc-clear-entry-then-enca.mp4", "eac3", ("cenc", "--variant", "leader_entry")),
    ("cenc-enca-only.mp4", "eac3", ("cenc", "--variant", "core")),
    ("cbcs-enca-only.mp4", "eac3", ("cbcs", "--variant", "core")),
)
DOLBY_OTT_FIXTURES = (
    ("dolby-ddp-cenc-iv8-piff.mp4", "eac3", ("cenc", "--profile", "dolby-ott")),
    ("dolby-ddp-cbcs-const-iv-empty-senc.mp4", "eac3", ("cbcs", "--profile", "dolby-ott")),
    ("dolby-ac4-cenc-subsample-piff.mp4", "ac4", ("cenc", "--profile", "dolby-ott")),
    ("dolby-ac4-cbcs-1-9-subsample.mp4", "ac4", ("cbcs", "--profile", "dolby-ott")),
)
SETS = {
    "refusal": REFUSAL_FIXTURES,
    "dolby-ott": DOLBY_OTT_FIXTURES,
    "all": REFUSAL_FIXTURES + DOLBY_OTT_FIXTURES,
}


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")


def clear_eac3_mp4(forge: Path, work: Path) -> Path:
    """A clear MP4 of FRAMES E-AC-3 access units, made by forge."""
    stream = work / "stream.ec3"
    run([str(forge), "eac3-sine", str(stream), "1", "96", "440", "50", "stereo", "quiet"])
    data = stream.read_bytes()
    cut = FRAMES * FRAME_BYTES
    if len(data) < cut or data[:2] != SYNC_WORD or data[cut : cut + 2] != SYNC_WORD:
        sys.exit("forge's E-AC-3 stream is not 384-byte frames; the constants here are stale")
    stream.write_bytes(data[:cut])
    mp4 = work / "clear-eac3.mp4"
    run([str(forge), "mp4", str(stream), str(mp4), "quiet"])
    return mp4


def clear_ac4_mp4(forge: Path, work: Path) -> Path:
    """A clear AC-4 MP4 from a generated stereo WAV; the callers keep the first FRAMES samples.

    The WAV is two sawtooth waves (440 and 660 Hz) in integer arithmetic, so that the input,
    and with it forge's output, does not depend on a platform's libm.
    """
    rate = 48000
    wav_path = work / "saw.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        frames = bytearray()
        for n in range(int(rate * AC4_SECONDS)):
            left = ((n * 440 * 65536 // rate) % 65536 - 32768) // 8
            right = ((n * 660 * 65536 // rate) % 65536 - 32768) // 8
            frames += struct.pack("<hh", left, right)
        wav.writeframes(bytes(frames))
    mp4 = work / "clear-ac4.mp4"
    run([str(forge), "ac4-encode", str(wav_path), str(mp4), "96"])
    return mp4


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    parser.add_argument("--forge", required=True, type=Path, help="a built forge")
    parser.add_argument("--out", type=Path, default=SEEDS, help="directory to write")
    parser.add_argument("--set", choices=sorted(SETS), default="all", help="which files to make")
    parser.add_argument("--check", action="store_true", help="compare, write nothing")
    args = parser.parse_args()

    differing = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        sources = {}
        for name, source, extra in SETS[args.set]:
            if source not in sources:
                make_clear = clear_eac3_mp4 if source == "eac3" else clear_ac4_mp4
                sources[source] = make_clear(args.forge, work)
            made = work / name
            run(
                [
                    sys.executable,
                    "-I",
                    str(MAKE_PROTECTED),
                    str(sources[source]),
                    str(made),
                    extra[0],
                    *extra[1:],
                    "--fragment",
                    "2",
                    "--max-samples",
                    str(FRAMES),
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
