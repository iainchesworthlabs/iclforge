#!/usr/bin/env python3
"""Which relative paths did a move break?

    c7_relative.py --old-rev <rev> [--root <worktree>] [--plan <plan.json>] [--show N]

The path passes rewrite a path written from the repository root. A path written from the file that
holds it (`../../docs/x.md` in a README, `'..\\..\\..'` in a PowerShell script, a Markdown link to a
sibling, `file("../../x")` in a Gradle script) names a place relative to where the file is, and a
file that moved a level deeper, or whose target moved, no longer reaches it. This reads every text
file of the tree, takes each relative path with a `..` in it and each target of a Markdown link,
resolves it from where the file was at <rev> and, when that named a file or directory of <rev>, from
where the file is now; what no longer resolves is listed with the place the old target went when the
plan says so. A path with a variable part is skipped, and so are URLs, anchors and files that are
not text. A token the stage's own scripts rewrote is skipped too (its old spelling is not in the
file any more). What is listed is for a person to read: a path relative to something else than the
file (a Qt kit's `../plugins/platforms/qoffscreen`, the published site's `../../wasm-demo/`) is
listed and is not a break.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
from pathlib import Path

TOKEN = re.compile(r"(?<![A-Za-z0-9_.])((?:\.\.[\\/])+[A-Za-z0-9_.@+\-\\/]*)")
MD_LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)\s]*)?(?:\s+\"[^\"]*\")?\)")
BARE_EXT = (".md", ".html")
SKIP = ("tools/n1b/", "planning/consolidation.md", "planning/monorepo.md")
BINARY = (".png", ".ico", ".icns", ".jpg", ".wav", ".ac3", ".ec3", ".ac4", ".ttf", ".mp4", ".bin",
          ".lock", ".gz", ".zip", ".woff", ".woff2", ".jar", ".webp", ".svg", ".pdf")


def git(root: str, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", root, *args], capture_output=True, check=True
    ).stdout.decode("utf-8", "surrogateescape")


def tree_sets(names: list[str]) -> tuple[set[str], set[str]]:
    files = set(names)
    dirs = set()
    for n in names:
        parts = n.split("/")
        for i in range(1, len(parts)):
            dirs.add("/".join(parts[:i]))
    return files, dirs


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--old-rev", required=True)
    ap.add_argument("--root", default=".")
    ap.add_argument("--plan")
    ap.add_argument("--show", type=int, default=400)
    a = ap.parse_args()
    root = Path(a.root)
    moves = json.loads(Path(a.plan).read_text())["moves"] if a.plan else {}
    new_of = dict(moves)
    old_of = {n: o for o, n in moves.items()}
    listed = git(a.root, "ls-tree", "-r", "--name-only", a.old_rev).split("\n")
    old_files, old_dirs = tree_sets([n for n in listed if n])
    new_files, new_dirs = tree_sets([n for n in git(a.root, "ls-files").split("\n") if n])
    # a rename git sees between the revision and the tree beyond the plan's
    for line in git(a.root, "diff", "-M", "--name-status", a.old_rev).split("\n"):
        parts = line.split("\t")
        if parts[0].startswith("R") and len(parts) == 3:
            new_of.setdefault(parts[1], parts[2])
            old_of.setdefault(parts[2], parts[1])
    # a directory that moved whole: old dir -> new dir
    dir_new: dict[str, str] = {}
    for o, n in new_of.items():
        po, pn = o.split("/"), n.split("/")
        for i in range(1, len(po)):
            tail = po[i:]
            if len(pn) > len(tail) and pn[len(pn) - len(tail):] == tail:
                dir_new.setdefault("/".join(po[:i]), "/".join(pn[: len(pn) - len(tail)]))

    def exists_old(p: str) -> bool:
        return p in old_files or p in old_dirs

    def exists_new(p: str) -> bool:
        return p in new_files or p in new_dirs

    broken: list[str] = []
    seen = 0
    for f in sorted(new_files):
        if f.endswith(BINARY) or f.startswith(SKIP):
            continue
        p = root / f
        if not p.is_file() or p.is_symlink():
            continue
        try:
            text = p.read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        o = old_of.get(f, f)
        if o not in old_files:
            continue  # a file the stage made: nothing to compare with
        odir, ndir = posixpath.dirname(o), posixpath.dirname(f)
        old_text: list[str] = []

        def untouched(tok: str, o=o, old_text=old_text) -> bool:
            # a token the stage's scripts rewrote is not the file's old spelling any more
            if not old_text:
                try:
                    old_text.append(git(a.root, "show", f"{a.old_rev}:{o}"))
                except subprocess.CalledProcessError:
                    old_text.append("")
            return tok in old_text[0]

        def report(n: int, tok: str, why: str, f: str = f) -> None:
            if untouched(tok):
                broken.append(f"{f}:{n}: {tok}  ({why})")

        toks: list[tuple[str, int]] = []
        for n, line in enumerate(text.split("\n"), 1):
            for m in TOKEN.finditer(line):
                toks.append((m.group(1), n))
            if f.endswith(BARE_EXT):
                for m in MD_LINK.finditer(line):
                    t = m.group(1)
                    if "://" in t or t.startswith(("mailto:", "/", "#")) or t.startswith(".."):
                        continue
                    toks.append((t, n))
        for tok, n in toks:
            t = tok.replace("\\", "/").rstrip("/.,;:'\"`)")
            while t.startswith("./"):
                t = t[2:]
            if not t or any(c in t for c in "${}*"):
                continue
            seen += 1
            old_target = posixpath.normpath(posixpath.join(odir, t))
            if old_target.startswith(".."):
                continue
            if not exists_old(old_target):
                # a build output or a file nobody tracks moves with the directory it is in
                held = [d for d in dir_new if old_target.startswith(d + "/")]
                if not held:
                    want = posixpath.relpath(old_target, ndir)
                    if odir != ndir and posixpath.normpath(t) != want:
                        report(n, tok, f"untracked {old_target}; from here {want}")
                    continue
                best = max(held, key=len)
                moved_to = dir_new[best] + old_target[len(best):]
                want = posixpath.relpath(moved_to, ndir)
                if posixpath.normpath(t) != want:
                    report(n, tok, f"untracked {old_target} -> {moved_to}; from here {want}")
                continue
            new_target = posixpath.normpath(posixpath.join(ndir, t))
            if not new_target.startswith("..") and exists_new(new_target):
                continue
            went = new_of.get(old_target) or dir_new.get(old_target) or ""
            hint = posixpath.relpath(went, ndir) if went else "?"
            report(n, tok, old_target + (f" -> {went}; from here {hint}" if went else ""))
    print(f"{seen} relative paths read; {len(broken)} resolved before the stage and do not now")
    for b in broken[: a.show]:
        print(" ", b)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
