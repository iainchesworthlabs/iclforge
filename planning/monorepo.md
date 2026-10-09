# A monorepo of self-contained projects (C7): a study

!!! note "Status as of 2026-10-09: decisions 1 to 15 taken and carried out; C7-1 to C7-7 run and proved"
    Asked for by the user on 2026-10-08: "turn this repository into a monorepo of self-contained
    projects". It follows [consolidation.md](consolidation.md), whose C0 to C6, the three merges its
    decision 14 left (M1 to M3) and the two items C3 and C6 left are run and proved on the local
    branches `chore/src-consolidation-c0` to `-exclude`
    ([what the runs found](consolidation.md#what-the-runs-found-that-the-plan-did-not)). It takes up
    the step [layout.md](layout.md) left open, L3 ("Pitchfork `libs/`, tests beside the code",
    [(d)](layout.md#l3-pitchfork-libs-tests-beside-the-code)), and extends it from the libraries to
    the products, the bindings and the firmware. Counts are of the tree at `33cec0856` (3,185 tracked
    files), measured by a dry run that maps every tracked file to its new path. The user took
    decisions 1 to 15 on 2026-10-08, every one (a). C7-1 ran on the local branch
    `chore/monorepo-c7-1`: the libraries, their tests and fuzz targets, the test-support helpers and
    the vendored time filter moved, and it changed no output byte, no exported name and no test name
    ([what the runs found](#what-the-runs-found-that-the-study-did-not)). C7-2 ran on
    `chore/monorepo-c7-2`: the products, the code they share, the demos and the notices moved, each
    program's tests went beside it, and it changed no output byte, no exported name and no test name; the
    one difference it leaves in a program is the order of the functions of its generated Qt code. C7-3
    ran on `chore/monorepo-c7-3`: the bindings and the firmware moved, Hearth's sink became a project of
    its own beside the ESP-IDF component, and the code of every image, extension and package it was
    compared on is the code it was. C7-4 ran on `chore/monorepo-c7-4`: the golden data moved to
    `testdata/`, and the one thing that differs in what the programs print is the name of a golden file
    they were given. C7-5 ran on `chore/monorepo-c7-5`: `tools/checks/projects.json` and `check_layering.py`
    hold the whole tree to the project graph, and nothing the build reads changed. C7-6 ran on
    `chore/monorepo-c7-6`: the CI planners read the rows of `projects.json` instead of directory
    lists, and a header of the ESP-IDF component that a library's tests include is built by the
    gate now. C7-7 ran on `chore/monorepo-c7-7`: the scripts of `tools/n1b/` are retired, with what
    adapting an open branch needs left in `tools/adapt/`, and the mechanical commits of the stages are in
    `.git-blame-ignore-revs`. The branches are on `github`; no pull request is open. The first runs of
    GitHub's own gates on the stack found 19 defects, none of them a moved byte, each fixed on a branch merged
    into `chore/monorepo-c7-7` ([After C7-7](#after-c7-7-2026-10-09-choremonorepo-c7-7-with-the-branches-merged-into-it)),
    and what that section left on purpose (five exceptions and a debt, a narrowing, a test name, two
    Windows tests, the Sonar scope) is remediated: one exception remains, by the owner's choice
    ([what was left](#what-was-left-and-its-remediation-2026-10-09-choremonorepo-c7-7-with-the-branches-merged-into-it)).

## In brief

- **The repository is a monorepo in fact but not in shape.** Libraries are in `src/`, products in
  `apps/`, bindings in `python/`, `rust/` and `js/`, firmware in `esp-idf/`, `esphome/` and
  `apps/baremetal/`; tests in `tests/<lib>/`, `tests/{cli,gui,hearth,crucible}/` and
  `apps/*/tests/`; every fuzz target in one `fuzz/`; and only the libraries have a dependency table.
- **The recommended layout, L4,** puts each project in one directory with its tests and fuzz targets:
  `libs/<lib>/`, `apps/<product>/` (Forge, Hearth, Crucible, the demos), `bindings/`, `firmware/`,
  with `tests/` for what crosses projects and `testdata/` for the golden data. 2,129 `git mv`
  operations; 1,079 other files name a moved path (5,945 mentions). No public name changes: targets,
  namespaces, header spellings, program and package names and every output byte stay as they are.
- **A project graph replaces `layering.json`:** `projects.json` gives every project (library, app,
  binding, firmware, example) its kind and what it may depend on, and `check_layering.py` holds the
  whole tree to it. Apps depend on libraries and never on each other.
- **It reverses two decisions of layout.md:** 3(a), the flat `src/<lib>` (the libraries stay flat but
  their root is `libs/`, and the products are grouped), and 4(a), the tests mirrored in
  `tests/<lib>/` with one binary (they move beside their code, a binary per project).
- **Stages C7-1 to C7-5**, each a local branch proved as C0 to C3 were, plus what C7 adds: every
  program, package and binding built and compared, paths aside; ctest's test names the same.

## (a) What is there

### The top level

| directory | files | what it is |
|---|---:|---|
| `apps` | 708 | products: `hearth` 159, `crucible` 149, `gui` 125 (44 tests), `wasm` 54 (41 tests), `android` 51, `cli` 48, `baremetal` 46, `windows` 34, `common` 20, `linux` 13, `notices` 9 |
| `src` | 655 | the twelve libraries: `ac4` 158, `ac3` 132, `sendspin` 89, `audio` 77, `dsp` 44, `base` 41, `containers` 36, `capi` 18, `objects` 17, `iab` 17, `adm` 16, `render` 10 |
| `tests` | 549 | `golden` 201; the libraries' (`ac3` 87, `ac4` 60, `sendspin` 28, `audio` 21, `containers` 11, `dsp` 10, `render` 9, `iab` 6, `base` 5, `adm` 4, `objects` 2, `capi` 2); the products' (`hearth` 38, `cli` 22, `crucible` 20, `gui` 5); `performance` 10, `platform` 3, `crt` 2, two shared headers and `CMakeLists.txt` |
| `tools` | 303 | checks, CI, generators, packaging, the N1B and consolidation scripts |
| `fuzz` | 292 | 26 targets, 2 shared headers, 4 scripts; seeds 235 and regressions 24 per target |
| `docs` | 217 | the site |
| `esp-idf` | 147 | the `iclforge` component: `include` 22, `src` 12, and `examples/hearth_sink` 96 (Hearth's sink firmware) and `examples/i2s_player` 8 |
| `.github`, `cmake`, `examples` | 46, 42, 35 | workflows and actions; CMake modules; library examples |
| `rust`, `js`, `python` | 31, 31, 30 | the bindings: a Cargo workspace (`iclforge`, `iclforge-sys`), the npm package, the wheel |
| `packaging`, `planning`, `requirements` | 24, 23, 15 | Homebrew, winget, Conan, the vcpkg port, Debian; plans; pinned Python locks |
| `docs-snippets`, `esphome`, `assets`, `overrides` | 10, 5, 2, 1 | |

### The projects

| project | today | kind | links | consumed by |
|---|---|---|---|---|
| `base`, `dsp`, `objects`, `render`, `containers` | `src/<lib>` | library, installed | as `layering.json` | the codecs, the products, the bindings |
| `ac3`, `ac4` | `src/<lib>` | library, installed | `base`, `dsp`, `objects`, `render` | the products, `capi`, the bindings, the firmware |
| `iab`, `adm`, `capi` | `src/<lib>` | library, installed (options) | `adm`: `iab`, `objects`; `capi`: `ac3`, `ac4` | the CLI, examples, Rust (through `capi`) |
| `audio`, `sendspin` | `src/<lib>` | library, **not installed** | `audio`: `base`, `render`, `containers`, `objects` | the CLI, the GUI, Hearth, Crucible, the sink firmware |
| Forge: `forge` | `apps/cli` + `apps/common` | program | `ac3`, `ac4`, `adm`, `audio`, `containers` | packaging (Homebrew, winget, deb) |
| Forge: `forge-gui` | `apps/gui` + `apps/common` | program (Qt) | `ac3`, `ac4`, `audio`, `containers` | packaging |
| `apps/common` | `apps/common` | shared code, **no target**: `forge` compiles nine of its sources in, Hearth's engine puts it on its include path | the libraries its sources name | Forge, Hearth |
| Hearth | `apps/hearth/{engine,ui,render,testsink,testserver}` | program (Qt) and test tools | `ac3`, `ac4`, `audio`, `containers`, `sendspin` | packaging |
| Hearth's sink | `esp-idf/iclforge/examples/hearth_sink` | firmware (ESP-IDF example) | the component | boards |
| Crucible | `apps/crucible/{engine,ui,runner,spikes}`, `apps/windows/{driver,driver-vm}`, `apps/linux/tray-vm` | program (Qt), a Windows driver (MS-PL), two test VMs | `ac3`, `audio`, `containers` | packaging |
| demos | `apps/android`, `apps/wasm` | Android app (Gradle), WASM build | `ac3`, `audio`; `ac3`, `ac4` | the npm package (wasm) |
| bindings | `python`, `rust`, `js` | wheel (scikit-build), crates, npm | `ac3`, `ac4`, `containers`; `capi`; the WASM build | PyPI, crates.io, npm |
| firmware | `esp-idf/iclforge`, `esphome`, `apps/baremetal` | ESP-IDF component, ESPHome external component, bare-metal probes | the minimum-footprint archives | boards, the QEMU legs |
| notices | `apps/notices`, `apps/crucible/notices`, `apps/hearth/notices` | three trees of fragments over one function, `cmake/Notices.cmake` | | each product's package |
| tests | `tests/`, `apps/*/tests` | one `iclforge-tests` (293 files, all libraries and the CLI), `iclforge-settings-tests`, `hearth_controller_tests`, the GUI's, Crucible's and Hearth's QML tests, the WASM E2E | everything | ctest, by Catch2 tag (`ADD_TAGS_AS_LABELS`) |

Cross-project test helpers: `tests/platform/process.hpp` (ten test trees), `tests/sanitized.hpp`
(AC-4, the CLI, Hearth), `tests/ac4_stream_kinds.hpp` (AC-4, Hearth), `tests/audio/alsa_null_device.hpp`
(the CLI's). The AC-4 decoder's and encoder's shared helpers (`tests/ac4/decoder/bits.hpp`,
`tests/ac4/core/toc_writer.hpp`) are inside AC-4's own tests and stay so.

### Everything keyed on a path

What names a path L4 moves, counted by line:

| where | lines | |
|---|---:|---|
| CI workflows (16 of 29) and actions (1 of 8) | 323, 3 | build trees, artefact paths, `paths:` filters |
| `tools/ci/plan_gate.py`, `classify_changes.py` and their tests | 92 | prefixes per lane; an unknown top-level directory means "build everything, with Qt" |
| `sonar-project.properties` | 84 | sources, tests, exclusions, the multicriteria ignores |
| the coverage floors (`tools/checks/coverage_report.sh`) | 63 | a line and a branch floor per `src/<lib>` |
| `tests/CMakeLists.txt` | 73 | 26 `target_include_directories`, the kernel sources compiled again |
| the root `CMakeLists.txt` and `cmake/` | 156 | `add_subdirectory`, install, notices, presets' toolchains |
| the table generators (`tools/generators`, 25 of 29) | 118 | where each writes its table |
| the ESP-IDF packer (`STAGED_TREES`, `PRUNE`) | 22 | |
| the Python package (`pyproject.toml`, `python/CMakeLists.txt`) | 14 | |
| the Rust workspace and `iclforge-sys/build.rs` | 12 | the bindgen header, the C API's build |
| the Android Gradle project | 9 | the `CMakeLists.txt` it builds and its relative root |
| `CMakePresets.json`, `.gitattributes`, `.clang-tidy`, `vcpkg.json` | 9, 8, 4, 2 | |
| `check_doc_paths.py`, `check_pages.py`, `mkdocs.yml` | 4, 3, 1 | |
| C and C++ `#include "../…"` | 69 in 43 files | the relative ones; the `iclforge/<lib>/…` spellings do not move |
| golden data by path | 540 in 158 files | 70 files in `tools/`, 26 in `tests/` |

The npm package's files name no moved path (its build reads the WASM output by artefact).

## (b) Principles

1. **Public names do not change.** Targets `iclforge::<lib>`, namespaces, header spellings
   `iclforge/<lib>/…`, program names, package names and every output byte stay exactly as they are.
   C7 moves paths and build structure only.
2. **A project is a directory** with everything it is made of: its `CMakeLists.txt`, its public
   headers, its sources, its build variants, its tests, its fuzz targets.
3. **One kind of project per root:** `libs/`, `apps/`, `bindings/`, `firmware/`. What crosses projects
   (integration tests, the test-support library, shared data, tools) has a root of its own.
4. **A path says what a thing is once.** No `src/` in the middle of a library's path but its own
   sources' directory (`libs/ac3/src/decoder/`, not `src/ac3/src/decoder/`).
5. **The dependency direction is data,** for every project: a manifest, checked, from which CI can
   tell which projects a change reaches.
6. **Every move is a move:** `R100`, scripted, before any edit; the proof is the one C0 to C3 used,
   and the packages built and compared besides.

## (c) Candidate layouts

The starting candidate, L4:

```text
libs/<lib>/       CMakeLists.txt  include/iclforge/<lib>/  src/  variants/  tests/  fuzz/{*.cpp,seeds,regressions}
                  ac3 ac4 adm audio base capi containers dsp iab objects render sendspin
                  (audio and sendspin marked internal: built, linked, never installed)
apps/forge/       cli/  gui/  notices/  packaging/
apps/shared/      media/ (an internal library, was apps/common)  theme/ (an internal Qt library, was apps/gui/system_theme.*)
apps/hearth/      engine/  ui/  render/  testsink/  testserver/  tests/  notices/
apps/crucible/    engine/  ui/  runner/  spikes/  windows/{driver,driver-vm}  linux/tray-vm  tests/  notices/
apps/demos/       android/  wasm/
bindings/         python/  rust/  js/
firmware/         esp-idf/iclforge/ (the component)  esphome/  baremetal/
tests/            cross-project integration tests; support/ (the test-support library: platform/, crt/, shared headers); performance/
testdata/         the golden data (was tests/golden)
tools/fuzz/       the fuzz scripts (run.sh, generate-seeds.sh, …)
examples/ tools/ docs/ planning/ packaging/ cmake/ requirements/ assets/      as now
```

Measured by the dry run (each tracked file mapped to its new path; a file "names a moved path" when its
text holds a moved prefix at a path boundary):

| | L4 | L4, golden stays | L4-root | L2+ |
|---|---:|---:|---:|---:|
| what changes | as above | `tests/golden` stays | bindings and firmware stay at the root | `src/` stays the library root, tests and fuzz beside each library; apps grouped |
| files moved | 2,129 | 1,928 | 1,839 | 1,184 |
| of them: libraries, tests and fuzz into libraries | 1,186 | 1,186 | 1,186 | 531 |
| products | 439 | 439 | 439 | 439 |
| bindings, firmware | 92, 198 | 92, 198 | 0, 0 | 0, 0 |
| golden data | 201 | 0 | 201 | 201 |
| other files naming a moved path | 1,079 | 1,038 | 1,005 | 653 |
| mentions | 5,945 | 5,470 | 5,379 | 2,845 |
| deepest path | 11 | 11 | 11 | 11 |

The deepest paths are the variant trees, unchanged by every candidate
(`libs/base/variants/arch-x86_64/iclforge/base/detail/simd.hpp`).

- **L4.** Each project is one directory; a root says what kind of thing it holds; the CI planner can
  map a path to a project by its first two components. Against: the most moves, and the bindings'
  and firmware's own tooling (the Cargo workspace root, the Python and npm package roots, the
  ESP-IDF component) all move.
- **L4-root.** The same for libraries and products, the bindings and firmware where their
  ecosystems' tooling expects them; 290 fewer moves and 74 fewer files to edit. Against: three kinds of
  project at the root beside the tools.
- **L2+.** Keeps `src/` as the library root, so every `src/<lib>` spelling stays and the edit is half
  L4's. Against: `src/<lib>/src/` stays, the thing the user named; and a library's root is named for
  a part of a project, not for a kind of project.

**Recommendation: L4.** It is the layout the request describes, and the extra cost over L4-root is
moves that are scripted and proved like the others. Golden data to `testdata/` (201 moves, 41 more
files edited) is a decision of its own below.

## (d) The project graph

`tools/checks/layering.json` covers the twelve libraries. C7 replaces it with `projects.json` at the
root of `tools/checks/`, one entry per project:

```json
{
  "projects": {
    "base":       {"kind": "library", "path": "libs/base", "may_use": []},
    "ac3":        {"kind": "library", "path": "libs/ac3", "may_use": ["base", "dsp", "objects", "render"]},
    "audio":      {"kind": "library", "path": "libs/audio", "internal": true, "may_use": ["base", "render", "containers", "objects"]},
    "forge":      {"kind": "app", "path": "apps/forge", "may_use": ["ac3", "ac4", "adm", "audio", "containers", "app-media", "app-theme"]},
    "app-media":  {"kind": "library", "path": "apps/shared/media", "internal": true, "may_use": ["ac3", "ac4", "containers", "audio"]},
    "app-theme":  {"kind": "library", "path": "apps/shared/theme", "internal": true, "may_use": []},
    "python":     {"kind": "binding", "path": "bindings/python", "may_use": ["ac3", "ac4", "containers"]},
    "esp-idf":    {"kind": "firmware", "path": "firmware/esp-idf/iclforge", "may_use": ["ac3", "ac4", "base", "dsp", "objects", "render", "sendspin"]},
    "examples":   {"kind": "example", "path": "examples", "may_use": ["ac3", "ac4", "adm", "capi", "containers", "iab"]}
  }
}
```

The rules `check_layering.py` enforces over the whole tree, from the includes as now and from the
CMake link lines as well: a library uses only libraries; an app, a binding, a firmware or an example
uses only libraries (an app's internal library, such as `forge-common`, is its own); no app uses
another app, and no library uses an app; `internal` libraries are never installed and no installed
library's public header includes one. Two edges between products exist today, both Forge's code
that another product takes: Hearth's engine includes `apps/common` (`media_info.hpp`,
`session.hpp`, `stream_decoder.hpp` and one more, through `hearth_engine`'s include path), and Hearth
and Crucible compile Forge GUI's `apps/gui/system_theme.cpp` into their programs. An include scan of
every app finds no other. Each becomes an internal library under `apps/shared/` that any product may
use (decision 5), so that no product uses another.

**The planner.** With the manifest, `plan_gate.py` and `classify_changes.py` can map each changed path
to its project (its longest `path` prefix) and then to every project that may use it, transitively,
and build and test only those: `ctest -L <project>` for each. Whether that is part of C7 or follows
it is a decision below: the manifest and the check are C7's either way, since they replace
`layering.json`; switching the planner from prefixes to the graph changes which jobs run, and the
proof for it (the planner chooses the same jobs, or a stated superset, for a sample of past pull
requests) is its own.

## (e) Tests

- **A binary per project,** `iclforge-<project>-tests`, built from `libs/<lib>/tests/` with a
  `CMakeLists.txt` of the library's own; `tests/CMakeLists.txt`'s 1,282 lines become one file per
  project plus the integration tests' (the CLI's run the program, so they stay in `apps/forge/cli/tests`
  as their own binary).
- **ctest's names do not change.** `catch_discover_tests()` names a test by its Catch2 test case,
  not by the binary, so the names are the same; each binary adds the project as a label
  (`ctest -L ac3`), beside the tags it adds now (`ctest -L ac4 -L decoder` still works).
- **The one-binary convenience.** `cmake --build … --target iclforge-tests` today builds everything
  a test needs in one link; after C7 the umbrella target `iclforge-tests` builds every project's test
  binary, and `ctest` runs them all. What goes: running one binary with a tag filter across every
  library (`iclforge-tests "[resampler]"` reaches `dsp`'s and `audio`'s tests at once), which becomes
  `ctest -L resampler`.
- **Shared helpers.** `tests/support/` becomes `iclforge::test_support`, an internal library of
  `process.hpp` (and its two platform sources), `sanitized.hpp`, `ac4_stream_kinds.hpp` and the CRT
  report; `alsa_null_device.hpp` stays `audio`'s and is exported to its users through that library.
- **Shared kernels.** `tests/CMakeLists.txt` compiles AC-4's kernels a second time into the test
  binary in a shared build (they are hidden in `libiclforge_ac4.so`); the same goes into
  `libs/ac4/tests/CMakeLists.txt`.

## (f) Hazards seen in advance

- **ESP-IDF's component layout.** A component is a directory with `CMakeLists.txt` and
  `idf_component.yml`, and the component registry ships `examples/` from inside it, so
  `hearth_sink` (96 files) is either an example of the component (as now, inside
  `firmware/esp-idf/iclforge/examples/`) or a firmware project of Hearth's that names the component
  as a dependency (`firmware/hearth-sink/`, `idf_component.yml` with an `override_path`). The packer's
  `STAGED_TREES` (22 lines) and the component's `CMakeLists.txt`, which reach the library's root by a
  relative path, move with it.
- **The Windows driver is MS-PL.** `apps/windows/driver/` carries its own `LICENSE` and the source
  headers name it; it stays a directory whose name and `LICENSE` say what it is, wherever it goes.
- **The bindings' roots.** The Cargo workspace root and `Cargo.lock`, `pyproject.toml` with `uv.lock`,
  and `package.json` with `package-lock.json` move as wholes; `iclforge-sys/build.rs` finds the C API
  by a relative path; scikit-build's `cmake.source-dir` names the repository root; CI caches are
  keyed on the lock paths.
- **Golden data by path:** 540 mentions in 158 files, 70 of them in `tools/`, and two compile-time
  definitions (`AC4_GOLDEN_DIR`); the CLI corpus (`tools/n1b/cli_bytes.py`) names it too.
- **Qt's translation catalogues** record a source location per message: 967 `<location>` lines in
  each of `forge_gui_*.ts` and as many in `crucible_*.ts`. A move changes every one; `lupdate` rewrites
  them, or a script does, with the catalogues' messages and translations unchanged.
- **Open branches.** `tools/n1b/adapt_branch.ps1` replays a branch over a move; it reads a move map,
  which C7's scripts write as N1B's did.
- **CI's path filters and caches** (`paths:` in 16 workflows, artefact names that carry a path, the
  vcpkg and ccache keys) and the coverage floors keyed on `src/<lib>`.
- **Windows path length.** The deepest paths do not grow (11 components), but the driver's tree and
  the Qt build directories are where MSVC's 260-character limit has bitten before; C7-2 checks them.
- **Two kinds of `tests/`.** `apps/gui/tests` and `apps/hearth/ui/tests` are QML test trees with their
  own runners; the products' C++ tests from `tests/{cli,gui,hearth,crucible}` join them, which is two
  test kinds in one directory.

## (g) Stages

Each a local branch `chore/monorepo-c7-<n>` from the one before; the moves first and alone (`git mv`,
every rename `R100`), then the include and path rewrites by script, then the build files, then what is
done by hand. The scripts extend `tools/n1b` (a `c7` module beside `consoldef.py`'s stages: the move map,
the path rules, the move map `adapt_branch.ps1` reads).

| stage | what moves |
|---|---|
| C7-1 | `src/<lib>` to `libs/<lib>`; `tests/<lib>` to `libs/<lib>/tests`; each fuzz target, its seeds and its regressions to `libs/<lib>/fuzz`; `tests/support` and the test-support library; a test binary per library |
| C7-2 | the products: `apps/forge/` (cli, gui, notices), `apps/shared/` (`app-media`, `app-theme`), Hearth's and Crucible's trees, the demos; their tests beside them; one notices system |
| C7-3 | `bindings/`, `firmware/` (and Hearth's sink firmware, by its decision) |
| C7-4 | `testdata/` (by its decision) |
| C7-5 | `projects.json`, `check_layering.py` over the whole tree; the planner, by its decision |

**The proof, after every stage,** C0 to C3's and what C7 adds:

- the builds (GCC 16 and Clang 22 `-Werror`, shared), the whole ctest, the pinned hashes, the CLI
  corpus, the exports and ABI allowlists, the IR of the moved units, the bare-metal probes,
  `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py`, `precheck.py`;
- **every program, package and binding built and its contents compared with the before-build, paths
  aside:** the CPack packages, the wheel, the crates, the npm package, the ESP-IDF pack `--verify`;
- **ctest's test names are the before-names**, apart from a label;
- `mkdocs build --strict`;
- **the CI planner selects the same jobs** for a sample of past pull requests (the planner reads paths,
  so every prefix it holds moves with the tree).

Not runnable here, and to be recorded at each stage rather than skipped: MSVC and clang-cl, macOS,
the Android build, the ESP-IDF build and the boards, the Python, Rust and WASM test suites
(`pybind11`, `cargo` and `emcc` are not installed), `mkdocs` (installable from the docs lock, as the
scoring venv was).

## (h) Decisions

1. **The library root.** (a) **`libs/`** (recommended): a kind of project per root, no `src/` but a
   library's own sources; 1,186 moves. (b) `src/` stays, tests and fuzz beside each library: 531
   moves, half the edits, and `src/<lib>/src/` stays.
2. **Headers and sources.** (a) **separate `include/iclforge/<lib>/` and `src/`** (recommended, as
   now): the install and the namespace check read `include/`, and a private header cannot be
   installed by accident. (b) Headers beside sources, a file set listing the public ones: every
   library's 30 to 60 public headers listed by hand, and the include spelling unchanged only through
   a `BASE_DIRS` per library.
3. **Tests and fuzz.** (a) **beside each project** (recommended): `libs/<lib>/{tests,fuzz}`, a binary
   per project, ctest labels; reverses layout.md decision 4(a). (b) Tests beside, fuzz central (26
   targets and their 259 seeds stay in `fuzz/`): 291 fewer moves, and the fuzz scripts keep one root.
   (c) Both central, as now.
4. **Apps by product.** (a) **`apps/forge`, `apps/hearth`, `apps/crucible`, `apps/demos`**
   (recommended): Crucible's driver and VM with Crucible, Forge's CLI and GUI together. (b) Flat
   `apps/<program>` as now, with only the tests and the shared code moved.
5. **`apps/common` and the shared theme.** (a) **two internal libraries in `apps/shared/`:
   `app-media` (was `apps/common`, which Forge and Hearth use) and `app-theme` (was
   `apps/gui/system_theme.*`, which Forge's GUI, Hearth and Crucible compile in)** (recommended): no
   product then uses another, and the graph says who uses each. (b) Keep both in Forge, the graph
   allowing Hearth and Crucible to use it: the rule "apps never use each other" gets two exceptions.
   (c) `app-media`'s codec-blind pieces (the recording sink, the stream playback) to `audio`, the
   theme to `apps/shared/theme`: `audio` grows by the code its two users need.
6. **Bindings and firmware.** (a) **`bindings/` and `firmware/`** (recommended); (b) at the root as
   now (L4-root): 290 fewer moves, the ecosystems' tools untouched.
7. **Golden data.** (a) **`testdata/`** (recommended): data a dozen projects share is no project's
   tests; 201 moves, 41 more files to edit. (b) `tests/golden` as now.
8. **Hearth's sink firmware.** (a) **`firmware/hearth-sink/`**, a firmware project of its own that
   names the component as a dependency (recommended: it is Hearth's, not the component's); the
   registry then ships `i2s_player` as the component's one example. (b) An example inside the
   component, as now.
9. **The Windows driver.** (a) **`apps/crucible/windows/driver/`**, its `LICENSE` and README beside it
   (recommended: it exists for Crucible). (b) `firmware/` or a root `drivers/`, apart from any product.
10. **Versions.** (a) **one lockstep version for every project** (recommended): the git tag, as CMake
    and Python read it today; the Rust workspace's `0.1.0`, the npm package's `0.0.0-dev` and the
    ESP-IDF component's `0.10.0-beta.1` follow it at release. (b) Per-project versions, each with its
    own tag prefix and changelog.
11. **Notices.** (a) **one system** (recommended): `cmake/Notices.cmake` and one tree of fragments,
    `apps/notices` moving to the root as `notices/`, each product listing the fragments it ships. (b)
    Keep a tree per product, over the one function.
12. **The CI planner.** (a) **after C7, a stage of its own** (recommended): C7 lands the manifest
    and the check; the planner moves from prefixes to the graph with its own proof. (b) In C7 as
    C7-5.
13. **The scripts afterwards.** (a) **retire `tools/n1b` and the consolidation scripts once C7 is
    proved** (recommended), keeping `adapt_branch.ps1` and the move maps until the open branches are
    adapted; (b) keep them.

Raised by the sample tree the user gave with decision 2 (`libs/<lib>/{include,src,tests}`,
`apps/<app>/{src,assets}`, `external/`):

14. **Third-party code.** (a) **`external/<name>/`** for what the tree carries (today one vendored
    library, `src/sendspin/third_party/time-filter`, 6 files with its `LICENSE` and `VENDORED.md`),
    and the FetchContent declarations (fmt, Catch2, libadm and libbw64) in `cmake/External.cmake`
    pointing at it or fetching into the build tree, as now (recommended: vcpkg stays the manager for
    what it provides); (b) `external/` holds FetchContent's sources as git submodules too; (c) the
    vendored library stays inside `sendspin`, the project that uses it.
15. **A program's directory.** (a) **`apps/<product>/<program>/{CMakeLists.txt, src/, assets/,
    tests/}`** (recommended): sources in `src/`, Qt resources (fonts, icons, QML, translations) in
    `assets/`; about 270 more moves inside the products, `.qrc` and `qt_add_qml_module` paths
    rewritten. (b) A program's sources at its directory's root, as now.

**Taken on 2026-10-08, every one (a):** 1 `libs/`; 2 separate `include/iclforge/<lib>/` and `src/`,
the user's sample tree with each library's `include/`, `src/`, `tests/` and `CMakeLists.txt`; 3 tests
and fuzz beside each project; 4 apps by product; 5 `apps/shared/{media,theme}`; 6 `bindings/` and
`firmware/`; 7 `testdata/`; 8 `firmware/hearth-sink/`; 9 the driver in
`apps/crucible/windows/driver/`; 10 one lockstep version; 11 one notices system, `notices/`; 12 the
planner after C7; 13 the scripts retired after C7; 14 `external/`; 15 a program's `src/`, `assets/`
and `tests/`.

The tree that follows from them:

```text
libs/<lib>/          CMakeLists.txt  include/iclforge/<lib>/  src/  variants/  tests/  fuzz/
apps/forge/          cli/  gui/                       each program: CMakeLists.txt  src/  assets/  tests/
apps/hearth/         engine/  ui/  render/  testsink/  testserver/
apps/crucible/       engine/  ui/  runner/  spikes/  windows/{driver,driver-vm}/  linux/tray-vm/
apps/demos/          android/  wasm/
apps/shared/         media/ (app-media)  theme/ (app-theme)
bindings/            python/  rust/  js/
firmware/            esp-idf/iclforge/  hearth-sink/  esphome/  baremetal/
external/            time-filter/
notices/             the fragments; cmake/Notices.cmake assembles them per product
tests/               integration tests; support/ (iclforge::test_support); performance/
testdata/            was tests/golden
tools/fuzz/          the fuzz scripts
examples/ tools/ docs/ planning/ packaging/ cmake/ requirements/ assets/
```

## How C7 is run

The stages run as C0 to C3 did ([how C0 to C3 are run](consolidation.md#how-c0-to-c3-are-run),
[tools/n1b/README.md](https://github.com/iainchesworthlabs/iclforge/blob/8d2507bae/tools/n1b/README.md)): one local branch each, `chore/monorepo-c7-<n>`, each
made from the one before. Within a stage the commits come in N1B's order, each script in a commit of
its own before the commit it makes: the moves alone (`git mv`, every rename `R100`), the include
spellings, the build files and the paths in text, then what is done by hand. The moves are data in
`consoldef.py` (`c7_1_new`); `consol_apply.py`, `consol_cmake.py` and `consol_paths.py` run them.

What C7 adds to the proof, and the scripts that give it (all in `tools/n1b`, which C7-7 retired: they
are in the history at `8d2507bae`, and `tools/ci/compare_planners.py` is the planner one's successor):

| script | what it shows |
|---|---|
| `c7_record.sh` | configures, builds and records the three host trees (GCC 16 and Clang 22, Release; Clang 22 shared, Debug): ctest's names, `baseline.py`'s pinned hashes, CLI corpus, installed tree and exported symbols |
| `c7_run.sh ctest` / `probes` | the whole ctest of both static trees with JUnit output, and the bare-metal probes under QEMU (decoder, encoder, AC-4 in float and fixed point, stage timers), each `--icount`, before and after |
| `flags_diff.py` | the flags each unit compiles with in two configured trees, minutes before a build; it renames a directory only when it moved whole |
| `ir_compare.py` | the LLVM IR of every unit, old tree against new, with the tree's path read alike |
| `c7_pathonly.py` | every change to a C or C++ file since the parent is a comment, an `#include`, layout or a path |
| `c7_planner_equiv.py` | the CI planners choose the same jobs for every tracked file, and for a sample of past commits |
| `c7_leftovers.py` | what still names a root C7 moved: a root built from parts, a regular expression, a glob, a directory a build writes into |
| `c7_cmake.py`, `c7_dirs.py`, `c7_roots.py`, `c7_relative.py` | the path passes after `consol_paths.py`: a build file's relative paths, a directory named whole, a root that is an ordinary word (python, rust, js), and the relative paths a move broke (`--fix` writes them again) |

The stage's parent is built in `build/wt/c7-before` (detached) and the stage in `build/wt/merge`; the
two are recorded and compared with `baseline.py compare`. A stage that shows a difference in output
bytes, exported symbols or test names stops, and the user is told before the next begins.

## What the runs found that the study did not

### C7-1, 2026-10-08 (`chore/monorepo-c7-1`)

**What moved.** 1,199 renames in one commit, every one `R100`: the libraries with their tests and fuzz
targets (1,179 files: `ac3` 350, `ac4` 260, `sendspin` 160, `audio` 97, `containers` 57, `dsp` 54,
`base` 53, `objects` 48, `adm` 35, `iab` 26, `capi` 20, `render` 19), `tests/support` 8,
`external/time-filter` 6, `tools/fuzz` 5 and `cmake/IclforgeFuzz.cmake` 1. The study counted 1,186
moves for the libraries, tests and fuzz targets; the rest are decisions 14's and the support
library's. Then 15 include respellings in 14 files (`consol_apply.py`), the paths in the build files
and scripts, and the paths in text; all in all 1,581 files changed (1,198 renamed, 352 modified, 30 added, 1 deleted).
The tree: `libs/<lib>/{CMakeLists.txt, include/iclforge/<lib>/, src/, variants/, tests/, fuzz/}`,
`tests/{support, cli, gui, hearth, crucible, performance, golden}`, `external/time-filter`,
`tools/fuzz`, `cmake/{IclforgeTests, IclforgeFuzz, External}.cmake`.

**A binary per library.** `iclforge_add_test_binary()` makes `iclforge-<lib>-tests` for each of the 12
libraries and labels its tests with the project: `ctest -L ac3` selects 1,071 tests, `-L ac4` 736,
and the 12 labels together 2,785; the 681 others are the products', which stay in
`iclforge-apps-tests` until C7-2. The old tag labels still combine (`-L resampler` 20, `-L ac4 -L
decoder` 313). The umbrella `iclforge-tests` builds them all. The names are the before-names: 3,466
on each static tree.

**What the dry run could not see** (each found by a build, a check or a comparison, and fixed by hand):

1. **A binary per library gives each unit only what its own library needs.** The monolith gave every
   unit the union of every include directory and definition, so the first build of the split failed on
   three: the `golden/*.hpp` tables (an include root of `tests/`), the CLI's ADM and IAB tests (no
   link to either), and `-Wno-null-dereference` on two Hearth test files (a per-source property the
   cut dropped). `flags_diff.py` over 832 units found the lost options and definitions before the
   fixes were read; its rule that renamed `tests/` because `tests/sanitized.hpp` moved was wrong, and
   now applies to a directory that moved whole.
2. **A path built from parts is not a path.** `${PROJECT_SOURCE_DIR}/src/dsp/variants/decode-scalar-${TIER}`
   (AC-4 stopped compiling), `${CMAKE_BINARY_DIR}/src/ac3/generated` (the installed export header),
   the ESP-IDF component's probe for `lib/src/ac3`, the ABI steps' `find build/…/src -name '*.so'`,
   Rust's `repo_root.join("src")`, `.clang-tidy`'s alternation `src/(ac3|base|…)/`, SonarCloud's keys.
   `c7_leftovers.py` lists them. The build tree's `build/<preset>/src/<lib>` is `libs/<lib>` now;
   the ABI steps look in whichever of the two a build has, since the comparison point is older.
3. **A check that passes because it reads nothing.** `check_doc_paths.py` read the literals starting
   `docs/`, `apps/`, `src/` and `tools/`, so "0 missing" meant nothing about `libs/`; with `libs/`
   and `external/` added, about 880 more literals are checked (6,489 at the stage's last code commit,
   against 5,607) and four were stale.
   `baseline.py`'s header record was empty under `libs/`, and `check_layering.py` and
   `check_namespaces.py` failed for want of files under `src/`. All three now read `libs/`.
4. **A library's `tests/` and `fuzz/` are inside the directory its code is in.** Each consumer of
   "the library" had to be told they are not it: the coverage filter (`--exclude`), the compare job
   and the ESP lane of the planners, the ESP packer (which would have shipped every library's tests
   and fuzz seeds in the component), `check_layering.py`, `.clang-tidy`'s header filter and
   SonarCloud (`sonar.test.inclusions`, with `libs` in both `sonar.sources` and `sonar.tests`).
5. **The planners.** `external/` was an unknown top-level directory: its C++ files lit everything, and
   its pages only the docs lane, where `src/` had made them core's. Taught `external/`, the planners
   give the same answers for all 3,185 tracked files and for the last 120 commits.
6. **What the packed ESP-IDF component holds.** It keeps the repository's layout under `lib/`, so
   `lib/src/ac3` is `lib/libs/ac3`; its file list is otherwise the before-list, with three more
   CMake modules (`External`, `IclforgeFuzz`, `IclforgeTests`) in `lib/cmake` that the root includes
   only when tests or fuzzers are asked for.
7. **Fuzz corpus and crash artifacts** are written under `build/` (`build/fuzz-corpus`,
   `build/fuzz-artifacts`), not in a tracked directory; `fuzz/` no longer exists.

**Found, and not C7's.** The shared Debug tree fails to link `iclforge-iab-tests`
(`iclforge::iab::DlcAudio::normalized()` is not exported); it failed before as the whole test binary,
and is now one binary. The fuzz build has failed to compile `fuzz_iec61937_unwrap` (it does not find
`iclforge/containers/iec61937/iec61937.hpp`) since C3, in the before-tree as in this one, so the
nightly fuzz job's build stops there. The CI step that binds `iclforge-tests` to the shared libraries named
`src/iec61937/libiclforge_containers.so`, a path C3 left; `esp-component.yml` and `wheels.yml`
filtered on `src/iec61937/**`, which has matched nothing since C3 (the lines are removed; the
wheel build is not triggered by a change to `libs/containers`, a decision for the user). The AC-4
fixed-point probe's stack is 22,712 bytes against a ceiling of 21,500, so its runner exits 1 before
and after. `tools/n1b/baselines/headers.json` predates C0. The path pass lengthened 14 C++ lines past
100 columns and about forty Python ones (`ruff check .` is a CI gate, `ruff format` is not);
`n1b_reflow.py` wrapped the C++ ones (clang-format 22.1.2) and the Python ones were wrapped by hand.

**The proof,** on this machine (WSL2, GCC 16 and Clang 22; `c7-before` is the commit before the stage):

| proof | result |
|---|---|
| builds, `-Werror`, every default target | GCC and Clang clean; the shared Debug tree has the one failure it had before |
| the whole ctest, GCC and Clang | 3,466 of 3,466 pass in each, before and after; the same 5 skipped; per-test outcomes identical by name. One run failed `group: a paired test sink decodes AC-4 sent over the extension role` (`0 == 256`) while other checks were loading the machine; it passes 25 times in a row on each tree and in a rerun of the whole ctest on an idle machine |
| ctest's names | identical to the before-names, a label added |
| pinned bitstream hashes, CLI corpus (44 commands) | identical, both compilers |
| exported names of every shared library | identical |
| installed tree | the same 213 files (203 regular, 10 symlinks) in both. Built with `git describe` pinned (a `git` that answers the stamping queries alike in both trees): the 12 ELF files have identical `.text` (5 are byte-identical), and so has every member of the 9 static archives; the 47 text files differ in comments only, 39 by a respelled path. Unpinned, `forge`, `hearth` and `libiclforge_base.so` differ, because `version_details()` folds the commit's `git describe` into code |
| IR of every unit (840, tests included) | 502 identical, 244 identical but for the whitespace of an assertion's text, 94 differing, none in code: the test units' strings that hold a directory (the scratch directory moved by design; the golden directories read the same but for the length of the worktree's name) and the `std::filesystem::path` instantiations named by those lengths; `git describe` in `version.cpp` and the units that print it; Hearth's generated Qt units (the resource tables, and 28 QML cache units, which regenerated in the before-tree equal the after-tree's once the tree path is normalised: 22 of the before-tree's copies were stale) |
| C and C++ edits since the parent | 1,009 of 1,010 files differ in comments, includes, layout and paths only; the other in a diagnostic string that now names `tools/fuzz/README.md` |
| public headers | the same 174, the same names; the `#include` lines of the 189 files edited are identical |
| bare-metal probes (QEMU, `--icount`) | five variants, the probes' own output (instructions per frame, heap, stack) identical byte for byte |
| libFuzzer harnesses (Clang, `ICLFORGE_BUILD_FUZZERS`) | the same 23 build and the same one does not (below); the 21 that are not differential replay the same 258 seed and regression inputs and exit 0 in both trees |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` | 228 edges in 21 pairs, 172 headers and 4 known debts, 0 problems, 0 missing: all as before |
| `precheck.py --unit`, `mkdocs build --strict`, `ruff check .` | pass; ruff has no finding the before-tree did not |
| the CI planners | the same answers for 3,185 files and 120 commits |
| `check_platform_macros.ps1` | not run (no PowerShell); its scan, redone in Python, covers the same 1,329 files and finds the same 2 lines |
| the ESP-IDF packer's staging (`stage()`, with and without `--with-ac4`) | the before-list of files (394 and 504), with the three CMake modules under `lib/cmake`; `lib/src/<lib>` is `lib/libs/<lib>` |

**Not run here, and recorded rather than skipped:** MSVC `/W4 /WX` and clang-cl; macOS; the Android
build; ESP-IDF `pack_esp_component.py --verify` and the boards (the staging was compared, the build
was not); the Python, Rust and WASM test suites (`pybind11`, `cargo` and `emcc` are not installed);
SonarCloud and CodeQL; the workflows themselves (their paths, filters and caches are read, not run).

**What C7-1 leaves.** `tests/{cli,gui,hearth,crucible}` and `iclforge-apps-tests` (C7-2);
`apps/common`; `tests/golden` and the `golden/…` include root of the four `ac3` tests that read the
generated tables (C7-4); `layering.json` and its debt files, still the libraries' alone (C7-5);
`.git-blame-ignore-revs`, which the consolidation stages did not extend either, gets the rewrite
commits of C0 to C7 when they land on `main`.

### C7-2, 2026-10-09 (`chore/monorepo-c7-2`)

**What moved.** 682 renames in one commit, every one `R100`: Forge's CLI 70 (`apps/forge/cli`) and GUI
91 (`apps/forge/gui`), Hearth's engine 96, window 48, test sink 9, test server 3 and `hearth-render` 1,
Crucible's engine 65, window 43 and runner 1, its driver and VM 34 and tray VM 13
(`apps/crucible/{windows,linux}`), the demos 105 (`apps/demos/android` 51, `apps/demos/wasm` 54), the
shared code 62 (`apps/shared/theme` 32, `media` 25, `preferences` 5) and the notices 41 (`notices/`).
Then four relative includes, `c7_cmake.py` (22 CMake files, 18 Qt catalogues, whose `<location>` lines
changed and whose messages did not), `consol_paths.py` (401 files), `c7_dirs.py` (the directories
named whole: 20 files, then a program's own names for its directories: 20 more), `c7_testnames.py`
(the comments that name a test binary or the `tests/` build file: 28 and 52 files), the reflow of 49 lines
in 33 files, and by hand a CMake file for each program's tests, the planners, the workflows and the
checks, README's and CONTRIBUTING's layout blocks and the pages that ran the one test binary. All in all
the stage changed 934 files since its parent (680 renames git still pairs after the edits, 14 added, 2 deleted,
238 modified). The tree: `apps/forge/{cli,gui}`, `apps/hearth/{engine,ui,render,testsink,testserver}`,
`apps/crucible/{engine,ui,runner,spikes,windows,linux}`, `apps/demos/{android,wasm}`,
`apps/shared/{media,theme,preferences}`, `notices/{fragments,licences,forge,crucible,hearth}`, each
program `{CMakeLists.txt, src/, assets/, tests/}`.

**A binary per program.** `iclforge-apps-tests` is gone. `iclforge-forge-cli-tests` (327 tests, label
`forge-cli`), `iclforge-forge-gui-tests` (14, `forge-gui`), `iclforge-app-media-tests` (55, `app-media`),
`iclforge-hearth-tests` (400, `hearth`) and `iclforge-crucible-tests` (158, `crucible`) are built from the
`tests/` beside each program and added to the umbrella `iclforge-tests`; `iclforge-settings-tests`
(19, `app-preferences`) and `hearth_controller_tests` stay the two small Qt binaries, and
`iclforge-perf` is the one thing `tests/` still builds beside `support/`. The names are the
before-names: 3,582 on the GUI-on trees of GCC 16 and Clang 22, 3,546 on the GUI-off ones, 3,189 on the
shared Debug tree. `ctest -L ac3` is 1,055 tests now, not 1,071: sixteen moved with the media code
they test. A project's label is also on the cases that carry no Catch2 tag for it.

**What the dry run could not see** (each found by a configure, a build, a check or a comparison):

1. **Three shared directories, not two (this extends decision 5).** The study found Hearth's and
   Crucible's programs taking `system_theme.*` from Forge's GUI. The run found that they take more:
   `language_manager.*` and `settings_migration.*` (all three programs run them at start-up), the typefaces
   and the icons, `forge-gui.rc.in`, and the thirteen "family" QML components
   (`cmake/SharedFamilyQml.cmake`'s list). So `apps/shared/preferences` is a third, beside
   `apps/shared/media` (was `apps/common`) and `apps/shared/theme`. None of the three has a CMake
   target: the programs compile the sources in as they did, so no object changes. The user is asked to
   check this extension.
2. **Qt names a QML cache unit by the path from the directory, and a path that climbs out of it loses
   the target.** Forge GUI's thirteen shared components are `apps/shared/theme/assets/qml` and no
   longer under the program's directory; `qt_add_qml_module` writes `${target}_<relative path>.cpp`, so
   `forge-gui` and `forge_gui_qmltests` (one directory scope, as Qt requires) named the same
   `.rcc/qmlcache/...` output twice ("already has a custom rule"). Hearth and Crucible had avoided it by
   staging copies into their own tree; forge-gui stages them too (`assets/qml/shared`, ignored by git),
   with the module's own URI, so the copies are the sources byte for byte. The resources are the same:
   the 18 `.qrc` lists (prefix, alias and order) and the 19 compiled `.qm` files are identical.
3. **Forge GUI's Qt Quick suite and its Catch2 cases share a directory,** and the first is `include()`d
   into the program's directory scope. The suite's file is `tests/qml.cmake` now and `tests/CMakeLists.txt`
   is the Qt-free binary that every CI leg builds, as it was in the monolith.
4. **Splitting the binary shows what each test needs.** The monolith compiled the shared sources once
   for everyone. The CLI's binary compiles `ac4_encode_core.cpp` and `ac4_objects_core.cpp` (it calls them),
   Hearth's compiles `container_input.cpp` (`hearth_engine` does not, by design), and the media binary
   compiles the six it holds. `libs/ac4/tests/decoder/test_object_render.cpp` stays in `libs/ac4` because it
   includes that directory's private `objects.hpp`, so ac4's binary still compiles
   `apps/shared/media/src/ac4_object_render.cpp`: a library-test to app edge, to be allowed by name in
   C7-5's check. The three tests of the media code that were in `libs/ac3/tests` and `libs/audio/tests`
   moved to `apps/shared/media/tests`.
5. **C7-1 left the workflows that ran the monolith by name,** and nothing noticed because they are not
   run here: Hearth's Sendspin job (`iclforge-tests "[sendspin],[hearth]"` and the two aiosendspin scripts),
   the Crucible pass, the shared-library binding check, the Windows coverage script, the generators'
   and race scripts' usage text, and some forty pages and comments. They name the binary that holds what
   they meant now (`iclforge-sendspin-tests`, `iclforge-hearth-tests`, `iclforge-crucible-tests`,
   `iclforge-audio-tests`, `iclforge-ac3-tests`: the binding check binds 382 symbols to the six
   libraries). Six stale literals that `check_doc_paths.py` already reported at C7-1 are fixed.
6. **A planner answers for a path, not for a project.** Moving a program's tests beside it changes
   which prefix a test file has: `tests/cli/` was core's and `apps/forge/cli/tests/` is under the program's
   prefix. `classify_changes.py` and `plan_gate.py` now name a program's Catch2 tests as core's (a window's
   `test_*` files by name, its Qt Quick suites staying with the program), keep Crucible's driver and tray
   VM to their own platform's lane, and give the notice fragments the lanes their old directories had.
   `c7_planner_equiv.py` found each of these: 3,213 files and the last 60 commits (48 touching files of
   the old tree) get the same answers.
7. **The text pass leaves what is not a file path.** A directory named whole (`apps/wasm/tests`), a
   program's directories as its own comments call them (`engine/platform`, `ui/qml`), a path written
   with backslashes (`$env:GITHUB_WORKSPACE\apps\windows\driver`, in a workflow and four READMEs), and
   every path written from the file that holds it. The first two are `c7_dirs.py`. The last is
   `c7_relative.py`, which reads the 16,425 relative paths of the tree and lists those that resolved
   from where a file was and do not from where it is: 55, of which the real ones were the Qt Quick
   tests of forge-gui (their input files, `../../../../libs/...`, five levels now), the WASM board tests
   and the Playwright configuration (`esp-idf/` and the build tree one level further), the tray VM's
   `Sync-Source.ps1` (the repository root, three levels up and four now), the Android app's native build
   (`add_subdirectory` of the root: six and seven), and the READMEs' links. The first run of it dropped
   a path made of nothing but `../`, which is how the Android one was found.
8. **A script that respells a name in a comment respelled a line of code.** `c7_testnames.py` took
   `include(tests/CMakeLists.txt)` in Hearth's window for a comment; the proof's configure of the
   stage's own tree stopped on it. The script leaves an `include()` and an `add_subdirectory()` alone.
9. **`flags_diff.py` could not see the stage until it learned two things:** a rename is made in one pass
   (`tests/crucible` to `apps/crucible/engine/tests` and the directory rule
   `apps/crucible/engine` to `apps/crucible/engine/src` applied one after the other turned the first
   into `apps/crucible/engine/src/tests`), and the build tree of a generated Qt directory has its own
   names (`--rename`, `--rename-re`). With them every one of the 913 units pairs, and what differs
   is, in the programs, the include roots (`src/`) and one definition, `QT_TESTCASE_SOURCEDIR`, which
   names the program's directory; in the test binaries, what the split removed.
10. **The proof's own scripts.** `c7_record.sh` takes extra configure arguments (the GUI on), and the IR
    comparison needs two source trees whose paths are as long as each other, because a string that holds a
    path has a length in its type: with `c7-before` and `merge` as the roots, 75 units differed by it. A
    QEMU started from a background job stops on its first read of the terminal, so the probes run with
    their input from `/dev/null`; the fuzz replay needs `REPO_ROOT` set to find a harness's seeds; and
    `forge-gui --smoke` is `--smoke <in.wav> <out.ac3>`, not a flag that opens the window.
11. **Moving a Qt program changes the order of its generated code, and nothing else.** `forge-gui` and
    `hearth` are the two installed binaries whose `.text` is not identical across the stage, with both
    compilers, and in one object each: AUTOMOC puts each moc file in a directory named by a hash of its
    source's path and includes them from `mocs_compilation.cpp` in that order, so a moved source moves its
    functions (Hearth's `LanguageManager`, which moved from `apps/gui` to `apps/shared/preferences`, was
    first and is last). The two objects hold the same 105 and 39 functions with the same symbols and sizes,
    and the bodies read alike but for the offsets into `.rodata` and the name a jump inside the object is
    printed against. Hearth's object has 9 bytes more `.text` (17,547 against 17,538), the padding that
    the new order needs between functions as far as can be told; Forge GUI's is the same size (41,186).
    No behaviour differs (the smoke encode and the Qt Quick tests are the same),
    but it is a difference in an installed binary and the user is told of it rather than it being passed
    over. C7-1 had the same kind of difference in Hearth's generated Qt units.

**Found, and not C7's.** The shared Debug tree still fails to link `iclforge-iab-tests`, as it did before.
`fuzz_iec61937_unwrap` does not compile (since C3), and the fixed-point AC-4 probe exits 1 in both trees,
over its stack ceiling.
`tests/golden/ac4/scalar-agreement*.json` key their pins by the GUI fixture's path, which follows the file
(the golden data is C7-4's otherwise). `.git-blame-ignore-revs` is not extended. The Windows coverage
script named a binary that had not existed since C7-1.

**The proof,** on this machine (WSL2 on Windows 11, GCC 16 and Clang 22, Qt 6.10; the parent is
`chore/monorepo-c7-1` at `21d1b2215`, built in `build/wt/c7-before`; the stage's tree in `build/wt/merge`,
at `d9a28c0fe`: the commits after it change a page of docs, `sonar-project.properties` and this record):

| proof | result |
|---|---|
| builds, `-Werror`, every default target, the GUI on and off | GCC 16 and Clang 22 clean in both; the shared Debug tree has the one failure it had before (`iclforge-iab-tests` does not link) |
| the whole ctest, GCC and Clang (GUI on) | 3,582 of 3,582 pass in each, before and after; the same 5 skipped (they read a stream the environment names); the outcome of each test, by name, identical (JUnit). The GUI-off trees and the shared Debug tree are compared by name |
| ctest's names | identical to the before-names on all five trees: 3,582 (GUI on, GCC and Clang), 3,546 (GUI off), 3,189 (shared Debug); a label added, and sixteen tests that moved with their code still named as they were |
| pinned bitstream hashes, CLI corpus | identical, both compilers |
| exported names of the shared libraries | identical |
| installed tree | the same 219 files. Three public headers differ, in a comment that names a moved path (`probe.hpp`, `bridge.hpp`, `render.hpp`). `NOTICES.txt` of Forge (which Forge GUI shares) and of Hearth: byte-identical in the installs of both compilers; Crucible's, configured with the stub, byte-identical (3,559 bytes) |
| `.text` of the installed binaries (`git describe` pinned, both compilers) | 11 ELF files have identical `.text` (5 byte-identical) and so has every member of the 9 static archives (171). Two differ, `forge-gui` and `hearth`, each in one object only, its `mocs_compilation.cpp.o`: the same functions (105 and 39), the same symbols and sizes, in another order (finding 11) |
| Qt resources | the 18 `.qrc` lists (prefix, alias, order) and the 19 compiled `.qm` files identical |
| flags of every unit (`flags_diff.py`, 913) | all 913 pair. 447 compile with other flags: in the programs the include roots (`src/`) and one definition, `QT_TESTCASE_SOURCEDIR`, which names the program's directory; in the test binaries the include roots and the sources the split took out (5 media units left `iclforge-ac3-tests`). Crucible's units, configured with the stub, differ in include roots alone |
| IR of every unit (728, tests included, Clang 22, two source trees whose paths are as long as each other) | 643 identical, 50 identical but for the whitespace of an assertion's text, 35 differing. In 33 the difference is a string that holds a path (and the `std::filesystem::path` constructors instantiated over the strings' lengths); the others are `version.cpp` (`git describe`) and `test_group.cpp`, where one of those constructors is no longer instantiated because two lengths have become equal |
| C and C++ edits since the parent (`c7_pathonly.py`) | 393 files; all differ in comments, includes, layout and paths only |
| public headers | the same 174, none missing, none changed (`baseline.py check-moves`) |
| `forge-gui --smoke`, Qt Quick tests | `--smoke` (offscreen) encodes the same 24,576-byte `.ac3` in both trees and with both compilers; Forge GUI's 14 Qt Quick suites and Hearth's are in the ctest above |
| bare-metal probes (QEMU, `--icount`) | five variants (decoder, encoder, AC-4 float and fixed, decoder with stage timers): the probes' own key=value lines identical; the fixed-point AC-4 probe exits 1 in both trees, over its stack ceiling, as before |
| libFuzzer harnesses (Clang, `ICLFORGE_BUILD_FUZZERS`) | the same 20 of 21 build and the same one does not (`fuzz_iec61937_unwrap`, since C3); 18 replay their seed and regression inputs (the same 237 files, and `fuzz_ac4_decode` those of `fuzz_ac4_parse`) and exit 0 in both; two have no inputs |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` | 12 libraries, 228 edges, 0 failures; 172 public headers in 11 libraries, 0 failures, 4 known debts; 0 problems; 0 missing of 6,589 checked |
| `precheck.py --unit`, `mkdocs build --strict`, `ruff check .` | pass; ruff finds the 25 it found in the parent, none new |
| `c7_relative.py` | of 16,471 relative paths, 7 resolved before and do not now, and none is the repository's: they name Qt's installation (`${Qt6_DIR}/../../../bin`), a deployed site's page, or a build directory |
| the CI planners | the same answers for 3,213 files and for the 48 commits, of the last 60, that touch files of the old tree |

**Not run here, and recorded rather than skipped:** MSVC `/W4 /WX` and clang-cl; macOS; the Android
build; the Windows and macOS halves of the notices (`components.cmake` and the packages themselves);
Crucible's units that need PipeWire (configured with a stub `libpipewire-0.3.pc`, so that its flags and
NOTICES.txt could be compared); the ESP-IDF build and the boards; the Python, Rust and WASM test suites
(`pybind11`, `cargo` and `emcc` are not installed); SonarCloud and CodeQL; the workflows themselves
(their paths, filters and the binaries they run are read, not run).

**What C7-2 leaves.** `apps/baremetal` and the other firmware and bindings (C7-3); `tests/golden` (C7-4);
`layering.json` and the check of the whole tree (C7-5), which is to allow by name the one edge from a
library's test to a program's code (finding 4). SonarCloud's mixed source and test configuration
(`apps/*/tests` is now in `sonar.tests` and the sources) is unverified. Two things are the user's to
check: the third shared directory (finding 1), and the difference in the order of the generated Qt code
of two binaries (finding 11).

### C7-3, 2026-10-09 (`chore/monorepo-c7-3`)

**What moved.** 290 renames in one commit, every one `R100`: the bindings 92 (`bindings/python` 30,
`bindings/rust` 31, `bindings/js` 31), the ESP-IDF component 51 (`firmware/esp-idf/iclforge`, with its one
example, `i2s_player`), the ESPHome component 5 (`firmware/esphome`), the bare-metal probes 46
(`firmware/baremetal`, was `apps/baremetal`) and Hearth's sink 96 (`firmware/hearth-sink`, was
`esp-idf/iclforge/examples/hearth_sink`). Then `c7_cmake.py --stage c7-3` (11 CMake files),
`consol_paths.py` (183 files, 460 lines), `c7_roots.py` (63 files, 204 mentions: the roots named alone),
`c7_relative.py --fix` (47 relative links in 5 files) and by hand the rest the bare names (28 places), the three files that reach the repository root one level further, the planners and their tests, the checks and tools that name a root as a path component, the packer, the sink's `override_path`, README's layout block, the sink's `OTA_PY`, the packaging version check and the Python lines the longer paths pushed past 100 columns. All in all the stage
changed 421 files since its parent. The tree: `bindings/{python,rust,js}`,
`firmware/{esp-idf,hearth-sink,esphome,baremetal}`; a binding or a firmware project keeps the layout of
its ecosystem (a wheel's `src/`, a Cargo workspace, an npm package, an ESP-IDF project's `main/`).

**Hearth's sink is a project of its own.** `firmware/hearth-sink/main/idf_component.yml` names
`iclforge` with an `override_path` into `firmware/esp-idf/iclforge`, and the project's `CMakeLists.txt`
no longer adds the component's directory to `EXTRA_COMPONENT_DIRS`. The component keeps one example,
`i2s_player`, which is what the registry now ships, so the packed archive is the before-archive without
the sink's 56 files (the `www/` stream set was already left out of it): `pack_esp_component.py` no longer
removes a directory that is not there.

**What the dry run could not see** (each found by a configure, a build, a check or a comparison):

1. **Five of the six roots are ordinary words.** `python`, `rust`, `js`, `esp-idf` and `esphome` are
   also what a command, a YAML tag and ESPHome's own tree are called. The path pass derives a directory
   rule from the moves and applies it to the bare name, which would have made `python tools/ci/x.py` in a
   workflow `bindings/python tools/ci/x.py`. So `consol_paths.py --stage c7-3` drops the rules of one
   component and rewrites a full path and a directory named below a root; `c7_roots.py` respells a root
   alone only where a slash follows it or a link into the repository ends at it, looks the rest up in the
   tree, and lists what is not there (`python/name` of a YAML tag, `esphome/core/log.h`, which is
   ESPHome's tree, `esp-idf/ac3forge/...`, a path from before a rename); `c7_cmake.py --stage c7-3`
   respells a one-word token only as the argument of `add_subdirectory()`. What is left is the roots in
   a path position without a slash: a `working-directory`, dependabot's `directory`, `package-dir`,
   `repository.directory`, `pip install ./python` and `cd js`, 28 places, found by a grep of those
   positions and edited by hand.
2. **A directory that went two ways is no rule.** `esp-idf/` was the component (51 files) and the sink
   (96), so `esp-idf`, `esp-idf/iclforge` and `esp-idf/iclforge/examples` have no one new place and
   `dir_rules` leaves all three to a person; `c7_roots.py` takes `esp-idf` to `firmware/esp-idf` and
   the sink's own directory to `firmware/hearth-sink`. `c7_relative.py` had the same fault the other
   way: it took `apps/` to be `firmware/` because `apps/baremetal` went there, and listed 91 paths, 37
   of them wrong. A directory is moved "whole" now only if every file under it went to the one place.
   It also writes the paths again (`--fix`): 47 relative links of the component's, the sink's and
   ESPHome's READMEs, which were the real ones.
3. **Three more levels, each by hand and each found by a check.** The wheel's `cmake.source-dir` and the
   version provider's `root` (`".."` to `"../.."`), `iclforge-sys/build.rs`'s two `parent()` calls (a
   third, and its message), and the component's `ICLFORGE_ROOT` (`../..` to `../../..`). The sink and the
   probes reach the component and each other by relative paths `c7_cmake.py` respelled
   (`${CMAKE_CURRENT_SOURCE_DIR}/../../baremetal`, the directory `EXTRA_COMPONENT_DIRS` wants).
4. **`sdkconfig.ac4` is not text to the path pass.** `.ac4` is a bitstream extension (`BINARY_EXT` of
   `n1b_paths.py`), so the probe's fragment and the sink's, which are Kconfig files, kept `apps/baremetal`
   and `examples/hearth_sink`.
5. **The build tree follows the source tree.** `build/<preset>/apps/baremetal/iclforge-probe.map` is
   `firmware/baremetal/` now, and so are the artefact paths of the ESP-IDF probe's summaries and the
   probe runners' `PROJECT=` (9 lines of `_build.yml`, `footprint_report.py`,
   `run_esp32{c3,s3}_probe.sh`).
6. **A planner answers for a path.** The sink was under `esp-idf/`, which is the `esp` lane's and which
   the Linux gate does not build; `firmware/hearth-sink/` is a prefix neither planner knew, and an unknown
   one lights every lane. Both name it. `c7_planner_equiv.py` found it: 3,227 files and the last 60
   commits (55 touching files of the old tree) get the same answers.
7. **The sink leaves the component's archive.** That is decision 8. `pack_esp_component.py` staged the
   component's directory whole, `examples/hearth_sink` with it, and removed its `www/` stream set; the
   sink being elsewhere, the 56 files that archive held of it are gone and the line that removed `www/`
   with them. Everything else the packer stages is the before-list (397 files; 507 with `--with-ac4`), 24
   of them with a respelled path in a comment, and `ICLFORGE_ROOT` of the wrapper, which the archive does
   not use (it carries its own `lib/`).
8. **A check that scans a root names it.** `check_platform_macros.ps1` scanned `apps` (which held the
   probes) and `python`; it scans `firmware/baremetal` and `bindings/python` now, the same files (the
   ESP-IDF trees are not scanned, as they were not). `check_esp_efuse_free.py` reads
   `firmware/{esp-idf,hearth-sink,baremetal}`, `check_pages.py` takes the component's headers from
   `firmware/esp-idf/iclforge/include`, `rewrite_roadmap_comments.py` and three test files built their
   paths from components (`ROOT / "python" / ...`), and `sonar.sources` listed `js,python` by bare name.
9. **ESPHome's component names the IDF component by its place in the repository** (`COMPONENT_PATH`) and
   clones the repository at `version:` (default `main`). `firmware/esp-idf/iclforge` exists from this
   stage on, so a `version:` has to name a ref after it. Unpublished, so nobody is pinned to an older one;
   recorded for the day somebody is.
10. **The npm package's error message names a path.** "... see bindings/js/README.md": the one string
    of `dist/` that differs. Without comments and source maps, which carry the source's text and its
    positions, the compiled package is identical.
11. **Two paths no scan read.** `os.path.join("..", "..", "..", "..", "tools", "hearth", "ota.py")` in
    the sink's `idf.py` extension (a relative path of components, which `c7_relative.py` does not
    tokenise; found by the one failing test of `tools/hearth`) and `"$root/python/pyproject.toml"` in
    `check_packaging_versions.sh` (after `$root/`, which no pass takes for the start of a path; found
    by the check failing at its first run on the stage). Both are fixed, and a grep of the quoted-component
    and variable-prefixed forms finds no third.
12. **The proof's own tooling.** `c7_run.sh` takes the trees and the output directory (`C7_WORK`,
    `BEFORE_WT`, `AFTER_WT`); the pinned-`.text` comparison names its two trees; the ESP-IDF images are
    built from trees whose names are as long as each other (`old-tree`, `new-tree`), because the
    `__FILE__` of an assert is in the image, and what they differ in is then the version string, the
    build time, the tree's name in ten strings and the two SHA-256 digests those change.

**Found, and not C7's.** The Rust crates do not build under MSVC 19.51 here, in the parent as in the stage: `iclforge-sys` builds the shared C library with `/WX` and C4275 (`base::LevelMeter` and `base::LoudnessMeter`, not dll-interface, as the bases of `ac3::analysis::LevelMeter` and `ac3::meta::LoudnessMeter`, `libs/ac3/include/iclforge/ac3/{analysis/levels,meta/loudness}.hpp`) stops it; `build-rust` has a Windows leg in CI, whose compiler is not this one. `gen_baremetal_fixture.py` does not write the committed `fixture.hpp` back with this tree's encoder (the bitstream bytes are those of an older one), as before. The shared Debug tree still fails to link `iclforge-iab-tests`, and `fuzz_iec61937_unwrap` does not compile (since C3).

**The proof,** on this machine (WSL2 on Windows 11, GCC 16 and Clang 22, Qt 6.10, ESP-IDF v6.1, Rust 1.98,
Node 22; the parent is `chore/monorepo-c7-2` at `f034941d1`, built in `build/wt/merge`; the stage's tree in
`build/wt/c73` at `bacd09416`):

| proof | result |
|---|---|
| builds, `-Werror`, every default target, the GUI on and off | GCC 16 and Clang 22 clean in both; the shared Debug tree has the one failure it had before (`iclforge-iab-tests` does not link) |
| the whole ctest, GCC and Clang (GUI on) | 3,582 of 3,582 pass in each, before and after; the same 5 skipped; the outcome of each test, by name, identical (JUnit) |
| ctest's names | identical on the five trees: 3,582 (GUI on, GCC and Clang), 3,546 (GUI off), 3,189 (shared Debug) |
| pinned bitstream hashes, CLI corpus | identical, both compilers |
| exported names of the shared libraries | identical |
| installed tree | the same files; two public headers differ in a comment that names a moved path (`trace_export.hpp`, `render.hpp`) |
| `.text` of the installed binaries (`git describe` pinned, both compilers) | all 13 ELF files have identical `.text` (5 byte-identical) and so has every member of the 9 static archives (171); `forge-gui` and `hearth` too, since no Qt source moved and the moc files keep their directories' names |
| flags of every unit (`flags_diff.py`, 913) | all 913 pair and none compiles with other flags |
| IR of every unit (798, tests included, Clang 22, two source trees whose paths are as long as each other) | 795 identical; 3 differ in the `git describe` string alone (`version.cpp` and the two Hearth units that embed it) |
| C and C++ edits since the parent (`c7_pathonly.py`) | 111 files; all differ in comments, includes, layout and paths only |
| public headers | the same 174, none moved; three differ in a comment (`trace_export.hpp`, `render.hpp`, `sendspin/player_session.hpp`) |
| bare-metal probes (QEMU, `--icount`), `firmware/baremetal` configured at its new place | five variants: the probes' own key=value lines identical; the fixed-point AC-4 probe exits 1 in both trees, over its stack ceiling, as before |
| libFuzzer harnesses (Clang, `ICLFORGE_BUILD_FUZZERS`) | the same 20 of 21 build and the same one does not (`fuzz_iec61937_unwrap`, since C3); 18 replay their inputs and exit 0 in both; two have none |
| ESP-IDF v6.1 images, built in both trees (`old-tree` and `new-tree`, names as long as each other) | the Hearth sink for the S3 in the shape CI runs under QEMU (610,608 bytes), for the P4 with Sendspin and AC-4 (2,474,544), for the C6 with 16 MB flash and Sendspin (1,681,152); `i2s_player` (267,088); the S3 probe (519,840). Each pair is the same size and differs in 89 to 111 bytes, every one in the version string, the build time, the tree's name in ten assert strings (`old` and `new`), the two SHA-256 digests those change, and, in the sink's FAT image, the volume serial (4 bytes) |
| the ESP-IDF packer (`stage()`, then `pack_esp_component.py --verify`) | 397 files staged before (507 with `--with-ac4`), 341 (451) after: the sink's 56 are gone, 24 of the others differ in a path in a comment and none in any other way. `--verify` passes in both trees (ESP32-S3 and ESP32-C3; ESP32-P4 with AC-4); the archives are 1,502,798 and 2,020,618 bytes before, 1,337,045 and 1,854,732 after |
| the Python extension, built with the wheel's CMake definitions (pybind11 3.1.0), then its pytest suite | built in both trees; the files a wheel carries are the same 4, `_iclforge` byte-identical and `__init__.pyi` differing in one comment path; 128 tests pass in each |
| the npm package (`npm ci`, `npm run build`, `npm test`) | 102 of 102 in both. `dist/` is identical but for comments and source maps, which carry the source's text and positions, and one error string ("see bindings/js/README.md") |
| the Cargo workspace (`cargo metadata --locked`) | the same 29 packages with the same targets and dependencies, `Cargo.lock` valid where it is now; the crates do not build here (below) |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` | 12 libraries, 228 edges, 0 failures; 172 public headers, 0 failures, 4 known debts; 0 problems; 0 missing of 6,504 checked |
| the static job's other checks | `check_packaging_versions.sh`, `check_corpus.py`, `check_platform_matrix.py`, `check_esp_efuse_free.py` (28 fragments, as before), `check_android_jni.py`, `generate_support_matrices.py --check` and the three AC-4 table generators' `--check` pass |
| unit tests of `tools/` | `tools/checks` 432, `tools/ci` 585, `tools/hearth` 102, `tools/n1b` 495 pass |
| `precheck.py --unit`, `mkdocs build --strict`, `ruff check .` | pass; ruff finds the 25 it found in the parent, one of them in the file's new place |
| `c7_relative.py` | of 16,508 relative paths, 24 are listed after the fixes, and none is a break: the fixed links themselves (the new spelling is a tail of the old one) and the probes' `../../../esp-idf` |
| the CI planners | the same answers for 3,227 files and for the 55 commits, of the last 60, that touch files of the old tree |

**Not run here, and recorded rather than skipped:** MSVC `/W4 /WX` and clang-cl; macOS; the Android build; the Windows and macOS halves of the notices; the Rust crates (above); the wheel itself (`scikit-build-core` and `setuptools_scm` are not installed; the CMake definitions it passes and the extension were built, and `root` and `cmake.source-dir` were read, not run); `esphome config` and the component's Python (`esphome` is not installed); the ESP32 images on a board or under QEMU (they are the same code, and the differences between them are named above); SonarCloud and CodeQL; the workflows themselves (their paths, filters, caches and the binaries they run are read, not run).

**What C7-3 leaves.** `tests/golden` (C7-4); `layering.json` and the check of the whole tree, which has
to know the bindings and the firmware as projects (C7-5).

### C7-4, 2026-10-09 (`chore/monorepo-c7-4`)

**What moved.** 201 renames in one commit, every one `R100`: `tests/golden` is `testdata/`, with the
layout it had below it ({ac4 141, external-baseline 35, audio 8, ac4-hsf 8, object-fixture 1} and eight
files at the top: the three generated C++ tables the ac3 tests include and five pin files). Then
`c7_cmake.py --stage c7-4` (3 CMake files), `consol_paths.py` (129 files, 478 lines), `c7_golden.py` (28
files, 49 mentions) and by hand the include root and the four includes of the golden tables, the test
support library's definitions, README's layout block, the test case that names the directory, the planners
and the fixture whose bytes the corpus pins. All in all the stage changed 357 files since its parent.

**What the dry run could not see** (each found by a configure, a build, a check or a comparison):

1. **The golden tables are C++, and they have an include root.** The four ac3 tests that include them
   spell `"golden/<name>_goldens.hpp"` against `tests/` as the root; `testdata/` has no `golden/` below
   it. They are `"<name>_goldens.hpp"` now, against `testdata/` (`libs/ac3/tests/CMakeLists.txt`), which
   leaves `tests/` no longer an include root of that binary. The 85 units of that binary are the only
   ones whose flags differ.
2. **The golden directories were computed, not spelled.** `tests/support/CMakeLists.txt` made
   `ICLFORGE_GOLDEN_AUDIO_DIR`, `_OBJECT_DIR`, `_EXTERNAL_BASELINE_DIR` and `AC4_GOLDEN_DIR` from its own
   directory's sibling (`cmake_path(GET ... PARENT_PATH)`, then `APPEND "golden"`), which a path pass
   cannot see. They are `${PROJECT_SOURCE_DIR}/testdata/...` now.
3. **Data that names a path, and a fixture that is hashed.** The passes rewrite what is in `testdata/`
   too: the scalar-agreement pins are keyed by the stream's path (146 lines in two files), the
   manifests record `source_wav`, and the pinned-hash file's comment names the WAV. That is right for
   the first two (the consumers build the same keys from the new paths) and it changes the third's git
   blob (`baseline.py`'s `pins_blob`, the one field of the hashes that differs). It was wrong for
   `reference_objects.paths`: the corpus manifest holds this fixture's SHA-256, and the comment in it that
   named `tests/golden/audio/reference_objects.wav` was changed by the pass; `check_corpus.py` failed
   ("every published trend series is measured against these bytes"). The comment keeps the old path.
4. **A test case's name names the directory.** `chunks: a burst chunk for the first syncframe of
   tests/golden's AC-3 5.1 fixture` is the one title in the tree that does. The pass changed it;
   `c7_pathonly.py` listed it before any build, and ctest's names must not change, so it keeps its
   spelling and its comment follows the move.
5. **The path boundary again.** `consol_paths.py` takes a path after a separator, a quote, `${VAR}/` or
   `../`, not after a shell variable (`"$REPO/tests/golden/..."`, in a dozen scripts), in a string that
   goes on (`ICLFORGE_SOURCE_DIR "/tests/golden/..."`), with a brace expansion or in a comment that names
   the directory. `tests/golden` being one string with one meaning, `c7_golden.py` respells it at a path
   component wherever a live file has it (28 files, 49 mentions), and not in the plans or the history.
6. **A new top-level directory is an unknown one.** The planners light every lane for a path they do not
   know; `testdata/` is core's (the classifier) and a tree the Linux gate builds without Qt (the gate), as
   `tests/` was. `c7_planner_equiv.py` found it: the first run differed for the golden files.
7. **The proof's own tools read the golden data.** `baseline.py` (the WAV it encodes from, the pin file's
   blob) and `cli_bytes.py` (the corpus's root) are the live ones of `tools/n1b`, so the passes leave them
   and `c7_golden.py` does not; each tree records itself with its own copy. `check_pages.py` skips
   `testdata/` where it skipped `tests/golden/`, and the AC-4 reference's `stream_label` looks for
   `/testdata/` in a stream's absolute path (the labels it writes, the path after that directory, are the
   same).
8. **The CLI prints the name it was given.** The corpus passes `testdata/...` instead of `tests/golden/...`,
   and `probe`, `levels`, `loudness`, `qc`, `cut` and `normalize` echo it: 15 of the 44 commands differ in
   stdout and in nothing they write. The comparison is `cli_stdout.py`, which runs each tree's own corpus
   with its own forge, in the same scratch directory (stdout also names the output file), and reads the old
   directory's name as the new one.

**Found, and not C7's.** The generators of the three C++ tables need the standard's text
(`spec/A52-2018.txt`), which is not in the repository, and `gen_baremetal_fixture.py` does not write the
committed `fixture.hpp` back with this tree's encoder, as before. The shared Debug tree still fails to
link `iclforge-iab-tests`, and `fuzz_iec61937_unwrap` does not compile (since C3).

**The proof,** on this machine (WSL2 on Windows 11, GCC 16 and Clang 22, Qt 6.10; the parent is
`chore/monorepo-c7-3` at `1b26fcb95`, built in `build/wt/c73`; the stage's tree in `build/wt/c74` at
`3f0e11b67`; the commits after it are the planners' and the record):

| proof | result |
|---|---|
| builds, `-Werror`, every default target, the GUI on and off | GCC 16 and Clang 22 clean in both; the shared Debug tree has the one failure it had before (`iclforge-iab-tests` does not link) |
| the whole ctest, GCC and Clang (GUI on) | 3,582 of 3,582 pass in each, before and after; the same 5 skipped; the outcome of each test, by name, identical (JUnit) |
| ctest's names | identical on the five trees: 3,582 (GUI on, GCC and Clang), 3,546 (GUI off), 3,189 (shared Debug) |
| pinned bitstream hashes | identical for the three streams in both modes with both compilers; the one field that differs is `pins_blob`, the git blob of the pin file, whose comment names the source WAV (finding 3) |
| CLI corpus (44 commands) | every file each command writes is identical; 15 commands print the path of the file they were given and so differ in stdout as printed, and in none once `tests/golden` is read as `testdata` in the old text (`cli_stdout.py`: 44 commands run in each tree with its own forge) |
| exported names of the shared libraries | identical |
| installed tree | the same files; one public header differs in a comment (`ac3/core/mdct.hpp` names the golden table's directory) |
| `.text` of the installed binaries (`git describe` pinned, both compilers) | all 13 ELF files have identical `.text` (5 byte-identical) and so has every member of the 9 static archives (171) |
| flags of every unit (`flags_diff.py`, 913) | all 913 pair; the 85 units of `iclforge-ac3-tests` compile with `-I testdata` where they had `-I tests`, and none differs in any other flag |
| IR of every unit (tests included, Clang 22, two source trees whose paths are as long as each other) | 798 units: 755 identical, 8 identical but for the white space of an assertion's text, 35 differing. In 31 of those the difference is a string that holds the golden directory (the macros of the test-support library: `<root>/tests/golden/ac4` and `<root>/testdata/ac4`) and, in one, the `std::filesystem::path` constructors instantiated over the strings' lengths; the other 3 are `git describe` (`version.cpp` and the two Hearth units that embed it) |
| C and C++ edits since the parent (`c7_pathonly.py`) | 40 files; all differ in comments, includes, layout and paths only (the four includes of the golden tables, one string literal, `tests/performance/real_audio.hpp`'s path of the reference WAV) |
| public headers | the same 174, none moved; one differs in a comment |
| bare-metal probes (QEMU, `--icount`), which read `testdata/ac4-probe-ceilings.json` and the PCM pins | five variants: the probes' own key=value lines identical; the fixed-point AC-4 probe exits 1 in both trees, over its stack ceiling, as before |
| libFuzzer harnesses (Clang, `ICLFORGE_BUILD_FUZZERS`) | the same 20 of 21 build and the same one does not (`fuzz_iec61937_unwrap`, since C3); 18 replay their inputs and exit 0 in both; two have none |
| the generators | `gen_gold_reference_wav.py` and `gen_stereo_reference_wav.py` write `testdata/audio/reference_51.wav` and `reference_stereo.wav` byte for byte as they are; the three table generators' `--check` pass; the others need inputs this machine lacks (the standard's text, a Dolby encoder) |
| `check_corpus.py`, `check_cross_platform_hash.py`'s pins, the scalar-agreement pins | all 7 fixtures match the corpus manifest; the pin files are read at their new place (the unit tests of `tools/checks` pass) |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` | 12 libraries, 228 edges, 0 failures; 172 public headers, 0 failures, 4 known debts; 0 problems; 0 missing of 6,504 checked |
| the static job's other checks and the unit tests of `tools/` | `check_packaging_versions.sh`, `check_platform_matrix.py`, `check_esp_efuse_free.py`, `check_android_jni.py`, `generate_support_matrices.py --check` and the AC-4 table generators pass; `tools/checks` 432, `tools/ci` 586, `tools/hearth` 102, `tools/n1b` 495 tests pass |
| `precheck.py --unit`, `mkdocs build --strict`, `ruff check .` | pass; ruff finds what it found in the parent |
| `c7_relative.py` | no relative path resolved before and not now |
| the CI planners | the same answers for 3,226 files and for the last 60 commits (finding 6) |

**Not run here, and recorded rather than skipped:** MSVC `/W4 /WX` and clang-cl; macOS; the Android build;
the Windows and macOS halves of the notices; the ESP-IDF images and the boards (no file of theirs reads the
golden data but the probe runners, which ran under QEMU above); the Python, Rust, npm and ESPHome suites (the
bindings read no golden data); SonarCloud and CodeQL; the workflows themselves.

**What C7-4 leaves.** `layering.json` and the check of the whole tree (C7-5).

### C7-5, 2026-10-09 (`chore/monorepo-c7-5`)

**What moved.** One rename, `R100` and alone: `tools/checks/layering.json` is `tools/checks/projects.json`.
Nothing else moves, and no path the build reads changes. Then by hand: the table (30 projects and five
named exceptions), `check_layering.py` (rewritten, with its 33 tests), the debt directory's README,
`CONTRIBUTING.md`, `README.md`, the static job's step (now "Project layering"), `precheck.py`, two comments
that named the old file (one in `libs/dsp`, one in `libs/ac4/tests/CMakeLists.txt`) and, found by the static
checks, the tools of `tools/n1b/` that read the old table (finding 5). All in all the stage
changed 18 files since its parent.

**What the check does.** `projects.json` lists every project of the tree with its kind, its path and the
projects it may use; a file belongs to the project with the longest path above it, and a project's `tests/`
and `fuzz/` (and the top-level `tests/`, which is a project of its own) are its consumers. The kinds are the
rules the table is held to as well as the tree:

| kind | where | may use |
|---|---|---|
| `library` | `libs/<name>` | other libraries and the vendored code |
| `app-library` | `apps/shared/<name>`, internal | libraries |
| `app` | a product: `apps/forge`, `hearth`, `crucible`, `demos/android`, `demos/wasm` | libraries and app-libraries, never another app |
| `binding` | `bindings/{python,rust,js}` | libraries |
| `firmware` | `firmware/{esp-idf,hearth-sink,esphome,baremetal}` | libraries |
| `example` | `examples/` | libraries |
| `tests` | `tests/support`, `tests/performance` | libraries and app-libraries |
| `vendored` | `external/time-filter` | nothing |

The edges are read from the tree: every `#include` of every C and C++ file, resolved as the compiler would
(relative to the including file, then by the spelling a header is reached by), and every
`target_link_libraries()`, `add_library()` and `iclforge_add_library()` (its `DEPENDS`, `EMBEDS` and `LINK_*`)
of every CMake file, a target found again under its alias. An `internal` project (`audio`, `sendspin`,
`app-media`, `app-theme`, `app-preferences`) is never installed: no installed header (one under a library's
`include/`) includes one, and `cmake/InstallLibrary.cmake` names none. The check fails a use the row does not
list, a C, C++ or CMake file in a tree the table covers that no project holds, a row that breaks the rules of
its kind or names a project that is not there or makes a cycle, an exception or a known debt that excuses
nothing any more, and an installed header or an `install()` that reaches an internal project. The debts
(`layering_debt/`, one file per cut) are the mechanism the libraries had; none is listed.

**What the dry run could not see** (each found by running the check on the tree, or the checks beside it):

1. **There are five whole-tree exceptions, where the study expected one.** `ac4` to `app-media`
   (`libs/ac4/tests/decoder/test_object_render.cpp`, finding 4 of C7-2: a library's test compiling an
   app-library); `ac3` to `esp-idf` (`libs/ac3/tests/`: the component's host-portable headers, `interleave.hpp`,
   `block_ring.hpp`, `sink_plan.hpp` and their kin, are tested on the host in ac3's test binary); `hearth` to
   `esp-idf` (`apps/hearth/engine/`: the engine speaks to a sink about its firmware with `firmware_image.hpp` and
   `firmware_status.hpp`, so that the board and the server agree by construction); `hearth-sink` to `esp-idf`
   (the sink is Hearth's firmware and the component is its dependency, `override_path`); `esp-idf` to
   `baremetal` (the `i2s_player` example decodes the fixture the bare-metal probe carries rather than a copy).
   They excuse 45 edges. Each has its reason in the table, and each is a cut a later change can make: the
   check then fails until the row is deleted.
2. **Consumers are a rule of their own.** A library's tests and fuzz targets use any library besides what its
   row lists, and the test support, never an app: `libs/ac3/tests` includes `containers` (its MP4, fMP4 and
   MPEG-TS tests), and without the rule every library would list what its tests need and the table would
   say nothing about the library. The top-level `tests/` is a project of its own, the support library and
   the performance tests, and uses `ac3`, `ac4` and `dsp`.
3. **The libraries' own 228 edges are 228 here.** Counting the include edges between two libraries outside
   consumers gives the number the old check printed; the other 1,721 edges of the 1,949 are the new ones
   (1,811 includes and 138 link lines in all): the programs' uses of libraries, the bindings', the
   firmware's, the examples' and the consumers'.
4. **A violation put into the tree fails the check.** On the real table, one at a time:
   a library including a library its row lacks, a program including another program's header, a library
   including an app-library, an installed header including an internal library, a library linking a library
   its row lacks or a program's target, an app-library including a library its row lacks, C++ in a tree no
   project holds, an exception whose edge is gone, and an `install()` naming an internal library's target or
   directory (eleven cases; each exits 1 with the line that names what is wrong, the clean tree exits 0).
5. **The checks beside it found what still read the old table.** `tools/n1b/check_pages.py`, one of the
   checks every stage runs, opened `layering.json` and failed on the first run, and so did a test of
   `test_layoutdef.py`; `violations.py` and the README of `tools/n1b` named it too. They read `projects.json`
   now (the libraries are the projects of the kind `library`). The history of the consolidation
   (`planning/consolidation.md`, `planning/layout*.md`) keeps naming `layering.json`, as it should: it is what
   the table was.

**Found, and not C7's.** The shared Debug tree still fails to link `iclforge-iab-tests`, `fuzz_iec61937_unwrap`
does not compile (since C3), and the fixed-point AC-4 probe exits 1 in both trees, over its stack ceiling.

**The proof,** on this machine (WSL2 on Windows 11, GCC 16 and Clang 22, Qt 6.10; the parent is
`chore/monorepo-c7-4` at `2a41e3223`, whose code is `3f0e11b67`'s, built in `build/wt/c74`; the stage's tree in
`build/wt/c75` at `f28611215`; the commits after it are a docstring's wrapping, the `tools/n1b` readers of the
table and this record):

| proof | result |
|---|---|
| builds, `-Werror`, every default target, the GUI on and off | GCC 16 and Clang 22 clean in both; the shared Debug tree has the one failure it had before (`iclforge-iab-tests` does not link) |
| the whole ctest, GCC and Clang (GUI on) | 3,582 of 3,582 pass in each; 3,580 test cases in the JUnit files, 3,575 pass and 5 skip in each, and the outcome of each, by name, is identical to the parent's |
| ctest's names | identical on the five trees: 3,582 (GUI on, GCC and Clang), 3,546 (GUI off), 3,189 (shared Debug) |
| pinned bitstream hashes, CLI corpus (44 commands), exported names of the shared libraries, installed tree, public headers | identical, including `pins_blob` and what every command prints |
| `.text` of the installed binaries (`git describe` pinned, both compilers) | all 13 ELF files have identical `.text` (5 byte-identical) and so has every member of the 9 static archives (171) |
| flags of every unit (`flags_diff.py`) and of every archive and link step (`--links`) | all 913 units (GCC and Clang) and 803 (shared) pair, and 88, 72 and 68 link steps; none differs in any flag |
| IR of every unit (tests included, Clang 22, two source trees whose paths are as long as each other) | 798 units: 795 identical and 3 differing, in the strings that hold `git describe` (the commit count and the two hashes, in `libs/base/src/version.cpp` and in two variants of Hearth's network controller); the new tree is `622720b80`'s |
| C and C++ edits since the parent (`c7_pathonly.py`) | 1 file, `libs/dsp/src/tiered/real_functions.hpp`, which differs in a comment (it names `projects.json`) |
| bare-metal probes (QEMU, `--icount`) | five variants: the probes' own key=value lines identical; the fixed-point AC-4 probe exits 1 in both trees, over its stack ceiling, as before |
| libFuzzer harnesses (Clang, `ICLFORGE_BUILD_FUZZERS`) | the same 20 of 21 build and the same one does not (`fuzz_iec61937_unwrap`, since C3); 18 replay their inputs and exit 0 in both; two have none |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` | 30 projects, 1,949 edges between them (1,811 includes, 138 link lines), 45 excused by 5 exceptions, 0 known debts, 0 failures; 172 public headers, 0 failures, 4 known debts; 0 problems; 0 missing of 6,508 checked |
| the check on the real table with a violation put in | eleven cases (finding 4): each exits 1 with the expected message; the clean tree exits 0 |
| the static job's other checks and the unit tests of `tools/` | `check_corpus.py`, `check_packaging_versions.sh`, `check_platform_matrix.py`, `check_esp_efuse_free.py`, `check_android_jni.py`, `generate_support_matrices.py --check` and the AC-4 table generators pass; `tools/checks` 445, `tools/ci` 586, `tools/hearth` 102, `tools/n1b` 495 tests pass |
| `precheck.py --unit`, `mkdocs build --strict`, `ruff check .` | pass; ruff finds what it found in the parent (29 findings in each) |
| the CI planners | the same answers for 3,228 files and for the last 60 commits |

No file the images, extensions or packages read is among the stage's: its changes are the table and the tools
that read it, the pages that describe them, the static job's step and two comments.

**Not run here, and recorded rather than skipped:** MSVC `/W4 /WX` and clang-cl; macOS; the Android build;
the Windows and macOS halves of the notices; the ESP-IDF images and the boards, the Python, Rust, npm and
ESPHome suites and the packages (nothing they read changed); SonarCloud and CodeQL; the workflows themselves,
the static job's included (its steps ran one by one).

**What C7-5 leaves, and with it C7.** Three things, each by its decision and none the user's to take again:
the planner reads `projects.json` instead of prefixes (decision 12: done in C7-6); the scripts of `tools/n1b/` are
retired once the user has done with them (decision 13: `adapt_branch.ps1` and the move maps stay); and
`.git-blame-ignore-revs`, which the consolidation stages did not extend either, has no entry for the move
commits of C7 (`43ecd2401`, `9aa9bfdfb`, `94c3bd8d6`, `030af657f` and `8a0d74baf`). The five exceptions are
the table's own debts.

### C7-6, 2026-10-09 (`chore/monorepo-c7-6`)

**What this is.** Decision 12 (a): the CI planners leave directory prefixes for the project graph, as a
stage of their own with its own proof. No path moves and nothing the build reads changes: the stage is
`tools/`, the gate's sparse checkout, three pages and this record.

**What changed.**

1. `tools/checks/project_graph.py` is the table, once. The classes and the loader that were in
   `check_layering.py` live there, and the planners read the table through them. It adds what the planners
   ask: the project of a path (the longest `path` above it), the projects an excused edge reaches a file
   from (`reached_from`), every project that uses one, transitively (`dependents`), and the projects a
   change reaches (`affected`). `python3 tools/checks/project_graph.py affected <path>...` prints them.
2. `projects.json` says what the planners need to know of a row. A top-level `lanes` lists the CI lanes;
   each project names the lanes its tree lights (`["core"]` for a library, the three desktop platforms
   for a program and for `apps/shared/`, `["wasm", "npm"]` for the npm package, and so on); the ESP-IDF
   component names the libraries it `ships` as source; and an exception names, in `to_paths`, the files
   its excused edge reaches (the 15 headers of the component that `libs/ac3/tests` includes, the two
   that Hearth's engine includes, the media code the AC-4 tests compile, the bare-metal fixture).
3. `check_layering.py` holds those to the tree: a project that lights no lane, a lane the list lacks, a
   `ships` that is not a library of a firmware project, a `to_paths` outside its target or with no file
   under it, an excused include whose file is not named, and an entry that no excused include names any
   more all fail. Its tests are 40, the graph's 13.
4. `classify_changes.py` and `plan_gate.py` build their tables from the rows. What is left in them is what
   no project holds (the products' notices and packaging, the tools, the build files, the golden data) and
   the refinements inside a project (the Catch2 tests among a program's files, Crucible's Windows driver
   and Linux tray VM, the Python examples, Forge's CLI against its GUI). The projects the Linux gate does
   not build are the ones whose rows name neither `core` nor `linux`; the comparisons are the kinds
   `library` and `vendored`.
5. `tools/ci/compare_planners.py` replays every tracked file and the last 150 merged pull requests through
   two versions of the planners and says in which direction they differ (it is a permanent tool: the next
   change to a planner is held to its predecessor the same way).
6. `pr-gate.yml`'s `plan` job checks out `plan_gate.py` alone; it checks out the module and the table with it.

**What the dry run could not see** (each found by the comparison, a test or running the gate's checkout):

1. **The gate had a false skip.** `libs/ac3/tests` includes 15 headers of the ESP-IDF component and Hearth's
   engine two, which is why the table has those exceptions; and the gate planned the component's whole
   tree as "nothing a Linux C++ build reads". A change to `block_ring.hpp` or `firmware_image.hpp` built
   nothing in a pull request, though the tests that include it are built there. They build now (and, for the
   two Hearth includes, with Qt), and so do the lanes of ac3 and Hearth after a merge. Three of the last 199
   pull requests touched one of these files.
2. **`examples/` was an unknown tree.** Only `examples/python/` was named, so a change to a C++ example lit
   every lane, `ci_self` and `docs` among them. It is a project now, core's, and the 34 cases of the
   comparison that are narrower for it are all under `examples/`.
3. **The first `to_paths` named directories, and over-lit.** `firmware/esp-idf/iclforge/include/` holds
   headers nothing on the host includes. The entries are files now, and the check that fails when an
   excused include is not named (and when a name has no include) keeps the list true.
4. **The gate does not check the repository out.** It sparse-checks one file, and the planner now reads
   a module and a table. The three files are named in `pr-gate.yml`, and the planner runs from a directory
   that holds only them.
5. **What the graph can say is less than it looks.** Of the last 199 pull requests, 72 touch no code. Of the
   other 127, 29 reach fewer than 6 of the 30 projects and 55 fewer than 21; 67 reach all of them, through
   the codec-blind libraries, `cmake/`, the golden data or the test support. Choosing the tests of the
   affected projects (`ctest -L <project>`, as the study imagined) would save the gate most of its work for
   a quarter of the pull requests that change code and nothing for half of them. It is not part of this
   stage.

**The proof,** against the stage's parent (`chore/monorepo-c7-5`, built in `build/wt/c75`; the stage changes
no file the build reads, so the builds, the ctest, the hashes and the `.text` of C7-5 are this stage's too):

| proof | result |
|---|---|
| the planners old and new (`compare_planners.py`, every tracked file and the last 199 merged pull requests) | 3,229 one-file changes and 199 pull requests: 3,374 answer the same; 20 are supersets (the 15 headers, the two files of the media code the AC-4 tests compile, three pull requests); 34 are narrower, all under `examples/` (finding 2); none is narrower otherwise |
| the planners' own tests | the classifier's 44 and the gate's 41 existing tests pass unchanged, and so do the 12 new ones (findings 1 and 2, and the table); `tools/ci` is 598 tests |
| the gate from its sparse checkout | the planner runs from a directory holding `plan_gate.py`, `project_graph.py` and `projects.json` alone, and answers |
| `check_layering.py` | 30 projects, 1,949 edges, 45 excused by 5 exceptions, 0 failures; 40 tests; `tools/checks` 465 tests pass |
| the static job's other checks | `check_namespaces.py`, `check_pages.py`, `check_doc_paths.py` (6,502 checked), `check_packaging_versions.sh` and the unit tests of `tools/hearth` (102) and `tools/n1b` (495) pass; `precheck.py --unit`, `mkdocs build --strict` and `ruff check .` pass, ruff finding what it found in the parent (29) |

**Not run here, and recorded rather than skipped:** the workflows themselves. `pr-gate.yml`'s sparse
checkout is read and its planner run from the same three files, not run by GitHub: a dispatch of
`pr-gate.yml` on the branch is the first run.

**What C7-6 leaves.** The graph does not yet choose what to build and test: the planners light the same
lanes as before, plus the files the table says an excused edge reaches. Choosing by the affected projects
(`ctest -L`), or lighting the satellites that use a changed library in the run after a merge, is a change to
what CI costs and not to how it is organised (finding 5 has the numbers). `layering.json` is gone from every
tool that read it; the history keeps naming it.

### C7-7, 2026-10-09 (`chore/monorepo-c7-7`)

**What this is.** Decision 13 (a): `tools/n1b/` and the consolidation scripts are retired now that C7 is
proved, keeping `adapt_branch.ps1` and the move maps for the branches that are still open; and the
mechanical commits of the stages go into `.git-blame-ignore-revs`. Nothing the build reads changes (two C++
comments name a script that is gone).

**What changed.**

1. **The moves, alone:** `adapt_branch.ps1`, the seven modules it needs and their four tests go from
   `tools/n1b/` to `tools/adapt/`, 12 renames, every one `R100`.
2. **The retirement:** the rest of `tools/n1b/` is deleted, 82 files and 32,117 lines: the N1B, consolidation
   and C7 scripts, their 15 test files, the baselines, the README and the S2 patch. They are in the history,
   last at `8d2507bae`.
3. **What the survivors need:** `tools/adapt/README.md` says what is there and how to adapt a branch;
   `moves.py <commit>` prints a stage's move map back from the commit of its renames (the commits are in the
   README, with their rename counts) and has 4 tests, so the maps are data in git and not a second copy
   of it; the static job and `precheck.py` run `tools/adapt`'s tests where they ran `tools/n1b`'s.
4. **The names of the scripts that were left in the tree:** `.gitattributes` (the S2 patch's line), the
   documentation-paths check (`tools/adapt/` is its tree of old-layout literals now), three pages and two
   comments that named `n1d_driver_names.py`, and five links of the planning pages that went to
   `tools/n1b/README.md` (they go to the commit that has it).
5. **`.git-blame-ignore-revs`** lists 53 more commits: what the scripts of C1 to C7-4 gave on their parents.

**What the dry run could not see:**

1. **Keeping `adapt_branch.ps1` is keeping seven modules.** It runs `n1b_apply.py`, `n1b_cmake.py`,
   `n1b_paths.py` and `n1b_names.py`, and those import `include_graph.py`, `layoutdef.py` (S2's move map) and
   `n1b_lib.py`: 75 KB of the 958 KB of Python in the directory. The other 55 modules and 15 test files go.
   `adapt_branch.ps1` itself adapts a branch to stages S2 and S3 only, as its header says; for the stages
   after them the README says to merge, with directory renames on, and to read the move map of the commit.
2. **The ignore file does not list renames,** by its own header: git blame follows a rename by itself. The
   move commits of C7 are not missing from it, as the summary of C7-5 said; the commits that rewrite
   text are, and they are the 53. Entries for commits that are not in a repository's history are
   ignored by `git blame`, so the file is safe to carry before the commits are where it says.
3. **A suite whose directory is gone is a failure, not a skip.** The static job and `precheck.py` loop over
   directories and `unittest discover` stops on one that does not exist; the documentation-paths check
   would have read `tools/adapt/`'s old-layout literals as stale paths without its entry.
4. **What the retirement costs.** `baseline.py`, `flags_diff.py`, `ir_compare.py` and `cli_bytes.py` were not
   only the layout scripts' tests: they are the proof that a change moved no output byte, no flag and no
   instruction, and every stage of C7 was proved with them. They are in the history and the next refactoring
   that has to be proved the same way takes them from there; `tools/ci/compare_planners.py` is the planner
   one's permanent successor (C7-6).
5. **Ruff.** The 27 findings of the tree were all in the retired directory; two are left, outside it.

**The proof,** against the stage's parent (`chore/monorepo-c7-6`; no file the build reads changed but two C++
comments, so the builds, the ctest, the hashes and the `.text` of C7-5 are this stage's too):

| proof | result |
|---|---|
| the suites | `tools/checks` 465, `tools/ci` 598, `tools/hearth` 102 and `tools/adapt` 85 tests pass (`tools/n1b`'s 495 went with the scripts: 81 of the 85 are the survivors', the 4 new are `moves.py`'s) |
| the move maps | `moves.py` reads each of the 17 commits of the README as renames alone, and fails on a commit that is not |
| the C++ edits | two files, `apps/crucible/engine/src/virtual_device.hpp` and `apps/shared/preferences/src/settings_migration.hpp`, 4 lines each, all comments |
| `.git-blame-ignore-revs` | 53 commits, each an ancestor of the branch and none a commit of renames alone (the script that wrote the list checks both); `git blame --ignore-revs-file` runs with it |
| `check_doc_paths.py`, `mkdocs build --strict`, `precheck.py --unit` | 0 missing of 6,496; pass; pass |
| `ruff check .` | 29 findings in the parent, 2 now, none new |
| `check_layering.py` and the other checks of the static job | unchanged: 30 projects, 1,949 edges, 0 failures |

**Not run here, and recorded rather than skipped:** the workflows (the static job's loop is read and its
suites run one by one).

**What C7-7 leaves.** The decisions are all carried out. The branches that are still open (`git branch -r
--no-merged`) are adapted by their owners with the README of `tools/adapt/`, and the part of the programme
that is not this one's is in the pull requests: `.git-blame-ignore-revs` names commits, which stay where they
are only if the stages merge with merge commits, as the queue does.

### After C7-7, 2026-10-09 (`chore/monorepo-c7-7`, with the branches merged into it)

**What this is.** C7-1 to C7-7 were proved on one machine: GCC 16 and Clang 22 on Linux, the bare-metal
probes under QEMU, six ESP-IDF images, and one MSVC build. The branches were pushed on 2026-10-09 and GitHub's
own gates ran on them for the first time: `pr-gate.yml`, then `ci.yml` (the legs of the platforms that
differ, then once in full), `wheels.yml`, `esp-component.yml` and `npm.yml`, each dispatched on
`chore/monorepo-c7-7`. They found what a Linux machine could not, and the defects of the nightly run on `main`
that the stack had not already fixed. Each is a branch off `chore/monorepo-c7-7`, merged back into it with a
merge commit, so that the history says what found what.

| found by | the defect | the branch |
|---|---|---|
| the static job (ruff, shellcheck) | two comments past 100 columns that the longer paths of C7 had made; `tools/fuzz/harnesses.sh`, sourced and with no shell named, was in `git ls-files '*.sh'` once the fuzz scripts moved under `tools/` | `bugfix/lint-after-the-moves` |
| the gate's Linux job | "Check translations are up to date": the `<location line=...>` of the Forge GUI's and Hearth's catalogues, nine lines off after the path pass wrapped `encoder_controller.cpp` | `bugfix/forge-gui-catalogue-locations` |
| the gate's Windows job (MSVC) | C4275, an exported `ac3::LevelMeter` and `LoudnessMeter` deriving from `base`'s, which C6 moved and a shared library embeds | `bugfix/msvc-c4275-exported-classes-derived-from-base` |
| the fuzz build | `fuzz_iec61937_unwrap` named no library since C3 moved IEC 61937 to `containers` | `bugfix/fuzz-iec61937-unwrap-links-containers` |
| the shared Debug build | `iab::DlcAudio::normalized()` defined in the library and exported by nothing; its ABI allowlist | `bugfix/iab-dlc-audio-normalized-export` |
| main's nightly (ESP32-S3, QEMU) | the AC-4 probe image, 0x107330 bytes, in the 1 MB app partition of the default table | `bugfix/esp32s3-ac4-probe-app-partition` |
| main's nightly (Python wheels, macOS Intel) | `-Wshift-count-negative` in a discarded `if constexpr` branch of `drc.cpp` | `bugfix/ac4-drc-negative-shift-in-a-discarded-branch` |
| main's nightly (WASM), then this branch's macOS and clang-cl legs | `-Wunused-template` on the fixed-point table builders and `time_to_output`; `-Wsign-conversion` on `kTable42[std::distance(...)]` | `bugfix/unused-templates-and-a-signed-table-index` |
| main's nightly and this branch's Windows wheel | C4244 for `std::optional<std::int8_t> = -20` in `iab/mxf.hpp` | `bugfix/iab-alignment-level-is-an-int8` |
| reading the workflows | `wheels.yml` ignored `libs/ac4`, `libs/containers` and `cmake/`, which the wheel is built from; and a filter that matches nothing is silent | `bugfix/workflow-path-filters-follow-the-graph` (`check_workflow_paths.py`) |
| reading the ESPHome page | its examples named `version: v0.10.0-beta.1`, a tag with the component at `esp-idf/iclforge` | `bugfix/esphome-example-names-a-ref-with-the-new-layout` |
| the full `ci.yml` run (WASM) | Emscripten's `size_t` is 32 bits: the IAMF writer reserved a 64-bit total in a vector (`-Wshorten-64-to-32`) and the Matroska writer compared a frame size with `1 << 40`, twice (`-Wtautological-constant-out-of-range-compare`) | `bugfix/iamf-container-reserve-on-a-32-bit-size-type` |
| the full run (Windows MSVC arm64) | the gold-reference ffmpeg was pinned to a daily autobuild tag, and BtbN keeps ten or so daily tags: the download was a 404; the pin is a month-end tag now, which they keep | `bugfix/winarm64-ffmpeg-pin-a-tag-that-is-kept` |
| the full run (ABI gate) | C7-1's own: `find_so` ended with `[ -d ... ] && find ...`, so for a build with no `src/` it returned 1 and the step, under `bash -e`, ended at the first assignment from it with no output and no finding | `bugfix/abi-gate-find-so-survives-a-missing-directory` (with `tools/ci/test_abi_find_so.py`) |
| the full run (Linux GCC, the float32 suite) | 43 cases named programs the directory did not build (28 `example.*`, 15 `hearth_qml_tests_*`): `ctest` says Not Run, and the step failed; the same 43 failed on main's nightly | `bugfix/float32-suite-leaves-out-unbuilt-example-and-qml-tests` |
| the full run (Linux LLVM, the install-consumer check) | C7-4's: `install_consumer/CMakeLists.txt` built the stream path with `cmake_path(APPEND ... tests golden ...)` from separate words, which no path rewrite reads | `bugfix/install-consumer-reads-the-stream-from-testdata` |
| the same check, run on the LLVM tree | a program linked with `pkg-config --libs iclforge-ac4` found `libiclforge_ac4` and not `libiclforge_base` it needs: its run path is `DT_RUNPATH`, which is not searched for a library's dependencies, and the libraries had none; they have `$ORIGIN` now | `bugfix/installed-shared-libraries-find-each-other` |
| reading `git ls-files -s` | three Python tools and one test that C7 added were mode 0755, and nearly all of the other 200 are 0644 | `bugfix/file-modes-of-the-new-tools` |

Two improvements to the structure came with them, as `feature/` branches: `feature/private-headers-are-private`
(`check_layering.py` holds what a project may *include* as well as what it may use: a header is public under the
project's `include/` or a directory its row `exposes`; the tree has one reach into another's implementation,
`libs/dsp/tests/tiered/test_dsp_exact.cpp` into AC-4's SBR generator, listed as a debt) and
`feature/project-pages-from-the-table` (`docs/projects.md` and a README for each of the 24 projects without one,
generated from `projects.json` and checked fresh by the static job).

**Where the CI stands.** The full run (`ci.yml`, 2026-10-09, on `04d2ec986`) had 40 jobs pass, 9 skipped (the
publishing jobs) and 6 fail: WASM, the ABI gate, Windows MSVC arm64, Linux GCC, Linux LLVM, and the aggregate
`Verify Status`. Everything that was red on `main`'s nightly and is not in that list passed on the branch:
Android, the Rust crates on three systems, the five wheels, ESP32-S3 and C3 under QEMU, the coverage job,
both macOS legs, Windows MSVC and clang-cl, the AppImage. The five jobs were fixed by the rows of the table from the full run down
to the run path, and re-run where a dispatch can: Linux GCC, Linux LLVM and Windows MSVC arm64 (`ci.yml -f legs=...`, run
37908516861, on `5887799ce`) all pass. The other two, which a `legs=` dispatch does not run, are checked
by what can be run here, and are the one thing a second full dispatch would settle:

- **WASM:** built here with Emscripten 6.0.6, the version the job pins, on the whole preset (197 steps, `-Werror`).
  The job's Node and browser tests are not affected by a C++ change of this size and were not run.
- **The ABI gate:** the step's own script, taken from the workflow, run against a `libs/` tree and a `src/`
  tree of real shared libraries with the real `abidiff`: it ended with no output before the fix, as in CI, and
  walks the libraries after it. The comparison against `v0.10.0-beta.1` itself is not reproduced.

**Not defects of the stack, and left.** The AC-4 fixed-point probe's stack, 22,712 bytes against a ceiling of
21,500 with this machine's `arm-none-eabi-gcc`, is the same on `main` and passes in CI's job there. Sonar's mixed
source and test scope (`sonar.sources` and `sonar.tests` both name `libs` and `apps`, the tests picked out by
`sonar.test.inclusions`) is the documented way and is read, not run: the first pull request's analysis is its
test. Two things the 32-bit pass and the move of the golden data leave on purpose: `libs/adm/src/adm.cpp:178` and
`:181` narrow a 64-bit count in a vector size and nothing builds ADM for a 32-bit target, and a test case of
`libs/sendspin` and a comment of `testdata/audio/reference_objects.paths` still say `tests/golden`, the first
being a test name, which does not change, and the second a data file whose bytes are read. The code needs no
modernising for C++23: the tree already uses `std::expected` (884 lines), `std::span`, `std::ranges`,
`constexpr`, `[[nodiscard]]`, concepts and `std::jthread`, has no `using namespace std` in a header, and its
`malloc`, `NULL` and `typedef` are the C API, a libxcb reply and a memory probe.

**What the run teaches about the stages.** Two of the six reds were the stack's own and not visible to a
dry run: a shell function that is only wrong under `bash -e` with a layout that only HEAD has, and a path built
from words. A third (the run path) was a mistake no job had been able to reach, found by running the next check
locally once the first was fixed. The rest were `main`'s, or the platforms': a 32-bit `size_t`, a download that
no longer exists, a test registered for a program that is not built. The method that found them is the one to
keep: dispatch the legs that differ, read every red to its first error, fix it on a branch of its own, and run
what can be run here before a second dispatch.

### What was left, and its remediation, 2026-10-09 (`chore/monorepo-c7-7`, with the branches merged into it)

**What this is.** The section above ends with what was left on purpose: five whole-tree exceptions in
`projects.json` and one known layering debt, a 64-to-32-bit narrowing in `libs/adm`, a test name and a
fixture comment that still said `tests/golden`, two `tools/checks` tests that failed on a Windows machine,
and a Sonar scope that nothing had read. All are done, each on a branch of its own merged into
`chore/monorepo-c7-7`. Two were the owner's to decide and were decided: the one exception that remains stays,
and the fixture's hash may change.

| what was left | what is there now | the branch |
|---|---|---|
| the debt: `libs/dsp/tests/tiered/test_dsp_exact.cpp` includes AC-4's SBR generator header, private to `ac4` | the pre-flattening comparison (the verbatim reference, its helper and its two test cases) is `libs/ac4/tests/core/test_aspx_exact.cpp`, beside `test_acpl_exact.cpp`; no test case changes its name or body; dsp's tests put AC-4's private root on their include path only for the sources a shared build compiles in | `feature/aspx-preflattening-test-moves-to-ac4`, `bugfix/dsp-tests-name-ac4-private-root-only-where-needed` |
| exceptions `ac3` and `hearth` to `esp-idf` (28 edges): a library's tests and a program using the ESP-IDF component's host-portable headers | **`libs/device`**, an internal header-only library with the 14 headers that need no ESP-IDF (the interleave and its slot conversion, the block ring, the unit hold, the DAC queue model, the sink planner, the playout model, the firmware image, status and trial rules, Improv, the pairing records, the log ring, the TCP arrival log, the hardware report) under the names they always had, and their 14 tests as `iclforge-device-tests` (`ctest -L device`). The component puts `libs/device` on its own include path (the conversion directory by `CONFIG_SOC_CPU_HAS_FPU`, as before), the packer stages it, Hearth's engine links `iclforge::device`, and the ESP component workflow builds on a change to it | `feature/device-library` (30 renames, every one R100, then the rest) |
| exception `hearth-sink` to `esp-idf`: a firmware project using another | **the kind `firmware-library`**: a component several firmware projects build on. It may use libraries; a firmware project may use libraries and firmware libraries and never another firmware project; nothing else may use a firmware library. `esp-idf` is the first and `hearth-sink` lists it | `feature/firmware-library-kind` |
| exception `esp-idf` to `baremetal`: the `i2s_player` example includes the probes' `fixture.hpp` | `firmware/baremetal/fixture.hpp` is `testdata/baremetal/fixture.hpp`, beside the other generated headers of `testdata/`; the probes of four chips, the hosted build and the example find it there, and the generator writes it there | `feature/baremetal-fixture-in-testdata` |
| `libs/adm/src/adm.cpp:178` and `:181`, `-Wshorten-64-to-32` | the frame count is converted once to the vector's size type, after it is bounded by the file's size; the 32-bit pass over `libs/adm` is clean | `bugfix/adm-frame-counts-as-size-t` |
| the first line of `testdata/audio/reference_objects.paths`, which the generator no longer writes that way | the generator's own output again (`write_paths()` alone, the WAV untouched): one comment line, 714 bytes to 710, and the entry's `sha256` and `bytes` in `corpus.json`. The placements the object-quality series are measured against are the same lines, so `corpus_version` stays 1 (it marks a change of the material, and this is a change of one comment); `check_corpus.py` passes. The file is pinned to LF in `.gitattributes`, because a Windows checkout with `core.autocrlf` wrote it CRLF and failed that check for a file nobody had changed | `bugfix/reference-objects-paths-comment-names-testdata`, `bugfix/hash-pinned-text-fixture-keeps-lf` |
| the one test case whose title carried `tests/golden` | `chunks: a burst chunk for the first syncframe of testdata's AC-3 5.1 fixture`. It is the one ctest name this work changes, and nothing else names it | `bugfix/sendspin-chunk-test-names-testdata` |
| two `tools/checks` tests that failed on Windows (the paths of a dry-run listing, a literal `/usr/bin/ffmpeg`) | `rewrite_roadmap_comments.py` prints its paths with forward slashes; the test compares with `str(Path(...))`. The four suites pass on Windows (484, 602, 102 and 85 tests, one skipped) | `bugfix/tools-tests-pass-on-windows` |
| the Sonar scope, read by nobody | `tools/checks/check_sonar_scope.py` (13 tests, the static job and `precheck.py`): a root that is not in the tree, a pattern that matches no tracked file, a test scanned as source and a test `sonar.exclusions` does not take out of the sources all fail. It found two rules that had silently stopped applying, `e80` (the `cppsecurity:S2083` ignore rule for `wav_stream_writer.cpp`, which is in `libs/base/src` since the consolidation, so its finding would have come back on the next scan) and the copy-paste exclusion `tables*.cpp`, which matched nothing | `feature/sonar-scope-check` |

**Where the table stands.** 31 projects, 1,945 edges (1,806 includes, 139 link lines), **one exception and no
debt** (from five and one), 0 failures. The one is `ac4` to `app-media`: `libs/ac4/tests/decoder/test_object_render.cpp`
compiles the media code's AC-4 object renderer into AC-4's test binary, because the streams it renders are
built by the AC-4 tests' own helper (`decoder/objects.hpp`, which includes the encoder's private headers).
Three ways to remove it were set out: a test that renders committed streams (a generator and fixtures, and
the helper's own coverage of moving objects goes); the renderer as a public part of `libs/ac4` (a new API and
a new set of exports, so the allowlist moves); or leaving the one named, test-only edge. **The owner kept
it**, and it stays with its reason in the table.

**What the remediation could not see, and the proof.**

| proof | result |
|---|---|
| ctest, the LLVM tree | 3,546 cases registered before and after the moves, their names the Catch2 cases' and unchanged but the one renamed; 3,531 run and pass, 0 fail (the other 15 are Hearth's QML suites, which need a display) |
| `precheck.py --unit` | every step passes (branch name, doc and script paths, platform matrix, project layering, workflow path filters, project pages, Sonar scope, library namespaces, ESP efuse-free settings, fixture corpus, support matrices, patch attribution, ruff and the four suites); `platform macros` is skipped without `pwsh` |
| `check_layering.py`, `generate_project_pages.py --check`, `check_workflow_paths.py`, `check_doc_paths.py`, `check_sonar_scope.py`, `mkdocs build --strict`, `ruff check .` | 0 failures, 26 pages current, 0 problems, 0 missing of 6,556 checked, 0 problems, pass, pass |
| unit suites | `tools/checks` 486 (+13 for Sonar), `tools/ci` 602, `tools/adapt` 85 |
| the bare-metal probes (`run_baremetal_probe.sh --host`, then `--icount` under QEMU) | both pass with the fixture in `testdata/`, with the same key=value lines and ceilings |
| ESP-IDF v6.1 images built in the tree before and in the final tree (names as long as each other): the Hearth sink for the S3 (610,592 bytes), for the C6 with 16 MB flash and Sendspin (1,681,120), `i2s_player` (267,088), and the four bare-metal probes: S3 (519,840), C3 (543,200), C6 (584,848), P4 (541,856) | each pair is the same size, and once the two trees' names are made the same, 75 to 79 bytes differ, every one in the app descriptor (the version string, the build time and date, the ELF's SHA-256) or the 33-byte trailer: the code and data are the same. The C6 and C3 are parts without an FPU, so they build `libs/device/conversion/bits`, the S3 and P4 the float one |

**A mistake the proof caught.** The `i2s_player` example reached the moved fixture from five directories up, which
was `firmware/` (where it had been) and not the repository root: `fixture.hpp: No such file or directory`, in the
final tree's build and in no other check. Fixed on the same branch (`139e43582`); the example's image is in the
table above.

**Not run here, and recorded rather than skipped:** SonarCloud itself (the analysis; the check reads only the
file), the shared-library configuration of the dsp tests (the one branch of `libs/dsp/tests/CMakeLists.txt` that was
kept as it was), the QEMU runs of the ESP images (their code and data are the same as the tree before's), and the
MSVC and macOS builds of the new test binary. The pull-request gate, the ESP component workflow (which builds the
packed archive, `libs/device` in it, for four parts) and one build leg per system are dispatched after the push.
