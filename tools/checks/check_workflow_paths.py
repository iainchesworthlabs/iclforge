#!/usr/bin/env python3
"""Hold the path filters of the workflows to the tree and to the project graph.

A workflow that triggers on `push` with `paths:` runs for a change to those paths and for nothing
else, so a filter that names a directory the tree no longer has never fires, and one that omits a
library its project is built from lets a change to that library through unbuilt. Both happened
when the tree was re-laid out (planning/monorepo.md); this makes them a failed check.

    python3 tools/checks/check_workflow_paths.py [--root <repo>]

Two things fail it:

  - a pattern of a `paths:` or `paths-ignore:` list that matches no tracked file (a directory that
    moved, a file that was deleted);
  - a workflow that builds a project (BUILDERS below) whose filter lacks the project's own tree,
    the tree of every library the project is built from (what a firmware project `ships`, else the
    libraries it uses, transitively, in tools/checks/projects.json), a tree the workflow reads that
    the table does not hold, or the build files every CMake project is built with (`cmake/`, the
    root CMakeLists.txt).

Patterns are GitHub's: `*` stays inside a directory, `**` crosses them, `?` is one character, and a
pattern that starts with `!` only subtracts and is not read. Stdlib only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from project_graph import Table, load_table  # noqa: E402


@dataclass(frozen=True)
class Builder:
    """A workflow file that builds projects of the table."""

    workflow: str
    projects: tuple[str, ...]
    extra: tuple[str, ...] = ()  # trees it reads that no project holds
    cmake: bool = True  # built with the repository's CMake


BUILDERS = (
    Builder("wheels.yml", ("python",), extra=("examples/python/**",)),
    Builder("npm.yml", ("js",), cmake=False),
    Builder(
        "esp-component.yml",
        ("esp-idf", "esphome"),
        extra=("tools/packaging/pack_esp_component.py",),
    ),
)
CMAKE_FILES = ("cmake/**", "CMakeLists.txt")

LIST_HEAD = re.compile(r"^(\s*)(paths|paths-ignore):\s*$")
LIST_ITEM = re.compile(r"^(\s*)-\s*[\"']?([^\"'#]+?)[\"']?\s*(#.*)?$")


def glob_to_re(pattern: str) -> re.Pattern[str]:
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
            continue
        if pattern.startswith("**", i):
            out += ".*"
            i += 2
            continue
        c = pattern[i]
        out += "[^/]*" if c == "*" else "[^/]" if c == "?" else re.escape(c)
        i += 1
    return re.compile("^" + out + "$")


def filters_of(text: str) -> list[tuple[int, str, str]]:
    """(line, list name, pattern) for every item of every `paths:`/`paths-ignore:` list."""
    lines = text.splitlines()
    found: list[tuple[int, str, str]] = []
    for i, line in enumerate(lines):
        head = LIST_HEAD.match(line)
        if not head:
            continue
        indent = len(head.group(1))
        for j in range(i + 1, len(lines)):
            nxt = lines[j]
            item = LIST_ITEM.match(nxt)
            if item and len(item.group(1)) >= indent:
                found.append((j + 1, head.group(2), item.group(2).strip()))
            elif nxt.strip() == "" or nxt.strip().startswith("#"):
                continue
            else:
                break
    return found


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
    ).stdout.decode("utf-8", "surrogateescape")
    return [f for f in out.split("\0") if f and (root / f).exists()]


def built_from(table: Table, name: str) -> list[str]:
    """The libraries a project is built from: what it ships, else what it uses, transitively."""
    project = table.projects[name]
    if project.ships:
        return sorted(project.ships)
    libraries: set[str] = set()
    todo = list(project.may_use)
    while todo:
        used = todo.pop()
        if used in libraries or used not in table.projects:
            continue
        libraries.add(used)
        todo.extend(table.projects[used].may_use)
    return sorted(n for n in libraries if table.projects[n].kind in ("library", "vendored"))


def problems(root: Path, table: Table, files: list[str]) -> list[str]:
    found: list[str] = []
    patterns_of: dict[str, set[str]] = {}
    for path in sorted((root / ".github" / "workflows").glob("*.yml")):
        patterns_of[path.name] = set()
        for line, kind, pattern in filters_of(path.read_text(encoding="utf-8")):
            patterns_of[path.name].add(pattern)
            if pattern.startswith("!"):
                continue
            rx = glob_to_re(pattern)
            if not any(rx.match(f) for f in files):
                found.append(
                    f".github/workflows/{path.name}:{line}: the {kind} pattern {pattern} matches "
                    "no tracked file"
                )
    for builder in BUILDERS:
        have = patterns_of.get(builder.workflow)
        if have is None:
            found.append(f".github/workflows/{builder.workflow} is a builder and is not a workflow")
            continue
        needed: dict[str, str] = {}
        for name in builder.projects:
            needed[f"{table.projects[name].path}/**"] = f"the tree of {name}"
            for library in built_from(table, name):
                needed[f"{table.projects[library].path}/**"] = (
                    f"{library}, which {name} is built from"
                )
        for pattern in builder.extra:
            needed[pattern] = "a tree the workflow reads"
        if builder.cmake:
            for pattern in CMAKE_FILES:
                needed[pattern] = "the build files every CMake project reads"
        found.extend(
            f".github/workflows/{builder.workflow}: no path filter for {pattern} ({why})"
            for pattern, why in needed.items()
            if pattern not in have
        )
    return found


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--root", type=Path, default=HERE.parents[1])
    args = ap.parse_args(argv)
    table = load_table(args.root / "tools" / "checks" / "projects.json")
    found = problems(args.root, table, tracked_files(args.root))
    for problem in found:
        print(f"::error::check_workflow_paths: {problem}")
    workflows = len(list((args.root / ".github" / "workflows").glob("*.yml")))
    print(
        f"check_workflow_paths: {workflows} workflows, {len(BUILDERS)} tied to projects, "
        f"{len(found)} problems"
    )
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
