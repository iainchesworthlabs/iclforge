"""Render a GitHub Release body from CHANGELOG.md, for a given release tag.

`.github/workflows/release.yml`'s `github-release` job used to call
`gh release create --generate-notes`, which drafts a commit list - useful as a first
draft, but not what actually ships: docs/releasing.md's post-release checklist has
always said the *curated* CHANGELOG.md section is the authoritative record and the
GitHub Release body should mirror it, not the other way round. That curation already
happens where it belongs - in CHANGELOG.md, as part of normal development, grouped by
user-facing area with bold lead-in bullets - so by release time there is nothing left
to draft: the matching `## [x.y.z] - date` section already reads exactly like a release
body wants to. This just extracts it.

A GitHub release page holds 125,000 characters at most. A section longer than `MAX_BODY_CHARS`
(0.11.0-beta.1's is about 540,000) is cut to its introduction, with a line that links to the whole
section in CHANGELOG.md at the tag. `--check` is the cheap half of this script for
release.yml's first job: it fails a section that is missing, has no date in its heading or is
empty, so that is found in the first minute and not after the build.

What is deliberately NOT reconstructed here, still left as optional human polish per
docs/releasing.md: an "Artifacts" section with per-package checksums (the checksums are
already attached to the release as individual `*.sha512` files and a `SHA512SUMS`
manifest - restating them in prose adds a second place to keep in sync) and the
pre-release caveat blockquote (picking the single biggest open gap to headline is a
judgement call, not an extraction).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# CHANGELOG.md's own heading shape (Keep a Changelog): "## [0.9.0-beta.1] - 2026-08-22".
# `[Unreleased]` uses the same "## [" prefix and is what bounds the newest released
# section from above; every other "## [" heading bounds sections from below.
_SECTION_START_RE = "^## \\[{version}\\].*$"
_ANY_SECTION_HEADING_RE = re.compile(r"^## \[([^\]]+)\]", re.MULTILINE)
# A released section's heading ends in the day it was cut: "## [0.9.0-beta.1] - 2026-08-22".
_DATED_HEADING_RE = re.compile(r"\] - \d{4}-\d{2}-\d{2}\s*$")

# GitHub refuses a release body of more than 125,000 characters ("body is too long"), and
# `gh release create` says so only after the build, the signing and the attestation have
# run. The 0.11.0-beta.1 section is about 540,000 characters, thirteen times the 0.10.0-beta.1
# one, so a section over this length is cut to its introduction (see `summarise`). The margin
# covers the compare line and what a count of characters and a count of code points disagree on.
MAX_BODY_CHARS = 120_000
_FULL_CHANGELOG_ROOM = 600


class ChangelogSectionNotFound(RuntimeError):
    pass


def find_section(changelog_text: str, version: str) -> tuple[str, str | None]:
    """Return (section_body, previous_version) for `version`'s CHANGELOG.md section.

    `previous_version` is the tag name (without a "v" prefix) of the next section
    below this one, or None if this is the first-ever release.
    """
    start_re = re.compile(_SECTION_START_RE.format(version=re.escape(version)), re.MULTILINE)
    start_match = start_re.search(changelog_text)
    if start_match is None:
        raise ChangelogSectionNotFound(
            f"no CHANGELOG.md section heading '## [{version}]' - update CHANGELOG.md "
            "before tagging (docs/releasing.md's pre-release checklist: move the "
            "section down from ## [Unreleased] first)"
        )

    body_start = start_match.end()
    next_heading = _ANY_SECTION_HEADING_RE.search(changelog_text, pos=body_start)
    body_end = next_heading.start() if next_heading else len(changelog_text)
    previous_version = next_heading.group(1) if next_heading else None

    return changelog_text[body_start:body_end].strip("\n"), previous_version


def find_heading(changelog_text: str, version: str) -> str:
    """Return the `## [version] ...` heading line of `version`'s section."""
    start_re = re.compile(_SECTION_START_RE.format(version=re.escape(version)), re.MULTILINE)
    start_match = start_re.search(changelog_text)
    if start_match is None:
        raise ChangelogSectionNotFound(f"no CHANGELOG.md section heading '## [{version}]'")
    return start_match.group(0)


def summarise(section_body: str, *, repo: str, tag: str, version: str, limit: int) -> str:
    """Cut a section that does not fit a release page down to its introduction.

    The introduction is everything before the first `### ` heading (the "This release
    adds:" list); the rest stays in CHANGELOG.md, and a line says so and links to the
    file at the tag. A section without a `### ` heading is cut at the last paragraph
    break that fits.
    """
    pointer = (
        f"The complete list of changes is the `## [{version}]` section of "
        f"[CHANGELOG.md](https://github.com/{repo}/blob/{tag}/CHANGELOG.md): "
        "it is too long for a release page."
    )
    first_detail = re.search(r"^### ", section_body, re.MULTILINE)
    intro = section_body[: first_detail.start()] if first_detail else section_body
    # "The sections below contain the complete change list and fixes." points at the text
    # that was just cut, and the pointer says it better.
    intro = re.sub(r"\n+The sections below[^\n]*(\n[^\n]+)*\s*$", "", intro)
    room = limit - len(pointer) - _FULL_CHANGELOG_ROOM
    if len(intro) > room:
        cut = intro.rfind("\n\n", 0, room)
        intro = intro[: cut if cut > 0 else room]
    return intro.rstrip("\n") + "\n\n" + pointer


def render(
    section_body: str,
    *,
    repo: str,
    tag: str,
    previous_version: str | None,
    version: str | None = None,
) -> str:
    """The release body: the section, then a compare link; the section's introduction
    and a pointer to CHANGELOG.md when the whole would not fit a GitHub release page."""
    version = version if version is not None else tag.removeprefix("v")
    tail = ""
    if previous_version is not None:
        compare_url = f"https://github.com/{repo}/compare/v{previous_version}...{tag}"
        tail = f"**Full Changelog**: {compare_url}"
    notes = "\n\n".join(part for part in (section_body, tail) if part) + "\n"
    if len(notes) <= MAX_BODY_CHARS:
        return notes
    short = summarise(section_body, repo=repo, tag=tag, version=version, limit=MAX_BODY_CHARS)
    return "\n\n".join(part for part in (short, tail) if part) + "\n"


def check(changelog_text: str, version: str) -> list[str]:
    """What is wrong with `version`'s section as a thing to release, empty if nothing.

    Run by release.yml's resolve-version job, so a missing, undated or empty section
    fails the release in the first minute and not after the build, the signing and the
    attestation have run.
    """
    try:
        heading = find_heading(changelog_text, version)
        body, _ = find_section(changelog_text, version)
    except ChangelogSectionNotFound as exc:
        return [str(exc)]
    problems = []
    if not _DATED_HEADING_RE.search(heading):
        problems.append(
            f"'{heading}' has no date: a release heading reads '## [{version}] - YYYY-MM-DD' "
            "with the day the tag is cut"
        )
    if not body.strip():
        problems.append(f"the CHANGELOG.md section '## [{version}]' is empty")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--version", required=True, help="bare version, e.g. 0.9.0-beta.1")
    parser.add_argument("--tag", required=True, help="full tag, e.g. v0.9.0-beta.1")
    parser.add_argument("--repo", required=True, help="owner/repo, e.g. iainchesworthlabs/iclforge")
    parser.add_argument("--output", type=Path, help="write notes here instead of stdout")
    parser.add_argument(
        "--check",
        action="store_true",
        help="only check that the section can be released (present, dated, not empty)",
    )
    args = parser.parse_args(argv)

    # The notes carry arrows and accented letters that a Windows console's code page cannot print.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    changelog_text = args.changelog.read_text(encoding="utf-8")
    if args.check:
        problems = check(changelog_text, args.version)
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        if not problems:
            body, previous_version = find_section(changelog_text, args.version)
            notes = render(
                body,
                repo=args.repo,
                tag=args.tag,
                previous_version=previous_version,
                version=args.version,
            )
            kind = "in full" if body in notes else "cut to its introduction"
            print(
                f"CHANGELOG.md section [{args.version}] is dated; "
                f"its release notes are {len(notes):,} characters, {kind}."
            )
        return 1 if problems else 0

    try:
        section_body, previous_version = find_section(changelog_text, args.version)
    except ChangelogSectionNotFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    notes = render(
        section_body,
        repo=args.repo,
        tag=args.tag,
        previous_version=previous_version,
        version=args.version,
    )
    if args.output is not None:
        args.output.write_text(notes, encoding="utf-8", newline="\n")
    else:
        print(notes, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
