#!/usr/bin/env python3
"""The symbols a Kconfig file declares, as far as the profile guard needs them.

`sink_profile.py check-change` holds what a change adds to or takes from the Kconfig against
profiles.json at the pull request, with no ESP-IDF: a new key nobody placed is otherwise first seen
as `[unplaced]` by a configured build on `main`. This is not Kconfig's grammar. It reads
`config`/`menuconfig` blocks with their type, `default`, `depends on`, and the `if`/`menu`/`choice`
blocks that enclose them, and steps over `help` text, which is all that question needs.

Standard library only.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_SYMBOL = re.compile(r"^(config|menuconfig)\s+([A-Za-z0-9_]+)\s*$")
_TYPES = ("bool", "int", "hex", "string", "tristate")
_OPEN_BLOCK = re.compile(r"^(if|menu|choice)\b(.*)$")
_CLOSE_BLOCK = {"endif": "if", "endmenu": "menu", "endchoice": "choice"}


@dataclass
class Symbol:
    name: str
    type: str = ""
    defaults: list[str] = field(default_factory=list)
    depends: list[str] = field(default_factory=list)
    enclosing: list[str] = field(default_factory=list)

    @property
    def conditional(self) -> bool:
        """Whether the symbol exists only under a condition (its own `depends on`, or an
        enclosing `if`/`menu ... depends on`)."""
        return bool(self.depends or self.enclosing)

    @property
    def plain_n(self) -> bool:
        """A bool that is off unless something turns it on: no default, or only `default n`."""
        if self.type not in ("bool", "tristate"):
            return False
        return all(re.fullmatch(r"n(\s+if\s+.*)?", value.strip()) for value in self.defaults)


def scan(text: str) -> dict[str, Symbol]:
    """The symbols of one Kconfig file, by name (without CONFIG_)."""
    found: dict[str, Symbol] = {}
    blocks: list[tuple[str, str]] = []  # (kind, condition), innermost last
    current: Symbol | None = None
    help_indent: int | None = None  # None: not in help; -1: the next text line sets it
    for raw in text.splitlines():
        stripped = raw.strip()
        indent = len(raw) - len(raw.lstrip())
        if help_indent is not None:
            if not stripped:
                continue
            if help_indent == -1:
                help_indent = indent
                continue
            if indent >= help_indent:
                continue
            help_indent = None  # the help ended: this line is Kconfig again
        if not stripped or stripped.startswith("#"):
            continue
        word = stripped.split(None, 1)[0]
        rest = stripped[len(word) :].strip()
        symbol = _SYMBOL.match(stripped)
        if symbol:
            current = Symbol(
                name=symbol.group(2),
                enclosing=[f"{kind} {cond}".strip() for kind, cond in blocks if cond],
            )
            found[current.name] = current
            continue
        opened = _OPEN_BLOCK.match(stripped)
        if opened:
            current = None
            # `if EXPR` is conditional on EXPR; `menu "x"` and `choice` are, unless a `depends on`
            # follows, which is read below as the block's condition
            blocks.append((opened.group(1), opened.group(2).strip() if word == "if" else ""))
            continue
        if word in _CLOSE_BLOCK:
            current = None
            if blocks:
                blocks.pop()
            continue
        if word == "help" or word == "---help---":
            help_indent = -1
            continue
        if word == "depends" and rest.startswith("on"):
            condition = rest[2:].strip()
            if current is not None:
                current.depends.append(condition)
            elif blocks:
                kind, existing = blocks[-1]
                blocks[-1] = (kind, f"{existing} && {condition}" if existing else condition)
            continue
        if current is None:
            continue
        if word in _TYPES:
            current.type = word
        elif word == "default":
            current.defaults.append(rest)
        elif word == "def_bool" or word == "def_int" or word == "def_hex" or word == "def_string":
            current.type = word[4:]
            current.defaults.append(rest)
    return found
