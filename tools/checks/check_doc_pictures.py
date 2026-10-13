#!/usr/bin/env python3
"""Assert every picture the documentation shows says how it was made.

A screenshot goes stale without anybody editing it: the program it shows moves on. Whether it can
be taken again then depends on something having written down, when it was taken, which program
drew it, with what command line, in what environment and from which build. The guide's first
karaoke pictures were taken by a harness that was thrown away, and could not be.

tools/pictures/pictures.json is where that is written, one entry per picture, and
tools/pictures/take_pictures.py takes the ones a command line can take. This check holds the two
together with the pages, stdlib-only, from _static.yml's script-lint job and the same way locally:

    python3 tools/checks/check_doc_pictures.py [--root <repo>]

(a) Every PNG file a Markdown page under docs/ or README.md embeds, as `![...](path.png)` or as
    an `<img src="path.png">`, has an entry in the manifest. A page's own pictures only: a
    picture at an http(s) address is somebody else's file (the spectrograms a workflow
    publishes), and the favicons the copied demo pages under docs/assets/ link are not pictures
    of anything. Fenced code and code spans are examples of the syntax, not embeds. The text
    of an embed may wrap across lines, so a page is matched whole and not a line at a time.

(b) Every entry names a file that exists and that some page embeds: one of those pages, or the
    README.md of a project (apps/, firmware/, examples/ and the rest), which may show a picture
    kept under docs/. An entry for a picture no page shows any more is the manifest going stale
    in its turn, so the picture and its entry leave together.

(c) Every entry is whole: a path, the program, the build it was taken from, and either the
    arguments take_pictures.py runs the program with (`args`, with `{out}` where the picture is
    written, in an `environment` the manifest defines) or, for a picture no command line on a
    developer's desk can take (an Android device, a virtual machine with a driver in it), how
    it was made, in words and commands (`by_hand`). Never both, never neither.

(d) Every build the manifest names is a commit of this repository, when the checkout has the
    history to say so. A picture taken from a commit that was later rebased away would name a
    build nobody can check out; a shallow clone, or a tree that is not a checkout, cannot tell,
    and says that it did not look.

Exit 1 with one ::error:: line per problem.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote

MANIFEST = "tools/pictures/pictures.json"
FORMAT = "iclforge-doc-pictures"
VERSION = 1
# What a page is: the site's own pages and the repository's front page.
PAGE_GLOBS = ("docs/**/*.md",)
PAGE_FILES = ("README.md",)
# The projects' own front pages, which may show a picture that has an entry without having to.
PROJECT_PAGE_GLOBS = tuple(
    f"{top}/**/README.md" for top in ("apps", "bindings", "examples", "firmware", "libs", "tools"))
# What take_pictures.py fills in an argument: where the picture is written, the folder
# make_media.py fills, and a folder for what a run writes besides its picture.
PLACEHOLDERS = ("out", "media", "scratch")

MARKDOWN_IMAGE = re.compile(r"!\[[^\[\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_IMAGE = re.compile(r"<img\b[^>]*?\bsrc\s*=\s*[\"']([^\"']+)[\"']", re.IGNORECASE)
CODE_SPAN = re.compile(r"`[^`\n]*`")
FENCE = re.compile(r"^ {0,3}(```|~~~)")
SCHEME = re.compile(r"^[a-z][a-z0-9+.-]*:", re.IGNORECASE)
PLACEHOLDER = re.compile(r"\{([^{}]*)\}")
COMMIT = re.compile(r"^[0-9a-f]{40}$")


@dataclass
class Report:
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    embedded: int = 0
    entries: int = 0


def load(root: Path) -> dict:
    """The manifest as it is written. Raises ValueError with a sentence when it is not there or
    is not JSON; what it holds is judged by check_manifest()."""
    path = root / MANIFEST
    if not path.is_file():
        raise ValueError(f"{MANIFEST} is not there")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{MANIFEST} is not JSON: {error}") from error
    if not isinstance(document, dict):
        raise ValueError(f"{MANIFEST} is not a JSON object")
    return document


def pages(root: Path) -> list[Path]:
    found = [p for pattern in PAGE_GLOBS for p in root.glob(pattern) if p.is_file()]
    found += [root / name for name in PAGE_FILES if (root / name).is_file()]
    return sorted(set(found))


def project_pages(root: Path) -> list[Path]:
    return sorted({p for pattern in PROJECT_PAGE_GLOBS for p in root.glob(pattern) if p.is_file()})


def _scrubbed(text: str) -> str:
    """The page with fenced code and code spans blanked, every line still in its place."""
    lines: list[str] = []
    fenced = False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            lines.append("")
        elif fenced:
            lines.append("")
        else:
            lines.append(CODE_SPAN.sub(lambda span: " " * len(span.group()), line))
    return "\n".join(lines)


def embedded_pictures(root: Path, among: list[Path] | None = None) -> dict[str, list[str]]:
    """Each PNG those pages embed (pages() unless told), as a path from the root, with the
    `page:line` every embed opens on."""
    found: dict[str, list[str]] = {}
    for page in pages(root) if among is None else among:
        where = page.relative_to(root).as_posix()
        text = _scrubbed(page.read_text(encoding="utf-8"))
        matches = list(MARKDOWN_IMAGE.finditer(text)) + list(HTML_IMAGE.finditer(text))
        for match in sorted(matches, key=lambda m: m.start()):
            target = unquote(match.group(1).split("#", 1)[0].split("?", 1)[0])
            if SCHEME.match(target) or not target.lower().endswith(".png"):
                continue
            resolved = root / target.lstrip("/") if target.startswith("/") else page.parent / target
            try:
                relative = resolved.resolve().relative_to(root.resolve()).as_posix()
            except ValueError:
                continue  # outside the tree: check_doc_paths.py says so
            number = text.count("\n", 0, match.start()) + 1
            found.setdefault(relative, []).append(f"{where}:{number}")
    return found


def _text(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _check_environments(document: dict, report: Report) -> set[str]:
    environments = document.get("environments")
    if not isinstance(environments, dict):
        report.problems.append(f"{MANIFEST}: \"environments\" must be an object")
        return set()
    for name, environment in environments.items():
        at = f"{MANIFEST}: environment \"{name}\""
        if not isinstance(environment, dict):
            report.problems.append(f"{at} must be an object")
            continue
        if not _text(environment.get("what")):
            report.problems.append(f"{at} does not say what it is (\"what\")")
        variables = environment.get("set", {})
        if not isinstance(variables, dict) or not all(
                isinstance(k, str) and isinstance(v, str) for k, v in variables.items()):
            report.problems.append(f"{at}: \"set\" must map names to text")
        unset = environment.get("unset", [])
        if not isinstance(unset, list) or not all(isinstance(k, str) for k in unset):
            report.problems.append(f"{at}: \"unset\" must be a list of names")
    return set(environments)


def _check_builds(document: dict, report: Report) -> dict[str, str]:
    """The builds by name, each with its commit."""
    builds = document.get("builds")
    if not isinstance(builds, dict):
        report.problems.append(f"{MANIFEST}: \"builds\" must be an object")
        return {}
    commits: dict[str, str] = {}
    for name, build in builds.items():
        at = f"{MANIFEST}: build \"{name}\""
        if not isinstance(build, dict):
            report.problems.append(f"{at} must be an object")
            continue
        commit = build.get("commit")
        if not isinstance(commit, str) or not COMMIT.match(commit):
            report.problems.append(f"{at}: \"commit\" must be the commit's full 40-digit name")
        else:
            commits[name] = commit
        if not _text(build.get("what")):
            report.problems.append(
                f"{at} does not say what was built (\"what\": the configuration, the compiler, Qt)")
    return commits


def _check_entry(entry: object, index: int, environments: set[str], builds: set[str],
                 report: Report) -> str | None:
    """The entry's path when it has a usable one."""
    if not isinstance(entry, dict):
        report.problems.append(f"{MANIFEST}: picture {index} must be an object")
        return None
    path = entry.get("path")
    if not _text(path):
        report.problems.append(f"{MANIFEST}: picture {index} has no \"path\"")
        return None
    at = f"{MANIFEST}: {path}"
    if not _text(entry.get("program")):
        report.problems.append(f"{at} does not name the program that drew it (\"program\")")
    if entry.get("build") not in builds:
        report.problems.append(
            f"{at}: \"build\" must name one of the manifest's builds, not {entry.get('build')!r}")
    has_args = "args" in entry
    has_by_hand = "by_hand" in entry
    if has_args == has_by_hand:
        report.problems.append(
            f"{at} needs either \"args\" (a command line take_pictures.py runs) or \"by_hand\" "
            "(how it was made), and not both")
    if has_args:
        args = entry["args"]
        if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
            report.problems.append(f"{at}: \"args\" must be a list of text arguments")
        else:
            names = [name for a in args for name in PLACEHOLDER.findall(a)]
            for name in names:
                if name not in PLACEHOLDERS:
                    report.problems.append(
                        f"{at}: {{{name}}} is not a placeholder; they are "
                        + ", ".join("{" + p + "}" for p in PLACEHOLDERS))
            if names.count("out") != 1:
                report.problems.append(
                    f"{at}: \"args\" must say where the picture is written, with {{out}}, once")
        if entry.get("environment") not in environments:
            report.problems.append(
                f"{at}: \"environment\" must name one of the manifest's environments, "
                f"not {entry.get('environment')!r}")
        extra = entry.get("env", {})
        if not isinstance(extra, dict) or not all(
                isinstance(k, str) and isinstance(v, str) for k, v in extra.items()):
            report.problems.append(f"{at}: \"env\" must map names to text")
    if has_by_hand and not _text(entry["by_hand"]):
        report.problems.append(f"{at}: \"by_hand\" must say how the picture was made")
    return path


def check_manifest(document: dict, root: Path, report: Report,
                   files_exist: bool = True) -> dict[str, dict]:
    """Judges the manifest on its own, and returns its usable entries by path. take_pictures.py
    asks with `files_exist` off: a new entry's picture is not there until it has been taken."""
    if document.get("format") != FORMAT or document.get("version") != VERSION:
        report.problems.append(
            f"{MANIFEST}: \"format\" must be \"{FORMAT}\" and \"version\" {VERSION}")
    environments = _check_environments(document, report)
    builds = _check_builds(document, report)
    pictures = document.get("pictures")
    if not isinstance(pictures, list):
        report.problems.append(f"{MANIFEST}: \"pictures\" must be a list")
        return {}
    entries: dict[str, dict] = {}
    for index, entry in enumerate(pictures):
        path = _check_entry(entry, index, environments, set(builds), report)
        if path is None:
            continue
        if path in entries:
            report.problems.append(f"{MANIFEST}: {path} has two entries")
            continue
        entries[path] = entry
        if Path(path).is_absolute() or ".." in Path(path).parts or "\\" in path:
            report.problems.append(
                f"{MANIFEST}: {path} must be a path from the repository's root, with /")
        elif files_exist and not (root / path).is_file():
            report.problems.append(f"{MANIFEST}: {path} is not a file")
    used = {entry.get("build") for entry in entries.values()}
    for name in builds:
        if name not in used:
            report.problems.append(f"{MANIFEST}: build \"{name}\" is the build of no picture")
    check_commits(builds, root, report)
    report.entries = len(entries)
    return entries


def _git(root: Path, *args: str) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(["git", "-C", str(root), *args], check=False, capture_output=True,
                              text=True)
    except OSError:
        return None


def check_commits(commits: dict[str, str], root: Path, report: Report) -> None:
    """Each build's commit is one this checkout has, where it can tell."""
    if not commits:
        return
    top = _git(root, "rev-parse", "--show-toplevel")
    if top is None or top.returncode != 0 or Path(top.stdout.strip()).resolve() != root.resolve():
        # Not a checkout, or a folder inside somebody else's: its history is not this tree's.
        report.notes.append("not a git checkout: the builds' commits were not looked up")
        return
    shallow = _git(root, "rev-parse", "--is-shallow-repository")
    if shallow is not None and shallow.stdout.strip() == "true":
        report.notes.append("a shallow clone: the builds' commits were not looked up")
        return
    for name, commit in sorted(commits.items()):
        found = _git(root, "cat-file", "-e", f"{commit}^{{commit}}")
        if found is None or found.returncode != 0:
            report.problems.append(
                f"{MANIFEST}: build \"{name}\" names commit {commit}, which this repository "
                "does not have")


def check_tree(root: Path) -> Report:
    report = Report()
    try:
        document = load(root)
    except ValueError as error:
        report.problems.append(str(error))
        return report
    entries = check_manifest(document, root, report)
    embedded = embedded_pictures(root)
    report.embedded = len(embedded)
    for path, places in sorted(embedded.items()):
        if path not in entries:
            report.problems.append(
                f"{places[0]}: {path} has no entry in {MANIFEST}: say which program drew it, "
                "with what command line and environment, and from which build")
    elsewhere = embedded_pictures(root, project_pages(root))
    for path in sorted(entries):
        if path not in embedded and path not in elsewhere and (root / path).is_file():
            report.problems.append(
                f"{MANIFEST}: {path} is embedded by no page under docs/, README.md or a "
                "project's README.md: remove the entry with the picture")
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: two directories above this script)",
    )
    args = parser.parse_args(argv[1:])
    root = args.root.resolve()
    report = check_tree(root)
    for note in report.notes:
        print(f"info: {note}")
    for problem in report.problems:
        print(f"::error::{problem}")
    print(
        f"{len(report.problems)} problem(s): {report.embedded} picture(s) embedded, "
        f"{report.entries} in {MANIFEST}, under {root}"
    )
    return 1 if report.problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
