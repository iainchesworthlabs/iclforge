"""What the generators that write one C++ header from computed tables share: wrapping a list of
literals into lines, and the check-or-write step of their `--check` option."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path


def wrap(items: list[str], indent: str = "    ", width: int = 100) -> list[str]:
    """`items`, each followed by a comma, packed into lines of at most `width` columns."""
    lines: list[str] = []
    line = indent
    for item in items:
        piece = item + ","
        if len(line) + len(piece) + 1 > width and line.strip():
            lines.append(line.rstrip())
            line = indent
        line += piece + " "
    if line.strip():
        lines.append(line.rstrip())
    return lines


def write_or_check(description: str, render: Callable[[], str], header: Path, repo_root: Path,
                   script: str) -> int:
    """Write `render()` to `header`, or with --check fail when the header is not what it writes."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--check", action="store_true", help="fail when the header is not what this would write"
    )
    args = parser.parse_args()
    text = render()
    if args.check:
        current = header.read_text(encoding="utf-8") if header.exists() else ""
        if current != text:
            print(f"{header.relative_to(repo_root)} is out of date; run {script}", file=sys.stderr)
            return 1
        return 0
    header.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {header.relative_to(repo_root)}")
    return 0
