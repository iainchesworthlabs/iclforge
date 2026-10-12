#!/usr/bin/env python3
"""The locked build profiles of hearth_sink's ESP32 boards (planning/esp32-platform-profiles.md).

    python tools/hearth/sink_profile.py list
    python tools/hearth/sink_profile.py defaults s3-n16r8 [--overlay NAME ...] [--wifi FILE]
    python tools/hearth/sink_profile.py verify s3-n16r8 --build-dir BUILD [--overlay NAME ...]
    python tools/hearth/sink_profile.py snapshot s3-n16r8 --build-dir BUILD [--write]
    python tools/hearth/sink_profile.py image hearth-sink-esp32s3
    python tools/hearth/sink_profile.py table [PROFILE ...]

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
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

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


# --- command line ---------------------------------------------------------------------


def _report(findings: list[Finding], ident: str, github: bool) -> None:
    for finding in findings:
        if github:
            print(f"::error title=profile {ident}, {finding.rule}::{finding.message}")
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

    snap = commands.add_parser("snapshot", help="the locked values a configured build resolved")
    snap.add_argument("profile")
    snap.add_argument("--build-dir", type=Path, required=True)
    snap.add_argument("--write", action="store_true", help="write them into the profiles file")

    rule = commands.add_parser("image", help="the rule for a published image, as JSON")
    rule.add_argument("name")

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
            findings = verify(data, args.profile, args.build_dir, tuple(args.overlay))
            _report(findings, args.profile, args.github)
            if findings:
                return 1
            print(f"{args.profile}: the build holds to its profile")
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
