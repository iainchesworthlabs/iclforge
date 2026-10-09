#!/usr/bin/env python3
"""Hold sonar-project.properties to the tree: what SonarCloud scans is what the file says.

    python3 tools/checks/check_sonar_scope.py [--root <repo>] [--properties <file>] [--unscanned]

The properties file divides the repository three ways with Ant-style patterns, and a move of files
(planning/monorepo.md, C7) changes which side a file falls on without a word from anyone:

  - sonar.sources lists the directories that are scanned, and sonar.exclusions takes files out of
    them;
  - sonar.tests lists the directories that hold tests, and sonar.test.inclusions says which of the
    files in them (and in sonar.sources) are tests. A test that is also scanned as source is counted
    twice, and a test that is scanned as source and not as a test has its issues held to the rules
    of production code;
  - sonar.coverage.exclusions, sonar.cpd.exclusions and the issue-ignore rules (the multicriteria
    resourceKeys) name files by pattern, and a pattern that matches nothing is a rule that quietly
    stopped applying.

It fails on: a source or test root that is not in the tree; a pattern that matches no tracked file;
a test (a file under a tests/ directory, or a library's fuzz/, that is scanned) that is not matched
by sonar.test.inclusions, and a test that sonar.exclusions does not take out of the sources. Only
files SonarCloud analyses by extension count. A tree of tests that is scanned as source on purpose
says so in the properties file, in a comment of the form

    # check_sonar_scope: tests-as-sources apps/demos/wasm/tests, bindings/js/tests

so that the decision is written where Sonar's configuration is, and a tree that is not named fails.
`--unscanned` also lists the top-level directories with analysable files that no root covers, so
that leaving firmware/ out is a decision someone can read.

Stdlib only. Runs in the static job and in precheck.py.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ANALYSED = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".py", ".js", ".mjs", ".ts", ".java",
    ".kt",
}  # fmt: skip
# A directory that holds tests: tests/ anywhere, and the libraries' fuzz/ harnesses (tools/fuzz is
# the drivers that run them, which are scripts).
TEST_DIRS = {"tests"}
FUZZ_UNDER = ("libs/",)
# Patterns for what is not in the tree on purpose: build output, installed packages, minified files.
FOR_UNTRACKED = {"build/**", "**/node_modules/**", "**/*.min.js"}
LIST_KEYS = (
    "sonar.exclusions",
    "sonar.test.inclusions",
    "sonar.coverage.exclusions",
    "sonar.cpd.exclusions",
)
DELIBERATE = re.compile(r"^#\s*check_sonar_scope:\s*tests-as-sources\s+(.+)$")
RESOURCE_KEY = re.compile(r"^sonar\.issue\.ignore\.multicriteria\.[^.]+\.resourceKey$")


def parse_properties(text: str) -> dict[str, str]:
    """A Java properties file: `key=value`, `#` comments, a trailing backslash continues a line."""
    out: dict[str, str] = {}
    pending = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not pending and (not line or line.startswith(("#", "!"))):
            continue
        pending += line[:-1].rstrip() if line.endswith("\\") else line
        if line.endswith("\\"):
            continue
        key, _, value = pending.partition("=")
        out[key.strip()] = value.strip()
        pending = ""
    return out


def deliberate_trees(text: str) -> list[str]:
    """The trees of tests the file says it scans as source on purpose."""
    out: list[str] = []
    for line in text.splitlines():
        m = DELIBERATE.match(line.strip())
        if m:
            out.extend(split_list(m.group(1)))
    return [tree.rstrip("/") for tree in out]


def split_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Ant-style: `**` any number of directories, `*` and `?` within one name."""
    pattern = pattern.strip().lstrip("./")
    if pattern.endswith("/"):
        pattern += "**"
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        elif pattern[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(out + r"\Z")


def under(path: str, roots: list[str]) -> bool:
    return any(path == r.rstrip("/") or path.startswith(r.rstrip("/") + "/") for r in roots)


def check(
    properties: dict[str, str], files: list[str], on_purpose: list[str] | None = None
) -> tuple[list[str], dict[str, int]]:
    problems: list[str] = []
    on_purpose = on_purpose or []
    sources = split_list(properties.get("sonar.sources", ""))
    tests = split_list(properties.get("sonar.tests", ""))
    tracked = set(files)

    for key, roots in (("sonar.sources", sources), ("sonar.tests", tests)):
        for root in roots:
            name = root.strip("/")
            if name not in tracked and not any(f.startswith(name + "/") for f in files):
                problems.append(f"{key} names {name}, which nothing in the tree is filed under")

    patterns: dict[str, list[str]] = {k: split_list(properties.get(k, "")) for k in LIST_KEYS}
    for key, value in properties.items():
        if RESOURCE_KEY.match(key):
            patterns.setdefault("an issue-ignore resourceKey", []).append(value)
    for key, items in patterns.items():
        for item in items:
            rx = glob_to_regex(item)
            if item not in FOR_UNTRACKED and not any(rx.match(f) for f in files):
                problems.append(f"{key}: {item} matches no file of the tree")

    for tree in on_purpose:
        if not any(f.startswith(tree + "/") for f in files):
            problems.append(
                f"tests-as-sources names {tree}, which nothing in the tree is filed under"
            )

    exclusions = [glob_to_regex(p) for p in patterns["sonar.exclusions"]]
    inclusions = [glob_to_regex(p) for p in patterns["sonar.test.inclusions"]]
    counts = {"source": 0, "test": 0, "excluded": 0}
    for f in files:
        if Path(f).suffix not in ANALYSED:
            continue
        in_source_root = under(f, sources)
        in_test_root = under(f, tests)
        if not (in_source_root or in_test_root):
            continue
        excluded = any(rx.match(f) for rx in exclusions)
        is_test = in_test_root and any(rx.match(f) for rx in inclusions)
        folders = set(f.split("/")[:-1])
        looks_like_test = bool(TEST_DIRS & folders) or (
            "fuzz" in folders and f.startswith(FUZZ_UNDER)
        )
        if is_test:
            counts["test"] += 1
            if in_source_root and not excluded:
                problems.append(
                    f"{f} is a test and is also scanned as source: sonar.exclusions lacks it"
                )
        elif in_source_root and not excluded:
            counts["source"] += 1
            if looks_like_test and not under(f, on_purpose):
                problems.append(
                    f"{f} is under a tests/ or fuzz/ directory and is scanned as source"
                )
        else:
            counts["excluded"] += 1
    return problems, counts


def unscanned(properties: dict[str, str], files: list[str]) -> list[str]:
    roots = split_list(properties.get("sonar.sources", "")) + split_list(
        properties.get("sonar.tests", "")
    )
    tops: dict[str, int] = {}
    for f in files:
        if Path(f).suffix in ANALYSED and not under(f, roots):
            top = "/".join(f.split("/")[:2]) if f.count("/") > 1 else f.split("/")[0]
            tops[top] = tops.get(top, 0) + 1
    return [f"{top} ({n} files)" for top, n in sorted(tops.items())]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", type=Path, default=HERE.parents[1])
    ap.add_argument("--properties", type=Path, default=None)
    ap.add_argument("--unscanned", action="store_true", help="also list what no root covers")
    args = ap.parse_args(argv)
    path = args.properties or args.root / "sonar-project.properties"
    text = path.read_text(encoding="utf-8")
    properties = parse_properties(text)
    out = subprocess.run(
        ["git", "-C", str(args.root), "ls-files"], capture_output=True, text=True, check=True
    )
    files = [f for f in out.stdout.splitlines() if f]
    problems, counts = check(properties, files, deliberate_trees(text))
    for problem in problems:
        print(f"::error::check_sonar_scope: {problem}")
    print(
        f"check_sonar_scope: {counts['source']} source, {counts['test']} test and "
        f"{counts['excluded']} excluded files scanned, {len(problems)} problems"
    )
    if args.unscanned:
        for line in unscanned(properties, files):
            print(f"  not scanned: {line}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
