#!/usr/bin/env python3
"""The changelog rule for a pull request: a fragment, never an edit to CHANGELOG.md.

    check_changelog_fragment.py --base <sha> [--head HEAD] [--allow-changelog] [--require-fragment]

CHANGELOG.md used to take every pull request's entry at the top of `## [Unreleased]`, so two
pull requests landing together collided, and a merge to fix the collision started CI again.
A change now adds changelog.d/<slug>.<type>.md (changelog.d/README.md has the format) and a
release assembles them (tools/release/changelog_tool.py).

Fails when the pull request
  - edits CHANGELOG.md, unless it deletes fragments (that is the release's assemble step) or
    --allow-changelog is given;
  - adds or changes a fragment whose name is not <slug>.<type>.md, or whose text is empty or
    does not open with a **bold lead-in**, or that starts as a list item.

--require-fragment also fails a pull request that changes code and adds no fragment. It is off
in the workflow: this repository never gated on having an entry, only on where the entry goes.

Standard library only, like the other scripts under tools/.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

TYPES = ("added", "changed", "deprecated", "removed", "fixed", "security")
NAME = re.compile(r"^changelog\.d/[A-Za-z0-9][A-Za-z0-9._-]*\.(" + "|".join(TYPES) + r")\.md$")
NOT_CODE = ("docs/", "planning/", ".github/", "tools/", "changelog.d/")
NOT_CODE_FILES = ("CHANGELOG.md", "ROADMAP.md", "README.md", "CONTRIBUTING.md", "SECURITY.md")


def problems_for(
    fragments: dict[str, str],
    changed: list[str],
    deleted_fragments: bool,
    allow_changelog: bool,
    require_fragment: bool,
) -> list[str]:
    out: list[str] = []
    if "CHANGELOG.md" in changed and not (deleted_fragments or allow_changelog):
        out.append(
            "CHANGELOG.md is not edited by a pull request: add changelog.d/<slug>.<type>.md "
            "instead (see changelog.d/README.md; tools/release/changelog_tool.py extract moves "
            "the entry you wrote)"
        )
    for rel, text in fragments.items():
        body = text.strip()
        if not NAME.match(rel):
            out.append(f"{rel}: the name must be <slug>.<type>.md, type one of {', '.join(TYPES)}")
        elif not body:
            out.append(f"{rel}: empty")
        elif body.startswith("- "):
            out.append(f"{rel}: write the entry without the leading dash")
        elif not body.startswith("**"):
            out.append(f"{rel}: open with a **bold lead-in** that says what changed")
    if require_fragment and not fragments:
        code = [
            p
            for p in changed
            if not p.startswith(NOT_CODE) and p not in NOT_CODE_FILES and not p.endswith(".md")
        ]
        if code:
            out.append(f"{code[0]} (and {len(code) - 1} more) changed with no changelog.d fragment")
    return out


def git(*args: str) -> list[str]:
    done = subprocess.run(["git", *args], capture_output=True, text=True, check=True)
    return [ln for ln in done.stdout.splitlines() if ln]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--base", required=True)
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--allow-changelog", action="store_true")
    ap.add_argument("--require-fragment", action="store_true")
    args = ap.parse_args(argv)

    span = f"{args.base}...{args.head}"
    changed = git("diff", "--name-only", span)
    kept = git("diff", "--name-only", "--diff-filter=AM", span, "--", "changelog.d")
    deleted = git("diff", "--name-only", "--diff-filter=D", span, "--", "changelog.d")
    fragments = {rel: git_show(args.head, rel) for rel in kept if rel != "changelog.d/README.md"}
    found = problems_for(
        fragments,
        changed,
        any(d != "changelog.d/README.md" for d in deleted),
        args.allow_changelog,
        args.require_fragment,
    )
    for line in found:
        print(line, file=sys.stderr)
    if found:
        return 1
    print(f"changelog check passed ({len(fragments)} fragment(s) in this change)")
    return 0


def git_show(head: str, rel: str) -> str:
    done = subprocess.run(
        ["git", "show", f"{head}:{rel}"], capture_output=True, text=True, check=True
    )
    return done.stdout


if __name__ == "__main__":
    sys.exit(main())
