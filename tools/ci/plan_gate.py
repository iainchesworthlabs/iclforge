#!/usr/bin/env python3
"""Decide what the PR gate builds (pr-gate.yml's `plan` job).

    gh api --paginate "repos/$REPO/pulls/$PR/files" --jq '.[].filename' \\
      | python3 tools/ci/plan_gate.py >> "$GITHUB_OUTPUT"

    python3 tools/ci/plan_gate.py --force-all >> "$GITHUB_OUTPUT"

Reads one repo-relative path per line from stdin and prints `key=value` lines
ready for `$GITHUB_OUTPUT`:

    build      true when the change can alter what the C++ build or the ctest
               suite does, so the Linux (and, in the queue, Windows) gate jobs run
    gui        true when the Qt trees are in play, so Qt is installed and the GUI
               is built; false keeps the gate to the library, the CLI and their tests
    docs_only  true when every path is documentation
    machinery  true when the change touches the gate itself (see GATE_MACHINERY), so the
               workflow also runs the Windows job, which a pull request otherwise skips
    compare    true when the change touches the library (COMPARE_PREFIXES), so a queue entry
               also runs the performance and memory comparisons (_compare.yml); the
               workflow decides whether the event is one that runs them
    esp        true when the change touches the ESP32 sinks' profiles, their Kconfig or
               their fragments (ESP_PREFIXES), so the gate configures every profile
               in the IDF container and holds it to its definition (_profiles.yml);
               a configure, not a build: nothing here compiles for a board
    esp_size   true when the change can alter the ESP32 sinks' images (ESP_IMAGE_PREFIXES: what
               `esp` reads, and the libraries that are compiled into them), so the gate builds the
               4 MB C6 image and holds it to its OTA slot (_esp_size.yml): a profile's budget there
               is the slot, and an image over it cannot be flashed over the network
    reason     the first path that forced `build`, for the run's summary
    gui_reason the first path that pulled Qt in

This is the gate's own, narrower cut of the question tools/ci/classify_changes.py
answers for the full matrix (docs/ci-lanes.md). That classifier fans a change out
to every platform and toolchain, which is right for the post-merge run and wrong
for a gate that only builds Linux: a Python-wheel or ESP-IDF change has nothing
for a Linux C++ build to say, and waits for the post-merge run instead. The
conservative rule is the same one, though: an empty list, a path nothing here
recognises, or the gate's own machinery builds everything, because a false skip
is silent and wrong while a false build costs a few minutes.

`--force-all` (queue entries, pushes to main, dispatches) skips stdin: those
have no single PR diff to read.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "checks"))

import project_graph

# The projects of the tree and the lanes they are built in: tools/checks/projects.json.
TABLE = project_graph.load_table()
# The lanes a Linux C++ build gate has something to say about.
BUILT_LANES = frozenset(("core", "linux"))

DOCS_PREFIXES = ("docs/", "docs-snippets/", "planning/", "overrides/", "assets/")
DOCS_ROOT_FILES = ("LICENSE", "mkdocs.yml")
DOCS_SUFFIX = ".md"

# The gate's own machinery. A change here is proven by running all of the gate,
# GUI included, so it never takes the skip paths below.
GATE_MACHINERY = (
    ".github/workflows/pr-gate.yml",
    ".github/workflows/_compare.yml",
    ".github/workflows/_static.yml",
    ".github/workflows/_toolchain-versions.yml",
    ".github/actions/",
    ".github/toolchain/",
    ".github/toolchain-versions.json",
)

# What a profile's configuration is read from: the sink's own Kconfig, fragments and profile
# file, the ESP-IDF component's Kconfig, and the tools that hold one to the other. A
# library's C++ is not here: it does not change what a configure resolves.
ESP_PREFIXES = (
    "firmware/hearth-sink/",
    "firmware/esp-idf/",
    "tools/hearth/sink_profile.py",
    "tools/hearth/kconfig_scan.py",
    ".github/workflows/_profiles.yml",
)

# What can change the size of an ESP32 sink's image: the above, and the libraries (and the build
# files and vendored code under them) that the sinks compile in, whatever `esp` makes of them. A
# library's own tests and fuzzers are not in the image (LIBRARY_CONSUMERS).
ESP_IMAGE_PREFIXES = (
    *ESP_PREFIXES,
    "libs/",
    "cmake/",
    "external/",
    ".github/workflows/_esp_size.yml",
)

# The projects whose change can alter how fast the encoder runs or how much it allocates:
# the libraries and the vendored code that was inside the sendspin library. Tests, apps and
# build files are not here on purpose, since the comparison measures the library's own
# benchmarks, at two builds a job. A library's own tests/ and fuzz/ are beside its code
# (planning/monorepo.md) and are tests all the same.
COMPARE_KINDS = ("library", "vendored")
LIBRARY_CONSUMERS = re.compile(r"^libs/[^/]+/(?:tests|fuzz)/")

# Trees a Linux C++ build has nothing to say about. Their own lanes run in the
# post-merge verification (docs/ci-agentic.md), and the static checks already
# lint and unit-test the scripts among them. Checked after GATE_MACHINERY, so the
# rest of .github/ (the other workflows) lands here and is linted, not built.
# The projects no Linux lane builds (the bindings, the firmware, the Android and WASM demos) are
# not listed: the table says so, in the lanes of their rows.
NOT_BUILT = (
    ".github/",
    "tools/ci/",
    "tools/hearth/",
    "tools/packaging/",
    "tools/release/",
    "requirements/",
    "packaging/",
    "apps/crucible/linux/",
    "examples/python/",
)

# Root-level files no Linux gate build reads: editor and lint configuration, the
# nightly analysers' settings and the TSan suppressions (a post-merge leg).
NOT_BUILT_ROOT_FILES = (
    ".clang-format",
    ".clang-tidy",
    ".clangd",
    ".gitattributes",
    ".gitignore",
    "ruff.toml",
    "sonar-project.properties",
    "tsan.supp",
)

# Trees the Qt build reads. Anything the C++ build reads that is NOT here and
# NOT in KNOWN_NON_GUI is unrecognised, and unrecognised means Qt too.
GUI_PREFIXES = (
    "apps/forge/gui/",
    "apps/hearth/",
    "apps/crucible/",
    "apps/shared/",
    "notices/crucible/",
    "notices/hearth/",
    "notices/fragments/",
    "notices/licences/",
    "cmake/",
)
# Inside those trees, and built by the Linux gate without Qt: Crucible's Windows driver (it was a
# top-level tree of its own), the three tests of the shared media code that were in libs/ac3/tests
# and libs/audio/tests, and Forge's five notice fragments (they were Forge's own, not Crucible's).
GUI_EXCEPTIONS = (
    "apps/crucible/windows/",
    "apps/shared/media/tests/test_container_input.cpp",
    "apps/shared/media/tests/test_stream_playback.cpp",
    "apps/shared/media/tests/test_sink_wait.cpp",
    "apps/shared/project/tests/",
    "notices/fragments/forge-",
    "notices/fragments/qt-linux.txt",
    "notices/fragments/qt-macos.txt",
    "notices/fragments/qt-windows.txt",
)
GUI_ROOT_FILES = ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json")

# Built by the Linux gate, and known not to need Qt: these, and the libraries, the vendored code,
# the examples and the cross-project tests (NON_GUI_KINDS).
KNOWN_NON_GUI = (
    "testdata/",
    "tools/fuzz/",
    "apps/forge/cli/",
    "notices/",
    "tools/checks/",
    "tools/generators/",
    "tools/references/",
    "tools/listening/",
    "tools/sendspin/",
)

NON_GUI_KINDS = ("library", "vendored", "tests", "example")


def _is_docs(path: str) -> bool:
    return (
        path.startswith(DOCS_PREFIXES)
        or path.endswith(DOCS_SUFFIX)
        or ("/" not in path and path in DOCS_ROOT_FILES)
    )


def plan(
    paths: Iterable[str], *, force_all: bool = False, gui_on_build: bool = False
) -> dict[str, str]:
    """Return the gate's plan for `paths` as `{key: value}` strings.

    `gui_on_build` is the merge queue's mode: whenever the entry builds at all it
    builds the Qt trees too, because the queue is the last place a library change
    and a GUI caller written against the old API meet before main.
    """
    if force_all:
        return {
            "build": "true",
            "gui": "true",
            "docs_only": "false",
            "machinery": "false",
            "compare": "false",
            "esp": "true",
            "esp_reason": "full run (queue entry, push to main or dispatch)",
            "esp_size": "true",
            "esp_size_reason": "full run (queue entry, push to main or dispatch)",
            "reason": "full run (queue entry, push to main or dispatch)",
            "gui_reason": "full run (queue entry, push to main or dispatch)",
        }

    seen = False
    all_docs = True
    machinery = False
    compare = False
    esp_reason = ""
    esp_size_reason = ""
    build_reason = ""
    gui_reason = ""

    for raw in paths:
        path = raw.strip()
        if not path:
            continue
        seen = True

        if _is_docs(path):
            continue
        all_docs = False
        if path.startswith(ESP_PREFIXES):
            esp_reason = esp_reason or path
        if path.startswith(ESP_IMAGE_PREFIXES) and not LIBRARY_CONSUMERS.match(path):
            esp_size_reason = esp_size_reason or path

        if path.startswith(GATE_MACHINERY):
            machinery = True
            build_reason = build_reason or f"{path} (the gate's own machinery)"
            gui_reason = gui_reason or f"{path} (the gate's own machinery)"
            continue
        project = TABLE.project_of(path)
        # A header of the ESP-IDF component that a library's tests include is built by the
        # gate through them, though the component is not: the table names the edge.
        includers = [i for i in TABLE.reached_from(path) if BUILT_LANES & set(i.lanes)]
        not_built = (
            path.startswith(NOT_BUILT)
            or ("/" not in path and path in NOT_BUILT_ROOT_FILES)
            or (project is not None and not BUILT_LANES & set(project.lanes))
        )
        if not_built and not includers:
            continue

        build_reason = build_reason or path
        compare = compare or (
            project is not None
            and project.kind in COMPARE_KINDS
            and not LIBRARY_CONSUMERS.match(path)
        )
        for includer in includers:
            if (includer.path + "/").startswith(GUI_PREFIXES):
                gui_reason = gui_reason or f"{path} (included by {includer.name})"
        if not_built:
            continue
        if path.startswith(GUI_EXCEPTIONS):
            pass
        elif path.startswith(GUI_PREFIXES) or ("/" not in path and path in GUI_ROOT_FILES):
            gui_reason = gui_reason or path
        elif not (
            path.startswith(KNOWN_NON_GUI)
            or (project is not None and project.kind in NON_GUI_KINDS)
        ):
            gui_reason = gui_reason or f"{path} (not a path this planner recognises)"

    if not seen:
        return {
            "build": "true",
            "gui": "true",
            "docs_only": "false",
            "machinery": "false",
            "compare": "false",
            "esp": "true",
            "esp_reason": "no file list; running everything",
            "esp_size": "true",
            "esp_size_reason": "no file list; running everything",
            "reason": "no file list; building everything",
            "gui_reason": "no file list; building everything",
        }

    if all_docs:
        return {
            "build": "false",
            "gui": "false",
            "docs_only": "true",
            "machinery": "false",
            "compare": "false",
            "esp": "false",
            "esp_reason": "",
            "esp_size": "false",
            "esp_size_reason": "",
            "reason": "documentation only",
            "gui_reason": "",
        }

    if gui_on_build and build_reason and not gui_reason:
        gui_reason = "merge queue entry (builds Qt whenever it builds)"

    return {
        "build": "true" if build_reason else "false",
        "gui": "true" if gui_reason else "false",
        "docs_only": "false",
        "machinery": "true" if machinery else "false",
        "compare": "true" if compare else "false",
        "esp": "true" if esp_reason else "false",
        "esp_reason": esp_reason,
        "esp_size": "true" if esp_size_reason else "false",
        "esp_size_reason": esp_size_reason,
        "reason": build_reason or "nothing a Linux C++ build reads (lint only)",
        "gui_reason": gui_reason,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--force-all",
        action="store_true",
        help="Plan a full run without reading stdin (queue entry, push to main, dispatch).",
    )
    parser.add_argument(
        "--gui-on-build",
        action="store_true",
        help="Build the Qt trees whenever the change builds at all (merge queue entries).",
    )
    args = parser.parse_args(argv[1:])

    paths = [] if args.force_all else sys.stdin.read().splitlines()
    result = plan(paths, force_all=args.force_all, gui_on_build=args.gui_on_build)

    keys = (
        "build",
        "gui",
        "docs_only",
        "machinery",
        "compare",
        "esp",
        "esp_size",
        "reason",
        "gui_reason",
        "esp_reason",
        "esp_size_reason",
    )
    for key in keys:
        print(f"{key}={result[key]}")
    print(
        f"plan: build={result['build']} gui={result['gui']} "
        f"docs_only={result['docs_only']} - {result['reason']}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
