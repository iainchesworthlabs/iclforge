#!/usr/bin/env python3
"""Take the documentation's pictures again, from a build.

tools/pictures/pictures.json lists every picture a page shows: the program, its arguments, the
environment and the build it was last taken from. This runs each entry that has arguments and
puts the picture where the pages embed it:

    python3 tools/pictures/take_pictures.py --build <build tree> [--only <part of a path> ...]

    --build   a build tree of THIS checkout with the programs in bin/ (forge, forge-gui, hearth)
    --only    only the pictures whose path contains one of these
    --out     write them under this folder instead of over the committed ones, to look first
    --media   where the files the pictures play are kept (default: <build>/pictures-media);
              make_media.py fills it, which takes a minute the first time
    --stamp   record the commit and configuration of the build in the manifest, as the build of
              every picture taken
    --list    say what each entry is and stop

Windows only. A picture is the program drawing its own window with the real Windows platform
plugin, on a desktop nobody sees (hidden_desktop.py says why it is neither the offscreen plugin
nor the desktop in use). The environment each entry names is the manifest's; the two settings
that matter are QT_QPA_PLATFORM=windows:darkmode=0, the light theme whatever the desktop is set
to, and QSG_DISTANCEFIELD_ANTIALIASING=gray, without which the text has coloured subpixel edges.

An entry with `by_hand` is not taken here: it needs something a desk does not have (an Android
device, a virtual machine with a driver in it) or is not safe to run unasked (Crucible's capture
reads the person's settings and starts the engine). `--list` prints how each was made.

A picture is taken into a scratch folder first and replaces the committed one only when the
program said it had drawn what was asked for (exit code 0). A failed capture prints what the
program wrote and leaves the picture as it was, and the run ends with exit code 1.

The machine shows through in three ways, which is why a retake is looked at before it is
committed: the monospaced face is the system's own, a window is no taller than the screen it is
on, and the Forge GUI's live-capture pictures list the machine's capture devices.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "checks"))

import check_doc_pictures  # noqa: E402 - found on the path set just above
import make_media  # noqa: E402

TIMEOUT_S = 180


def expand(args: list[str], out: Path, media: Path, scratch: Path) -> list[str]:
    """The arguments with their placeholders filled: where the picture goes, the media folder
    and a folder for what the run writes besides. A media path is written with forward slashes,
    which every program here takes and which is what a settings value shows."""
    values = {"out": str(out), "media": media.as_posix(), "scratch": scratch.as_posix()}
    return [check_doc_pictures.PLACEHOLDER.sub(lambda m: values[m.group(1)], arg) for arg in args]


def environment(document: dict, entry: dict, base: dict[str, str]) -> dict[str, str]:
    """This process's environment, as the entry's environment and then the entry change it."""
    named = document["environments"][entry["environment"]]
    env = {k: v for k, v in base.items() if k not in set(named.get("unset", []))}
    env.update(named.get("set", {}))
    env.update(entry.get("env", {}))
    return env


def command_line(entry: dict) -> str:
    """The entry as a person would type it, for --list and for a caption."""
    extra = " ".join(f"{k}={v}" for k, v in entry.get("env", {}).items())
    line = subprocess.list2cmdline([entry["program"], *entry["args"]])
    return f"{extra} {line}" if extra else line


def tidy(picture: Path) -> None:
    """Stores the picture as the others are: RGB with no alpha channel, compressed as far as it
    goes. Pillow does it where it is installed; without it the file stays as the program wrote
    it, which shows the same and is a third larger."""
    try:
        from PIL import Image  # noqa: PLC0415 - optional, and only this function wants it
    except ImportError:
        return
    with Image.open(picture) as image:
        image.convert("RGB").save(picture, optimize=True)


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def _cache(build: Path) -> dict[str, str]:
    cache: dict[str, str] = {}
    text = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    return cache


def describe_build(build: Path) -> tuple[str, dict[str, str]]:
    """The build's name in the manifest and its entry: the commit checked out, and what CMake
    configured. Refuses a tree the commit does not describe."""
    cache = _cache(build)
    home = Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve()
    if home != ROOT.resolve():
        raise SystemExit(f"--stamp: {build} was configured from {home}, not from this checkout")
    dirty = _git("status", "--porcelain", "--untracked-files=no", "--", "apps", "libs", "cmake",
                 "CMakeLists.txt", "examples")
    if dirty:
        raise SystemExit("--stamp: the checkout has changes its commit does not have, so the "
                         "commit would not be the build:\n" + dirty)
    commit = _git("rev-parse", "HEAD")
    qt = Path(cache.get("Qt6_DIR", "")).as_posix().split("/lib/cmake/")[0]
    what = (f"{cache.get('CMAKE_BUILD_TYPE', '?')} build on Windows: "
            f"{Path(cache.get('CMAKE_CXX_COMPILER', '?')).name}, Qt from {qt or '?'}, "
            f"taken {_git('show', '-s', '--format=%cs', 'HEAD')} or after")
    return commit[:9], {"commit": commit, "what": what}


def select(document: dict, only: list[str]) -> list[dict]:
    pictures = document["pictures"]
    if not only:
        return pictures
    chosen = [e for e in pictures if any(part in e["path"] for part in only)]
    if not chosen:
        raise SystemExit("no picture's path contains " + " or ".join(only))
    return chosen


def list_entries(entries: list[dict]) -> None:
    for entry in entries:
        print(f"{entry['path']}  [build {entry['build']}]")
        if "args" in entry:
            print(f"    {entry['environment']}: {command_line(entry)}")
        else:
            print(f"    by hand ({entry['program']}): {entry['by_hand']}")
        if "note" in entry:
            print(f"    note: {entry['note']}")


def take(document: dict, entries: list[dict], build: Path, media: Path,
         out: Path | None) -> list[dict]:
    """Takes each entry that has arguments. Returns the ones that were taken; raises SystemExit
    when any failed, after trying them all."""
    from hidden_desktop import TIMED_OUT, HiddenDesktop  # noqa: PLC0415 - Windows only

    taken: list[dict] = []
    failed: list[str] = []
    with tempfile.TemporaryDirectory(prefix="iclforge-pictures-") as scratch_name, \
            HiddenDesktop() as desktop:
        scratch = Path(scratch_name)
        for entry in entries:
            if "args" not in entry:
                print(f"by hand, not taken: {entry['path']}")
                continue
            program = build / "bin" / (entry["program"] + ".exe")
            if not program.is_file():
                failed.append(entry["path"])
                print(f"FAILED {entry['path']}: no {program}")
                continue
            shot = scratch / (Path(entry["path"]).stem + ".png")
            log = scratch / (Path(entry["path"]).stem + ".log")
            shot.unlink(missing_ok=True)
            code = desktop.run(
                [str(program), *expand(entry["args"], shot, media, scratch)],
                environment(document, entry, dict(os.environ)), ROOT,
                entry.get("timeout_s", TIMEOUT_S), log)
            if code != 0 or not shot.is_file():
                failed.append(entry["path"])
                why = "timed out" if code == TIMED_OUT else f"exit code {code}"
                print(f"FAILED {entry['path']}: {why}; the picture is left as it was")
                if log.is_file():
                    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                        print(f"    {line}")
                continue
            tidy(shot)
            destination = (out / entry["path"]) if out is not None else (ROOT / entry["path"])
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(shot, destination)
            taken.append(entry)
            print(f"taken  {entry['path']}  ({destination.stat().st_size} bytes)")
    if failed:
        raise SystemExit(f"{len(failed)} picture(s) could not be taken: " + ", ".join(failed))
    return taken


def stamp(document: dict, taken: list[dict], build: Path) -> None:
    """Writes the build into the manifest as the build of what was taken, and drops a build
    that no picture is of any more."""
    name, described = describe_build(build)
    document["builds"][name] = described
    for entry in taken:
        entry["build"] = name
    used = {entry["build"] for entry in document["pictures"]}
    document["builds"] = {k: v for k, v in document["builds"].items() if k in used}
    path = ROOT / check_doc_pictures.MANIFEST
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                    newline="\n")
    print(f"{len(taken)} picture(s) are now of build {name} in {check_doc_pictures.MANIFEST}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, help="the build tree, with the programs in bin/")
    parser.add_argument("--only", nargs="+", default=[], metavar="PART",
                        help="only the pictures whose path contains one of these")
    parser.add_argument("--out", type=Path, help="write the pictures under this folder instead")
    parser.add_argument("--media", type=Path, help="the folder of files the pictures play")
    parser.add_argument("--stamp", action="store_true",
                        help="record the build in the manifest as the build of what was taken")
    parser.add_argument("--list", action="store_true", help="say what each entry is, and stop")
    args = parser.parse_args(argv[1:])

    document = check_doc_pictures.load(ROOT)
    report = check_doc_pictures.Report()
    check_doc_pictures.check_manifest(document, ROOT, report, files_exist=False)
    if report.problems:
        for problem in report.problems:
            print(problem, file=sys.stderr)
        return 2
    entries = select(document, args.only)
    if args.list:
        list_entries(entries)
        return 0
    if sys.platform != "win32":
        print("the pictures are taken on Windows: see this script's header", file=sys.stderr)
        return 2
    if args.build is None or not (args.build / "bin").is_dir():
        print("--build <build tree> is needed, with the programs in its bin/", file=sys.stderr)
        return 2
    if args.stamp and args.out is not None:
        print("--stamp records what was put in the tree, so it does not go with --out",
              file=sys.stderr)
        return 2
    build = args.build.resolve()
    if args.stamp:
        describe_build(build)  # refuse now, before a minute of captures
    media = (args.media or build / "pictures-media").resolve()
    forge = build / "bin" / "forge.exe"
    make_media.make(media, forge if forge.is_file() else None)
    taken = take(document, entries, build, media, args.out.resolve() if args.out else None)
    if args.stamp and taken:
        stamp(document, taken, build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
