#!/usr/bin/env python3
"""The golden directory C7-4 moved, named wherever consol_paths.py leaves it.

    c7_golden.py [--root <worktree>] [--dry-run]

consol_paths.py rewrites a path it can find in the move plan, at a path boundary: after a separator,
a quote, `${VAR}/` or `../`. The golden directory is named after a shell variable
(`"$REPO/tests/golden/audio"`), in a string that goes on (`ICLFORGE_SOURCE_DIR "/tests/golden/..."`),
with a brace expansion (`tests/golden/audio/programme_{speech,music}_stereo.flac`) or a glob, and in
comments that name a directory. `tests/golden` is one string with one meaning, so this respells it to
`testdata` wherever it is a whole path component, in the live files. Left alone: the documents that
name the paths before and after on purpose (KEEP_OLD_PATHS of n1b_paths.py, the two plans), the
history (`CHANGELOG.md`, `planning/consolidation.md`), and the scripts of tools/n1b that move an
older layout, apart from the two the proof reads the golden data with (baseline.py, cli_bytes.py) and
the page checker's list of trees it skips.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

OLD = re.compile(r"(?<![A-Za-z0-9_.-])tests/golden(?![A-Za-z0-9_-])")
SKIP = (
    "planning/consolidation.md",
    "planning/monorepo.md",
    "planning/layout.md",
    "planning/layout-inventory.md",
    "CHANGELOG.md",
    "testdata/",
)
# tools/n1b is skipped but for these.
N1B_LIVE = ("tools/n1b/baseline.py", "tools/n1b/cli_bytes.py", "tools/n1b/check_pages.py")
BINARY = (".png", ".wav", ".ac3", ".ec3", ".ac4", ".bin", ".mp4", ".jpg", ".ico", ".icns", ".ttf",
          ".flac", ".lock")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=".")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    files = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
    ).stdout.decode("utf-8", "surrogateescape").split("\0")
    changed = total = 0
    for f in files:
        if not f or f.startswith(SKIP) or f.endswith(BINARY):
            continue
        if f.startswith("tools/n1b/") and f not in N1B_LIVE:
            continue
        p = root / f
        if not p.is_file() or p.is_symlink():
            continue
        try:
            text = p.read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new, n = OLD.subn("testdata", text)
        if n:
            changed += 1
            total += n
            if not a.dry_run:
                p.write_bytes(new.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files, {total} mentions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
