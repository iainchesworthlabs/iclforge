#!/usr/bin/env python3
"""
Decodes the QR matrices the Android app's QrCodeTest writes, and says whether they read back.

The encoder in apps/hearth/android (QrCode.kt) is written for the pairing token on the idle screen
and has no dependency, so its output is checked here by an independent decoder: OpenCV's
QRCodeDetector. QrCodeTest writes qr-matrices.txt to the app's files directory (lyric-renders/);
this pulls nothing itself:

    adb pull /sdcard/Android/data/com.iclforge.hearth/files/lyric-renders/qr-matrices.txt
    python tools/hearth/check_qr_matrices.py qr-matrices.txt

Each line is the payload's bytes in hex, a tab, and the matrix's rows joined by "/" ("#" dark, "."
light). Exit status 0 when every line decodes to its own bytes, 1 otherwise, 2 without OpenCV.

OpenCV's detector locates a symbol before it decodes it, and misses some small ones at some pixel
sizes (a version 1 code at ten pixels a module and a four-module quiet zone, say) while reading
the same modules at another. A line therefore passes when ANY of a few renderings decodes to its
bytes: a wrong matrix reads back as nothing or as other bytes at every size, so this still catches
a bad encoder. Needs opencv-python and numpy; it is a check run by hand beside the on-device test,
not a CI step.
"""
from __future__ import annotations

import argparse
import sys

try:
    import cv2
    import numpy as np
except ImportError:  # the message is printed by main()
    cv2 = None
    np = None

# (pixels per module, quiet-zone modules): the standard's four first, then a wider one.
RENDERINGS = ((4, 4), (8, 4), (10, 4), (4, 6), (8, 6), (12, 8))


def render(rows: list[str], scale: int, quiet: int):
    size = len(rows)
    side = (size + 2 * quiet) * scale
    image = np.full((side, side), 255, dtype=np.uint8)
    for y, row in enumerate(rows):
        for x, cell in enumerate(row):
            if cell == "#":
                top = (y + quiet) * scale
                left = (x + quiet) * scale
                image[top : top + scale, left : left + scale] = 0
    return image


def read_back(detector, rows: list[str], expected: bytes) -> tuple[bool, str]:
    """Whether some rendering decodes to `expected`, and the last thing read otherwise."""
    last = ""
    for scale, quiet in RENDERINGS:
        text, _, _ = detector.detectAndDecode(render(rows, scale, quiet))
        last = text
        if text.encode("utf-8", "surrogateescape") == expected:
            return True, text
    return False, last


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().split("\n", maxsplit=1)[0])
    parser.add_argument("matrices", help="qr-matrices.txt as QrCodeTest wrote it")
    parser.add_argument("--save", help="a directory to write each picture to, as a PNG")
    args = parser.parse_args(argv)
    if cv2 is None:
        print("check_qr_matrices: needs opencv-python (and numpy)", file=sys.stderr)
        return 2

    detector = cv2.QRCodeDetector()
    failures = 0
    checked = 0
    with open(args.matrices, encoding="utf-8") as lines:
        for number, text_line in enumerate(lines, 1):
            entry = text_line.rstrip("\n")
            if not entry:
                continue
            hex_bytes, rows_text = entry.split("\t")
            expected = bytes.fromhex(hex_bytes)
            rows = rows_text.split("/")
            if args.save:
                cv2.imwrite(f"{args.save}/qr-{number:02d}.png", render(rows, 10, 4))
            ok, got = read_back(detector, rows, expected)
            checked += 1
            shown = expected.decode("utf-8", "replace")[:36]
            label = "ok  " if ok else "FAIL"
            print(f"{label} {len(rows):2d} modules, {len(expected):3d} bytes  {shown!r}")
            if not ok:
                failures += 1
                print(f"     read back: {got!r}")
    print(f"check_qr_matrices: {checked - failures} of {checked} read back")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
