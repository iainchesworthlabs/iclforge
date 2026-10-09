#!/usr/bin/env python3
"""The move map of a commit of renames, read back from git.

    moves.py <commit> [--root <worktree>] [--json <file>]

A stage of the re-layout (planning/layout.md, planning/consolidation.md, planning/monorepo.md) moved
its files in a commit of their own, every rename `R100`, before any text was changed. The commit
holds the map, so none is kept as data: this prints it as `old<TAB>new`, one move a line, or writes
`{"moves": {...}}`. It fails when the commit holds anything but R100 renames, which is how a commit
that is not a stage's moves is told from one that is.

README.md lists the commits.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def moves_of(root: str, commit: str) -> dict[str, str]:
    out = subprocess.run(
        ["git", "-C", root, "show", "-M100%", "--name-status", "--format=", commit],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    moves: dict[str, str] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if parts[0] != "R100" or len(parts) != 3:
            raise SystemExit(f"moves: {commit} is not a commit of R100 renames alone: {line!r}")
        moves[parts[1]] = parts[2]
    return moves


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("commit", help="the commit of the renames alone")
    ap.add_argument("--root", default=".")
    ap.add_argument("--json", help="write {'moves': {old: new}} here instead of printing")
    a = ap.parse_args(argv)
    moves = moves_of(a.root, a.commit)
    if a.json:
        Path(a.json).write_text(json.dumps({"moves": moves}, indent=1), encoding="utf-8")
        print(f"{len(moves)} moves")
    else:
        for old, new in moves.items():
            print(f"{old}\t{new}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
