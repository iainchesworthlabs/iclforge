#!/usr/bin/env python3
"""The directories C7-2 moved, named as directories in the text that consol_paths.py leaves.

    c7_dirs.py [--root <worktree>] [--stage c7-2] [--dry-run]

consol_paths.py rewrites a path it can find in the move plan, which is a file. A text that names a
directory (`apps/wasm/tests`, a `directory:` of dependabot's, a prose "the code in apps/common",
`tests/cli/`) is left, and so is a file the stage moved into a program's src/ when it is named
through its old directory. This respells the directories that moved whole, at a path boundary:

    apps/android -> apps/demos/android        apps/wasm -> apps/demos/wasm
    apps/windows -> apps/crucible/windows     apps/linux -> apps/crucible/linux
    apps/notices -> notices                   apps/common -> apps/shared/media
    apps/gui -> apps/forge/gui                tests/cli -> apps/forge/cli/tests
    tests/gui -> apps/forge/gui/tests         tests/hearth -> apps/hearth/engine/tests
    tests/crucible -> apps/crucible/engine/tests   (and apps/gui/{tests,translations,packaging})

A name that goes on below the directory is looked up in the tree: where `<new>/<rest>` is not there
but `<new>/src/<rest>` (or assets/, tests/, packaging/) is, that is the file's place. Where neither
is there but the first directory of the name is (an ignored build output, a name wrapped across
two lines of a comment), the directory is respelled; a name below a directory the tree does not have
(`apps/windows/engine/...`, from before Crucible's own tree) is left and listed. A directory split
between places (apps/gui/qml, apps/cli, apps/gui/icons...) is not in the table: it is read and
settled by a person, and listed by c7_leftovers.py. The documents that name the paths before and
after on purpose (KEEP_OLD_PATHS of n1b_paths.py and the two plans), this package's own scripts and
the changelog are left.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

DIRS = [
    ("apps/android", "apps/demos/android"),
    ("apps/wasm", "apps/demos/wasm"),
    ("apps/windows", "apps/crucible/windows"),
    ("apps/linux", "apps/crucible/linux"),
    ("apps/notices", "notices"),
    ("apps/common", "apps/shared/media"),
    ("apps/gui/tests", "apps/forge/gui/tests"),
    ("apps/gui/translations", "apps/forge/gui/assets/translations"),
    ("apps/gui/packaging", "apps/forge/gui/packaging"),
    ("tests/cli", "apps/forge/cli/tests"),
    ("tests/gui", "apps/forge/gui/tests"),
    ("tests/hearth", "apps/hearth/engine/tests"),
    ("tests/crucible", "apps/crucible/engine/tests"),
]
# Below these a name is looked up under the first of these subdirectories that has it.
SUBDIRS = ("", "src/", "assets/", "tests/", "packaging/")
SKIP = (
    "planning/consolidation.md",
    "planning/monorepo.md",
    "planning/layout.md",
    "planning/layout-inventory.md",
    "tools/n1b/",
    "tests/golden/",
    "CHANGELOG.md",
)
BINARY = (".png", ".wav", ".ac3", ".ec3", ".ac4", ".bin", ".mp4", ".jpg", ".ico", ".icns", ".ttf")


def rules() -> list[tuple[re.Pattern[str], str]]:
    out = []
    for old, new in sorted(DIRS, key=lambda d: -len(d[0])):
        rx = re.compile(
            r"(?<![A-Za-z0-9_.-])(?<![A-Za-z0-9_.-]/)" + re.escape(old) + r"(?![A-Za-z0-9_-])"
            r"((?:/[A-Za-z0-9_.@*{},+-]+)*/?)"
        )
        out.append((rx, new))
    return out


def respell(text: str, rs, known: set[str], dirs: set[str]) -> tuple[str, int, list[str]]:
    count = 0
    unplaced: list[str] = []
    stems = {k.rsplit(".", 1)[0] for k in known if "." in k.rsplit("/", 1)[-1]}

    def exists(path: str) -> bool:
        return path in known or path in dirs or path in stems

    def sub(new: str):
        def f(m: re.Match[str]) -> str:
            nonlocal count
            rest = m.group(1)
            tail = rest.rstrip("/")
            if not tail or "..." in tail or any(c in tail for c in "*{}"):
                count += 1
                return new + rest
            if exists((new + tail).strip("/")):
                count += 1
                return new + rest
            head = new + "/"
            for sub_dir in SUBDIRS[1:]:
                cand = (head + sub_dir + tail.lstrip("/")).strip("/")
                if exists(cand):
                    count += 1
                    return head + sub_dir + tail.lstrip("/") + ("/" if rest.endswith("/") else "")
            if exists(new + "/" + tail.lstrip("/").split("/")[0]):
                count += 1
                return new + rest
            unplaced.append(m.group(0))
            return m.group(0)

        return f

    for rx, new in rs:
        text = rx.sub(sub(new), text)
    return text, count, unplaced


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
    files = [f for f in files if f]
    known = set(files)
    dirs: set[str] = set()
    for f in files:
        parts = f.split("/")
        for i in range(1, len(parts)):
            dirs.add("/".join(parts[:i]))
    rs = rules()
    changed = 0
    total = 0
    unplaced_all: list[tuple[str, str]] = []
    for f in files:
        if f.startswith(SKIP) or f.endswith(BINARY):
            continue
        p = root / f
        if not p.is_file() or p.is_symlink():
            continue
        try:
            raw = p.read_bytes()
            text = raw.decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new, n, unplaced = respell(text, rs, known, dirs)
        if n and new != text:
            changed += 1
            total += n
            unplaced_all += [(f, u) for u in unplaced]
            if not a.dry_run:
                p.write_bytes(new.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files, {total} mentions")
    for f, u in unplaced_all:
        print(f"  not a file or directory of the tree: {u}  in {f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
