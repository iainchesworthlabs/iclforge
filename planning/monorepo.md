# A monorepo of self-contained projects (C7): a study

!!! note "Status as of 2026-10-08: decisions 1 to 15 taken; C7-1 run and proved; C7-2 to C7-5 not begun"
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
    ([what the runs found](#what-the-runs-found-that-the-study-did-not)). Nothing is pushed.

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
[tools/n1b/README.md](../tools/n1b/README.md)): one local branch each, `chore/monorepo-c7-<n>`, each
made from the one before. Within a stage the commits come in N1B's order, each script in a commit of
its own before the commit it makes: the moves alone (`git mv`, every rename `R100`), the include
spellings, the build files and the paths in text, then what is done by hand. The moves are data in
`consoldef.py` (`c7_1_new`); `consol_apply.py`, `consol_cmake.py` and `consol_paths.py` run them.

What C7 adds to the proof, and the scripts that give it (all in `tools/n1b`):

| script | what it shows |
|---|---|
| `c7_record.sh` | configures, builds and records the three host trees (GCC 16 and Clang 22, Release; Clang 22 shared, Debug): ctest's names, `baseline.py`'s pinned hashes, CLI corpus, installed tree and exported symbols |
| `c7_run.sh ctest` / `probes` | the whole ctest of both static trees with JUnit output, and the bare-metal probes under QEMU (decoder, encoder, AC-4 in float and fixed point, stage timers), each `--icount`, before and after |
| `flags_diff.py` | the flags each unit compiles with in two configured trees, minutes before a build; it renames a directory only when it moved whole |
| `ir_compare.py` | the LLVM IR of every unit, old tree against new, with the tree's path read alike |
| `c7_pathonly.py` | every change to a C or C++ file since the parent is a comment, an `#include`, layout or a path |
| `c7_planner_equiv.py` | the CI planners choose the same jobs for every tracked file, and for a sample of past commits |
| `c7_leftovers.py` | what still names a root C7 moved: a root built from parts, a regular expression, a glob, a directory a build writes into |

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
