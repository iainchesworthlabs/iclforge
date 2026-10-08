"""The move map of a stage that has run, read back from the commit that made the moves.

    c7_moves.py --root <worktree> --commit <hash> --json <plan.json>

consol_apply.py resolves a stage's plan on the tree as it was before the moves, so it cannot be run
again once they are made. consol_cmake.py, consol_paths.py and c7_cmake.py need the map (`moves`:
old path -> new path); the commit that holds the renames alone (every rename R100) has it. Writes
the same `{"moves": {...}}` shape and fails if the commit holds anything but R100 renames.
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
        capture_output=True, text=True, check=True,
    ).stdout
    moves: dict[str, str] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if parts[0] != "R100" or len(parts) != 3:
            raise SystemExit(f"c7_moves: {commit} is not a commit of R100 renames alone: {line!r}")
        moves[parts[1]] = parts[2]
    return moves


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=".")
    ap.add_argument("--commit", required=True, help="the commit of the renames alone")
    ap.add_argument("--json", required=True)
    a = ap.parse_args()
    moves = moves_of(a.root, a.commit)
    Path(a.json).write_text(json.dumps({"moves": moves}, indent=1), encoding="utf-8")
    print(f"{len(moves)} moves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
