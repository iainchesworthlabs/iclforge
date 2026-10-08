# ---------------------------------------------------------------------------
# iclforge::test_support - what every project's test binary is built with (planning/monorepo.md,
# (e)): the CRT hook and the process seam below, the headers beside them (platform/process.hpp,
# sanitized.hpp, ac4_stream_kinds.hpp, alsa_null_device.hpp) and the definitions that say where
# the golden data is and how the library under test was built. Internal: built for the tests,
# never installed.
#
# An OBJECT library, so that every binary that links it carries both sources: nothing calls the
# CRT hook by name, and a static archive would leave it out.
# ---------------------------------------------------------------------------
add_library(iclforge_test_support OBJECT)
add_library(iclforge::test_support ALIAS iclforge_test_support)
target_include_directories(iclforge_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(iclforge_test_support PRIVATE iclforge::warnings iclforge::coverage)

# ---------------------------------------------------------------------------
# Toolchain hook: keep a failing assert from opening a modal dialog that hangs
# a headless CTest run. Each directory holds the same filenames; CMake selects
# the one for the target, which is why no source file here needs an #ifdef.
# The axis is the CRT, not the OS - <crtdbg.h> ships with the MSVC runtime, so
# clang-cl needs the hook and MinGW does not - so this branches on MSVC (true
# for clang-cl too) rather than on WIN32.
# ---------------------------------------------------------------------------
if(MSVC)
    target_sources(iclforge_test_support PRIVATE crt/msvc/crt_report.cpp)
else()
    target_sources(iclforge_test_support PRIVATE crt/stub/crt_report.cpp)
endif()

# ---------------------------------------------------------------------------
# The test suite's own platform seam - the same shape as the CRT hook above,
# and as libs/audio/src/backend/<backend>/ and apps/forge/cli/src/platform/<os>/.
#
# Two things a test needs that the standard library does not portably give it:
# this process's id (for the per-process scratch leaf every writing test takes)
# and running a command with its real exit code back (std::system() returns the
# child's exit code on Windows but a raw wait() status word on POSIX, and
# cmd.exe needs a quote-wrapping that sh must not get). Both used to be written
# inline behind `#ifdef _WIN32` in nineteen and eight test files respectively.
#
# Each directory holds the same filename; CMake compiles the one for the
# target, so no test source has to ask which platform it is on - which is what
# lets tools/checks/check_platform_macros.ps1 cover tests/ as it already covers
# src/ and apps/. The axis is the OS, not the CRT, so this branches on WIN32
# rather than on MSVC: a MinGW build wants <process.h>'s _getpid() and cmd.exe's
# quoting just as much as an MSVC one does.
# ---------------------------------------------------------------------------
if(WIN32)
    target_sources(iclforge_test_support PRIVATE platform/windows/process.cpp)
else()
    target_sources(iclforge_test_support PRIVATE platform/posix/process.cpp)
endif()

# The golden data the tests read, passed as paths rather than resolved from argv[0]: ctest runs a
# binary from the build tree, which is nowhere near the fixtures.
#
# ICLFORGE_GOLDEN_AUDIO_DIR: test_selfcheck.cpp and test_eac3_selfcheck.cpp run the encoder/decoder
# mirror check (iclforge::ac3::verify) over the committed programme-material fixtures rather than a
# synthetic signal - the delta bit allocation state it compares only ends part-way through a frame
# on material that really does change character mid-frame, which is what made those files the
# reproduction for the bug this check exists to catch, and the Annex E tools it also compares (AHT
# gains, coupling and spectral-extension coordinates) say nothing at all on silence.
#
# ICLFORGE_GOLDEN_OBJECT_DIR: the third-party Dolby fixture libs/ac3/tests/oba/test_dee_joc_fixture.cpp
# reads. ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR: the real DEE-produced AC-4 fixtures
# libs/ac4/tests/core/test_toc.cpp reads (tools/generators/gen_ac4_baseline.py). AC4_GOLDEN_DIR:
# libs/ac4/tests/decoder/test_syntax.cpp holds the AC-4 decoder's syntax trace to the digests
# tools/references/ac4_syntax.py wrote for the same streams.
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _iclforge_golden)
cmake_path(APPEND _iclforge_golden "golden")
target_compile_definitions(iclforge_test_support PUBLIC
    "ICLFORGE_GOLDEN_AUDIO_DIR=\"${_iclforge_golden}/audio\""
    "ICLFORGE_GOLDEN_OBJECT_DIR=\"${_iclforge_golden}/object-fixture\""
    "ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR=\"${_iclforge_golden}/external-baseline\""
    "AC4_GOLDEN_DIR=\"${_iclforge_golden}/ac4\"")
unset(_iclforge_golden)

# The decode scalar the library under test was built with (ICLFORGE_DECODE_SCALAR: double, float or
# fixed), for the few tolerances that are stated in terms of the arithmetic - the float decode
# agrees with the double direct-form transform to float rounding, not to 1e-7.
target_compile_definitions(iclforge_test_support
    PUBLIC "ICLFORGE_TEST_DECODE_SCALAR=\"${ICLFORGE_DECODE_SCALAR}\"")

# Whether the tests run under the sanitizers (cmake/Sanitizers.cmake), for
# sanitized.hpp's kSanitized: there the heaviest tests take fewer frames,
# shorter signals and fewer cases, each still running every code path it
# covers, so that the ASan+UBSan leg, which runs ctest serially in a debug
# build, finishes within its time (.github/workflows/_ci-linux.yml). Defined as
# 0 otherwise, so that no source asks the preprocessor.
if(ICLFORGE_SANITIZERS)
    target_compile_definitions(iclforge_test_support PUBLIC ICLFORGE_TEST_SANITIZED=1)
else()
    target_compile_definitions(iclforge_test_support PUBLIC ICLFORGE_TEST_SANITIZED=0)
endif()

# ICLFORGE_TEST_SCRATCH_DIR, which iclforge_add_test_binary() (cmake/IclforgeTests.cmake) gives each
# binary, is where every test that writes a file writes it. Each such file's own scratch_dir()
# helper appends its own leaf name under here, so the artefacts stay separated by subject the way
# their old per-subject temp directories were.
#
# The point of routing them through a define at all is the ROOT of that path.
# These used to sit under fs::temp_directory_path(), which depends on nothing but
# the machine - so two test binaries built from different checkouts of this
# repo (different worktrees, different build directories, different branches)
# shared one directory and, because the filenames underneath are fixed, could
# overwrite each other's intermediate files mid-test. What that produces is not an
# obvious collision but a test failure in whichever run lost the race, in a branch
# that has nothing to do with the change under test. CI never sees it - a runner
# has its checkout to itself - so the whole cost lands on local development, where
# building several worktrees of this repo at once is normal.
#
# Keying it to the binary's build directory gives each build tree its own, which is
# also the more useful place for it: the artefacts a failing test left behind end
# up next to the binary that wrote them rather than pooled in the machine's temp
# directory with every other build's, and `rm -rf` on a build tree takes them with
# it.
