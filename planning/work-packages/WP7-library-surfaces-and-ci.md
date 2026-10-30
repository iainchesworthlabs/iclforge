# WP7: library surfaces and the remaining CI lanes

Goal: the library surfaces DD+ has and TrueHD lacks (bindings, a stream-level API, packaging) exist, and the CI covers the three Qt applications and the remaining TrueHD gaps. The Qt coverage lane is first, because WP2, WP3 and WP4 each add a
window change and need a build and a floor to put it in.

Read first: [README.md](README.md) (the Qt recipe is the starting point of item 1), [../truehd-parity-library.md](../truehd-parity-library.md) (phases 0 to 8, bindings, packaging), `planning/truehd.md` ("CI: what each lane holds for TrueHD, and what is open"),
`.github/workflows/`, `.github/ci/legs.jsonc`, `tools/checks/coverage_report.sh`, `docs/building.md`.

## Scope, in this order

1. **The Qt coverage lane and the sandbox recipe.**
   - A script in the tree (for example `tools/dev/qt-sandbox-setup.sh`) that does what README.md's "The build environment" does by hand: aqtinstall at the pinned commit with `qttasktree qtquick3d qtshadertools`, the apt packages, and for Hearth the private prefix (mbedTLS 3.6.x built from the release tarball, `httplib.h` with the repo's patch, `mdns.h`, the package files for httplib, FLAC and Opus, the no-op vcpkg toolchain file). It takes a prefix directory and prints the configure line. Test it from a clean directory.
   - A Linux CI leg with Qt 6.12.0 (aqtinstall as the Windows leg does; vcpkg builds the Hearth dependencies there, since CI has network) that builds forge-gui, Crucible and Hearth, runs their tests offscreen, and measures coverage of the C++ in `apps/forge/gui`, `apps/crucible/ui` and `apps/hearth/ui` with floors in `tools/checks/coverage_report.sh`. QML line coverage is not available from gcov; the QML suites stay the QML check. Today the `coverage` preset keeps the GUI off, with the comment that no Linux leg installs Qt, and only the Windows LLVM coverage covers Crucible.
   - Notes from the first runs: Ubuntu 24.04's PipeWire 1.0.5 headers need `-Wno-error=old-style-cast`; without a PipeWire client config 26 forge tests that compare stderr fail unless `PIPEWIRE_DEBUG=0`; a Qt kit without `qtshadertools` makes Quick 3D unusable and one Crucible test fail; `hearth_engine`'s tests include `<httplib.h>` directly, so the include directory must be on the path.
2. **A stream-level API** (audit Co11): scan, timing and seek over a TrueHD or MLP stream, in `libs/truehd` and the C API, so Hearth's index, forge's `cut` and the bindings share one implementation. Hearth has its own index today (`apps/hearth/engine/src/truehd_index.hpp`); coordinate with WP2 and move it when both agree.
3. **Bindings**: Python over the C API first (the wheel builds the library without using it today), then Rust (the crate's build script lists no `libs/truehd` path), then JavaScript/WebAssembly. Each with tests on the committed fixtures and the equality with `forge decode`.
4. **CI housekeeping, each a small change**: clang-tidy on `libs/truehd` and `libs/ac4` (neither is in `static-analysis.yml`; a first nightly with findings); a lane that builds `ICLFORGE_BUILD_TRUEHD=OFF` with tests on; promote the fuzz differential to a failing step after a clean week against FFmpeg 8.0.1; `fuzz_iamf_parse` seeds and its place in `BASE_TARGETS`; a cyclic `ch_assign` stream in `testdata/truehd/handbuilt`; the ABI allowlists for `containers` and `ac4`, which the assembler found still differ from their allowlists (look at why, report, fix the cause); packaging (vcpkg port, Conan recipe offer `truehd`, off by default; confirm); triage the Windows and macOS legs' first TrueHD results when they exist.
5. **Output stage** (audit Me4: apply DRC and downmix on decode, opt-in) stays blocked until the owner answers ERRATA 51 to 55; do not start it.

Out of scope: any codec behaviour change (WP1, WP5), the windows' own features (WP2, WP3, WP4).

## Questions to put to the owner

Add a Linux leg for the Qt applications to the tier-2 set or to nightly only (it adds a long build)? Recommended: nightly first, then tier-2 once the time is known. Should the vcpkg port and Conan recipe stay off by default? Recommended: yes. Add clang-tidy to `libs/truehd` and `libs/ac4` or leave them to CodeQL and Sonar? Recommended: add, nightly.
Wide paths for non-ASCII output on Windows (`std::ofstream` with a narrow path): a CLI-wide decision, not TrueHD's; recommended: ask, do not change in one command.

## Proof required

The lane runs green on a pull request branch and its coverage numbers are in the floors; the script works from a clean directory (run it and paste the tail); bindings tests pass in their lanes; every housekeeping item shown in its own lane. The ERRATA numbers for this package are 110 to 119.
