"""The moved paths in the build files and scripts of a consolidation stage (C1 to C3).

    consol_cmake.py --root <worktree> --plan <plan.json> [--dry-run]

n1b_cmake.py's path rewrite (rewrite_paths, dir_rules, relative_rules) pointed at the plan
consol_apply.py wrote before it moved the files: every full old path of the move map in a build
file or a script, the directories the moves keep together, and in tests/CMakeLists.txt the paths
it names relative to itself. N1B's own rename tables (the L2 aliases, raw targets and output names)
are not applied; a stage's target names are consol_text.py's. The directories whose files went to
more than one place are listed for a person, as n1b_cmake.py lists them.
"""

from __future__ import annotations

import json
import posixpath
import re
from pathlib import Path

from n1b_cmake import (
    _PATH_END,
    _PATH_START,
    BUILD_FILES,
    RELATIVE_BASES,
    SKIP_FILES,
    SKIP_PREFIX,
    dir_rules,
    held_dirs,
    path_index,
    relative_rules,
    rewrite_paths,
)
from n1b_lib import Repo, base_parser


def transform(text: str, moves, dirs, relative, hold, known) -> str:
    text = rewrite_paths(text, moves, dirs, hold, known)
    if relative:
        rel_files, rel_dirs = relative
        for old in sorted(rel_files, key=len, reverse=True):
            if old in text:
                text = re.sub(_PATH_START + re.escape(old) + _PATH_END, rel_files[old], text)
        for old, new in rel_dirs:
            if old in text:
                text = re.sub(_PATH_START + re.escape(old) + _PATH_END, new, text)
    return text


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--plan", required=True, help="the plan consol_apply.py --json wrote")
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    moves = json.loads(Path(a.plan).read_text(encoding="utf-8"))["moves"]
    dirs, split = dir_rules(moves)
    hold = held_dirs(dirs, split)
    known = path_index(repo.files)
    print(f"move map: {len(moves)} files, {len(dirs)} directories ({len(split)} split)")
    relative = {base: relative_rules(moves, base) for base in RELATIVE_BASES}
    changed = 0
    hits: dict[str, list[str]] = {}
    for f in repo.files:
        if f in SKIP_FILES or f.startswith(SKIP_PREFIX) or not BUILD_FILES.search(f):
            continue
        p = root / f
        try:
            text = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = transform(text, moves, dirs, relative.get(posixpath.dirname(f)), hold, known)
        for old_dir in split:
            if re.search(_PATH_START + re.escape(old_dir) + _PATH_END, out):
                hits.setdefault(old_dir, []).append(f)
        if out != text:
            changed += 1
            if not a.dry_run:
                p.write_bytes(out.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files")
    if hits:
        print("directories split between places and named in a build file (review each):")
        applied = {r[0] for r in dirs}
        for old_dir, files in sorted(hits.items()):
            s = split[old_dir]
            state = "rewritten to" if old_dir in applied else "left as is; candidate"
            print(
                f"  {old_dir}: {state} {s['to']} ({s['elsewhere']} of {s['files']} files went "
                f"elsewhere): {', '.join(files[:4])}"
            )


if __name__ == "__main__":
    main()
