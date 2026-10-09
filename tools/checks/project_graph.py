#!/usr/bin/env python3
"""The project graph of tools/checks/projects.json, as one module for everything that reads it.

`check_layering.py` holds the tree to the table; the CI planners (`tools/ci/classify_changes.py`,
`tools/ci/plan_gate.py`) ask it which project a changed path belongs to, which lanes that project
is built and tested in, and which other projects an excused edge reaches into it from. They did
this with directory lists of their own before (planning/monorepo.md, decision 12); a project is a
row of the table now, and a path the table does not know is an unknown path to the planners too.

    python3 tools/checks/project_graph.py affected <path>...    # the projects a change reaches
    python3 tools/checks/project_graph.py lanes <path>...       # and the lanes they light

Stdlib only, because the gate's job checks out this file, the table and the planner and nothing
else.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TABLE = HERE / "projects.json"
# A project's tests/ and fuzz/ directories are its consumers.
CONSUMER_DIRS = ("tests", "fuzz")


@dataclass(frozen=True)
class Project:
    name: str
    kind: str
    path: str
    internal: bool
    may_use: tuple[str, ...]
    # The lanes of tools/ci/classify_changes.py this project's own tree lights.
    lanes: tuple[str, ...] = ()
    # Firmware only: the libraries its package ships as source (pack_esp_component.py), whose
    # change lights the project's lanes after a merge as well as in the nightly run.
    ships: tuple[str, ...] = ()
    # The directories, besides include/, that other projects may include from: the headers a
    # project shares on purpose and does not install (base's internal/, the source of an
    # app-library, which is compiled into its programs). Any other header is private.
    exposes: tuple[str, ...] = ()


@dataclass(frozen=True)
class Exception_:
    source: str
    target: str
    paths: tuple[str, ...]
    why: str
    # The files of the target the excused edge reaches, as path prefixes. Empty: any file of it.
    to_paths: tuple[str, ...] = ()


@dataclass(frozen=True)
class Table:
    projects: dict[str, Project]
    exceptions: tuple[Exception_, ...]
    install_files: tuple[str, ...]
    lanes: tuple[str, ...] = ()
    by_length: tuple[Project, ...] = field(default=(), compare=False)

    def project_of(self, path: str) -> Project | None:
        """The project whose path is the longest one above `path`."""
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

    def published(self, name: str, path: str) -> bool:
        """Whether another project may include `path`, a header of the project `name`."""
        project = self.projects[name]
        return path.startswith((project.path + "/include/", *project.exposes))

    def reached_from(self, path: str) -> list[Project]:
        """The projects an excused edge reaches `path` from: the ones whose files include it.

        A change to such a file is a change to what those projects build, though the table
        lists the target as nothing they may use.
        """
        target = self.project_of(path)
        if target is None:
            return []
        out = []
        for e in self.exceptions:
            if e.target != target.name or not e.to_paths or not path.startswith(e.to_paths):
                continue
            source = self.projects.get(e.source)
            if source is not None and source not in out:
                out.append(source)
        return out

    def dependents(self, name: str) -> set[str]:
        """Every project that uses `name`, directly or through others (excused edges included)."""
        users: dict[str, set[str]] = {n: set() for n in self.projects}
        for p in self.projects.values():
            for used in p.may_use:
                users.setdefault(used, set()).add(p.name)
        for e in self.exceptions:
            users.setdefault(e.target, set()).add(e.source)
        found: set[str] = set()
        todo = [name]
        while todo:
            for user in users.get(todo.pop(), ()):
                if user not in found and user != name:
                    found.add(user)
                    todo.append(user)
        return found

    def affected(self, path: str) -> set[str] | None:
        """The projects a change to `path` reaches: its own and everything that uses it.

        None when it cannot be said: a path no project holds, or one every project reads (the
        test support, the build files).
        """
        project = self.project_of(path)
        if project is None or project.kind == "tests":
            return None
        names = {project.name} | self.dependents(project.name)
        for source in self.reached_from(path):
            names |= {source.name} | self.dependents(source.name)
        return names


def load_table(path: Path = DEFAULT_TABLE) -> Table:
    raw = json.loads(path.read_text(encoding="utf-8"))
    projects = {
        name: Project(
            name=name,
            kind=row["kind"],
            path=row["path"].rstrip("/"),
            internal=bool(row.get("internal", False)),
            may_use=tuple(row.get("may_use", [])),
            lanes=tuple(row.get("lanes", [])),
            ships=tuple(row.get("ships", [])),
            exposes=tuple(row.get("exposes", [])),
        )
        for name, row in raw["projects"].items()
    }
    exceptions = tuple(
        Exception_(
            e["from"],
            e["to"],
            tuple(e.get("paths", [])),
            e.get("why", ""),
            tuple(e.get("to_paths", [])),
        )
        for e in raw.get("exceptions", [])
    )
    ordered = tuple(sorted(projects.values(), key=lambda p: -len(p.path)))
    return Table(
        projects,
        exceptions,
        tuple(raw.get("install_files", [])),
        tuple(raw.get("lanes", [])),
        ordered,
    )


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("affected", "lanes"):
        print(__doc__, file=sys.stderr)
        return 2
    table = load_table()
    names: set[str] = set()
    for path in argv[2:]:
        hit = table.affected(path)
        if hit is None:
            print(f"{path}: not in one project, or read by all of them", file=sys.stderr)
            hit = set(table.projects)
        names |= hit
    if argv[1] == "affected":
        print(",".join(sorted(names)))
    else:
        print(",".join(sorted({lane for n in names for lane in table.projects[n].lanes})))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
