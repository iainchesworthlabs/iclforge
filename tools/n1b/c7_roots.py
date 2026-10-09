#!/usr/bin/env python3
"""The roots C7-3 moved, named as directories in the text that consol_paths.py leaves.

    c7_roots.py [--root <worktree>] [--dry-run]

consol_paths.py rewrites a path that goes on below a root (`python/pyproject.toml`,
`esp-idf/iclforge/src/control.cpp`). python, rust, js, esp-idf and esphome are ordinary words, so it
leaves the root named alone, and this respells it at a path boundary, only where a slash follows or
where it ends a link into the repository (`tree/main/js`):

    python -> bindings/python      rust -> bindings/rust      js -> bindings/js
    esphome -> firmware/esphome    esp-idf -> firmware/esp-idf
    esp-idf/iclforge/examples/hearth_sink -> firmware/hearth-sink

What follows the root is looked up in the tree. A name that is a tracked path or a directory of
one is respelled; so is a glob and a build output the roots are known to write (`js/dist`,
`js/node_modules`, `python/dist`, `rust/target`, `hearth_sink/qemu-*.txt`). Anything else the tree
does not have (`esp-idf/components`, which is Espressif's own tree, `esp-idf/ac3forge/...`, a path
from before a rename, `python/name:` in a YAML tag) is left and listed. The documents that name
the paths before and after on purpose (KEEP_OLD_PATHS of n1b_paths.py and the two plans), this
package's own scripts, the golden data and the changelog are left.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

DIRS = [
    ("esp-idf/iclforge/examples/hearth_sink", "firmware/hearth-sink", False),
    ("esp-idf", "firmware/esp-idf", True),
    ("python", "bindings/python", True),
    ("rust", "bindings/rust", True),
    ("js", "bindings/js", True),
    ("esphome", "firmware/esphome", True),
]
# What the roots write, and git does not track.
OUTPUTS = ("dist", "node_modules", "target", "build", ".venv", "coverage", "qemu-")
SKIP = (
    "planning/consolidation.md",
    "planning/monorepo.md",
    "planning/layout.md",
    "planning/layout-inventory.md",
    "tools/n1b/",
    "tests/golden/",
    "testdata/",
    "CHANGELOG.md",
)
BINARY = (".png", ".wav", ".ac3", ".ec3", ".ac4", ".bin", ".mp4", ".jpg", ".ico", ".icns", ".ttf",
          ".fmp4", ".gz", ".tgz", ".wasm", ".lock")
START = r"(?:(?<![A-Za-z0-9_.-])(?<![A-Za-z0-9_.-]/)|(?<=\.\./))"
TAIL = r"((?:/[A-Za-z0-9_.@*{},+-]+)*/?)"


def rules() -> list[tuple[str, str, re.Pattern[str]]]:
    out = []
    for old, new, single in DIRS:
        if single:
            # a slash after it, or the end of a link into the repository
            link = "|".join(f"(?<={kind}/main/{re.escape(old)})" for kind in ("tree", "blob"))
            rx = re.compile(
                START + re.escape(old) + r"(?![A-Za-z0-9_-])(?:(?=/)|" + link + r")" + TAIL
            )
        else:
            rx = re.compile(START + re.escape(old) + r"(?![A-Za-z0-9_-])" + TAIL)
        out.append((old, new, rx))
    return out


def respell(text: str, rs, known: set[str], dirs: set[str]) -> tuple[str, int, list[str]]:
    count = 0
    unplaced: list[str] = []

    def exists(path: str) -> bool:
        return path in known or path in dirs

    def sub(new: str):
        def f(m: re.Match[str]) -> str:
            nonlocal count
            rest = m.group(1)
            body = rest.rstrip(".,")  # the full stop or comma after a path is not part of it
            tail = body.rstrip("/")
            first = tail.lstrip("/").split("/")[0]
            parts = tail.strip("/").split("/")
            if (
                not tail
                or any(c in tail for c in "*{}")
                or exists((new + tail).strip("/"))
                or first.startswith(OUTPUTS)
                or (len(parts) > 1 and exists(new + "/" + "/".join(parts[:-1])))  # a generated file
            ):
                count += 1
                return new + rest
            unplaced.append(m.group(0))
            return m.group(0)

        return f

    for _old, new, rx in rs:
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
            text = p.read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new, n, unplaced = respell(text, rs, known, dirs)
        unplaced_all += [(f, u) for u in unplaced]
        if n and new != text:
            changed += 1
            total += n
            if not a.dry_run:
                p.write_bytes(new.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files, {total} mentions")
    for f, u in unplaced_all:
        print(f"  not a file or directory of the tree: {u}  in {f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
