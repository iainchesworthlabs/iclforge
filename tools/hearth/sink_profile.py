#!/usr/bin/env python3
"""The locked build profiles of hearth_sink's ESP32 boards (planning/esp32-platform-profiles.md).

    python tools/hearth/sink_profile.py list
    python tools/hearth/sink_profile.py defaults s3-n16r8 [--overlay NAME ...] [--wifi FILE]
    python tools/hearth/sink_profile.py verify s3-n16r8 --build-dir BUILD [--overlay NAME ...]
    python tools/hearth/sink_profile.py snapshot s3-n16r8 --build-dir BUILD [--write]
    python tools/hearth/sink_profile.py image hearth-sink-esp32s3
    python tools/hearth/sink_profile.py table [PROFILE ...]
    python tools/hearth/sink_profile.py check-change --base REF [--all]

A profile is one board's build: the fragments of firmware/hearth-sink in the
order they are given to ESP-IDF, the values of the keys that decide whether a
play runs on it (placement, sizes, caches, flash and PSRAM mode), and the
budgets its image is held to. They live in firmware/hearth-sink/profiles.json,
and this is the only reader.

`verify` takes the build directory of a configured (or built) profile and
fails when

- the fragments the build was given are not the profile's, in its order;
- a locked key has a value other than the profile's, and no overlay named
  with --overlay is declared to move it;
- the final sdkconfig has a key that looks like a size, a placement or a
  cache (the profile's `watch` patterns) which the profile neither locks nor
  lists as free: a new stack option cannot arrive without someone saying where
  it lives on every board;
- the app image or the bootloader of the build is over the profile's budget.

`snapshot` is how a profile's locked values are written: the keys its
fragments set, and the keys its `watch` patterns match, at the values the
build resolved. A profile changes in a pull request that runs it, reads what
changed and attaches a board run (the planning note's "Changing a profile").

Standard library only, as the CI container's Python has nothing else.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kconfig_scan
from package_firmware import TARGETS

REPO = Path(__file__).resolve().parents[2]
PROJECT = REPO / "firmware" / "hearth-sink"
PROFILES = PROJECT / "profiles.json"
FORMAT = 1

APP_NAME = "iclforge_hearth_sink.bin"
ABSENT = "<absent>"

_SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
_NOT_SET = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+) is not set\s*$")
_CACHE_DEFAULTS = re.compile(r"^SDKCONFIG_DEFAULTS(?::[A-Z]+)?=(.*)$")


class ProfileError(Exception):
    """Why a request cannot be answered; main() prints it."""


@dataclass(frozen=True)
class Finding:
    rule: str
    message: str


# --- a change to the Kconfig, held to the profiles ------------------------------------

# The files that declare the keys a hearth_sink build configures. The probes' and examples' own
# Kconfig files are other projects and are not profiles.
KCONFIG_FILES = (
    "firmware/hearth-sink/main/Kconfig.projbuild",
    "firmware/esp-idf/iclforge/Kconfig",
)

_TOKEN = re.compile(r'"[^"]*"|[A-Za-z0-9_]+|&&|\|\||!=|=|!|\(|\)')


def _value_of(data: dict[str, Any], ident: str, name: str) -> bool | None:
    """Whether a symbol is on in a profile, from what the profile locks and its target; None where
    that is not known (a symbol ESP-IDF or the build decides)."""
    spec = profile(data, ident)
    if name.startswith("IDF_TARGET_"):
        return name == "IDF_TARGET_" + str(spec.get("target", "")).upper()
    value = spec.get("locked", {}).get("CONFIG_" + name)
    if value == "y":
        return True
    if value == "n":
        return False
    return None


def evaluate(data: dict[str, Any], ident: str, expression: str) -> bool | None:
    """A Kconfig condition (`A && !B || C`, symbols and parentheses) in a profile, three-valued:
    True, False, or None when a symbol in it is not known. Anything it cannot read is None."""
    tokens = _TOKEN.findall(expression)
    position = 0

    def peek() -> str | None:
        return tokens[position] if position < len(tokens) else None

    def take() -> str | None:
        nonlocal position
        token = peek()
        position += 1
        return token

    def disjunction() -> bool | None:
        left = conjunction()
        while peek() == "||":
            take()
            right = conjunction()
            if left is True or right is True:
                left = True
            elif left is None or right is None:
                left = None
            else:
                left = False
        return left

    def conjunction() -> bool | None:
        left = unary()
        while peek() == "&&":
            take()
            right = unary()
            if left is False or right is False:
                left = False
            elif left is None or right is None:
                left = None
            else:
                left = True
        return left

    def unary() -> bool | None:
        if peek() == "!":
            take()
            inner = unary()
            return None if inner is None else not inner
        return primary()

    def primary() -> bool | None:
        token = take()
        if token is None:
            return None
        if token == "(":
            inner = disjunction()
            if peek() == ")":
                take()
            return inner
        if token == "y":
            return True
        if token == "n":
            return False
        if token.startswith('"'):
            return None
        if peek() in ("=", "!="):  # a comparison: not evaluated
            take()
            take()
            return None
        return _value_of(data, ident, token)

    return disjunction()


def _present_in(data: dict[str, Any], ident: str, symbol: kconfig_scan.Symbol) -> bool:
    """Whether the symbol is in a configured build of the profile, as far as it can be told: its
    conditions all hold, and it is not a bool that is off unless something turns it on. Where a
    condition cannot be evaluated the answer is no: the build's own guard decides those."""
    conditions = [*symbol.depends, *(c.split(None, 1)[1] for c in symbol.enclosing if " " in c)]
    for condition in conditions:
        if evaluate(data, ident, condition) is not True:
            return False
    if symbol.type in ("bool", "tristate"):
        # off unless something turns it on, or on only under a condition the profile may not
        # meet: only a plain `default y` is decided here
        return any(default.strip() == "y" for default in symbol.defaults)
    return True


def kconfig_symbols(texts: dict[str, str]) -> dict[str, tuple[str, kconfig_scan.Symbol]]:
    """{name: (file, Symbol)} over the files given as {path: text}."""
    found: dict[str, tuple[str, kconfig_scan.Symbol]] = {}
    for path, text in texts.items():
        for name, symbol in kconfig_scan.scan(text).items():
            found[name] = (path, symbol)
    return found


def check_change(
    data: dict[str, Any],
    head: dict[str, str],
    base: dict[str, str] | None,
) -> list[Finding]:
    """What the Kconfig says that the profiles do not, at the pull request.

    * a symbol the head declares and the base did not (every one, with no base), that the guard
      watches and that is present in a profile which neither locks nor frees it: the first build
      after the merge reports it `[unplaced]`;
    * a key a profile locks (`CONFIG_ICLFORGE_*`, not `n`) that no Kconfig of the build declares:
      the guard says `<absent>` for it, which is a removal or a typo.

    A symbol whose presence cannot be told from the profile's locked values and target is not
    reported: the configured build's own guard decides those (planning/esp32-platform-profiles.md,
    'Where each check runs')."""
    findings: list[Finding] = []
    now = kconfig_symbols(head)
    before = set(kconfig_symbols(base)) if base is not None else set()
    for name in sorted(set(now) - before):
        path, symbol = now[name]
        key = "CONFIG_" + name
        if not is_watched(data, key) or is_free(data, key):
            continue
        absent_in = [
            ident
            for ident, spec in sorted(data.get("profiles", {}).items())
            if key not in spec.get("locked", {}) and _present_in(data, ident, symbol)
        ]
        if absent_in:
            findings.append(
                Finding(
                    "kconfig",
                    f"{path}: {key} is new and the guard watches it, and it is in "
                    f"{', '.join(absent_in)}, which neither lock nor free it: lock its value in "
                    "profiles.json (the hash moves: baselines are re-recorded or accepted, "
                    "planning/esp32-platform-profiles.md 'Changing a profile'), or name it in "
                    "`free`",
                )
            )
    for ident, spec in sorted(data.get("profiles", {}).items()):
        for key, value in sorted(spec.get("locked", {}).items()):
            name = _bare(key)
            if name.startswith("ICLFORGE_") and name not in now and value != "n":
                findings.append(
                    Finding(
                        "kconfig",
                        f"profile {ident} locks {key}={value} and no Kconfig of the build declares "
                        f"it (read: {', '.join(KCONFIG_FILES)}): a removed key's lock goes with "
                        "it, and a misspelt one is never held",
                    )
                )
    return findings


def _git_text(root: Path, ref: str, path: str) -> str | None:
    """`path` as it is at `ref`, or None where it did not exist."""
    result = subprocess.run(
        ["git", "-C", str(root), "show", f"{ref}:{path}"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return result.stdout if result.returncode == 0 else None


# --- the file -------------------------------------------------------------------------


def load(path: Path = PROFILES) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise ProfileError(f"{path} cannot be read: {error}") from error
    if data.get("format") != FORMAT:
        raise ProfileError(f"{path} is format {data.get('format')}, this reads {FORMAT}")
    return data


def profile(data: dict[str, Any], ident: str) -> dict[str, Any]:
    found = data.get("profiles", {}).get(ident)
    if found is None:
        known = ", ".join(sorted(data.get("profiles", {})))
        raise ProfileError(f"no profile '{ident}' (known: {known})")
    return found


# --- fragments ------------------------------------------------------------------------


def parse_config(text: str) -> dict[str, str]:
    """`CONFIG_X=value` lines and `# CONFIG_X is not set` ones, the latter as "n"."""
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        match = _SET.match(line)
        if match:
            value = match.group(2).strip()
            if len(value) >= 2 and value[0] == value[-1] == '"':
                value = value[1:-1]
            values[match.group(1)] = value
            continue
        match = _NOT_SET.match(line)
        if match:
            values[match.group(1)] = "n"
    return values


def overlay_spec(data: dict[str, Any], name: str) -> dict[str, Any]:
    spec = data.get("overlays", {}).get(name)
    if spec is None:
        known = ", ".join(sorted(data.get("overlays", {})))
        raise ProfileError(f"no overlay '{name}' (declared: {known})")
    return spec


def fragment_names(data: dict[str, Any], ident: str, overlays: tuple[str, ...] = ()) -> list[str]:
    """The sdkconfig files of the build, in the order ESP-IDF reads them."""
    names = ["sdkconfig.defaults"]
    names += [f"sdkconfig.{name}" for name in profile(data, ident)["fragments"]]
    for overlay in overlays:
        fragment = overlay_spec(data, overlay).get("fragment")
        if fragment:
            names.append(f"sdkconfig.{fragment}")
    return names


def defaults_string(
    data: dict[str, Any], ident: str, overlays: tuple[str, ...] = (), wifi: str | None = None
) -> str:
    parts = fragment_names(data, ident, overlays)
    if wifi:
        parts.append(wifi)
    return ";".join(parts)


def requested(
    data: dict[str, Any], ident: str, overlays: tuple[str, ...] = (), project: Path = PROJECT
) -> dict[str, str]:
    """What the fragments ask for, later files over earlier ones."""
    merged: dict[str, str] = {}
    for name in fragment_names(data, ident, overlays):
        path = project / name
        try:
            merged.update(parse_config(path.read_text("utf-8")))
        except OSError as error:
            raise ProfileError(f"{path} cannot be read: {error}") from error
    return merged


# --- keys -----------------------------------------------------------------------------


def _bare(key: str) -> str:
    return key[len("CONFIG_") :] if key.startswith("CONFIG_") else key


def _matches(key: str, patterns: Any) -> bool:
    bare = _bare(key)
    return any(fnmatch.fnmatchcase(bare, pattern) for pattern in patterns)


def is_free(data: dict[str, Any], key: str) -> bool:
    return _matches(key, data.get("free", {}))


def is_watched(data: dict[str, Any], key: str) -> bool:
    return _matches(key, data.get("watch", []))


def touched(data: dict[str, Any], overlays: tuple[str, ...]) -> list[str]:
    patterns: list[str] = []
    for overlay in overlays:
        patterns += overlay_spec(data, overlay).get("touches", [])
    return patterns


def snapshot(
    data: dict[str, Any], ident: str, final: dict[str, str], asked: dict[str, str]
) -> dict[str, str]:
    """The profile's locked values from a resolved sdkconfig `final`: the keys the
    fragments set, and the keys the watch patterns match that are on or have a
    value (the members of a choice that were not taken are not each a decision),
    less the free ones."""
    keys = set(asked) | {key for key in final if is_watched(data, key) and final[key] != "n"}
    out: dict[str, str] = {}
    for key in sorted(keys):
        if is_free(data, key):
            continue
        out[key] = final.get(key, ABSENT)
    return out


# --- the build directory --------------------------------------------------------------


def cache_defaults(build: Path) -> list[str] | None:
    """SDKCONFIG_DEFAULTS as CMake kept it, or None if the build has no cache."""
    try:
        text = (build / "CMakeCache.txt").read_text("utf-8", "replace")
    except OSError:
        return None
    for line in text.splitlines():
        match = _CACHE_DEFAULTS.match(line.strip())
        if match:
            return [part for part in match.group(1).split(";") if part]
    return []


def cache_value(build: Path, name: str) -> str | None:
    """A variable of the build's CMakeCache.txt, or None if it has none by that name."""
    try:
        text = (build / "CMakeCache.txt").read_text("utf-8", "replace")
    except OSError:
        return None
    prefix = f"{name}:"
    for line in text.splitlines():
        if line.startswith(prefix):
            _, _, value = line.partition("=")
            return value.strip()
    return None


def profile_hash(data: dict[str, Any], ident: str) -> str:
    """A short hash of what a profile is: its fragments and its locked values. A board reports
    the one its image was built with, so one running an older definition is seen."""
    spec = profile(data, ident)
    canon = json.dumps(
        {"fragments": spec["fragments"], "locked": spec.get("locked", {})},
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()[:12]


def read_size_report(path: Path) -> dict[str, int]:
    """The memory types of `idf.py size --format json2`, as name -> bytes used.

    The report may follow other output (a CMake run the command made first), so the
    first JSON object with a `layout` is the one."""
    text = path.read_text("utf-8", "replace")
    decoder = json.JSONDecoder()
    start = text.find("{")
    while start != -1:
        try:
            found, _ = decoder.raw_decode(text[start:])
        except ValueError:
            found = None
        if isinstance(found, dict) and isinstance(found.get("layout"), list):
            return {str(item["name"]): int(item["used"]) for item in found["layout"]}
        start = text.find("{", start + 1)
    raise ProfileError(f"{path} holds no `idf.py size --format json2` report")


def read_final(build: Path) -> dict[str, str]:
    path = build / "sdkconfig"
    try:
        return parse_config(path.read_text("utf-8", "replace"))
    except OSError as error:
        raise ProfileError(f"{path} cannot be read: {error}") from error


# --- the guard ------------------------------------------------------------------------


def verify(
    data: dict[str, Any],
    ident: str,
    build: Path,
    overlays: tuple[str, ...] = (),
    size_json: Path | None = None,
    app_headroom: int = 0,
) -> list[Finding]:
    spec = profile(data, ident)
    findings: list[Finding] = []
    moved = touched(data, overlays)
    final = read_final(build)

    # 1. The fragments the build was given.
    actual = cache_defaults(build)
    if actual is None:
        findings.append(
            Finding("fragments", f"{build} has no CMakeCache.txt, so its fragments cannot be read")
        )
    else:
        names = [Path(part).name for part in actual]
        extras = [name for name in names if not name.startswith("sdkconfig.")]
        own = [name for name in names if name.startswith("sdkconfig.")]
        want = fragment_names(data, ident, overlays)
        if own != want:
            findings.append(
                Finding(
                    "fragments",
                    "the build was given "
                    + ";".join(own)
                    + " and the profile's are "
                    + ";".join(want),
                )
            )
        if extras and "wifi-credentials" not in overlays:
            findings.append(
                Finding(
                    "fragments",
                    f"the build was given {';'.join(extras)}, which no declared overlay is",
                )
            )

    # 1b. The profile the image says it is (main/CMakeLists.txt, ICLFORGE_PROFILE), which is
    # what GET /hardware reports and what the board suite refuses a board for.
    if actual is not None:
        built_as = cache_value(build, "ICLFORGE_PROFILE")
        built_hash = cache_value(build, "ICLFORGE_PROFILE_HASH")
        if not built_as:
            findings.append(
                Finding(
                    "identity",
                    "the build does not carry its profile: configure it with "
                    f"`sink_profile.py identity {ident} --cmake`",
                )
            )
        elif built_as != ident:
            findings.append(Finding("identity", f"the build says it is {built_as}, not {ident}"))
        elif built_hash != profile_hash(data, ident):
            findings.append(
                Finding(
                    "identity",
                    f"the build carries hash {built_hash or '(none)'} and profile {ident} is "
                    f"{profile_hash(data, ident)}: it was configured from another definition",
                )
            )

    # 2. Locked values.
    locked: dict[str, str] = spec.get("locked", {})
    if not locked:
        findings.append(Finding("locked", f"profile {ident} locks no key: run snapshot"))
    for key, want_value in sorted(locked.items()):
        if _matches(key, moved):
            continue
        got = final.get(key, ABSENT)
        if got == want_value:
            continue
        if got == ABSENT and want_value == "n":
            continue
        findings.append(Finding("locked", f"{key} is {got}, the profile locks {want_value}"))

    # 3. A key that looks like a size, a placement or a cache, and nobody placed.
    for key in sorted(final):
        if key in locked or is_free(data, key) or _matches(key, moved):
            continue
        if is_watched(data, key) and final[key] != "n":
            findings.append(
                Finding(
                    "unplaced",
                    f"{key}={final[key]} matches a watch pattern and the profile neither locks "
                    "nor frees it",
                )
            )

    # 4. Budgets.
    budget = spec.get("budget", {})
    app = build / APP_NAME
    if app.is_file() and budget.get("app_bytes_max") is not None:
        size = app.stat().st_size
        if size > budget["app_bytes_max"]:
            findings.append(
                Finding(
                    "budget",
                    f"the app image is {size:,} bytes, over the profile's "
                    f"{budget['app_bytes_max']:,}",
                )
            )
        elif size > budget["app_bytes_max"] - app_headroom:
            # Under the budget but with less room than the caller asks a change to leave: the
            # next change that adds a feature would be the one that fails, with no lead time.
            findings.append(
                Finding(
                    "budget",
                    f"the app image is {size:,} bytes, {budget['app_bytes_max'] - size:,} under "
                    f"the profile's {budget['app_bytes_max']:,}: a change must leave "
                    f"{app_headroom:,} free",
                )
            )
    boot = build / "bootloader" / "bootloader.bin"
    if boot.is_file() and budget.get("bootloader_bytes_max") is not None:
        size = boot.stat().st_size
        if size > budget["bootloader_bytes_max"]:
            findings.append(
                Finding(
                    "budget",
                    f"the bootloader is {size:,} bytes, over the profile's "
                    f"{budget['bootloader_bytes_max']:,}",
                )
            )
    # Static internal RAM: each byte of it is a byte less in the heap's largest block on any
    # board, so this is the figure that makes "RAM erodes as features land" a build failure.
    if size_json is not None and budget.get("internal_static_bytes_max") is not None:
        used = read_size_report(size_json).get("DIRAM")
        if used is None:
            findings.append(Finding("budget", f"{size_json} has no DIRAM in its layout"))
        elif used > budget["internal_static_bytes_max"]:
            findings.append(
                Finding(
                    "budget",
                    f"static internal RAM (DIRAM) is {used:,} bytes, over the profile's "
                    f"{budget['internal_static_bytes_max']:,}",
                )
            )
    return findings


# --- what other tools ask for ---------------------------------------------------------


def image_rule(data: dict[str, Any], name: str) -> dict[str, Any]:
    """The rule tools/ci/check_firmware_package.py holds a published image to."""
    for ident, spec in data.get("profiles", {}).items():
        image = spec.get("image")
        if image and image.get("name") == name:
            chip, chip_id = TARGETS[spec["target"]]
            return {
                "profile": ident,
                "name": name,
                "target": spec["target"],
                "chip": chip,
                "chip_id": chip_id,
                "flash_size": image["flash_size"],
                "psram": image["psram"],
                "lowest": image["lowest"],
                "highest": image["highest"],
                "refuse_above": image.get("refuse_above"),
            }
    raise ProfileError(f"no profile publishes an image named '{name}'")


def expected_console(data: dict[str, Any], ident: str) -> list[str]:
    """The console lines a play and the Sendspin player print that say where the profile put
    its rings and stacks: what the QEMU twin of the release shape is held to, so that a stack
    that falls back to internal RAM is a missing line, whatever the emulated heap could spare.
    Only what the profile locks is expected: a profile with no PSRAM expects none of the
    PSRAM lines."""
    locked = profile(data, ident).get("locked", {})

    def value(name: str) -> str | None:
        return locked.get(f"CONFIG_ICLFORGE_EXAMPLE_{name}")

    lines: list[str] = []
    if locked.get("CONFIG_SPIRAM") == "y":
        if value("RING_IN_PSRAM") == "1":
            lines.append(f"player: ring {value('RING_BYTES')} bytes in PSRAM")
        if value("DECODE_STACK_IN_PSRAM") == "1":
            lines.append(f"player: decode stack {value('DECODE_STACK_BYTES')} bytes in PSRAM")
        if locked.get("CONFIG_ICLFORGE_SENDSPIN") == "y":
            lines.append(
                f"burst player: ring {value('SENDSPIN_RING_BYTES')} bytes and buffers in PSRAM"
            )
            if value("SENDSPIN_DECODE_STACK_IN_PSRAM") == "1":
                stack = value("SENDSPIN_DECODE_STACK_BYTES")
                lines.append(f"burst player: decode stack {stack} bytes in PSRAM")
    core = value("DECODE_CORE")
    if value("DECODE_LANE") == "1" and core in ("0", "1"):
        lines.append(f"player: decode stages on cores {core} and {1 - int(core)}")
    return lines


def table(data: dict[str, Any], idents: list[str]) -> str:
    rows = [
        "| Profile | Fragments after `sdkconfig.defaults`, in order | Release image | Boards |",
        "|---|---|---|---|",
    ]
    for ident in idents or sorted(data.get("profiles", {})):
        spec = profile(data, ident)
        image = spec.get("image")
        rows.append(
            "| `{}` | {} | {} | {} |".format(
                ident,
                ", ".join(f"`{name}`" for name in spec["fragments"]),
                f"`{image['name']}`" if image else "none",
                ", ".join(spec.get("boards", [])) or "none",
            )
        )
    return "\n".join(rows) + "\n"


# --- the docs -------------------------------------------------------------------------

STACK = re.compile(r"sdkconfig\.defaults(?:;sdkconfig\.[A-Za-z0-9_-]+)+")
TABLE_BEGIN = "<!-- sink-profiles:begin -->"
TABLE_END = "<!-- sink-profiles:end -->"

# Where the tree states which fragments a build takes, as globs from the repository root.
DOC_FILES = (
    "firmware/hearth-sink/README.md",
    "firmware/hearth-sink/CMakeLists.txt",
    "firmware/hearth-sink/sdkconfig.*",
    "firmware/esp-idf/iclforge/README.md",
    "docs/hearth/*.md",
    "docs/platforms/bare-metal/esp32-*.md",
    "planning/esp32-*.md",
    "apps/demos/wasm/tests/device-ui/payloads/README.md",
)


def known_stacks(data: dict[str, Any]) -> dict[str, str]:
    """Every fragment list the tree may spell: the profiles', and the others listed with why."""
    known = {defaults_string(data, ident): f"profile {ident}" for ident in data.get("profiles", {})}
    known.update(data.get("other_stacks", {}))
    return known


def check_docs(
    data: dict[str, Any], root: Path = REPO, files: list[Path] | None = None, write: bool = False
) -> list[Finding]:
    """Every `sdkconfig.defaults;sdkconfig.x;...` the docs and fragments spell is a profile's,
    a CI shape's or listed in `other_stacks`; and a generated table is the file's own."""
    known = known_stacks(data)
    findings: list[Finding] = []
    paths = files
    if paths is None:
        paths = sorted({path for pattern in DOC_FILES for path in root.glob(pattern)})
    for path in paths:
        try:
            text = path.read_text("utf-8", "replace")
        except OSError as error:
            raise ProfileError(f"{path} cannot be read: {error}") from error
        try:
            where = path.relative_to(root).as_posix()
        except ValueError:
            where = path.as_posix()
        for number, line in enumerate(text.splitlines(), 1):
            for stack in STACK.findall(line):
                if stack in known or "sdkconfig.ci" in stack:
                    continue
                findings.append(
                    Finding(
                        "docs",
                        f"{where}:{number}: '{stack}' is no profile's fragment list, and "
                        "profiles.json's other_stacks does not list it",
                    )
                )
        begin = text.find(TABLE_BEGIN)
        if begin == -1:
            continue
        end = text.find(TABLE_END, begin)
        if end == -1:
            findings.append(Finding("docs", f"{where}: {TABLE_BEGIN} has no {TABLE_END}"))
            continue
        inner = text[begin + len(TABLE_BEGIN) : end]
        wanted = "\n\n" + table(data, []) + "\n"
        if inner.replace("\r\n", "\n") != wanted:
            if write:
                path.write_text(
                    text[: begin + len(TABLE_BEGIN)] + wanted + text[end:], "utf-8", newline="\n"
                )
            else:
                findings.append(
                    Finding(
                        "docs",
                        f"{where}: the profile table is not the file's: run "
                        "`sink_profile.py check-docs --write`",
                    )
                )
    return findings


# --- command line ---------------------------------------------------------------------


def _report(findings: list[Finding], ident: str, github: bool, level: str = "error") -> None:
    for finding in findings:
        if github:
            print(f"::{level} title=profile {ident}, {finding.rule}::{finding.message}")
        else:
            print(f"{ident}: [{finding.rule}] {finding.message}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--profiles", type=Path, default=PROFILES, help="the profiles file")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("list", help="the profiles")

    one = commands.add_parser("defaults", help="the SDKCONFIG_DEFAULTS string of a profile")
    one.add_argument("profile")
    one.add_argument("--overlay", action="append", default=[])
    one.add_argument("--wifi", help="a file of the board's network, appended last")

    check = commands.add_parser("verify", help="hold a configured build to its profile")
    check.add_argument("profile")
    check.add_argument("--build-dir", type=Path, required=True)
    check.add_argument("--overlay", action="append", default=[])
    check.add_argument("--github", action="store_true", help="print GitHub annotations")
    check.add_argument(
        "--size-json", type=Path, help="`idf.py size --format json2` output, for the RAM budget"
    )
    check.add_argument(
        "--warn-only",
        action="store_true",
        help="report findings as warnings and exit 0 (the repository's ESP32_PROFILE_GUARD=warn)",
    )
    check.add_argument(
        "--app-headroom",
        type=int,
        default=0,
        metavar="BYTES",
        help="also fail an app image with fewer than this many bytes left under its budget",
    )

    expect = commands.add_parser(
        "expect-console",
        help="the console lines that say where a profile puts its rings and stacks",
    )
    expect.add_argument("profile")

    who = commands.add_parser("identity", help="a profile's id and the hash of what it locks")
    who.add_argument("profile")
    who.add_argument("--cmake", action="store_true", help="as the -D options of a configure")

    docs = commands.add_parser("check-docs", help="the fragment lists the docs spell")
    docs.add_argument("--write", action="store_true", help="refresh a generated table")
    docs.add_argument("--root", type=Path, default=REPO)
    docs.add_argument("files", nargs="*", type=Path)

    snap = commands.add_parser("snapshot", help="the locked values a configured build resolved")
    snap.add_argument("profile")
    snap.add_argument("--build-dir", type=Path, required=True)
    snap.add_argument("--write", action="store_true", help="write them into the profiles file")

    rule = commands.add_parser("image", help="the rule for a published image, as JSON")
    rule.add_argument("name")

    change = commands.add_parser(
        "check-change",
        help="hold the Kconfig keys a change adds or removes to the profiles (no ESP-IDF)",
    )
    change.add_argument("--base", help="the commit the change is made on (git ref)")
    change.add_argument(
        "--all", action="store_true", help="every key, as if no base: a lint of the tree"
    )
    change.add_argument("--root", type=Path, default=REPO)

    tab = commands.add_parser("table", help="a markdown table of profiles")
    tab.add_argument("idents", nargs="*", metavar="PROFILE")

    args = parser.parse_args(argv)
    try:
        data = load(args.profiles)
        if args.command == "list":
            for ident in sorted(data["profiles"]):
                spec = data["profiles"][ident]
                print(f"{ident}\t{spec['target']}\t{defaults_string(data, ident)}")
        elif args.command == "defaults":
            print(defaults_string(data, args.profile, tuple(args.overlay), args.wifi))
        elif args.command == "verify":
            findings = verify(
                data,
                args.profile,
                args.build_dir,
                tuple(args.overlay),
                args.size_json,
                args.app_headroom,
            )
            _report(findings, args.profile, args.github, "warning" if args.warn_only else "error")
            if findings and not args.warn_only:
                return 1
            if not findings:
                print(f"{args.profile}: the build holds to its profile")
        elif args.command == "expect-console":
            for line in expected_console(data, args.profile):
                print(line)
        elif args.command == "identity":
            digest = profile_hash(data, args.profile)
            if args.cmake:
                print(f"-DICLFORGE_PROFILE={args.profile} -DICLFORGE_PROFILE_HASH={digest}")
            else:
                print(f"{args.profile} {digest}")
        elif args.command == "check-docs":
            findings = check_docs(data, args.root, args.files or None, args.write)
            _report(findings, "docs", False)
            if findings:
                return 1
            print("check-docs: every fragment list the tree spells is a profile's or a listed one")
        elif args.command == "check-change":
            if not args.base and not args.all:
                raise ProfileError("check-change needs --base REF, or --all")
            head = {}
            for path in KCONFIG_FILES:
                try:
                    head[path] = (args.root / path).read_text("utf-8", "replace")
                except OSError as error:
                    raise ProfileError(f"{path} cannot be read: {error}") from error
            base = None
            if args.base and not args.all:
                base = {}
                for path in KCONFIG_FILES:
                    text = _git_text(args.root, args.base, path)
                    if text is not None:
                        base[path] = text
            findings = check_change(data, head, base)
            _report(findings, "kconfig", False)
            if findings:
                return 1
            print("check-change: the keys the Kconfig adds are placed in the profiles")
        elif args.command == "snapshot":
            final = read_final(args.build_dir)
            asked = requested(data, args.profile)
            values = snapshot(data, args.profile, final, asked)
            if args.write:
                data["profiles"][args.profile]["locked"] = values
                args.profiles.write_text(json.dumps(data, indent=2) + "\n", "utf-8")
                print(f"{args.profile}: {len(values)} locked keys written to {args.profiles}")
            else:
                print(json.dumps(values, indent=2))
            ignored = {
                key: (asked[key], final.get(key, ABSENT))
                for key in asked
                if not is_free(data, key) and final.get(key, ABSENT) != asked[key]
            }
            for key, (want, got) in sorted(ignored.items()):
                print(
                    f"note: a fragment asks for {key}={want} and the build has {got}",
                    file=sys.stderr,
                )
        elif args.command == "image":
            print(json.dumps(image_rule(data, args.name), indent=2))
        elif args.command == "table":
            print(table(data, args.idents), end="")
    except ProfileError as error:
        print(f"sink_profile: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
