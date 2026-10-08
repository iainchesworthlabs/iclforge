"""Compare how two configured trees compile each unit: the flags, in order, of compile_commands.json

    flags_diff.py --old <build dir> --new <build dir> [--moves <plan.json>] [--show 20]
                  [--rename OLD=NEW ...] [--rename-re PATTERN=>NEW ...]

A stage that changes how the libraries are made (planning/consolidation.md, C0) must compile every
unit with the same flags: the same definitions, the same include directories in the same order,
the same options. A configure is enough to see it, minutes before a build would. Each tree's own
paths are written as tokens (its source tree, its build tree, the vcpkg install it was given), the
output and dependency-file options are dropped, and a unit is found again under its new path when
a move plan (n1b_apply.py --json, consol_apply.py --json) says it moved; an include directory that
moved is compared by its new name too. What differs is printed unit by unit, as the flags one side
has and the other has not, and as "order" where the two hold the same flags in another order.
`--links` compares the archive and link steps instead (`ninja -t commands`): the objects and
libraries each output is made from, in order. `--ignore` drops a flag the stage changes by design
from both sides (C0: AC-4's profiling directory, which is iclforge::base's now).

A rename is made in one pass, the longest name first, so that a path a rule has written is not
read by another (C7-2: `tests/crucible` -> `apps/crucible/engine/tests` and the directory
`apps/crucible/engine` -> `apps/crucible/engine/src` both apply, and the first must not be
renamed again by the second). `--rename OLD=NEW` adds a literal rule, for what a plan does not
know, such as the build tree of a generated Qt directory (`<build>/apps/gui` ->
`<build>/apps/forge/gui`); `--rename-re PATTERN=>NEW` adds a regular expression with groups.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from collections import Counter
from pathlib import Path

DROP = re.compile(r"^(-o|-MF|-MT|-MQ)$")
DROP_JOINED = re.compile(r"^(-MD|-MMD|-o.+|--dependency-file=.*)$")


def cache(build: Path, key: str) -> str:
    text = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", text, re.MULTILINE)
    return m.group(1).strip() if m else ""


def tokens_of(build: Path) -> list[tuple[str, str]]:
    build = build.resolve()
    source = cache(build, "CMAKE_HOME_DIRECTORY")
    vcpkg = cache(build, "VCPKG_INSTALLED_DIR") or str(build / "vcpkg_installed")
    out = [(str(Path(vcpkg).resolve()), "<vcpkg>"), (str(build), "<build>")]
    if source:
        out.append((source, "<source>"))
    # longest first, so that a build tree inside its source tree is named as the build tree
    return sorted(out, key=lambda t: -len(t[0]))


def normalise(text: str, tokens: list[tuple[str, str]]) -> str:
    for real, token in tokens:
        text = text.replace(real, token)
    return text


def load(
    build: Path, renames: dict[str, str], ignore: list[re.Pattern] | None = None
) -> dict[str, list[tuple[str, ...]]]:
    tokens = tokens_of(build)
    out: dict[str, list[tuple[str, ...]]] = {}
    for entry in json.loads((build / "compile_commands.json").read_text(encoding="utf-8")):
        args = entry.get("arguments") or entry["command"].split()
        kept: list[str] = []
        skip = False
        for a in args[1:]:
            if skip:
                skip = False
                continue
            if DROP.match(a):
                skip = True
                continue
            if DROP_JOINED.match(a):
                continue
            flag = rename(normalise(a, tokens), renames)
            if ignore and any(rx.fullmatch(flag) for rx in ignore):
                continue
            kept.append(flag)
        unit = rename(normalise(entry["file"], tokens), renames)
        # the source file itself is the last argument, and a target compiles a unit once
        if kept and kept[-1] == unit:
            kept.pop()
        if "-c" in kept:
            kept.remove("-c")
        out.setdefault(unit, []).append(tuple(kept))
    return {k: sorted(v) for k, v in out.items()}


RENAME_RE: list[tuple[re.Pattern[str], str]] = []


def rename(text: str, renames: dict[str, str]) -> str:
    if renames:
        # one pass, the longest name first (the dict is ordered that way), so that what a rule
        # wrote is not renamed again
        rx = re.compile("|".join(re.escape(k) for k in sorted(renames, key=lambda k: -len(k))))
        text = rx.sub(lambda m: renames[m.group(0)], text)
    for pattern, new in RENAME_RE:
        text = pattern.sub(new, text)
    return text


def plan_renames(plan: Path | None, old_files: list[str] | None = None) -> dict[str, str]:
    """`<source>/<old path>` -> `<source>/<new path>`, for the files and directories that moved.

    With `old_files`, the old tree's tracked files, a directory is renamed only when it moved whole:
    every file below it is in the plan and lands at the same place below its new name. A file that
    moves out of `tests/` does not rename `tests/` (planning/monorepo.md, C7-1), which the
    parent-of-a-moved-file rule below, used without `old_files`, did. A CMake binary directory
    follows its source directory (`<build>/src/ac3` -> `<build>/libs/ac3`).
    """
    if plan is None:
        return {}
    moves: dict[str, str] = json.loads(plan.read_text(encoding="utf-8"))["moves"]
    out = {f"<source>/{o}": f"<source>/{n}" for o, n in moves.items()}
    if old_files is not None:
        under: Counter = Counter()
        for f in old_files:
            parts = f.split("/")
            for i in range(1, len(parts)):
                under["/".join(parts[:i])] += 1
        whole: Counter = Counter()
        for old, new in moves.items():
            po, pn = old.split("/"), new.split("/")
            # an ancestor of the old path whose remaining path is the same below the new one
            for i in range(1, len(po)):
                tail = po[i:]
                if len(pn) > len(tail) and pn[len(pn) - len(tail) :] == tail:
                    whole[("/".join(po[:i]), "/".join(pn[: len(pn) - len(tail)]))] += 1
        for (o, n), count in whole.items():
            if o != n and count == under[o]:
                out[f"<source>/{o}"] = f"<source>/{n}"
                out[f"<build>/{o}"] = f"<build>/{n}"
        return dict(sorted(out.items(), key=lambda kv: -len(kv[0])))
    dirs: dict[str, set[str]] = {}
    for old, new in moves.items():
        o, n = old.rsplit("/", 1)[0], new.rsplit("/", 1)[0]
        dirs.setdefault(o, set()).add(n)
    for o, ns in dirs.items():
        if len(ns) == 1:
            out[f"<source>/{o}"] = f"<source>/{next(iter(ns))}"
    # the longest first, so that a file is renamed before the directory that holds it
    return dict(sorted(out.items(), key=lambda kv: -len(kv[0])))


def load_links(build: Path, renames: dict[str, str]) -> dict[str, list[tuple[str, ...]]]:
    """Every archive and link step of the tree (`ninja -t commands`), by its output."""
    tokens = tokens_of(build)
    text = subprocess.run(
        ["ninja", "-C", str(build), "-t", "commands"], capture_output=True, text=True, check=True
    ).stdout
    out: dict[str, list[tuple[str, ...]]] = {}
    for line in text.splitlines():
        # a link step is a compiler driver with -o and no -c; an archive is `ar qc <out> ...`
        for step in line.split(" && "):
            try:
                args = shlex.split(step)
            except ValueError:
                continue
            if not args:
                continue
            tool = Path(args[0]).name
            if tool.endswith("ar") and len(args) > 2 and args[1].startswith("q"):
                output, rest = args[2], args[3:]
            elif "-o" in args and "-c" not in args and re.search(r"(g\+\+|clang\+\+|c\+\+)", tool):
                i = args.index("-o")
                output, rest = args[i + 1], args[1:i] + args[i + 2 :]
            else:
                continue
            kept = []
            for a in rest:
                if a.startswith("-Wl,--dependency-file="):
                    continue
                a = rename(normalise(a, tokens), renames)
                # a path below the build tree is written relative to it by one generator and
                # absolute by another
                a = a.replace("<build>/", "")
                if a.startswith("vcpkg_installed/"):
                    a = "<vcpkg>/" + a[len("vcpkg_installed/") :]
                kept.append(a)
            key = rename(normalise(output, tokens), renames).replace("<build>/", "")
            out.setdefault(key, []).append(tuple(kept))
    return {k: sorted(v) for k, v in out.items()}


def compare(old: dict, new: dict, show: int) -> int:
    gone, added = sorted(set(old) - set(new)), sorted(set(new) - set(old))
    differ = [u for u in old if u in new and old[u] != new[u]]
    print(
        f"{len(old)} units before, {len(new)} after; {len(gone)} only before, "
        f"{len(added)} only after, {len(differ)} compiled differently"
    )
    for u in gone[:show]:
        print("  - unit", u)
    for u in added[:show]:
        print("  + unit", u)
    kinds: Counter = Counter()
    for u in differ[: max(show, 0)]:
        print(" ", u)
        for a, b in zip(old[u], new[u], strict=False):
            if sorted(a) == sorted(b):
                print("      order:", [x for x, y in zip(a, b, strict=True) if x != y][:6])
                kinds["order"] += 1
                continue
            sa, sb = Counter(a), Counter(b)
            print("      -", list((sa - sb).elements()))
            print("      +", list((sb - sa).elements()))
        if len(old[u]) != len(new[u]):
            print(f"      compiled {len(old[u])} times before and {len(new[u])} after")
    return 1 if gone or added or differ else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--old", required=True, type=Path)
    ap.add_argument("--new", required=True, type=Path)
    ap.add_argument("--moves", type=Path, default=None, help="a move plan; units follow it")
    ap.add_argument("--show", type=int, default=20)
    ap.add_argument("--rename", action="append", default=[], help="OLD=NEW, a literal rule")
    ap.add_argument(
        "--rename-re", action="append", default=[], help="PATTERN=>NEW, a regular expression"
    )
    ap.add_argument(
        "--links", action="store_true", help="compare the archive and link steps instead"
    )
    ap.add_argument(
        "--ignore",
        action="append",
        default=[],
        help="a flag, as a regular expression on its tokenised text, the stage changes by design",
    )
    a = ap.parse_args()
    old_files = None
    if a.moves is not None:
        home = cache(a.old.resolve(), "CMAKE_HOME_DIRECTORY")
        listed = subprocess.run(
            ["git", "-C", home, "ls-files", "-z"], capture_output=True, check=True
        )
        old_files = [f for f in listed.stdout.decode("utf-8", "surrogateescape").split("\0") if f]
    renames = plan_renames(a.moves, old_files)
    for rule in a.rename:
        old, _, new = rule.partition("=")
        renames[old] = new
    for rule in a.rename_re:
        pattern, _, new = rule.partition("=>")
        RENAME_RE.append((re.compile(pattern), new))
    if a.links:
        return compare(load_links(a.old, renames), load_links(a.new, {}), a.show)
    ignore = [re.compile(x) for x in a.ignore]
    return compare(load(a.old, renames, ignore), load(a.new, {}, ignore), a.show)


if __name__ == "__main__":
    raise SystemExit(main())
