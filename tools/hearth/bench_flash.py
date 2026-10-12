#!/usr/bin/env python3
"""The bench's writes: build an image from a profile, put it on a board, check it, push it.

    python tools/hearth/bench_flash.py build  PROFILE --tag TAG [--worktree DIR]
                                              [--overlay NAME ...] [--wifi] [--no-verify]
    python tools/hearth/bench_flash.py flash  BOARD --build-dir DIR --session ID
                                              [--app-only] [--erase]
    python tools/hearth/bench_flash.py verify BOARD --build-dir DIR [--profile PROFILE]
    python tools/hearth/bench_flash.py ota    BOARD --build-dir DIR --session ID [--force]

It replaces the scripts that lived in D:\\ac3bld\\esp-reflash (planning/esp32-board-bench.md),
and what they took from a hand-kept list of fragments it takes from the profile:

- **build** is the build CI makes of the same profile (`sink_profile.py defaults`, the identity
  options, `idf.py size`, `sink_profile.py verify`), in the ESP-IDF environment, into
  <builds>/<tag>-<profile>. `--wifi` appends the bench's network fragment (never printed) with the
  `wifi-credentials` overlay, which the guard then allows to touch the network keys.
- **flash** needs the board's lock to be this session's, reads the MAC of the chip on the port and
  writes nothing unless its last three octets are the bench's for that board, then writes with
  esptool. `--app-only` writes the app and otadata and leaves the bootloader, the table and NVS
  (the P4b's recovery); a board whose row says `after_flash: watchdog-reset` is started that way.
  `--erase` wipes the flash first, and the board's pairing and stored network with it.
- **verify** compares what GET /hardware says (project, version, profile, hash) with the build.
- **ota** is `ota.py push` with the board's lock and address taken from the table, and refused for
  a board whose row says it takes no OTA.

Nothing prints or writes a network value: the logs go through bench.mask_text.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import sink_profile
from bench import Bench, BenchError

REPO = Path(__file__).resolve().parents[2]
OTA = Path(__file__).resolve().parent / "ota.py"

# argv, a working directory and whether to run it in the ESP-IDF environment in; the exit code
# and the output (both streams) out.
Runner = Callable[..., tuple[int, str]]
# GET /hardware as a dict, or None when the board does not answer.
Prober = Callable[[str], dict[str, Any] | None]

# A Git Bash or MSYS shell sets these, and ESP-IDF's export script refuses to run under them
# ("MSys/Mingw is not supported"): a child that is to run in the IDF environment goes without.
_MSYS_PREFIXES = ("MSYS", "MINGW")


def clean_environment() -> dict[str, str]:
    return {
        key: value
        for key, value in os.environ.items()
        if not key.upper().startswith(_MSYS_PREFIXES)
    }


def run_process(
    argv: Sequence[str], cwd: Path | None = None, env: dict[str, str] | None = None
) -> tuple[int, str]:
    try:
        done = subprocess.run(
            list(argv),
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as error:
        return 127, f"{argv[0]}: {error}"
    return done.returncode, done.stdout


def probe_hardware(host: str) -> dict[str, Any] | None:
    try:
        status, _, body = bench.http_get(host, "/hardware", timeout=4.0)
        return json.loads(body) if status == 200 else None
    except (BenchError, ValueError):
        return None


# --- the ESP-IDF environment ---------------------------------------------------------------


def _quote_powershell(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def in_idf(table: Bench, argv: Sequence[str], windows: bool | None = None) -> list[str]:
    """`argv` run after the ESP-IDF environment is exported: PowerShell and export.ps1 on
    Windows, where the bench is, bash and export.sh elsewhere. The command that is not found
    after the export is an exit code of 127, not the nothing PowerShell would otherwise leave."""
    windows = os.name == "nt" if windows is None else windows
    command = argv[0]
    if windows:
        script = (
            f"$env:IDF_TOOLS_PATH={_quote_powershell(table.idf_tools.as_posix())}; "
            f". {_quote_powershell((table.idf / 'export.ps1').as_posix())} *> $null; "
            f"if (-not (Get-Command {_quote_powershell(command)} -ErrorAction SilentlyContinue)) "
            "{ [Console]::Error.WriteLine('the ESP-IDF environment did not export "
            f"{command.replace(chr(39), '')}'); exit 127 }}; "
            f"& {' '.join(_quote_powershell(a) for a in argv)}; "
            "if ($null -eq $LASTEXITCODE) { exit 1 }; exit $LASTEXITCODE"
        )
        shell = shutil.which("pwsh") or shutil.which("powershell") or "powershell"
        return [shell, "-NoProfile", "-NonInteractive", "-Command", script]
    script = (
        f"export IDF_TOOLS_PATH={shlex.quote(table.idf_tools.as_posix())}; "
        f". {shlex.quote((table.idf / 'export.sh').as_posix())} >/dev/null 2>&1; "
        f"exec {shlex.join(list(argv))}"
    )
    return ["bash", "-c", script]


def make_runner(table: Bench) -> Runner:
    """The real runner: an `idf=True` command is wrapped by in_idf and gets an environment
    without the MSYS variables."""

    def run(argv: Sequence[str], cwd: Path | None = None, *, idf: bool = False) -> tuple[int, str]:
        if idf:
            env = clean_environment()
            # idf.py warns, and kconfgen can fail on a non-ASCII path, without Python's UTF-8 mode
            env.setdefault("PYTHONUTF8", "1")
            return run_process(in_idf(table, argv), cwd, env)
        return run_process(argv, cwd)

    return run


# --- build --------------------------------------------------------------------------------


def build_directory(table: Bench, tag: str, ident: str) -> Path:
    return table.builds / f"{tag}-{ident}"


def cmd_build(
    table: Bench, args: argparse.Namespace, run: Runner, out: Callable[[str], None]
) -> int:
    data = sink_profile.load()
    spec = sink_profile.profile(data, args.profile)
    overlays = tuple(args.overlay or ())
    if args.wifi and "wifi-credentials" not in overlays:
        overlays += ("wifi-credentials",)
    worktree = Path(args.worktree) if args.worktree else REPO
    project = worktree / "firmware" / "hearth-sink"
    if not project.is_dir():
        raise BenchError(f"{project} is not a checkout's firmware/hearth-sink")
    wifi = table.wifi_fragment.as_posix() if args.wifi else None
    if wifi and not table.wifi_fragment.is_file():
        raise BenchError(f"the network fragment {table.wifi_fragment} is not there")
    directory = build_directory(table, args.tag, args.profile)
    defaults = sink_profile.defaults_string(data, args.profile, overlays, wifi)
    digest = sink_profile.profile_hash(data, args.profile)
    code, described = run(
        ["git", "-C", str(worktree), "describe", "--tags", "--always", "--dirty"], None
    )
    version = described.strip() if code == 0 else ""
    argv = [
        "idf.py",
        "-C",
        str(project),
        "-B",
        str(directory),
        f"-DSDKCONFIG={directory / 'sdkconfig'}",
        f"-DIDF_TARGET={spec['target']}",
        f"-DSDKCONFIG_DEFAULTS={defaults}",
        f"-DICLFORGE_PROFILE={args.profile}",
        f"-DICLFORGE_PROFILE_HASH={digest}",
        *([f"-DPROJECT_VER={version}"] if version else []),
        "build",
    ]
    values = bench.wifi_values(table.wifi_fragment)
    log = directory.parent / f"{directory.name}.log"
    directory.parent.mkdir(parents=True, exist_ok=True)
    out(f"building {args.profile} ({spec['target']}) into {directory}")
    out(f"  fragments: {defaults.replace(table.wifi_fragment.as_posix(), '<network fragment>')}")
    code, text = run(argv, None, idf=True)
    if code == 0 and not (directory / "project_description.json").is_file():
        text += "\nidf.py exited 0 and left no build in the directory"
        code = 1
    lines = [f"profile {args.profile} hash {digest} version {version or '(none)'}"]
    lines.append(bench.mask_text(text, values))
    lines.append(f"BUILD_EXIT={code}")
    status = code
    if code == 0 and not args.no_verify:
        sized, size_text = run(
            ["idf.py", "-C", str(project), "-B", str(directory), "size", "--format", "json2"],
            None,
            idf=True,
        )
        size_json = directory / "size.json"
        if sized == 0:
            size_json.write_text(size_text, "utf-8")
        checked, verdict = run(
            [
                sys.executable,
                str(Path(__file__).resolve().parent / "sink_profile.py"),
                "verify",
                args.profile,
                "--build-dir",
                str(directory),
                *(["--size-json", str(size_json)] if sized == 0 else []),
                *[f"--overlay={name}" for name in overlays],
            ],
            None,
        )
        verdict = bench.mask_text(verdict, values)
        lines.append(verdict)
        out(verdict.strip())
        status = checked
    log.write_text("\n".join(lines) + "\n", "utf-8")
    if code != 0:
        tail = "\n".join(lines[1].splitlines()[-15:])
        out(tail)
    out(f"{'built' if status == 0 else 'FAILED'}; log {log}")
    return 0 if status == 0 else 1


# --- flash --------------------------------------------------------------------------------

_MAC = re.compile(r"(?m)^(BASE MAC|MAC):\s+((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\s*$")


def read_mac(esptool_output: str) -> str | None:
    """The chip's 6-octet MAC from esptool read-mac. esptool 5 prints an 8-octet EUI-64 on the
    C6's `MAC:` line and the 6-octet factory MAC on `BASE MAC:`; the S3 and the P4 only have
    the first form."""
    found = {m.group(1): m.group(2) for m in _MAC.finditer(esptool_output)}
    return found.get("BASE MAC") or found.get("MAC")


def mac_tail(mac: str) -> str:
    return "".join(mac.split(":")[-3:])


def esptool(board: bench.Board, *tail: str, baud: int | None = None) -> list[str]:
    head = ["python", "-m", "esptool", "--chip", board.chip, "--port", board.port]
    if baud:
        head += ["--baud", str(baud)]
    return [*head, *tail]


def flash_plan(build: Path, app_only: bool) -> list[str]:
    """The esptool arguments after `write-flash`, from the build's own files: all of it as
    `@flash_args` (which also writes the audio sample and the storage image), or, for
    `app_only`, the `app` and `otadata` sections of flasher_args.json and nothing else, so that
    the bootloader, the partition table, NVS and storage stay as the board has them."""
    if not app_only:
        if not (build / "flash_args").is_file():
            raise BenchError(f"{build / 'flash_args'} is not there: is it a finished build?")
        return ["@flash_args"]
    try:
        info = json.loads((build / "flasher_args.json").read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise BenchError(f"{build / 'flasher_args.json'}: {error}") from error
    if "app" not in info:
        raise BenchError(f"{build / 'flasher_args.json'} has no app section to write")
    sections = [info[name] for name in ("otadata", "app") if name in info]
    settings = info.get("flash_settings", {})
    args: list[str] = []
    for key, option in (
        ("flash_mode", "--flash-mode"),
        ("flash_freq", "--flash-freq"),
        ("flash_size", "--flash-size"),
    ):
        if settings.get(key):
            args += [option, str(settings[key])]
    for section in sorted(sections, key=lambda item: int(item["offset"], 16)):
        args += [section["offset"], section["file"]]
    return args


def wait_for_board(
    host: str, probe: Prober, seconds: float = 120.0, sleep: float = 3.0
) -> dict[str, Any] | None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        hardware = probe(host)
        if hardware is not None:
            return hardware
        time.sleep(sleep)
    return None


def cmd_flash(
    table: Bench,
    args: argparse.Namespace,
    run: Runner,
    out: Callable[[str], None],
    probe: Prober = probe_hardware,
    wait: float = 120.0,
) -> int:
    board = table.board(args.board)
    out(f"{board.name}: {bench.require_lock(board, args.session)}")
    build = Path(args.build_dir)
    plan = flash_plan(build, args.app_only)
    # The chip is left as the bench's row starts it: running its app, or, for a board whose
    # resets land in the ROM's download mode, started by a watchdog reset.
    code, text = run(
        esptool(board, "--before", "default-reset", "--after", board.after_flash, "read-mac"),
        None,
        idf=True,
    )
    mac = read_mac(text)
    if mac is None:
        raise BenchError(f"{board.port}: no MAC read (esptool exit {code}); nothing was written")
    out(
        f"{board.port}: chip MAC ends {mac_tail(mac)}, the bench's {board.name} is {board.mac_tail}"
    )
    if mac_tail(mac) != board.mac_tail:
        raise BenchError(f"{board.port} is not {board.name}: its MAC differs; nothing was written")
    values = bench.wifi_values(table.wifi_fragment)
    log = build / f"flash-{board.name}.log"
    lines: list[str] = []
    if args.erase:
        out(f"{board.name}: erasing the flash, and with it the pairing and the stored network")
        code, text = run(esptool(board, "erase-flash"), None, idf=True)
        lines.append(text)
        if code != 0:
            log.write_text(bench.mask_text("\n".join(lines), values), "utf-8")
            raise BenchError(f"erase-flash exited {code}; see {log}")
    started = time.monotonic()
    code, text = run(
        esptool(
            board,
            "--before",
            "default-reset",
            "--after",
            board.after_flash,
            "write-flash",
            *plan,
            baud=460800,
        ),
        build,
        idf=True,
    )
    lines.append(text)
    log.write_text(bench.mask_text("\n".join(lines), values), "utf-8")
    out(f"{board.name}: esptool exit {code} in {time.monotonic() - started:.0f} s; log {log}")
    if code != 0:
        return 1
    hardware = wait_for_board(board.host, probe, wait)
    if hardware is None:
        out(f"{board.name}: did not answer on {board.host} within {wait:.0f} s")
        return 1
    project, version = hardware.get("project"), hardware.get("version")
    out(f"{board.name}: answers; project {project} version {version}")
    return 0


# --- verify -------------------------------------------------------------------------------


def cmd_verify(
    table: Bench,
    args: argparse.Namespace,
    _run: Runner,
    out: Callable[[str], None],
    probe: Prober = probe_hardware,
) -> int:
    board = table.board(args.board)
    build = Path(args.build_dir)
    try:
        built = json.loads((build / "project_description.json").read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise BenchError(f"{build / 'project_description.json'}: {error}") from error
    hardware = probe(board.host)
    if hardware is None:
        raise BenchError(f"{board.name} does not answer on {board.host}")
    ok = True
    for label, says, has in (
        ("project", hardware.get("project"), built.get("project_name")),
        ("version", hardware.get("version"), built.get("project_version")),
    ):
        same = says == has
        ok = ok and same
        out(f"{label}: the board says {says}, the build {has}: {'matches' if same else 'DIFFERS'}")
    if args.profile:
        data = sink_profile.load()
        want = sink_profile.profile_hash(data, args.profile)
        same = hardware.get("profile") == args.profile and hardware.get("profile_hash") == want
        ok = ok and same
        out(
            f"profile: the board says {hardware.get('profile')} at {hardware.get('profile_hash')},"
            f" the file's {args.profile} is {want}: {'matches' if same else 'DIFFERS'}"
        )
    elif hardware.get("profile"):
        out(f"profile: {hardware.get('profile')} at {hardware.get('profile_hash')}")
    return 0 if ok else 1


# --- ota ----------------------------------------------------------------------------------


def cmd_ota(table: Bench, args: argparse.Namespace, run: Runner, out: Callable[[str], None]) -> int:
    board = table.board(args.board)
    if not board.ota:
        raise BenchError(
            f"{board.name} takes no OTA ({board.note or 'see bench.json'}): "
            f"bench_flash.py flash {board.name} --app-only puts the image on by USB"
        )
    out(f"{board.name}: {bench.require_lock(board, args.session)}")
    argv = [
        sys.executable,
        str(OTA),
        "push",
        "--build-dir",
        str(args.build_dir),
        "--host",
        board.host,
        *(["--force"] if args.force else []),
    ]
    code, text = run(argv, None)
    out(bench.mask_text(text, bench.wifi_values(table.wifi_fragment)).rstrip())
    return code


# --- command line --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="a profile's image, held to the profile")
    build.add_argument("profile")
    build.add_argument(
        "--tag", required=True, help="the build directory is <builds>/<tag>-<profile>"
    )
    build.add_argument("--worktree", help="a checkout other than this one")
    build.add_argument("--overlay", action="append", default=[])
    build.add_argument("--wifi", action="store_true", help="append the bench's network fragment")
    build.add_argument("--no-verify", action="store_true", help="skip idf.py size and the guard")

    flash = sub.add_parser("flash", help="write a build to a board by USB, after a MAC check")
    flash.add_argument("board")
    flash.add_argument("--build-dir", required=True)
    flash.add_argument("--session", required=True)
    flash.add_argument("--app-only", action="store_true")
    flash.add_argument("--erase", action="store_true")

    verify = sub.add_parser("verify", help="what a board says against what was built")
    verify.add_argument("board")
    verify.add_argument("--build-dir", required=True)
    verify.add_argument("--profile")

    ota = sub.add_parser("ota", help="ota.py push to a board the session has booked")
    ota.add_argument("board")
    ota.add_argument("--build-dir", required=True)
    ota.add_argument("--session", required=True)
    ota.add_argument("--force", action="store_true")
    return parser


def main(
    argv: list[str] | None = None,
    run: Runner | None = None,
    probe: Prober = probe_hardware,
    out: Callable[[str], None] = print,
    table: Bench | None = None,
) -> int:
    args = build_parser().parse_args(argv)
    try:
        chosen = table or bench.load()
        runner = run or make_runner(chosen)
        if args.command == "build":
            return cmd_build(chosen, args, runner, out)
        if args.command == "flash":
            return cmd_flash(chosen, args, runner, out, probe)
        if args.command == "verify":
            return cmd_verify(chosen, args, runner, out, probe)
        return cmd_ota(chosen, args, runner, out)
    except (BenchError, sink_profile.ProfileError) as error:
        print(f"bench_flash: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
