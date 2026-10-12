#!/usr/bin/env python3
"""Fail when the ESP-IDF component links an iclforge:: target its own option set never defines.

firmware/esp-idf/iclforge/CMakeLists.txt turns its Kconfig symbols into the root build's options
(ICLFORGE_MINIMAL_DECODER, ICLFORGE_SENDSPIN_PLAYER_ONLY, ...), pulls the root in with
add_subdirectory(), and then links the libraries it needs by name. Which of those libraries the root
and the library directories define under those options is decided by `if()` guards in files the
component's author is not editing. On 2026-10-12 two pull requests each passed their own gate and
broke the sink firmware together: one made the root add libs/lyrics only when neither
minimum-footprint profile is on, the other made the sink's player link iclforge::lyrics, and a sink
is a minimum-footprint build. CMake stopped at the link ("Target "__idf_iclforge" links to:
iclforge::lyrics but the target was not found"), in a build the pull-request gate does not run (it
builds no ESP-IDF image; ci.yml's ESP legs run after the merge).

This is the part of that build that can be checked without ESP-IDF. It reads the component's file
and runs it, with the root, every directory the root adds and every file they include, through a
small interpreter of the CMake language, once for each combination of the Kconfig symbols and cache
strings the component's own conditions read. In each run it collects the iclforge:: targets the
component links and the iclforge:: targets the run defined (add_library(), the aliases, and what
iclforge_add_library() makes), and holds every link to a definition:

  - a link that nothing defined is a finding, with the file that defines it and the guard that kept
    that file from running (the `if()` it sat under, with the values it read, or the return()
    before it);
  - a link no run reaches is a finding too: when every combination that reaches it stops at a
    message(FATAL_ERROR), the check has seen nothing of it, and says so.

A combination the build itself refuses (message(FATAL_ERROR), for instance the encoder profile with
AC-4) is not a finding: those are the guards doing their work.

What the interpreter models is if(), set(), option(), unset(), list(APPEND),
get_filename_component(), include(), add_subdirectory(), return(), message(FATAL_ERROR),
add_library(), iclforge_add_library() and target_link_libraries(). A command it does not know is
ignored, so a variable that only such a command sets (string(), find_package()) reads as unset,
as CMake would read one that nothing set; --verbose lists every variable a condition read that
nothing the interpreter ran had set, so that a reader can see which guards that touched. A
condition it cannot read stops the check with its file and line. foreach(), while(), function()
and macro() bodies are not run, so a target defined or a variable set inside one is not seen
(a definition made there shows as a finding, loudly). The component's own helper files (opus.cmake
downloads libopus into it) are not run either: they define no iclforge:: target, and a test holds
them to that.

  python3 tools/checks/check_esp_component_targets.py
  python3 tools/checks/check_esp_component_targets.py --verbose

Exit status 0 when every link is defined, 1 for a finding or for a condition the interpreter cannot
read, 2 for a usage error. Standard library only, like the other scripts here; its tests
(test_check_esp_component_targets.py) run in the static job with the rest of tools/checks.
"""

from __future__ import annotations

import argparse
import contextlib
import itertools
import posixpath
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The checkout as the interpreter sees it: every path is POSIX and absolute under this.
VROOT = "/repo"

COMPONENT = "firmware/esp-idf/iclforge"

# Where the interpreter looks for targets by name when it explains a finding.
INDEX_TREES = ("libs", "cmake", "firmware/esp-idf")

# The cache variables and the compiler the component's build has before its file runs. The
# component's own FORCEd cache variables are the point; these are what ESP-IDF and its
# xtensa-esp-elf toolchain contribute.
ENVIRONMENT = {
    "CMAKE_CXX_COMPILER_ID": "GNU",
    "CMAKE_C_COMPILER_ID": "GNU",
    "CMAKE_SYSTEM_NAME": "Generic",
    "CMAKE_CROSSCOMPILING": "TRUE",
    "ESP_PLATFORM": "1",
    "IDF_TARGET": "esp32s3",
    "COMPONENT_LIB": "__idf_iclforge",
}

FALSE_CONSTANTS = {"0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""}
TRUE_CONSTANTS = {"1", "ON", "YES", "TRUE", "Y"}
CONSTANTS = TRUE_CONSTANTS | FALSE_CONSTANTS

UNARY = {"EXISTS", "COMMAND", "DEFINED", "TARGET", "IS_DIRECTORY", "POLICY", "IS_ABSOLUTE"}
BINARY = {
    "EQUAL", "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL", "STREQUAL", "STRLESS",
    "STRGREATER", "STRLESS_EQUAL", "STRGREATER_EQUAL", "VERSION_LESS", "VERSION_GREATER",
    "VERSION_EQUAL", "VERSION_LESS_EQUAL", "VERSION_GREATER_EQUAL", "MATCHES", "IN_LIST",
}


class ParseError(Exception):
    """A CMake file the tokenizer could not read."""


class Unsupported(Exception):
    """A construct the interpreter cannot evaluate; the run's answer would be a guess."""


class Aborted(Exception):
    """message(FATAL_ERROR), or a configure CMake itself would stop at."""

    def __init__(self, message: str, file: str, line: int) -> None:
        super().__init__(message)
        self.message, self.file, self.line = message, file, line


class _Return(Exception):
    """return() in a directory's file or an included file."""


# --------------------------------------------------------------------------------- tokenizer


@dataclass(frozen=True)
class Arg:
    text: str
    quoted: bool = False  # a quoted or bracket argument: never a variable name in a condition


@dataclass
class Command:
    name: str  # lower case
    args: list[Arg]
    line: int


@dataclass
class IfBlock:
    line: int
    # (the if/elseif command, or None for else; its body)
    branches: list[tuple[Command | None, list]] = field(default_factory=list)


@dataclass
class Opaque:
    """A foreach/while/function/macro block: not run, and its commands are not looked into."""

    kind: str
    command: Command


_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_BRACKET_OPEN = re.compile(r"\[(=*)\[")


def _skip_comment(text: str, i: int, line: int) -> tuple[int, int]:
    """Past the comment that starts at text[i] == '#'."""
    m = _BRACKET_OPEN.match(text, i + 1)
    if m:
        close = "]" + m.group(1) + "]"
        end = text.find(close, m.end())
        if end < 0:
            raise ParseError(f"line {line}: unterminated bracket comment")
        return end + len(close), line + text.count("\n", i, end)
    end = text.find("\n", i)
    return (len(text) if end < 0 else end), line


def _parse_arguments(text: str, i: int, line: int) -> tuple[list[Arg], int, int]:
    n, depth, args = len(text), 1, []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t":
            i += 1
        elif c == "#":
            i, line = _skip_comment(text, i, line)
        elif c == "(":
            depth += 1
            args.append(Arg("("))
            i += 1
        elif c == ")":
            depth -= 1
            i += 1
            if depth == 0:
                return args, i, line
            args.append(Arg(")"))
        elif c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    nxt = text[j + 1]
                    if nxt == "\n":
                        line += 1
                    buf.append({"n": "\n", "t": "\t"}.get(nxt, nxt) if nxt in "nt" else nxt)
                    j += 2
                    continue
                if text[j] == "\n":
                    line += 1
                buf.append(text[j])
                j += 1
            if j >= n:
                raise ParseError(f"line {line}: unterminated quoted argument")
            args.append(Arg("".join(buf), quoted=True))
            i = j + 1
        else:
            m = _BRACKET_OPEN.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                end = text.find(close, m.end())
                if end < 0:
                    raise ParseError(f"line {line}: unterminated bracket argument")
                args.append(Arg(text[m.end() : end], quoted=True))
                line += text.count("\n", i, end)
                i = end + len(close)
                continue
            j = i
            while j < n and text[j] not in " \t\n()#":
                if text[j] == '"':  # a legacy unquoted argument with a quoted part
                    k = j + 1
                    while k < n and text[k] != '"':
                        k += 2 if text[k] == "\\" else 1
                    j = k + 1
                    continue
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                j += 1
            args.append(Arg(text[i:j]))
            i = j
    raise ParseError(f"line {line}: a command is not closed")


def tokenize(text: str) -> list[Command]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    i, n, line, commands = 0, len(text), 1, []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c.isspace():
            i += 1
        elif c == "#":
            i, line = _skip_comment(text, i, line)
        else:
            m = _NAME.match(text, i)
            if not m:
                raise ParseError(f"line {line}: unexpected {text[i : i + 20]!r}")
            j = m.end()
            while j < n and text[j] in " \t":
                j += 1
            if j >= n or text[j] != "(":
                raise ParseError(f"line {line}: {m.group(0)!r} is not followed by '('")
            start = line
            args, i, line = _parse_arguments(text, j + 1, line)
            commands.append(Command(m.group(0).lower(), args, start))
    return commands


_BLOCKS = {
    "foreach": "endforeach",
    "while": "endwhile",
    "function": "endfunction",
    "macro": "endmacro",
}


def build_tree(commands: list[Command]) -> list:
    """The commands as nodes: Command, IfBlock (with its branches) and Opaque."""

    def block(pos: int, until: set[str]) -> tuple[list, int]:
        nodes: list = []
        while pos < len(commands):
            cmd = commands[pos]
            if cmd.name in until:
                return nodes, pos
            if cmd.name == "if":
                node = IfBlock(cmd.line)
                current: Command | None = cmd
                pos += 1
                while True:
                    body, pos = block(pos, {"elseif", "else", "endif"})
                    node.branches.append((current, body))
                    if pos >= len(commands):
                        raise ParseError(f"line {cmd.line}: if() is not closed")
                    closer = commands[pos]
                    pos += 1
                    if closer.name == "endif":
                        break
                    current = closer if closer.name == "elseif" else None
                nodes.append(node)
            elif cmd.name in _BLOCKS:
                depth, pos = 1, pos + 1
                while pos < len(commands) and depth:
                    if commands[pos].name == cmd.name:
                        depth += 1
                    elif commands[pos].name == _BLOCKS[cmd.name]:
                        depth -= 1
                    pos += 1
                if depth:
                    raise ParseError(f"line {cmd.line}: {cmd.name}() is not closed")
                nodes.append(Opaque(cmd.name, cmd))
            else:
                nodes.append(cmd)
                pos += 1
        return nodes, pos

    nodes, pos = block(0, set())
    if pos < len(commands):
        raise ParseError(f"line {commands[pos].line}: stray {commands[pos].name}()")
    return nodes


# ---------------------------------------------------------------------------------- the tree


class Tree:
    """The files of a checkout, read from disk, or from a dict of POSIX path -> text."""

    def __init__(self, root: Path | None = None, files: dict[str, str] | None = None,
                 overrides: dict[str, str] | None = None) -> None:
        self.root, self.files, self.overrides = root, files, overrides or {}
        self._parsed: dict[str, list] = {}
        self._disk: dict[str, str | None] = {}

    @staticmethod
    def rel(vpath: str) -> str:
        norm = posixpath.normpath(vpath)
        if norm == VROOT:
            return "."
        if norm.startswith(VROOT + "/"):
            return norm[len(VROOT) + 1 :]
        return ""  # outside the checkout

    def read(self, vpath: str) -> str | None:
        rel = self.rel(vpath)
        if not rel or rel == ".":
            return None
        if rel in self.overrides:
            return self.overrides[rel]
        if self.files is not None:
            return self.files.get(rel)
        assert self.root is not None
        if rel not in self._disk:
            path = self.root / rel
            self._disk[rel] = (path.read_text(encoding="utf-8", errors="replace")
                               if path.is_file() else None)
        return self._disk[rel]

    def is_file(self, vpath: str) -> bool:
        return self.read(vpath) is not None

    def is_dir(self, vpath: str) -> bool:
        rel = self.rel(vpath)
        if not rel:
            return False
        if rel == ".":
            return True
        if self.files is not None or self.overrides:
            prefix = rel + "/"
            names = list(self.overrides) + list(self.files or {})
            if any(f.startswith(prefix) for f in names):
                return True
        return self.root is not None and (self.root / rel).is_dir()

    def exists(self, vpath: str) -> bool:
        return self.is_file(vpath) or self.is_dir(vpath)

    def nodes(self, vpath: str) -> list:
        key = posixpath.normpath(vpath)
        if key not in self._parsed:
            text = self.read(key)
            if text is None:
                raise Aborted(f"{key} does not exist", key, 0)
            try:
                self._parsed[key] = build_tree(tokenize(text))
            except ParseError as err:
                raise Unsupported(f"{self.rel(key)}: {err}") from err
        return self._parsed[key]

    def cmake_files(self, trees: tuple[str, ...]) -> list[str]:
        """The virtual paths of the CMake files under the trees (and the root's own)."""
        found: set[str] = {f"{VROOT}/CMakeLists.txt"}
        if self.files is not None or self.overrides:
            for name in list(self.files or {}) + list(self.overrides):
                if re.search(r"(^|/)(CMakeLists\.txt|[^/]+\.cmake)$", name):
                    found.add(f"{VROOT}/{name}")
        if self.root is not None:
            for tree in trees:
                base = self.root / tree
                if base.is_dir():
                    for path in list(base.rglob("CMakeLists.txt")) + list(base.rglob("*.cmake")):
                        found.add(f"{VROOT}/{path.relative_to(self.root).as_posix()}")
        return sorted(f for f in found if self.is_file(f))


# ------------------------------------------------------------------------------ interpreter


@dataclass
class Scope:
    vars: dict[str, str]
    parent: Scope | None = None

    def child(self) -> Scope:
        return Scope(dict(self.vars), self)


@dataclass
class Skipped:
    """An add_subdirectory() or include() the run did not take, and why."""

    kind: str
    path: str  # virtual path of the directory or file
    file: str
    line: int
    reason: str


@dataclass
class Returned:
    file: str
    line: int
    conditions: list[tuple[str, int, str, dict[str, str]]]


@dataclass
class Link:
    target: str
    file: str
    line: int


@dataclass
class Run:
    assignment: dict[str, str]
    aborted: Aborted | None = None
    links: list[Link] = field(default_factory=list)
    targets: dict[str, tuple[str, int]] = field(default_factory=dict)
    executed: set[str] = field(default_factory=set)
    skipped: list[Skipped] = field(default_factory=list)
    returned: list[Returned] = field(default_factory=list)
    not_run: set[str] = field(default_factory=set)
    unset_reads: dict[str, str] = field(default_factory=dict)


class Interpreter:
    def __init__(self, tree: Tree, component_file: str, cache: dict[str, str]) -> None:
        self.tree = tree
        self.component_file = component_file
        self.cache = dict(cache)
        self.run = Run(assignment={})
        self.functions: set[str] = set()
        # The if() branches being run, innermost last: (file, line, text, values read).
        self.cond_stack: list[tuple[str, int, str, dict[str, str]]] = []
        # Variables a condition read that nothing had set, by where they were read.
        self.unset_reads: dict[str, str] = {}

    # ---- variables

    def lookup(self, name: str, scope: Scope) -> str | None:
        if name in scope.vars:
            return scope.vars[name]
        return self.cache.get(name)

    def expand(self, text: str, scope: Scope) -> str:
        for _ in range(64):
            m = re.search(r"\$\{([^${}]*)\}", text)
            if not m:
                break
            text = text[: m.start()] + (self.lookup(m.group(1), scope) or "") + text[m.end() :]
        return re.sub(r"\$ENV\{[^}]*\}", "", text)

    def arguments(self, cmd: Command, scope: Scope) -> list[Arg]:
        out: list[Arg] = []
        for arg in cmd.args:
            text = self.expand(arg.text, scope)
            if arg.quoted:
                out.append(Arg(text, quoted=True))
            else:
                out.extend(Arg(part) for part in text.split(";") if part)
        return out

    # ---- conditions

    @staticmethod
    def _is_false(value: str) -> bool:
        upper = value.upper()
        if upper in FALSE_CONSTANTS or upper.endswith("-NOTFOUND"):
            return True
        try:
            return float(value) == 0.0
        except ValueError:
            return False

    def truth(self, arg: Arg, scope: Scope) -> bool:
        text, upper = arg.text, arg.text.upper()
        if upper in TRUE_CONSTANTS:
            return True
        if upper in FALSE_CONSTANTS or upper.endswith("-NOTFOUND"):
            return False
        try:
            return float(text) != 0.0
        except ValueError:
            pass
        if arg.quoted:
            return False
        value = self.lookup(text, scope)
        return value is not None and not self._is_false(value)

    def value(self, arg: Arg, scope: Scope) -> str:
        if not arg.quoted:
            found = self.lookup(arg.text, scope)
            if found is not None:
                return found
        return arg.text

    def condition(self, args: list[Arg], scope: Scope, where: str) -> bool:
        pos = 0

        def peek() -> str | None:
            return args[pos].text if pos < len(args) and not args[pos].quoted else None

        def or_expr() -> bool:
            nonlocal pos
            result = and_expr()
            while peek() == "OR":
                pos += 1
                rhs = and_expr()
                result = result or rhs
            return result

        def and_expr() -> bool:
            nonlocal pos
            result = not_expr()
            while peek() == "AND":
                pos += 1
                rhs = not_expr()
                result = result and rhs
            return result

        def not_expr() -> bool:
            nonlocal pos
            if peek() == "NOT":
                pos += 1
                return not not_expr()
            return primary()

        def primary() -> bool:
            nonlocal pos
            if pos >= len(args):
                raise Unsupported(f"{where}: a condition ends early")
            tok = args[pos]
            if not tok.quoted and tok.text == "(":
                pos += 1
                result = or_expr()
                if peek() != ")":
                    raise Unsupported(f"{where}: unbalanced parentheses in a condition")
                pos += 1
                return result
            if not tok.quoted and tok.text in UNARY and pos + 1 < len(args):
                operand = args[pos + 1]
                pos += 2
                return self.unary(tok.text, operand, scope, where)
            pos += 1
            if peek() in BINARY:
                op = args[pos].text
                pos += 1
                if pos >= len(args):
                    raise Unsupported(f"{where}: {op} has no right-hand side")
                rhs = args[pos]
                pos += 1
                return self.binary(op, tok, rhs, scope, where)
            return self.truth(tok, scope)

        result = or_expr()
        if pos != len(args):
            raise Unsupported(f"{where}: cannot read the condition near {args[pos].text!r}")
        return result

    def unary(self, op: str, operand: Arg, scope: Scope, where: str) -> bool:
        name = operand.text
        if op == "DEFINED":
            if name.startswith("CACHE{") and name.endswith("}"):
                return name[6:-1] in self.cache
            return self.lookup(name, scope) is not None
        if op == "TARGET":
            return name in self.run.targets
        if op == "EXISTS":
            return self.tree.exists(name)
        if op == "IS_DIRECTORY":
            return self.tree.is_dir(name)
        if op == "IS_ABSOLUTE":
            return name.startswith("/")
        if op == "COMMAND":
            return name.lower() in self.functions or name.lower() in KNOWN_COMMANDS
        raise Unsupported(f"{where}: cannot evaluate {op}")

    def binary(self, op: str, lhs: Arg, rhs: Arg, scope: Scope, where: str) -> bool:
        left, right = self.value(lhs, scope), self.value(rhs, scope)
        if op == "STREQUAL":
            return left == right
        if op == "MATCHES":
            try:
                return re.search(rhs.text, left) is not None
            except re.error as err:
                raise Unsupported(f"{where}: MATCHES pattern {rhs.text!r}: {err}") from err
        if op == "IN_LIST":
            return left in (self.lookup(rhs.text, scope) or "").split(";")
        if op in ("EQUAL", "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL"):
            try:
                a, b = float(left), float(right)
            except ValueError:
                return False
            return {"EQUAL": a == b, "LESS": a < b, "GREATER": a > b,
                    "LESS_EQUAL": a <= b, "GREATER_EQUAL": a >= b}[op]
        if op.startswith("VERSION_"):
            def parts(v: str) -> list[int]:
                return [int(p) if p.isdigit() else 0 for p in v.split(".")]
            a, b = parts(left), parts(right)
            width = max(len(a), len(b))
            a, b = a + [0] * (width - len(a)), b + [0] * (width - len(b))
            return {"VERSION_EQUAL": a == b, "VERSION_LESS": a < b, "VERSION_GREATER": a > b,
                    "VERSION_LESS_EQUAL": a <= b, "VERSION_GREATER_EQUAL": a >= b}[op]
        if op in ("STRLESS", "STRGREATER", "STRLESS_EQUAL", "STRGREATER_EQUAL"):
            return {"STRLESS": left < right, "STRGREATER": left > right,
                    "STRLESS_EQUAL": left <= right, "STRGREATER_EQUAL": left >= right}[op]
        raise Unsupported(f"{where}: cannot evaluate {op}")

    # ---- execution

    def where(self, file: str, line: int) -> str:
        return f"{self.tree.rel(file)}:{line}"

    def execute(self, nodes: list, scope: Scope, file: str) -> None:
        for node in nodes:
            if isinstance(node, Opaque):
                if node.kind in ("function", "macro") and node.command.args:
                    self.functions.add(node.command.args[0].text.lower())
            elif isinstance(node, IfBlock):
                self.execute_if(node, scope, file)
            else:
                self.command(node, scope, file)

    def execute_if(self, node: IfBlock, scope: Scope, file: str) -> None:
        taken = False
        for cmd, body in node.branches:
            if taken:
                self.note_skipped(body, scope, file, "an earlier branch of the same if() ran")
                continue
            if cmd is None:
                result, text, values = True, "else", {}
            else:
                args = self.arguments(cmd, scope)
                where = self.where(file, cmd.line)
                result = self.condition(args, scope, where)
                text = " ".join(f'"{a.text}"' if a.quoted else a.text for a in args)
                names = [a.text for a in args if not a.quoted
                         and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", a.text)
                         and a.text.upper() not in BINARY | UNARY | {"NOT", "AND", "OR"}]
                values = {n: self.lookup(n, scope) or "" for n in names}
                for n in names:
                    if self.lookup(n, scope) is None and n.upper() not in CONSTANTS:
                        self.unset_reads.setdefault(n, where)
            if result:
                taken = True
                self.cond_stack.append((file, cmd.line if cmd else node.line, text, values))
                try:
                    self.execute(body, scope, file)
                finally:
                    self.cond_stack.pop()
            else:
                reads = fmt_values(values)
                self.note_skipped(body, scope, file,
                                  f"`if({text})` is false" + (f" ({reads})" if reads else ""))

    def note_skipped(self, body: list, scope: Scope, file: str, reason: str) -> None:
        """Record the add_subdirectory() and include() calls of a branch that did not run."""
        for node in body:
            if isinstance(node, IfBlock):
                for _, inner in node.branches:
                    self.note_skipped(inner, scope, file, reason)
            elif isinstance(node, Command) and node.name in ("add_subdirectory", "include"):
                args = self.arguments(node, scope)
                path = self.resolve_path(args[0].text, node.name, scope) if args else ""
                if path:
                    self.run.skipped.append(Skipped(node.name, path, file, node.line, reason))

    def resolve_path(self, text: str, command: str, scope: Scope) -> str:
        if not text or "$" in text:
            return ""
        if text.startswith("/"):
            return posixpath.normpath(text)
        base = (scope.vars.get("CMAKE_CURRENT_LIST_DIR") if command == "include"
                else scope.vars.get("CMAKE_CURRENT_SOURCE_DIR")) or VROOT
        return posixpath.normpath(posixpath.join(base, text))

    def command(self, cmd: Command, scope: Scope, file: str) -> None:
        name = cmd.name
        handler = getattr(self, f"cmd_{name}", None)
        if handler is None:
            return  # a command that changes nothing the interpreter reads
        handler(cmd, self.arguments(cmd, scope), scope, file)

    def run_directory(self, vdir: str, scope: Scope) -> None:
        cmake = f"{vdir}/CMakeLists.txt"
        scope = scope.child()
        scope.vars["CMAKE_CURRENT_SOURCE_DIR"] = vdir
        scope.vars["CMAKE_CURRENT_LIST_DIR"] = vdir
        scope.vars["CMAKE_CURRENT_LIST_FILE"] = cmake
        scope.vars["CMAKE_CURRENT_BINARY_DIR"] = f"/build/{self.tree.rel(vdir)}"
        self.run.executed.add(cmake)
        with contextlib.suppress(_Return):
            self.execute(self.tree.nodes(cmake), scope, cmake)

    def run_component(self, scope: Scope) -> None:
        vdir = posixpath.dirname(self.component_file)
        scope.vars["CMAKE_CURRENT_SOURCE_DIR"] = vdir
        scope.vars["CMAKE_CURRENT_LIST_DIR"] = vdir
        scope.vars["CMAKE_CURRENT_LIST_FILE"] = self.component_file
        scope.vars["CMAKE_CURRENT_BINARY_DIR"] = "/build/esp-idf/iclforge"
        self.run.executed.add(self.component_file)
        with contextlib.suppress(_Return):
            self.execute(self.tree.nodes(self.component_file), scope, self.component_file)

    # ---- the commands that carry state

    def cmd_set(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if not args:
            return
        name, rest = args[0].text, [a.text for a in args[1:]]
        if "CACHE" in rest:
            at = rest.index("CACHE")
            values, tail = rest[:at], rest[at + 1 :]
            if "FORCE" in tail or name not in self.cache:
                self.cache[name] = ";".join(values)
            return
        if "PARENT_SCOPE" in rest:
            if scope.parent is not None:
                rest.remove("PARENT_SCOPE")
                scope.parent.vars[name] = ";".join(rest)
            return
        if not rest:
            scope.vars.pop(name, None)
        else:
            scope.vars[name] = ";".join(rest)

    def cmd_unset(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if not args:
            return
        name = args[0].text
        if len(args) > 1 and args[1].text == "CACHE":
            self.cache.pop(name, None)
        else:
            scope.vars.pop(name, None)

    def cmd_option(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if not args:
            return
        name = args[0].text
        if name in self.cache or name in scope.vars:
            return
        self.cache[name] = args[2].text if len(args) > 2 else "OFF"

    def cmd_list(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if len(args) >= 2 and args[0].text.upper() == "APPEND":
            name = args[1].text
            items = [a.text for a in args[2:]]
            current = self.lookup(name, scope)
            scope.vars[name] = ";".join(([current] if current else []) + items)

    def cmd_get_filename_component(self, cmd: Command, args: list[Arg], scope: Scope,
                                   file: str) -> None:
        if len(args) < 3:
            return
        var, path, mode = args[0].text, args[1].text, args[2].text.upper()
        if mode == "ABSOLUTE":
            base = scope.vars.get("CMAKE_CURRENT_SOURCE_DIR") or VROOT
            full = path if path.startswith("/") else posixpath.join(base, path)
            scope.vars[var] = posixpath.normpath(full)
        elif mode == "DIRECTORY":
            scope.vars[var] = posixpath.dirname(path)
        elif mode == "NAME":
            scope.vars[var] = posixpath.basename(path)
        else:
            scope.vars[var] = ""

    def cmd_return(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        self.run.returned.append(Returned(file, cmd.line, list(self.cond_stack)))
        raise _Return

    def cmd_message(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if args and args[0].text in ("FATAL_ERROR", "SEND_ERROR"):
            text = " ".join(a.text for a in args[1:])
            raise Aborted(re.sub(r"\s+", " ", text).strip(), file, cmd.line)

    def cmd_include(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if not args:
            return
        target = args[0].text
        optional = any(a.text == "OPTIONAL" for a in args[1:])
        if target.endswith(".cmake") or "/" in target:
            path = self.resolve_path(target, "include", scope)
        else:
            path = ""
            for directory in (self.lookup("CMAKE_MODULE_PATH", scope) or "").split(";"):
                candidate = posixpath.normpath(posixpath.join(directory, f"{target}.cmake"))
                if directory and self.tree.is_file(candidate):
                    path = candidate
                    break
            if not path:
                return  # a module CMake ships
        if not path or not self.tree.is_file(path):
            if optional or not (target.endswith(".cmake") or "/" in target):
                return
            raise Aborted(f"include could not find {target}", file, cmd.line)
        if posixpath.dirname(path) == posixpath.dirname(self.component_file):
            # The component's own helper files build third-party sources into it (opus.cmake
            # downloads libopus); they define no iclforge:: target and fetch from the network.
            self.run.not_run.add(path)
            return
        self.run.executed.add(path)
        saved = scope.vars.get("CMAKE_CURRENT_LIST_DIR")
        scope.vars["CMAKE_CURRENT_LIST_DIR"] = posixpath.dirname(path)
        try:
            self.execute(self.tree.nodes(path), scope, path)
        except _Return:
            pass
        finally:
            if saved is None:
                scope.vars.pop("CMAKE_CURRENT_LIST_DIR", None)
            else:
                scope.vars["CMAKE_CURRENT_LIST_DIR"] = saved

    def cmd_add_subdirectory(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if not args:
            return
        vdir = self.resolve_path(args[0].text, "add_subdirectory", scope)
        if not vdir:
            raise Unsupported(f"{self.where(file, cmd.line)}: add_subdirectory({args[0].text})")
        if not self.tree.is_file(f"{vdir}/CMakeLists.txt"):
            raise Aborted(f"add_subdirectory given source {vdir} which has no CMakeLists.txt",
                          file, cmd.line)
        self.run_directory(vdir, scope)

    def cmd_add_library(self, cmd: Command, args: list[Arg], scope: Scope, file: str) -> None:
        if args:
            self.run.targets.setdefault(args[0].text, (file, cmd.line))

    def cmd_iclforge_add_library(self, cmd: Command, args: list[Arg], scope: Scope,
                                 file: str) -> None:
        # cmake/IclforgeLibrary.cmake: iclforge::<stem>_static always, iclforge::<stem>_shared
        # unless the minimum-footprint profile is on, and iclforge::<stem> always.
        if not args:
            return
        texts = [a.text for a in args]
        name = texts[0]
        stem = texts[texts.index("STEM") + 1] if "STEM" in texts[1:] else name
        minimal = (self.truth(Arg("ICLFORGE_MINIMAL_DECODER"), scope)
                   or self.truth(Arg("ICLFORGE_MINIMAL_ENCODER"), scope))
        made = [f"iclforge::{stem}", f"iclforge::{stem}_static", f"iclforge_{name}_objects",
                f"iclforge_{name}_static"]
        if not minimal:
            made += [f"iclforge::{stem}_shared", f"iclforge_{name}_shared"]
        for target in made:
            self.run.targets.setdefault(target, (file, cmd.line))

    def cmd_target_link_libraries(self, cmd: Command, args: list[Arg], scope: Scope,
                                  file: str) -> None:
        if posixpath.dirname(file) != posixpath.dirname(self.component_file):
            return
        for arg in args:
            if arg.text.startswith("iclforge::"):
                self.run.links.append(Link(arg.text, file, cmd.line))


# The commands COMMAND() answers true for; the CMake the tree is read with has all of them.
KNOWN_COMMANDS = {
    "add_library", "add_executable", "add_subdirectory", "target_link_libraries", "install",
    "cmake_path", "target_sources", "target_compile_options", "find_package", "include",
    "iclforge_add_library", "iclforge_install_library",
}


# ----------------------------------------------------------------------- the combinations


def condition_variables(nodes: list) -> tuple[set[str], dict[str, set[str]]]:
    """The names a component file's conditions read, and the literals each is compared with."""
    names: set[str] = set()
    compared: dict[str, set[str]] = {}

    def walk(items: list) -> None:
        for node in items:
            if not isinstance(node, IfBlock):
                continue
            for cmd, body in node.branches:
                if cmd is not None:
                    args = cmd.args
                    for i, arg in enumerate(args):
                        if arg.quoted or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", arg.text):
                            continue
                        if arg.text.upper() in BINARY | UNARY | {"NOT", "AND", "OR"}:
                            continue
                        names.add(arg.text)
                        if i + 2 < len(args) and args[i + 1].text == "STREQUAL":
                            compared.setdefault(arg.text, set()).add(args[i + 2].text)
                walk(body)

    walk(nodes)
    return names, compared


def assignments(component_nodes: list) -> list[dict[str, str]]:
    """Every combination of the CONFIG_ symbols the component's conditions read (set or not set)
    and of the strings its other cache variables are compared with."""
    names, compared = condition_variables(component_nodes)
    config = sorted(n for n in names if n.startswith("CONFIG_"))
    strings = {n: sorted(vals) for n, vals in compared.items() if n.startswith("ICLFORGE_")}
    axes: list[tuple[str, list[str | None]]] = [(n, ["y", None]) for n in config]
    axes += [(n, list(vals)) for n, vals in sorted(strings.items())]
    out = []
    for combo in itertools.product(*(values for _, values in axes)):
        chosen = zip((name for name, _ in axes), combo, strict=True)
        out.append({name: value for name, value in chosen if value is not None})
    return out


def run_assignment(tree: Tree, component_file: str, assignment: dict[str, str]) -> Run:
    cache = dict(ENVIRONMENT)
    cache.update(assignment)
    interp = Interpreter(tree, component_file, cache)
    interp.run.assignment = dict(assignment)
    scope = Scope({"CMAKE_SOURCE_DIR": "/idf-project", "PROJECT_SOURCE_DIR": "/idf-project"})
    try:
        interp.run_component(scope)
    except Aborted as err:
        interp.run.aborted = err
    interp.run.unset_reads = dict(interp.unset_reads)
    return interp.run


# ---------------------------------------------------------------------------- the findings


@dataclass
class Finding:
    file: str
    line: int
    message: str


def definitions(tree: Tree, target: str) -> list[tuple[str, int]]:
    """Where the tree's CMake files define a target, found by reading the commands, not by running
    them."""
    out: list[tuple[str, int]] = []
    for path in tree.cmake_files(INDEX_TREES):
        try:
            commands = tokenize(tree.read(path) or "")
        except ParseError:
            continue
        for cmd in commands:
            if cmd.name == "add_library" and cmd.args and cmd.args[0].text == target:
                out.append((path, cmd.line))
            elif cmd.name == "iclforge_add_library" and cmd.args:
                texts = [a.text for a in cmd.args]
                stem = texts[texts.index("STEM") + 1] if "STEM" in texts[1:] else texts[0]
                if target in (f"iclforge::{stem}", f"iclforge::{stem}_static",
                              f"iclforge::{stem}_shared"):
                    out.append((path, cmd.line))
    return out


def fmt_values(values: dict[str, str]) -> str:
    return ", ".join(f"{k}={v or 'unset'}" for k, v in sorted(values.items()))


def describe(assignment: dict[str, str]) -> str:
    return ", ".join(f"{k}={v}" for k, v in sorted(assignment.items())) or "no CONFIG_ symbol set"


def explain(tree: Tree, run: Run, target: str) -> str:
    defined = definitions(tree, target)
    if not defined:
        return f"nothing in the tree's CMake files defines {target}"
    parts = []
    for path, line in defined:
        where = f"{tree.rel(path)}:{line}"
        directory = posixpath.dirname(path)
        if path not in run.executed:
            reason = ""
            for skip in run.skipped:
                inside = directory == skip.path or directory.startswith(skip.path + "/")
                if path == skip.path or inside:
                    reason = (f"; {tree.rel(skip.file)}:{skip.line} would run "
                              f"{tree.rel(skip.path)}, but {skip.reason}")
                    break
            parts.append(f"{where} defines it, and that file is not run{reason}")
            continue
        reason = ""
        for ret in run.returned:
            if ret.file == path and ret.line < line:
                guards = "; ".join(f"`if({text})` ({fmt_values(values)})"
                                   for f, _, text, values in ret.conditions if f == path)
                reason = (f"; {tree.rel(path)}:{ret.line} returns first, under "
                          f"{guards or 'no condition'}")
                break
        parts.append(f"{where} defines it, and the run reaches that file but not the line{reason}")
    return " | ".join(parts)


def check(tree: Tree, component: str = COMPONENT) -> tuple[list[Finding], list[Run]]:
    component_file = f"{VROOT}/{component}/CMakeLists.txt"
    nodes = tree.nodes(component_file)
    runs = [run_assignment(tree, component_file, a) for a in assignments(nodes)]
    findings: list[Finding] = []

    # Every link statement the component has, by (line, target).
    statements: dict[tuple[int, str], list[tuple[Run, bool]]] = {}
    for run in runs:
        for link in run.links:
            statements.setdefault((link.line, link.target), []).append(
                (run, link.target in run.targets))
    # A statement no run executed: find them from the file's own commands.
    declared: set[tuple[int, str]] = set()
    for line, target in _declared_links(nodes):
        declared.add((line, target))

    rel = f"{component}/CMakeLists.txt"
    for line, target in sorted(declared | set(statements)):
        seen = statements.get((line, target), [])
        broken = [run for run, ok in seen if not ok]
        if broken:
            first = broken[0]
            # What every failing combination has in common says which options matter.
            common = {k: v for k, v in first.assignment.items()
                      if all(run.assignment.get(k) == v for run in broken)}
            findings.append(Finding(rel, line,
                f"{target} is linked here when {describe(common)} ({len(broken)} of "
                f"{len(seen)} combinations that reach this line have no definition of it): the "
                f"root build, with those options, runs no directory that defines it. "
                f"{explain(tree, first, target)}"))
        elif not seen:
            aborts = sorted({(r.aborted.message, r.aborted.file, r.aborted.line)
                             for r in runs if r.aborted})
            why = (f"every combination stops first; the first stop is `{aborts[0][0]}` at "
                   f"{tree.rel(aborts[0][1])}:{aborts[0][2]}") if aborts else \
                  "no combination reaches it"
            findings.append(Finding(rel, line,
                f"{target} is linked here, and no combination of the component's options "
                f"gets as far: {why}"))
    return findings, runs


def _declared_links(nodes: list) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    for node in nodes:
        if isinstance(node, IfBlock):
            for _, body in node.branches:
                out.extend(_declared_links(body))
        elif isinstance(node, Command) and node.name == "target_link_libraries":
            out.extend((node.line, a.text) for a in node.args if a.text.startswith("iclforge::"))
    return out


# ----------------------------------------------------------------------------------- the CLI


def tree_rel(root: Path, vpath: str) -> str:
    return Tree.rel(vpath) or str(root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fail when the ESP-IDF component links an iclforge:: target that its own "
                    "option set never defines.")
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2],
                        help="the checkout (default: the one this script is in)")
    parser.add_argument("--component", default=COMPONENT,
                        help=f"the component's directory under the root (default: {COMPONENT})")
    parser.add_argument("--verbose", action="store_true",
                        help="print every combination and how it ended")
    args = parser.parse_args(argv)

    if not (args.root / args.component / "CMakeLists.txt").is_file():
        print(f"check_esp_component_targets: no {args.component}/CMakeLists.txt under "
              f"{args.root}", file=sys.stderr)
        return 2

    try:
        findings, runs = check(Tree(root=args.root), args.component)
    except (Unsupported, Aborted) as err:
        print(f"::error::check_esp_component_targets: cannot evaluate the component's build: "
              f"{err}")
        return 1

    if args.verbose:
        for run in runs:
            state = f"stops: {run.aborted.message}" if run.aborted else "configures"
            linked = ", ".join(sorted({link.target for link in run.links})) or "no iclforge:: link"
            print(f"  {describe(run.assignment)}: {state}; links {linked}")

    if args.verbose:
        unset: dict[str, str] = {}
        for run in runs:
            unset.update(run.unset_reads)
        print("  read by a condition and set by nothing the interpreter ran (read as false):")
        for name, where in sorted(unset.items()):
            print(f"    {name} ({where})")
        not_run = sorted({tree_rel(args.root, p) for run in runs for p in run.not_run})
        for path in not_run:
            print(f"  not run: {path} (the component's own helper file)")

    for finding in findings:
        print(f"::error file={finding.file},line={finding.line}::check_esp_component_targets: "
              f"{finding.message}")
    if findings:
        return 1

    configured = [r for r in runs if not r.aborted]
    linked = sorted({link.target for run in configured for link in run.links})
    print(f"check_esp_component_targets: {len(runs)} combinations of {args.component}'s options, "
          f"{len(configured)} configure and {len(runs) - len(configured)} are refused by their "
          f"own guards; every iclforge:: target it links is defined in each: {', '.join(linked)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
