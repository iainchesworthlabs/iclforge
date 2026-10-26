#!/usr/bin/env python3
"""Writes the gzip of a file for the component to embed: gzip_asset.py SOURCE DESTINATION.

The web page (ui/iclforge_ui.html and ui/iclforge_ui.js) is 69 KB as written and 22 KB
gzipped, and an image that carries it carries one or the other for ever, so it carries the smaller
and the control surface sends it with `Content-Encoding: gzip` (src/control.cpp). The bytes are the
same every time (compression level 9, no name and no time in the header, the same operating system
byte), so a build is reproducible. Standard library only: IDF's Python has it.
"""

from __future__ import annotations

import gzip
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, destination = Path(argv[1]), Path(argv[2])
    destination.parent.mkdir(parents=True, exist_ok=True)
    # mtime=0, and gzip.compress writes no file name: 255 ("unknown") is the operating system.
    destination.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
