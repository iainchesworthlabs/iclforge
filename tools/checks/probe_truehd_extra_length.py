#!/usr/bin/env python3
"""Does FFmpeg's strict decode of a TrueHD stream with extra data care about the extra data's
length word? Evidence for libs/truehd/ERRATA.md open question 19.

Each access unit of the stream ends with extra data (TH spec 16.1) whose first word is a check
nibble and a 12 bit length. The script rewrites that word in every unit, three ways (the length
less one, plus five, and a wrong check nibble), and decodes each variant with FFmpeg. If FFmpeg
ignores the word, every variant decodes to the samples of the original and the question of what
the length counts has no outside answer.

    probe_truehd_extra_length.py [STREAM]    # default: testdata/truehd/handbuilt/hb_extra_data.thd

Needs ffmpeg. The stream has to be one the library's own writers made: one substream, so that
the extra data starts right after the first segment. Exit status 1 if FFmpeg notices any variant.
"""

import argparse
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT = REPO / "testdata" / "truehd" / "handbuilt" / "hb_extra_data.thd"
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]
SYNC = b"\xf8\x72\x6f"


def units_of(data):
    units, at = [], 0
    while at + 4 <= len(data):
        length = ((data[at] & 0x0F) << 8 | data[at + 1]) * 2
        units.append(bytearray(data[at : at + length]))
        at += length
    return units


def check_nibble(length):
    return ((length >> 8) ^ (length >> 4) ^ length ^ 0xF) & 0xF


def rewrite(data, delta, wrong_nibble=False):
    """The stream with every unit's extra data length changed by delta words."""
    units = units_of(data)
    for unit in units:
        directory = 4 + (28 if unit[4:7] == SYNC else 0)
        end = ((unit[directory] & 0x0F) << 8) | unit[directory + 1]  # one substream
        at = directory + 2 + end * 2
        length = (((unit[at] & 0x0F) << 8) | unit[at + 1]) + delta
        nibble = check_nibble(length) ^ (1 if wrong_nibble else 0)
        unit[at] = (nibble << 4) | (length >> 8)
        unit[at + 1] = length & 0xFF
    return b"".join(bytes(unit) for unit in units)


def decode(data, scratch):
    scratch.write_bytes(data)
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", *STRICT, "-f", "truehd", "-i", str(scratch), "-f", "s32le", "-"],
        capture_output=True,
        check=False,
    )
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("stream", nargs="?", type=pathlib.Path, default=DEFAULT)
    args = parser.parse_args()
    data = args.stream.read_bytes()
    scratch = pathlib.Path(args.stream.name + ".probe.thd")
    try:
        base_status, base_pcm = decode(data, scratch)
        if base_status != 0:
            sys.exit("probe_truehd_extra_length: FFmpeg does not decode the stream as it is")
        noticed = 0
        for label, blob in (
            ("length less one", rewrite(data, -1)),
            ("length plus five", rewrite(data, 5)),
            ("wrong check nibble", rewrite(data, 0, wrong_nibble=True)),
        ):
            status, pcm = decode(blob, scratch)
            same = status == 0 and pcm == base_pcm
            noticed += not same
            print(f"{label:20} ffmpeg {'does not notice' if same else 'NOTICES'}")
        return 1 if noticed else 0
    finally:
        scratch.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
