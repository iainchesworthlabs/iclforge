#!/usr/bin/env python3
"""Fail when a project of the tree uses a project its row of the table does not allow.

The tree is a monorepo of projects (planning/monorepo.md): libraries under libs/, the products'
programs under apps/, the code several programs share under apps/shared/, the language bindings
under bindings/, the firmware under firmware/, the examples, and the vendored code under external/.
What keeps the graph between them acyclic and one-way is nothing but the habit of the people editing
it, and a habit does not survive the next project: the day a codec-blind library includes a
codec's header, that codec can no longer be left out of a build that does not want it; the day one
product includes another's, the two can no longer be packaged apart.
tools/checks/projects.json lists every project, its kind, its path and the projects it may use.
This script resolves every #include of every C/C++ file the way the compiler does (relative to the
including file, then by the spelling a header is reached by), reads every target_link_libraries() of
every CMake file, and reports each use that crosses from one project into another the table does
not allow. Stdlib-only, run from _static.yml's static job and runnable the same way locally:

    python3 tools/checks/check_layering.py [--root <repo>] [--table <json>] [--edges]

A project is the directory its row names; a file belongs to the project with the longest such
path above it. A project's tests/ and fuzz/ directories (and the top-level tests/) are its
consumers: they are read, and may use every library besides what the project's own row
lists, but nothing else.

The kinds, and what each may use (the table is held to this as well as the tree):

  library      other libraries (libs/<name>, and external/ code)
  app-library  libraries (apps/shared/<name>: code several programs compile in; internal)
  app          libraries and app-libraries (a program of a product); never another app
  binding      libraries
  firmware     libraries
  example      libraries
  tests        libraries and app-libraries (tests/support, tests/performance)

An `internal` project is never installed, and no installed header (a header under a non-internal
library's include/) includes one: cmake/InstallLibrary.cmake names none of them.

Named exceptions. An edge the kinds forbid but the tree has, and that is deliberate, is listed
in the table's "exceptions" with the files it is in (or all of the project's files) and why.
An exception that no longer excuses an edge fails, which is what makes the change that
removes the edge delete it.

Known debts. A cut that removes an include the table forbids lands as its own change, and the
include is there until it does. Each such include is listed in a file of
tools/checks/layering_debt/ (one file per cut, so that two cuts landing together never edit the
same lines): a line is `<path of the including file> <the spelling it includes>`, blank lines
and # comments are ignored. A listed include is reported as known and does not fail. A listed
include that is no longer in the tree fails. With the cuts landed no file is left, and the
directory's README.md keeps it in the tree (git has no empty directory) and says how a debt is
listed.

Five things fail the check, each with a line that names the file and the line of the include (or the
CMake command) where there is one:

  - a use of a project the table does not list for the user, and no exception excuses;
  - a file in no project of the table that holds C/C++ or CMake, under a tree the table covers;
  - a table whose row breaks the rules of the kinds, names an unknown project, has a cycle, or
    names a path nothing is filed under;
  - an exception or a known debt that no longer applies;
  - an installed header that includes an internal project's, or an install() that names one.

`--edges` prints the edges between projects with their counts and stops, which is how a table row is
written in the first place; `--debt-lines` prints every include the table forbids in the form a
debt file takes.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONSUMER_DIRS = ("tests", "fuzz")
DEFAULT_TABLE = HERE / "projects.json"
DEFAULT_DEBT = HERE / "layering_debt"

# What a kind may use. A row of the table that lists more is a problem of the table.
ALLOWED: dict[str, set[str]] = {
    "library": {"library", "vendored"},
    "app-library": {"library", "vendored"},
    "app": {"library", "app-library", "vendored"},
    "binding": {"library", "vendored"},
    "firmware": {"library", "vendored"},
    "example": {"library", "vendored"},
    "tests": {"library", "app-library", "vendored"},
    "vendored": set(),
}
# What a project's own tests/ and fuzz/ may use besides its row.
CONSUMER_MAY_USE = {"library", "vendored", "tests"}
# The trees the table covers: C/C++ or CMake under one that no project holds is a problem.
COVERED_ROOTS = ("libs/", "apps/", "bindings/", "firmware/", "examples/", "external/", "tests/")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)
SOURCE_EXT = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".mm"}
HEADER_EXT = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"}
CMAKE_FILE = re.compile(r"(^|/)(CMakeLists\.txt|[^/]+\.cmake(\.in)?)$")
# A private header is reached by the tail of its path, the way an include directory would let a
# source name it; the tail is bounded so a long variant path does not index every suffix.
MAX_SUFFIX_PARTS = 5

# A CMake command with its argument list: the commands that name a target another project owns.
COMMAND_RE = re.compile(
    r"\b(target_link_libraries|add_library|add_executable|qt_add_executable|install|iclforge_add_library)\s*\(",
    re.I,
)
GENERATOR_EXPR = re.compile(r"\$<[^>]*>")
LINK_KEYWORDS = {
    "PUBLIC",
    "PRIVATE",
    "INTERFACE",
    "LINK_PUBLIC",
    "LINK_PRIVATE",
    "debug",
    "optimized",
    "general",
}
# cmake/IclforgeLibrary.cmake's iclforge_add_library(<name> ...): its keywords, and the ones whose
# items are other targets (DEPENDS names libraries by stem: iclforge::<stem>).
LIBRARY_KEYWORDS = {
    "SOURCES",
    "DEPENDS",
    "EMBEDS",
    "LINK_PUBLIC",
    "LINK_PRIVATE",
    "LINK_PRIVATE_FIRST",
    "BUILD_TREE_DEPENDS",
    "NO_C4251_SUPPRESSION",
    "PUBLIC_INCLUDES",
    "PRIVATE_INCLUDES",
    "STEM",
    "EXPORT_BASE",
    "EXPORT_HEADER",
    "EXPORT_CUSTOM_CONTENT_VAR",
}
LIBRARY_LINK_KEYWORDS = {"DEPENDS", "EMBEDS", "LINK_PUBLIC", "LINK_PRIVATE", "LINK_PRIVATE_FIRST"}


@dataclass(frozen=True)
class Project:
    name: str
    kind: str
    path: str
    internal: bool
    may_use: tuple[str, ...]


@dataclass(frozen=True)
class Exception_:
    source: str
    target: str
    paths: tuple[str, ...]
    why: str


@dataclass(frozen=True)
class Table:
    projects: dict[str, Project]
    exceptions: tuple[Exception_, ...]
    install_files: tuple[str, ...]
    by_length: tuple[Project, ...] = field(default=(), compare=False)

    def project_of(self, path: str) -> Project | None:
        for project in self.by_length:
            if path == project.path or path.startswith(project.path + "/"):
                return project
        return None

    def is_consumer(self, path: str) -> bool:
        """Whether a file is in a project's tests/ or fuzz/ (or the top-level tests/)."""
        project = self.project_of(path)
        if project is None:
            return False
        rest = path[len(project.path) :].lstrip("/").split("/")
        return project.kind == "tests" or any(part in CONSUMER_DIRS for part in rest[:-1])


@dataclass(frozen=True)
class Edge:
    file: str
    line: int
    what: str  # the include spelling, or the target a link line names
    target: str  # the file included, or the target linked
    source: str
    destination: str
    via: str  # "include" or "link"


def load_table(path: Path) -> Table:
    raw = json.loads(path.read_text(encoding="utf-8"))
    projects = {
        name: Project(
            name=name,
            kind=row["kind"],
            path=row["path"].rstrip("/"),
            internal=bool(row.get("internal", False)),
            may_use=tuple(row.get("may_use", [])),
        )
        for name, row in raw["projects"].items()
    }
    exceptions = tuple(
        Exception_(e["from"], e["to"], tuple(e.get("paths", [])), e.get("why", ""))
        for e in raw.get("exceptions", [])
    )
    ordered = tuple(sorted(projects.values(), key=lambda p: -len(p.path)))
    return Table(projects, exceptions, tuple(raw.get("install_files", [])), ordered)


def tracked_files(root: Path) -> list[str]:
    """The files git tracks, or every file when this is not a work tree."""
    try:
        listed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
        ).stdout.decode("utf-8", "surrogateescape")
    except (OSError, subprocess.CalledProcessError):
        files = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
        return [f for f in files if not f.startswith((".git/", "build/"))]
    files = sorted(f for f in listed.split("\0") if f)
    # A path git lists and the checkout lacks (a deletion not yet committed) is not a file.
    return [f for f in files if (root / f).is_file()]


def header_spellings(path: str) -> list[str]:
    """The strings an #include can use to reach this header, from outside its own directory."""
    generated = path[:-3] if path.endswith((".hpp.in", ".h.in")) else path
    if "/include/" in generated:
        return [generated.split("/include/", 1)[1]]
    parts = generated.split("/")
    return [
        "/".join(parts[i:])
        for i in range(len(parts))
        if "/".join(parts[i:]).count("/") < MAX_SUFFIX_PARTS
    ]


def build_index(files: list[str]) -> dict[str, list[str]]:
    index: dict[str, list[str]] = defaultdict(list)
    for f in files:
        ext = posixpath.splitext(f)[1].lower()
        if ext not in HEADER_EXT and not f.endswith((".hpp.in", ".h.in")):
            continue
        for spelling in header_spellings(f):
            index[spelling].append(f)
    return index


def resolve(
    fileset: set[str], index: dict[str, list[str]], table: Table, frm: str, spelling: str
) -> str | None:
    """The file an include names, or None for a system, external or generated header."""
    relative = posixpath.normpath(posixpath.join(posixpath.dirname(frm), spelling))
    if relative in fileset:
        return relative
    hits = index.get(spelling)
    if not hits:
        return None
    own = table.project_of(frm)
    same = [h for h in hits if table.project_of(h) is own]
    if same:
        return sorted(same)[0]
    public = [h for h in hits if "/include/" in h]
    return sorted(public or hits)[0]


def include_edges(root: Path, files: list[str], table: Table) -> list[Edge]:
    fileset = set(files)
    index = build_index(files)
    edges: list[Edge] = []
    for f in files:
        if posixpath.splitext(f)[1].lower() not in SOURCE_EXT:
            continue
        source = table.project_of(f)
        if source is None:
            continue
        text = (root / f).read_text(encoding="utf-8", errors="replace")
        for match in INCLUDE_RE.finditer(text):
            spelling = match.group(2).strip()
            target = resolve(fileset, index, table, f, spelling)
            if target is None:
                continue
            destination = table.project_of(target)
            if destination is None or destination is source:
                continue
            line = text.count("\n", 0, match.start()) + 1
            edges.append(Edge(f, line, spelling, target, source.name, destination.name, "include"))
    return edges


def split_arguments(text: str, start: int) -> tuple[list[str], int]:
    """The arguments of the command whose `(` is at start - 1, and the index after its `)`."""
    depth, i, args, current = 1, start, [], []
    while i < len(text) and depth:
        c = text[i]
        if c == "#":  # a comment to the end of the line
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            current.append(text[i : j + 1])
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if not depth:
                break
        if c.isspace():
            if current:
                args.append("".join(current))
                current = []
        else:
            current.append(c)
        i += 1
    if current:
        args.append("".join(current))
    return args, i + 1


def cmake_commands(text: str):
    """Yield (command, arguments, line) for the commands COMMAND_RE names."""
    for match in COMMAND_RE.finditer(text):
        # not in a comment
        line_start = text.rfind("\n", 0, match.start()) + 1
        if "#" in text[line_start : match.start()]:
            continue
        args, _ = split_arguments(text, match.end())
        yield match.group(1).lower(), args, text.count("\n", 0, match.start()) + 1


def target_owners(root: Path, files: list[str], table: Table) -> dict[str, str]:
    """Every target (and alias) a CMake file defines, and the project whose file defines it."""
    owners: dict[str, str] = {}
    for f in files:
        if not CMAKE_FILE.search(f):
            continue
        project = table.project_of(f)
        if project is None:
            continue
        text = (root / f).read_text(encoding="utf-8", errors="replace")
        for command, args, _ in cmake_commands(text):
            if command in ("add_library", "add_executable", "qt_add_executable") and args:
                name = args[0]
                if "$" in name or name.startswith('"'):
                    continue
                owners.setdefault(name, project.name)
            elif command == "iclforge_add_library" and args:
                name = args[0]
                stem = args[args.index("STEM") + 1] if "STEM" in args[1:] else name
                for target in (
                    f"iclforge::{stem}",
                    f"iclforge::{stem}_static",
                    f"iclforge::{stem}_shared",
                    f"iclforge_{name}_objects",
                    f"iclforge_{name}_static",
                    f"iclforge_{name}_shared",
                ):
                    owners.setdefault(target, project.name)
    return owners


def library_links(args: list[str]) -> list[str]:
    """The targets an iclforge_add_library() call links or embeds: its DEPENDS (by stem), EMBEDS,
    LINK_PUBLIC, LINK_PRIVATE and LINK_PRIVATE_FIRST items."""
    out: list[str] = []
    section = ""
    for arg in args[1:]:
        if arg in LIBRARY_KEYWORDS:
            section = arg
            continue
        if section not in LIBRARY_LINK_KEYWORDS:
            continue
        item = GENERATOR_EXPR.sub("", arg).strip()
        if item and not item.startswith(("$", '"')):
            out.append(f"iclforge::{item}" if section == "DEPENDS" else item)
    return out


def link_edges(root: Path, files: list[str], table: Table) -> list[Edge]:
    owners = target_owners(root, files, table)
    edges: list[Edge] = []
    for f in files:
        if not CMAKE_FILE.search(f):
            continue
        source = table.project_of(f)
        if source is None:
            continue
        text = (root / f).read_text(encoding="utf-8", errors="replace")
        for command, args, line in cmake_commands(text):
            if command == "target_link_libraries" and len(args) >= 2:
                items = args[1:]
            elif command == "iclforge_add_library" and args:
                items = library_links(args)
            else:
                continue
            for raw in items:
                item = GENERATOR_EXPR.sub("", raw).strip()
                if not item or item in LINK_KEYWORDS or item.startswith(("$", '"', "-")):
                    continue
                destination = owners.get(item)
                if destination is None or destination == source.name:
                    continue
                edges.append(Edge(f, line, item, item, source.name, destination, "link"))
    return edges


def allowed_for(table: Table, edge_file: str, source: str) -> set[str]:
    """The projects a file of `source` may use: the row, and for a consumer every library too."""
    project = table.projects[source]
    allowed = set(project.may_use) | {source}
    if table.is_consumer(edge_file):
        allowed |= {p.name for p in table.projects.values() if p.kind in CONSUMER_MAY_USE}
    return allowed


def excused_by(table: Table, edge: Edge) -> int | None:
    """The index of the exception that excuses an edge, if one does."""
    for i, e in enumerate(table.exceptions):
        if (
            e.source == edge.source
            and e.target == edge.destination
            and (not e.paths or edge.file.startswith(e.paths))
        ):
            return i
    return None


def load_debt(directory: Path) -> dict[tuple[str, str], str]:
    """The known debts: (including file, spelling) -> the debt file that lists it."""
    debt: dict[tuple[str, str], str] = {}
    if not directory.is_dir():
        return debt
    for path in sorted(directory.glob("*.txt")):
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 2:
                raise ValueError(f"{path.name}:{number}: want '<file> <spelling>', got {raw!r}")
            debt[(fields[0], fields[1])] = path.name
    return debt


def table_problems(table: Table, files: list[str]) -> list[str]:
    """Problems in the table itself: unknown names, the rules of the kinds, an empty path."""
    problems: list[str] = []
    for project in table.projects.values():
        if project.kind not in ALLOWED:
            problems.append(
                f"{project.name} has the kind {project.kind!r}, which is not one of "
                f"{sorted(ALLOWED)}"
            )
            continue
        for used in project.may_use:
            if used not in table.projects:
                problems.append(
                    f"{project.name} may use {used}, which is not a project of the table"
                )
                continue
            kind = table.projects[used].kind
            if kind not in ALLOWED[project.kind]:
                problems.append(
                    f"{project.name} ({project.kind}) may use {used} ({kind}): a {project.kind} "
                    f"uses {', '.join(sorted(ALLOWED[project.kind])) or 'nothing'} only"
                )
        if project.name in project.may_use:
            problems.append(f"{project.name} lists itself")
        if project.kind == "app-library" and not project.internal:
            problems.append(f"{project.name} is an app-library and so internal: say so")
        if not any(f == project.path or f.startswith(project.path + "/") for f in files):
            problems.append(
                f"{project.name} is in the table and nothing is filed under {project.path}"
            )
    for e in table.exceptions:
        for end in (e.source, e.target):
            if end not in table.projects:
                problems.append(f"an exception names {end}, which is not a project of the table")
        if not e.why:
            problems.append(f"the exception {e.source} -> {e.target} gives no reason")
    graph = {name: list(p.may_use) for name, p in table.projects.items()}
    problems.extend(f"cycle in the table: {' -> '.join(c)}" for c in cycles(graph))
    return problems


def cycles(graph: dict[str, list[str]]) -> list[list[str]]:
    """Every strongly connected set of more than one project, by Tarjan's algorithm."""
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    found: list[list[str]] = []
    counter = [0]

    def visit(node: str) -> None:
        index[node] = low[node] = counter[0]
        counter[0] += 1
        stack.append(node)
        on_stack.add(node)
        for other in graph.get(node, []):
            if other not in graph:
                continue
            if other not in index:
                visit(other)
                low[node] = min(low[node], low[other])
            elif other in on_stack:
                low[node] = min(low[node], index[other])
        if low[node] == index[node]:
            members = []
            while True:
                member = stack.pop()
                on_stack.discard(member)
                members.append(member)
                if member == node:
                    break
            if len(members) > 1:
                found.append(sorted(members))

    for name in sorted(graph):
        if name not in index:
            visit(name)
    return sorted(found)


def uncovered(table: Table, files: list[str]) -> list[str]:
    """The directories under the trees the table covers that hold C/C++ or CMake and no project."""
    where: set[str] = set()
    for f in files:
        if not f.startswith(COVERED_ROOTS) or table.project_of(f) is not None:
            continue
        if posixpath.splitext(f)[1].lower() in SOURCE_EXT or CMAKE_FILE.search(f):
            where.add("/".join(f.split("/")[:2]))
    return sorted(where)


def install_problems(root: Path, table: Table, files: list[str]) -> list[str]:
    """An install() that names an internal project's target or path, outside comments."""
    internal = {p.name: p for p in table.projects.values() if p.internal}
    if not internal:
        return []
    owners = target_owners(root, files, table)
    names = {t: owner for t, owner in owners.items() if owner in internal}
    problems = []
    for f in table.install_files:
        path = root / f
        if not path.is_file():
            problems.append(f"install_files lists {f}, which is not a file")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for command, args, line in cmake_commands(text):
            if command != "install":
                continue
            for arg in args:
                bare = GENERATOR_EXPR.sub("", arg)
                hit = names.get(bare) or next(
                    (p.name for p in internal.values() if p.path in bare), None
                )
                if hit:
                    problems.append(
                        f"{f}:{line}: install() names {arg}, which belongs to the internal "
                        f"project {hit}"
                    )
    return problems


def print_edges(edges: list[Edge]) -> None:
    counts: dict[tuple[str, str, str], int] = defaultdict(int)
    for e in edges:
        counts[(e.source, e.destination, e.via)] += 1
    for (source, destination, via), n in sorted(counts.items()):
        print(f"{source} -> {destination} ({via}): {n}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", type=Path, default=HERE.parent.parent, help="repository root")
    parser.add_argument("--table", type=Path, default=DEFAULT_TABLE, help="the table of projects")
    parser.add_argument("--debt", type=Path, default=DEFAULT_DEBT, help="the known-debt directory")
    parser.add_argument("--edges", action="store_true", help="print the project edges and stop")
    parser.add_argument(
        "--debt-lines", action="store_true", help="print the forbidden includes as debt lines"
    )
    args = parser.parse_args(argv)

    table = load_table(args.table)
    files = tracked_files(args.root)
    if not any(table.project_of(f) for f in files):
        print(
            f"::error::check_layering: no file of {args.root} is in a project; nothing was checked"
        )
        return 1
    edges = include_edges(args.root, files, table) + link_edges(args.root, files, table)
    if args.edges:
        print_edges(edges)
        return 0

    def forbidden(e: Edge) -> bool:
        return e.destination not in allowed_for(table, e.file, e.source)

    if args.debt_lines:
        for e in edges:
            if e.via == "include" and forbidden(e) and excused_by(table, e) is None:
                print(f"{e.file} {e.what}  # {e.source} -> {e.destination}")
        return 0

    failures = 0
    for problem in table_problems(table, files):
        print(f"::error::check_layering: {problem}")
        failures += 1
    for where in uncovered(table, files):
        print(
            f"::error::check_layering: {where}/ holds C/C++ or CMake files of no project of the "
            "table"
        )
        failures += 1

    debt = load_debt(args.debt)
    seen_debt: set[tuple[str, str]] = set()
    used_exceptions: set[int] = set()
    known = excused = 0
    for e in edges:
        if not forbidden(e):
            continue
        index = excused_by(table, e)
        if index is not None:
            used_exceptions.add(index)
            excused += 1
            continue
        key = (e.file, e.what)
        if e.via == "include" and key in debt:
            seen_debt.add(key)
            known += 1
            continue
        row = ", ".join(table.projects[e.source].may_use) or "nothing"
        verb = "include" if e.via == "include" else "link"
        print(
            f"::error file={e.file},line={e.line}::check_layering: {e.source} may not {verb} "
            f"{e.destination} ({e.what}); the table lists {row} for it"
        )
        failures += 1
    for i, e in enumerate(table.exceptions):
        if i not in used_exceptions:
            print(
                f"::error::check_layering: the exception {e.source} -> {e.target} excuses no edge "
                "any more: delete it"
            )
            failures += 1
    for (path, spelling), origin in sorted(debt.items()):
        if (path, spelling) not in seen_debt:
            print(
                f"::error::check_layering: {origin} lists {path} including {spelling}, which is "
                f"no longer a forbidden include: delete that line (and the file when it is empty)"
            )
            failures += 1

    # An installed header (under a non-internal library's include/) that includes an internal one.
    for e in edges:
        if e.via != "include":
            continue
        source, destination = table.projects[e.source], table.projects[e.destination]
        if (
            destination.internal
            and not source.internal
            and source.kind == "library"
            and "/include/" in e.file
        ):
            print(
                f"::error file={e.file},line={e.line}::check_layering: an installed header of "
                f"{source.name} includes {e.what}, which belongs to the internal project "
                f"{destination.name}"
            )
            failures += 1
    for problem in install_problems(args.root, table, files):
        print(f"::error::check_layering: {problem}")
        failures += 1

    print(
        f"check_layering: {len(table.projects)} projects, {len(edges)} edges between them "
        f"({sum(e.via == 'include' for e in edges)} includes, "
        f"{sum(e.via == 'link' for e in edges)} "
        f"link lines), {excused} excused by {len(table.exceptions)} exceptions, {known} known "
        f"debts, {failures} failures"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
