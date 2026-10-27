#!/usr/bin/env python3
"""Writes the gzip of a file for the component to embed: gzip_asset.py SOURCE DESTINATION.

The web page (ui/iclforge_ui.html and ui/iclforge_ui.js) is 69 KB as written and 22 KB
gzipped, and an image that carries it carries one or the other for ever, so it carries the smaller
and the control surface sends it with `Content-Encoding: gzip` (src/control.cpp). The header is
written here, not by Python's gzip module (which puts the platform in it): no name, no time,
"unknown" as the operating system, so the same zlib gives the same bytes on every machine and a
build is reproducible. Standard library only: IDF's Python has it.
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

# RFC 1952: magic, deflate, no flags, no modification time, "maximum compression", "unknown" system.
HEADER = b"\x1f\x8b\x08\x00" + b"\x00\x00\x00\x00" + b"\x02" + b"\xff"


def gzip_bytes(data: bytes) -> bytes:
    """`data` as one gzip member, level 9."""
    deflate = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = deflate.compress(data) + deflate.flush()
    trailer = struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)
    return HEADER + body + trailer


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, destination = Path(argv[1]), Path(argv[2])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(gzip_bytes(source.read_bytes()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
