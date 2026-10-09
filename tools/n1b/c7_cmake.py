"""The moved paths in the CMake files and Qt translation catalogues of C7-2 and C7-3
(planning/monorepo.md).

    c7_cmake.py --root <worktree> --plan <plan.json> [--stage c7-2|c7-3] [--dry-run] [--report <file>]

consol_cmake.py rewrites a path a build file spells in full. The products' build files spell most
of theirs the way CMake reads them: relative to the file (`commands/probe.cpp`, `../common/x.cpp`),
or after `${CMAKE_CURRENT_SOURCE_DIR}/`, `${CMAKE_CURRENT_LIST_DIR}/`, `${CMAKE_SOURCE_DIR}/` or
`${PROJECT_SOURCE_DIR}/`. This resolves every such token against the tree as it was before the
moves, and where it names a file the stage moved, a directory in DIRECTORIES, or a file that did
not move but whose build file did, writes it again in the same form from the file's new place.

- A token with a variable part (`${CMAKE_SOURCE_DIR}/apps/gui/qml/${name}`) is not a path: listed.
- A token that names a directory the stage split or one DIRECTORIES does not hold is listed.
- A build file that another one include()s is read from the includer's directory, as CMake does
  (INCLUDED_FROM).
- C7-3 moves directories whose names are ordinary words (python, rust, js): a token of one name, with
  no slash, is respelled only as the argument of add_subdirectory(), and a directory that moved whole
  needs no entry in DIRECTORIES (the one place all its files went to is read from the plan).
- A Qt catalogue's `<location filename="...">` is relative to the catalogue; the same rule applies,
  and a location that names no file is left.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
import sys
from pathlib import Path

# A directory a build file names whole, old -> new. Each is a directory whose files all went to
# one place (or whose build-file role is that place's); the others are listed for a person.
DIRECTORIES = {
    "apps/common": "apps/shared/media/src",
    "apps/gui/fonts": "apps/shared/theme/assets/fonts",
    "apps/gui/icons": "apps/shared/theme/assets/icons",
    "apps/gui/translations": "apps/forge/gui/assets/translations",
    "apps/gui/qml": "apps/forge/gui/assets/qml",
    "apps/gui/tests": "apps/forge/gui/tests",
    "apps/gui/packaging": "apps/forge/gui/packaging",
    "apps/crucible/translations": "apps/crucible/ui/assets/translations",
    "apps/crucible/ui/qml": "apps/crucible/ui/assets/qml",
    "apps/crucible/ui/tests": "apps/crucible/ui/tests",
    "apps/crucible/engine": "apps/crucible/engine/src",
    "apps/crucible/engine/platform": "apps/crucible/engine/src/platform",
    "apps/crucible/engine/platform/linux": "apps/crucible/engine/src/platform/linux",
    "apps/crucible/ui/platform": "apps/crucible/ui/src/platform",
    "apps/hearth/ui/qml": "apps/hearth/ui/assets/qml",
    "apps/hearth/ui/translations": "apps/hearth/ui/assets/translations",
    "apps/hearth/ui/tests": "apps/hearth/ui/tests",
    "apps/hearth/engine/platform": "apps/hearth/engine/src/platform",
    "apps/windows/driver": "apps/crucible/windows/driver",
    "apps/windows/driver/Source/Main": "apps/crucible/windows/driver/Source/Main",
    "apps/windows/driver-vm": "apps/crucible/windows/driver-vm",
    "apps/linux/tray-vm": "apps/crucible/linux/tray-vm",
    "apps/android": "apps/demos/android",
    "apps/wasm": "apps/demos/wasm",
}

# A build file that is include()d, not add_subdirectory()'d, reads ${CMAKE_CURRENT_SOURCE_DIR} and
# its relative paths from the directory of the one that includes it (old path -> that directory).
INCLUDED_FROM = {
    "apps/gui/tests/CMakeLists.txt": "apps/gui",
    "apps/hearth/ui/tests/CMakeLists.txt": "apps/hearth/ui",
    "apps/crucible/ui/tests/CMakeLists.txt": "apps/crucible",
}
# Where those includers are after the stage.
INCLUDER_NEW = {"apps/gui": "apps/forge/gui", "apps/hearth/ui": "apps/hearth/ui",
                "apps/crucible": "apps/crucible"}

MOVED_ROOTS = ("apps/cli", "apps/gui", "apps/common", "apps/notices", "apps/windows", "apps/linux",
               "apps/crucible", "apps/hearth", "tests/cli", "tests/gui", "tests/hearth",
               "tests/crucible")
COMPOSED = re.compile(
    r"apps/(cli|gui|common|notices|windows|linux|android|wasm)\b|tests/(cli|gui|hearth|crucible)\b"
)
WHOLE_DIRS = False

# C7-3: the bindings and the firmware. The three directories that were split need an entry; the rest
# moved whole.
C7_3_DIRECTORIES = {
    "esp-idf": "firmware/esp-idf",
    "esp-idf/iclforge": "firmware/esp-idf/iclforge",
    "esp-idf/iclforge/examples": "firmware/esp-idf/iclforge/examples",
}
C7_3_MOVED_ROOTS = ("python", "rust", "js", "esp-idf", "esphome", "apps/baremetal")
C7_3_COMPOSED = re.compile(r"(?<![\w/.\-])(python|rust|js|esp-idf|esphome)/|apps/baremetal\b")


def use_stage(stage: str) -> None:
    global DIRECTORIES, INCLUDED_FROM, INCLUDER_NEW, MOVED_ROOTS, COMPOSED, WHOLE_DIRS
    if stage == "c7-3":
        DIRECTORIES, INCLUDED_FROM, INCLUDER_NEW = C7_3_DIRECTORIES, {}, {}
        MOVED_ROOTS, COMPOSED, WHOLE_DIRS = C7_3_MOVED_ROOTS, C7_3_COMPOSED, True


VARS_SOURCE = ("CMAKE_CURRENT_SOURCE_DIR", "CMAKE_CURRENT_LIST_DIR")
VARS_ROOT = ("CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR")
RUN = re.compile(r"[A-Za-z0-9_.\-+@/${}]+")
CMAKE_FILE = re.compile(r"(^|/)(CMakeLists\.txt|[^/]+\.cmake(\.in)?)$")
SKIP_PREFIX = ("cmake/vcpkg/", "cmake/toolchains/", "build/", "docs/", "planning/")
LOCATION = re.compile(r'(<location filename=")([^"]+)(")')


class Tree:
    def __init__(self, moves: dict[str, str], old_files: list[str]) -> None:
        self.moves = moves
        self.new_of = dict(moves)
        self.old_of = {new: old for old, new in moves.items()}
        self.files = set(old_files)
        self.dirs: set[str] = set()
        for f in old_files:
            d = posixpath.dirname(f)
            while d:
                self.dirs.add(d)
                d = posixpath.dirname(d)

    def target(self, p: str) -> tuple[str | None, str]:
        """(its new path, 'file' | 'dir' | 'none') for the old path p."""
        if p in self.files:
            return self.moves.get(p, p), "file"
        if p in DIRECTORIES:
            return DIRECTORIES[p], "dir"
        if p in self.dirs:
            moved = {self.moves[f] for f in self.files if f.startswith(p + "/") and f in self.moves}
            if not moved:
                return p, "dir"
            if WHOLE_DIRS:
                whole = self.whole(p)
                if whole:
                    return whole, "dir"
            return None, "dir"  # a directory some of whose files moved, and not in DIRECTORIES
        return None, "none"

    def whole(self, d: str) -> str | None:
        """The one directory every file under d went to, with the same layout below it."""
        found = set()
        for f in self.files:
            if f.startswith(d + "/"):
                new, rest = self.moves.get(f, f), f[len(d):]
                if not new.endswith(rest):
                    return None
                found.add(new[: len(new) - len(rest)])
        return found.pop() if len(found) == 1 else None


def resolve_token(tok: str, base_cur: str) -> tuple[str, str, str] | None:
    """(old repository path, form, variable) of a path token, or None if it is not one.

    form: 'cur' (${CMAKE_CURRENT_*_DIR}/…), 'root' (${CMAKE_SOURCE_DIR}/…), 'rel' (relative)."""
    m = re.match(r"^\$\{(\w+)\}/(.*)$", tok)
    if m:
        var, rest = m.groups()
        if "${" in rest or "}" in rest:
            return None
        if var in VARS_SOURCE:
            return posixpath.normpath(posixpath.join(base_cur, rest)), "cur", var
        if var in VARS_ROOT:
            return posixpath.normpath(rest), "root", var
        return None
    if "${" in tok or "}" in tok or tok.startswith("/"):
        return None
    return posixpath.normpath(posixpath.join(base_cur, tok)), "rel", ""


def respell(tok: str, form: str, var: str, new_target: str, new_base: str) -> str:
    if form == "root":
        return f"${{{var}}}/{new_target}"
    rel = posixpath.relpath(new_target, new_base)
    if form == "cur":
        return f"${{{var}}}/{rel}"
    return rel


def rewrite_cmake(text: str, f_old: str, f_new: str, tree: Tree, listed: list[str]) -> str:
    base_cur = INCLUDED_FROM.get(f_old, posixpath.dirname(f_old))
    new_cur = INCLUDER_NEW.get(base_cur, posixpath.dirname(f_new)) if f_old in INCLUDED_FROM \
        else posixpath.dirname(f_new)
    own_old, own_new = posixpath.dirname(f_old), posixpath.dirname(f_new)

    def sub(m: re.Match[str]) -> str:
        tok = m.group(0)
        if tok.rstrip("/") != tok:
            return tok
        r = resolve_token(tok, base_cur)
        if r is None:
            if COMPOSED.search(tok):
                listed.append(f"{f_new}: composed or unresolvable: {tok}")
            return tok
        p_old, form, var = r
        if form == "cur" and var == "CMAKE_CURRENT_LIST_DIR":
            p_old = posixpath.normpath(posixpath.join(own_old, tok.split("/", 1)[1]))
        new_target, kind = tree.target(p_old)
        if kind == "none":
            return tok
        if WHOLE_DIRS and "/" not in tok and kind == "dir" and form == "rel" \
                and not text[: m.start()].endswith("add_subdirectory("):
            return tok  # a word that is also the name of a directory
        if new_target is None:
            if tok not in (".", "..") and p_old.startswith(MOVED_ROOTS):
                listed.append(f"{f_new}: a split directory: {tok} ({p_old})")
            return tok
        base_new = own_new if var == "CMAKE_CURRENT_LIST_DIR" else new_cur
        old_base = own_old if var == "CMAKE_CURRENT_LIST_DIR" else base_cur
        if new_target == p_old and base_new == old_base:
            return tok
        if form == "rel" and "/" not in tok and kind == "file" and new_target == p_old:
            return tok
        return respell(tok, form, var, new_target, base_new)

    return RUN.sub(sub, text)


def rewrite_ts(text: str, f_old: str, f_new: str, tree: Tree, stats: dict[str, int]) -> str:
    d_old, d_new = posixpath.dirname(f_old), posixpath.dirname(f_new)

    def sub(m: re.Match[str]) -> str:
        p_old = posixpath.normpath(posixpath.join(d_old, m.group(2)))
        new_target, kind = tree.target(p_old)
        if kind != "file" or new_target is None:
            stats["left"] = stats.get("left", 0) + 1
            return m.group(0)
        rel = posixpath.relpath(new_target, d_new)
        if rel != m.group(2):
            stats["changed"] = stats.get("changed", 0) + 1
        return m.group(1) + rel + m.group(3)

    return LOCATION.sub(sub, text)


def main() -> int:
    ap = argparse_parser()
    a = ap.parse_args()
    use_stage(a.stage)
    root = Path(a.root)
    plan = json.loads(Path(a.plan).read_text(encoding="utf-8"))
    moves = plan["moves"]
    now = subprocess.run(["git", "-C", str(root), "ls-files"], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    old_of = {new: old for old, new in moves.items()}
    old_files = sorted({old_of.get(f, f) for f in now})
    tree = Tree(moves, old_files)
    listed: list[str] = []
    ts_stats: dict[str, int] = {}
    changed = 0
    for f_new in now:
        if f_new.startswith(SKIP_PREFIX):
            continue
        f_old = old_of.get(f_new, f_new)
        is_cmake = bool(CMAKE_FILE.search(f_new))
        is_ts = f_new.endswith(".ts")
        if not (is_cmake or is_ts):
            continue
        p = root / f_new
        try:
            text = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = rewrite_ts(text, f_old, f_new, tree, ts_stats) if is_ts \
            else rewrite_cmake(text, f_old, f_new, tree, listed)
        if out != text:
            changed += 1
            if not a.dry_run:
                p.write_bytes(out.encode("utf-8"))
    verb = "would change" if a.dry_run else "changed"
    print(f"{verb} {changed} files; catalogue locations {ts_stats}")
    if a.report:
        Path(a.report).write_text("\n".join(sorted(set(listed))) + "\n", encoding="utf-8")
    for line in sorted(set(listed))[:60]:
        print("  LISTED", line)
    return 0


def argparse_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=".")
    ap.add_argument("--plan", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", default=None)
    ap.add_argument("--stage", default="c7-2", choices=("c7-2", "c7-3"))
    return ap


if __name__ == "__main__":
    sys.exit(main())
