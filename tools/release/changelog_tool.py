#!/usr/bin/env python3
"""Changelog fragments: assemble them at release time, preview them, move a branch's entries.

A pull request does not edit CHANGELOG.md: every PR appending at the top of `## [Unreleased]`
is what made it collide. Each change adds its own file, changelog.d/<slug>.<type>.md, and a
release moves them all into a section of CHANGELOG.md in one step (changelog.d/README.md has
the format; docs/releasing.md has the release step).

    changelog_tool.py assemble --version 0.11.0-beta.1 --date 2026-11-14 [--root .]
        Write "## [0.11.0-beta.1] - 2026-11-14" under "## [Unreleased]": the fragments grouped
        under ### Added / Changed / Deprecated / Removed / Fixed / Security, then whatever the
        hand-written "## [Unreleased]" held before fragments existed (kept as it was, so the
        entries written before the migration are not rewritten). The fragments are deleted and
        "## [Unreleased]" is left holding a pointer to changelog.d. Exit 1 when there is
        nothing to assemble or the version is already there.

    changelog_tool.py pending [--root .]
        Print what assemble would write, without changing a file.

    changelog_tool.py extract --base github/main [--write] [--restore] [--root .]
        For a branch written before the migration: find the entries it added to CHANGELOG.md
        since it left <base> and show the fragment each becomes. --write creates the files,
        --restore also puts CHANGELOG.md back as it was at the merge base, so the branch no
        longer touches it. A lead-in that does not start with a type ("**Added: ...**") needs
        --default-type.

Standard library only, like the other scripts under tools/.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

TYPES = ("added", "changed", "deprecated", "removed", "fixed", "security")
NAME = re.compile(r"^(?P<slug>[A-Za-z0-9][A-Za-z0-9._-]*)\.(?P<type>" + "|".join(TYPES) + r")\.md$")
SECTION = re.compile(r"^## \[", re.MULTILINE)
UNRELEASED = re.compile(r"^## \[Unreleased\][^\n]*\n", re.MULTILINE)
POINTER = (
    "Changes pending release are one file each in [`changelog.d/`](changelog.d/README.md); "
    "a release moves them into the section below.\n"
)


def collect(root: Path) -> tuple[dict[str, list[str]], list[Path]]:
    groups: dict[str, list[str]] = {t: [] for t in TYPES}
    files: list[Path] = []
    for path in sorted((root / "changelog.d").glob("*.md")):
        m = NAME.match(path.name)
        if m is None:
            continue
        groups[m.group("type")].append(path.read_text(encoding="utf-8").strip())
        files.append(path)
    return groups, files


def bullet(text: str) -> str:
    """A fragment as a list item: the first line after the dash, the rest indented under it."""
    lines = text.splitlines()
    out = ["- " + lines[0]]
    out.extend(("  " + ln) if ln.strip() else "" for ln in lines[1:])
    return "\n".join(out) + "\n"


def render_groups(groups: dict[str, list[str]]) -> str:
    parts: list[str] = []
    for t in TYPES:
        if groups[t]:
            parts.append(f"### {t.capitalize()}\n\n" + "\n".join(bullet(x) for x in groups[t]))
    return "\n".join(parts)


def split_unreleased(text: str) -> tuple[str, str, str]:
    """(everything up to and including the Unreleased heading, its body, the rest)."""
    m = UNRELEASED.search(text)
    if m is None:
        raise ValueError("CHANGELOG.md has no '## [Unreleased]' heading")
    nxt = SECTION.search(text, m.end())
    end = nxt.start() if nxt else len(text)
    return text[: m.end()], text[m.end() : end], text[end:]


def assemble_text(text: str, groups: dict[str, list[str]], version: str, date: str) -> str:
    if re.search(rf"^## \[{re.escape(version)}\]", text, re.MULTILINE):
        raise ValueError(f"CHANGELOG.md already has a section for {version}")
    head, body, rest = split_unreleased(text)
    legacy = body.replace(POINTER, "").strip("\n")
    if not any(groups.values()) and not legacy:
        raise ValueError("nothing to assemble: no fragments and nothing under '## [Unreleased]'")
    pieces = [render_groups(groups).rstrip("\n"), legacy]
    section = f"## [{version}] - {date}\n\n" + "\n\n".join(p for p in pieces if p) + "\n\n"
    return head + "\n" + POINTER + "\n" + section + rest


def assemble(root: Path, version: str, date: str) -> int:
    groups, files = collect(root)
    changelog = root / "CHANGELOG.md"
    try:
        new = assemble_text(changelog.read_text(encoding="utf-8"), groups, version, date)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    changelog.write_text(new, encoding="utf-8", newline="\n")
    for f in files:
        f.unlink()
    print(f"assembled {len(files)} fragment(s) into {version}")
    return 0


def pending(root: Path) -> int:
    groups, files = collect(root)
    if not files:
        print("no fragments in changelog.d", file=sys.stderr)
        return 1
    print(render_groups(groups))
    return 0


# --- extract: the entries a branch added to CHANGELOG.md, as fragments -----------------------

LEAD = re.compile(r"^- \*\*(?P<type>" + "|".join(t.capitalize() for t in TYPES) + r")\s*:\s*", re.I)


def entries_from_added_lines(added: list[str]) -> list[list[str]]:
    """Split the lines a branch added into entries: a '- ' line starts one, the rest continue it.

    A line that is not part of an entry (a sub-header such as '### Added', a blank line that
    ends nothing) is dropped; a continuation before the first '- ' is dropped with it.
    """
    entries: list[list[str]] = []
    for line in added:
        if line.startswith("- "):
            entries.append([line])
        elif entries and not line.startswith("#"):
            entries[-1].append(line)
    for e in entries:
        while e and not e[-1].strip():
            e.pop()
    return entries


def slug_of(lead: str, taken: set[str]) -> str:
    words = re.findall(r"[a-z0-9]+", lead.lower())[:6]
    base = "-".join(words)[:50].strip("-") or "change"
    slug, n = base, 2
    while slug in taken:
        slug, n = f"{base}-{n}", n + 1
    taken.add(slug)
    return slug


def fragment_of(entry: list[str], default_type: str | None) -> tuple[str, str]:
    """(type, fragment text) for one entry; the type word leaves the lead-in it was written in."""
    first = entry[0]
    m = LEAD.match(first)
    if m is not None:
        kind = m.group("type").lower()
        first = "- **" + first[m.end() :]
    elif default_type is not None:
        kind = default_type
    else:
        raise ValueError(f"no type in the lead-in (use --default-type): {first[:90]}")
    # The entry's continuation lines were indented under the dash; take that indent off.
    body = [first[2:]] + [ln[2:] if ln.startswith("  ") else ln for ln in entry[1:]]
    return kind, "\n".join(body).strip() + "\n"


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


def extract(root: Path, base: str, write: bool, restore: bool, default_type: str | None) -> int:
    merge_base = git(root, "merge-base", base, "HEAD").strip()
    diff = git(root, "diff", "--unified=0", merge_base, "HEAD", "--", "CHANGELOG.md")
    added = [ln[1:] for ln in diff.splitlines() if ln.startswith("+") and not ln.startswith("+++")]
    entries = entries_from_added_lines(added)
    if not entries:
        print(f"CHANGELOG.md is unchanged since {merge_base[:9]}: nothing to extract")
        return 0
    taken = {
        m.group("slug") for p in (root / "changelog.d").glob("*.md") if (m := NAME.match(p.name))
    }
    made: list[tuple[str, str]] = []
    try:
        for entry in entries:
            kind, text = fragment_of(entry, default_type)
            lead = re.sub(r"[`*_]", "", text.split("**", 2)[1] if "**" in text else text)
            made.append((f"{slug_of(lead, taken)}.{kind}.md", text))
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    for name, text in made:
        print(f"--- changelog.d/{name}\n{text}")
    if not write:
        print(f"{len(made)} fragment(s); run again with --write to create them")
        return 0
    for name, text in made:
        (root / "changelog.d" / name).write_text(text, encoding="utf-8", newline="\n")
    if restore:
        git(root, "checkout", merge_base, "--", "CHANGELOG.md")
    print(
        f"wrote {len(made)} fragment(s)"
        + ("; CHANGELOG.md restored to the merge base" if restore else "")
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("assemble")
    a.add_argument("--version", required=True)
    a.add_argument("--date", required=True)
    sub.add_parser("pending")
    e = sub.add_parser("extract")
    e.add_argument("--base", required=True)
    e.add_argument("--write", action="store_true")
    e.add_argument("--restore", action="store_true")
    e.add_argument("--default-type", choices=TYPES)
    for p in sub.choices.values():
        p.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(
        encoding="utf-8"
    )  # entries hold arrows and the like; a cp1252 console would crash
    root = Path(args.root)
    if args.cmd == "assemble":
        return assemble(root, args.version, args.date)
    if args.cmd == "pending":
        return pending(root)
    return extract(root, args.base, args.write or args.restore, args.restore, args.default_type)


if __name__ == "__main__":
    sys.exit(main())
