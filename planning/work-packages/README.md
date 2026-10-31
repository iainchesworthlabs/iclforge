# TrueHD work packages

As of 2026-10-30. The TrueHD implementation appraisal (the owner's report of the same date) split the remaining TrueHD work into seven
packages. Each is a brief for one agent session. This page holds what they share: the rules, the build environment, the order, and who
owns which files. Read it, then your package's page, then start.

| Package | Page | One line | Needs from the owner |
|---|---|---|---|
| WP1 | [WP1-atmos-object-layer.md](WP1-atmos-object-layer.md) | TrueHD Atmos: the bed, the objects, writing them, to the standard `libs/ac3`'s E-AC-3 JOC work meets | Disc probes; one scope decision |
| WP2 | [WP2-hearth-network-and-esp32.md](WP2-hearth-network-and-esp32.md) | Hearth network output and the ESP32 sinks carry TrueHD | A P4 board for tests; a receiver for MAT |
| WP3 | [WP3-forge-cli-and-gui.md](WP3-forge-cli-and-gui.md) | `forge` and `forge-gui` reach DD+ parity for TrueHD | None to start |
| WP4 | [WP4-crucible.md](WP4-crucible.md) | Crucible taps, promotes and shows TrueHD like DD+ | A receiver for the hardware legs |
| WP5 | [WP5-codec-quality-and-performance.md](WP5-codec-quality-and-performance.md) | The encoder and decoder measured against DEE and FFmpeg, and the tests and documents that show it | None to start |
| WP6 | [WP6-validation-and-provisional-marks.md](WP6-validation-and-provisional-marks.md) | Close the provisional marks: the DEE rescore, disc probes, spec follow-ups | The owner's PC; time-critical until 2026-11-04 |
| WP7 | [WP7-library-surfaces-and-ci.md](WP7-library-surfaces-and-ci.md) | Bindings, the Qt coverage lane, the remaining CI lanes | None to start |

## Order

WP6 is first because DEE's licence ends on 2026-11-06 and the last day to start a new DEE leg is 2026-11-04; it is the owner's time more than an
agent's. WP7's Qt lane comes next for the three that change a window (WP2, WP3, WP4), since each needs the Qt build and a coverage floor.
WP1 starts with the bed (no object metadata needed). WP5 and WP7's bindings are independent and can run at once with anything.

WP2, WP3 and WP4 are separate sessions on purpose: Hearth with the ESP32 sinks, Forge (command line and window), and Crucible are different
programs with different owners of risk. They share only `apps/shared/media` (see Ownership).

## Rules that apply to every package

These are the owner's standing rules, from CONTRIBUTING.md and from the sessions that built the codec. They are not optional.

1. **The specifications are not in the repository.** The owner's TrueHD Reference Specification (v3.9 at the time of writing) and AC-4 Content
   Signing Specification (v2.6) are supplied by the owner in the session. Cite them as "TH spec §n" and "CS spec §n". Ask the owner for the current
   files before you start. Their provenance (analysis of DEE and of FFmpeg, per the owner) is the owner's to document; do not edit the clean-room
   text in CONTRIBUTING.md or the ROADMAP's out-of-scope line.
2. **Wire facts only.** Do not copy RVAs, in-memory offsets, Ghidra descriptors, DEE error strings or Dolby-internal function names from a
   specification into code, comments or documents.
3. **Oracles.** FFmpeg's decoded output is a fine oracle; do not read FFmpeg's source, the `truehd` Rust crate, or any third-party TrueHD or Atmos
   implementation, or the old branch `origin/feature/truehd-atmos-support`.
4. **Dolby streams.** Do not analyse a Dolby- or DEE-produced stream to learn format facts. DEE's streams are test inputs: decode them and compare
   with the source. The owner's own Blu-ray discs may settle format questions, run by the owner on the owner's machine with the repository's tools
   (`forge probe DISC detail=structure|evolution|format`); the owner pastes the output back. Never commit disc or DEE content beyond the 13 small
   fixtures already in `testdata/truehd-dee`.
5. **Ask, do not settle.** Where a specification is silent or contradicts itself, or a design choice is the owner's, write a numbered question with a
   recommendation in the right ERRATA.md or planning page, build the conservative reading, and mark it provisional. Do not decide it quietly.
6. **Code.** Tests that use real audio; tests that fail when the check they cover is removed (mutation-proof: revert the line and see it fail);
   `std::expected`; fmt; `-Wsign-conversion` clean; no preprocessor conditionals in `libs/`; one ERRATA.md entry per spec gap.
   User-facing text is plain and professional.
7. **Git.** Branch `feature/<kebab-name>` (or `bugfix/`, `docs/`, `chore/`), trunk-based, small commits in the repository's voice. Commit messages carry
   no AI or vendor mention and no session trailer; the author is the owner. Do not push to another package's branch. Do not open a pull request unless
   asked. Run `python3 tools/ci/precheck.py --base <merge-base> --unit` before you push; it must pass except for the branch-name check if you use a
   scratch branch.
8. **Report.** End with at most 450 words: commits, what you ran (counts and exit codes), what you found and left alone, numbered questions with a
   recommendation each.

## Numbers to avoid colliding on

`libs/truehd/ERRATA.md` numbers its open questions in one series (56 at the time of writing) and its entries in another (50). Several sessions will
add to both. Each package has a block; start at the first number of yours, and rebase before you push.

| Package | Open questions | Entries |
|---|---|---|
| WP1 | 60 to 79 | 60 to 79 |
| WP2, WP3, WP4 | the program's own planning page; libs/truehd/ERRATA.md only for a codec fact | 80 to 89 |
| WP5 | 90 to 99 | 90 to 99 |
| WP6 | edits existing numbers; new ones from 100 | 100 to 109 |
| WP7 | 110 to 119 | 110 to 119 |

CHANGELOG.md and ROADMAP.md will conflict between sessions. Add your bullet under Unreleased, one bullet per change, and resolve by keeping both.

## Ownership of shared files

| Area | Owner | Others |
|---|---|---|
| `libs/truehd` decoder core | WP1 (substream 3), WP5 (performance, encoder) | Ask before changing a shared kernel; the other package rebases |
| `apps/shared/media` (including a streaming demuxer for large files) | WP2 builds it | WP3 and WP4 use it; ask WP2's session or the owner before changing its interface |
| `tools/checks/projects.json` (layering edges) | the package that adds the edge | One line each |
| Qt build and coverage lane | WP7 | WP2, WP3 and WP4 add their floors in the lane WP7 builds |
| `tools/checks/score_truehd_decode.py` and the DEE fixtures | WP6 | WP5 reads them |

## The build environment

A cloud session starts with GCC 13 on Ubuntu 24.04, FFmpeg 6.1.1 and Python 3.13, and without Qt. What worked on 2026-10-30:

**Qt 6.12.0** (the version CI pins, in `.github/toolchain-versions.json`), with aqtinstall at the pinned commit, into a directory outside the repository:

```sh
python3 -m venv aqt-venv
aqt-venv/bin/pip install "git+https://github.com/miurahr/aqtinstall.git@076e1659807d0b362a3ed684d54c2e9c775eb9c7" "py7zr>=1.1.3"
aqt-venv/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O kit -m qttasktree qtquick3d qtshadertools
sudo apt-get install -y libegl1 libgl1 libxkbcommon0 libfontconfig1 libdbus-1-3 libglib2.0-0 libgl-dev libegl-dev libxkbcommon-dev \
    libasound2-dev libpipewire-0.3-dev libspa-0.2-dev libflac-dev libopus-dev
```

Without `qtshadertools`, Qt Quick 3D is found but unusable, CMake drops the Qt and font sections from Crucible's notices and one Crucible test fails
for that reason. After installing a missing module, clear the stale cache entries (`cmake . -UQt6Quick3D* -UQt6ShaderTools*`).

**Configure** for the three windows (forge-gui, Crucible, Hearth):

```sh
cmake -S . -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DICLFORGE_QT_ROOT=$PWD/kit/6.12.0/gcc_64 \
  -DICLFORGE_BUILD_GUI=ON -DICLFORGE_BUILD_CRUCIBLE=ON -DICLFORGE_BUILD_HEARTH=ON \
  -DICLFORGE_WITH_ALSA=OFF -DICLFORGE_WITH_PIPEWIRE=ON \
  "-DCMAKE_CXX_FLAGS=-Wno-error=old-style-cast -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized -Wno-error=null-dereference"
```

`-Wno-error=old-style-cast` is only for Ubuntu 24.04's PipeWire 1.0.5 headers, whose macros cast with a C cast. Run the tests with `QT_QPA_PLATFORM=offscreen`
and `PIPEWIRE_DEBUG=0` (the container has no PipeWire client config, whose warnings otherwise break the 26 forge tests that compare stderr exactly).
`forge-gui --smoke` needs its arguments: `forge-gui --smoke libs/base/fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav out.ac3 out.png`.

**Hearth's network side** needs mbedTLS 3.6 (the PSA Crypto API), cpp-httplib 0.56.0 with `cmake/vcpkg/ports/cpp-httplib/wait-inside-a-frame.patch`,
and `mdns.h`. vcpkg builds them from GitHub source archives, which the sandbox proxy refuses (archive URLs return 403; release assets and
`raw.githubusercontent.com` work). The route that worked: the mbedTLS 3.6.3 release tarball built by hand into a private prefix, `httplib.h` v0.56.0 and
`mdns.h` from raw GitHub, small CMake package files for httplib, FLAC and Opus over apt's libraries, a no-op vcpkg toolchain file, then
`-DCMAKE_PREFIX_PATH=<prefix> -DICLFORGE_MDNS_INCLUDE_DIR=<prefix>/include -DVCPKG_INSTALLED_DIR=<dir with share/<port>/copyright and vcpkg.spdx.json for
cpp-httplib, mbedtls, mdns, libflac, opus> -DVCPKG_TARGET_TRIPLET=x64-linux-gcc -DCMAKE_TOOLCHAIN_FILE=<the no-op file>` and `-isystem <prefix>/include` in the
C++ flags. Building a downloaded tarball needs the owner to allow it in the session's permission rules. WP7 turns this into a script in the tree.

If FetchContent cannot reach GitHub, fetch fmt, Catch2, fast_float and nlohmann_json another way and pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=`.

## Definition of done for a package

The package's page lists its own. All of them include: tests that pass on the full `ctest` run of your branch; the CI lanes that apply; the documents
(library page, capabilities table, verification page, the program's own docs, CHANGELOG, ROADMAP) say what is now true and no more; the questions you
raised are in the right ERRATA or planning page with recommendations; and your report says what was not run.
