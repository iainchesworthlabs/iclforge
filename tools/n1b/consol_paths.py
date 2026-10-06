"""The moved paths in every other text of a consolidation stage (C1 to C3).

    consol_paths.py --root <worktree> --plan <plan.json> --stage c1|c2|c3 [--dry-run]

n1b_paths.py with two changes: the consolidation's own plan (planning/consolidation.md), which names
the paths before and after every stage on purpose, is left alone as the layout study was; and the
renames no move plan lists are the stage's ABI allowlists (the libraries that merge take the name of
the one that replaces them) and the files folded into another (consoldef.FOLDED: the AC-4 encoder's
errata are a section of src/ac4/ERRATA.md), not S2's. A directory the stage keeps is not renamed
(consol_cmake.kept_dirs_out).
"""

from __future__ import annotations

import sys

import consoldef
import n1b_paths
from consol_cmake import kept_dirs_out
from n1b_cmake import path_index
from n1b_lib import DEFAULT_ROOT, Repo

ALLOWLISTS = "tools/ci/abi-allowlist/"


def allowlist_renames(stage: str) -> dict[str, str]:
    out = {}
    for old, new in consoldef.LIBRARY_MAP[stage].items():
        if old != new:
            out[f"{ALLOWLISTS}libiclforge_{old}.so.txt"] = f"{ALLOWLISTS}libiclforge_{new}.so.txt"
    return out


def main() -> None:
    argv = sys.argv[1:]
    if "--stage" not in argv:
        raise SystemExit("consol_paths: --stage c1|c2|c3 is required")
    i = argv.index("--stage")
    stage = argv[i + 1]
    del argv[i : i + 2]
    n1b_paths.KEEP_OLD_PATHS = (*n1b_paths.KEEP_OLD_PATHS, "planning/consolidation.md")
    n1b_paths.HAND_RENAMES = {**allowlist_renames(stage), **consoldef.FOLDED[stage]}
    root = argv[argv.index("--root") + 1] if "--root" in argv else DEFAULT_ROOT
    known = path_index(Repo(root).files)
    derive = n1b_paths.dir_rules

    def dir_rules(moves):
        rules, split = derive(moves)
        return kept_dirs_out(rules, known), split

    n1b_paths.dir_rules = dir_rules
    sys.argv = [sys.argv[0], *argv]
    n1b_paths.main()


if __name__ == "__main__":
    main()
