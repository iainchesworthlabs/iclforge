# ---------------------------------------------------------------------------
# iclforge::base - what every codec and every service builds on and none of them owns: the MSB-first
# bit reader and writer, the speaker vocabulary (Location, Layout), the downmix target names, the CPU
# feature probe, the operator's signing key with SHA-256 and HMAC-SHA-256 (iclforge::base::crypto),
# the scalar building blocks the codecs' arithmetic tiers share (Fixed32, MantExp and the project's
# own float functions), and the build-variant directories the probe, the profiling markers and the
# SIMD kernels are selected through.
#
# It knows no codec. A codec, a container or a renderer links it; it links none of them.
#
# The variant directories are chosen here, by CMake, and put on the include path of everything that
# links this library: a translation unit includes iclforge/base/detail/profiling.hpp and never asks
# what it is being compiled for, which is the rule tools/checks/check_platform_macros.ps1 enforces.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The target architecture, resolved once for everything that keys on it.
# ---------------------------------------------------------------------------
# CMAKE_SYSTEM_PROCESSOR names the machine CMake was configured for, which on
# Apple platforms is NOT the same thing as the machine the compiler is asked to
# emit code for: CMAKE_OSX_ARCHITECTURES overrides it per compile line. An
# Intel Mac cross-building an arm64 wheel (cibuildwheel does exactly this - see
# python/pyproject.toml and .github/workflows/wheels.yml) still reports
# CMAKE_SYSTEM_PROCESSOR=x86_64 while every compile line carries `-arch arm64`,
# so anything keyed on the raw variable - the SIMD directory and the AVX2 tier
# below both are - silently picks the HOST's answer and hands the compiler an
# x86 flag for an ARM target. That is a hard error, not a slow path -
# `clang++: error: unsupported option '-mavx2' for target
# 'x86_64-apple-darwin24.6.0'`, a diagnostic that unhelpfully names the host
# triple rather than the -arch the flag is actually invalid for.
#
# `-arch` therefore wins wherever one is set. More than one of them is a
# universal binary, where the same source is compiled once per slice from a
# single configure: no compile-time SIMD choice can be right for both slices,
# so that case gets its own name and lands on generic below rather than
# guessing at whichever slice happens to be listed first.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES ICLFORGE_OSX_ARCH_COUNT)
    if(ICLFORGE_OSX_ARCH_COUNT GREATER 1)
        set(ICLFORGE_TARGET_PROCESSOR "universal")
    else()
        set(ICLFORGE_TARGET_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    unset(ICLFORGE_OSX_ARCH_COUNT)
else()
    set(ICLFORGE_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(ICLFORGE_TARGET_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(ICLFORGE_BUILD_ARCH "x86_64")
elseif(ICLFORGE_TARGET_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(ICLFORGE_BUILD_ARCH "arm64")
elseif(ICLFORGE_TARGET_PROCESSOR MATCHES "^(x86|i686|X86)$")
    set(ICLFORGE_BUILD_ARCH "x86")
else()
    set(ICLFORGE_BUILD_ARCH "${ICLFORGE_TARGET_PROCESSOR}")
endif()

# ---------------------------------------------------------------------------
# The SIMD arch tree.
#
# The mechanism src/internal/profiling/tracy_{enabled,disabled}/ uses for the
# profiling seam, on the architecture axis: arch/{generic,x86_64,aarch64}/ each
# hold an identically-pathed iclforge/base/detail/simd.hpp, one of them goes on the
# include path, and every `#include "iclforge/base/detail/simd.hpp"` in the
# codec resolves to it. No translation unit ever asks what it is being
# compiled for - the rule tools/checks/check_platform_macros.ps1 enforces -
# and adding an architecture means adding a directory, not editing
# conditionals in the kernels.
#
# The kernels themselves are NOT duplicated per directory. They live once,
# in shared code (iclforge::ac3's src/core/mdct.cpp, bitalloc.cpp and exponents.cpp,
# src/ac4/src/core's src/dsp/qmf_vector.hpp), written against the 128-bit vector types
# the header defines; the directories carry the types and their operations. See
# the generic header for why that is what makes the bit-exactness claim testable.
#
# The directories were src/forge/src/internal/arch/ until src/ac4/src/core's QMF
# banks took vector kernels of their own (planning/ac4.md, D14a, and decision 31's
# "the vector types" in a header-only target both consumers link): iclforge::base_arithmetic below
# carries the include path, so linking it is all a consumer does.
#
# ICLFORGE_SIMD forces a directory. `generic` in particular is what a
# reproducibility comparison wants (is a difference between two machines the
# vector kernels or something else?), and is the reference
# tests/ac3/core/test_simd_kernels.cpp's scalar side is written to match.
# ---------------------------------------------------------------------------
set(ICLFORGE_SIMD "auto" CACHE STRING
    "SIMD kernel directory: auto (from the effective target architecture), generic, x86_64, or aarch64")
set_property(CACHE ICLFORGE_SIMD PROPERTY STRINGS auto generic x86_64 aarch64)

if(ICLFORGE_SIMD STREQUAL "auto")
    # ICLFORGE_BUILD_ARCH is resolved above, from the effective target
    # architecture (CMAKE_SYSTEM_PROCESSOR, or CMAKE_OSX_ARCHITECTURES where a
    # cross-build sets one); reuse it rather than spelling the platform regexes a
    # second time and letting the two drift - and rather than reading
    # CMAKE_SYSTEM_PROCESSOR directly,
    # which is the host's answer on an Apple cross-build and was how an
    # arm64 wheel built on an Intel runner ended up being handed -mavx2.
    # Anything that is not one of the two architectures with a directory -
    # 32-bit x86, WebAssembly (Emscripten reports "x86"), a macOS universal
    # binary, a soft-float embedded target - lands on generic, which is a
    # complete implementation and not a stub.
    if(ICLFORGE_BUILD_ARCH STREQUAL "x86_64")
        set(ICLFORGE_SIMD_RESOLVED "x86_64")
    elseif(ICLFORGE_BUILD_ARCH STREQUAL "arm64")
        set(ICLFORGE_SIMD_RESOLVED "aarch64")
    else()
        set(ICLFORGE_SIMD_RESOLVED "generic")
    endif()
else()
    set(ICLFORGE_SIMD_RESOLVED "${ICLFORGE_SIMD}")
endif()

set(ICLFORGE_ARCH_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/variants/arch-${ICLFORGE_SIMD_RESOLVED}")
if(NOT IS_DIRECTORY "${ICLFORGE_ARCH_INCLUDE_DIR}")
    message(FATAL_ERROR
        "ICLFORGE_SIMD=${ICLFORGE_SIMD} resolved to '${ICLFORGE_SIMD_RESOLVED}', which has no "
        "directory under src/base/variants/. Add "
        "src/base/variants/arch-<name>/iclforge/base/detail/simd.hpp - the arch-generic "
        "copy is the portable implementation and is a working starting point.")
endif()

# The scalar building blocks (iclforge/base/arithmetic/: Fixed32, the Q7.24 fixed-point tier's type,
# MantExp, and the cross-platform-identical float transcendentals) and the SIMD directory, as a
# header-only target: iclforge::ac3, its minimum-footprint archive, src/ac4/src/core and src/dsp
# include them, and the minimum-footprint profile and AC-4 link no iclforge::base, so they take the
# include path from this target rather than from the library. One copy of Fixed32, which decision
# 31 of planning/ac4.md asks for, without AC-4 linking the AC-3 codec to reach it. Not installed and
# not exported, as iclforge::warnings is not: in-tree build plumbing, which the library's install
# leaves out (cmake/InstallLibrary.cmake).
add_library(iclforge_base_arithmetic INTERFACE)
add_library(iclforge::base_arithmetic ALIAS iclforge_base_arithmetic)
target_include_directories(iclforge_base_arithmetic INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${ICLFORGE_ARCH_INCLUDE_DIR}>")
target_compile_features(iclforge_base_arithmetic INTERFACE cxx_std_23)

# tests/CMakeLists.txt reads this so test_simd_kernels.cpp sees the same simd.hpp the library was
# built with, the same white-box arrangement ICLFORGE_AUDIO_BACKEND_DIR has there.
set(ICLFORGE_ARCH_INCLUDE_DIR "${ICLFORGE_ARCH_INCLUDE_DIR}" PARENT_SCOPE)
# For the configure summary at the end of the top-level CMakeLists.txt - which a minimum-footprint
# configure reaches too, so that summary names the directory that build actually compiled rather
# than assuming generic.
set(ICLFORGE_SIMD_RESOLVED "${ICLFORGE_SIMD_RESOLVED}" PARENT_SCOPE)
# For iclforge::ac3's version strings and its AVX2 tier, which key on the same answer.
set(ICLFORGE_BUILD_ARCH "${ICLFORGE_BUILD_ARCH}" PARENT_SCOPE)


# AVX2 is not architectural: a build machine cannot know the deployment machine has it, so it needs a
# runtime answer. The kernels live in src/ac3 (iclforge_ac3_simd_avx2); the probe that answers
# has_avx2() lives here, and one of variants/cpu-probe-{none,msvc,builtin}/ goes on the include path.
# msvc/ is shared by real MSVC and clang-cl: clang-cl reports CMAKE_CXX_COMPILER_ID "Clang" with
# CMAKE_CXX_COMPILER_FRONTEND_VARIANT "MSVC", and __builtin_cpu_supports needs compiler-rt's cpu-model
# support, which the MSVC-CRT-linked clang-cl configuration does not guarantee.
option(ICLFORGE_AVX2
    "Compile an AVX2 SIMD tier (x86_64 only) alongside the baseline SSE2 one, selected at runtime"
    ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(ICLFORGE_IS_MSVC_LIKE TRUE)
else()
    set(ICLFORGE_IS_MSVC_LIKE FALSE)
endif()

if(NOT (ICLFORGE_SIMD_RESOLVED STREQUAL "x86_64" AND ICLFORGE_AVX2))
    set(ICLFORGE_CPU_PROBE_DIR "none")
elseif(ICLFORGE_IS_MSVC_LIKE)
    set(ICLFORGE_CPU_PROBE_DIR "msvc")
else()
    set(ICLFORGE_CPU_PROBE_DIR "builtin")
endif()

# tests/CMakeLists.txt compiles cpu_features.cpp a SECOND time, directly into iclforge-tests, against these
# directories - exposed unconditionally, since has_avx2() correctly answering "false" on a generic or
# AVX2-disabled build is as worth testing as it answering "true".
set(ICLFORGE_CPU_FEATURES_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" PARENT_SCOPE)
set(ICLFORGE_CPU_PROBE_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/variants/cpu-probe-${ICLFORGE_CPU_PROBE_DIR}" PARENT_SCOPE)
set(ICLFORGE_CPU_FEATURES_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu_features.cpp" PARENT_SCOPE)
set(ICLFORGE_IS_MSVC_LIKE "${ICLFORGE_IS_MSVC_LIKE}" PARENT_SCOPE)

# Which iclforge/base/detail/profiling.hpp the zone markers resolve to: CMake chooses the directory, not
# an #ifdef in the header. The stage-timer variant belongs to the minimum-footprint profile only.
if(ICLFORGE_ENABLE_TRACY)
    set(_iclforge_profiling_dir "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-tracy_enabled")
else()
    set(_iclforge_profiling_dir "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-tracy_disabled")
endif()
set(ICLFORGE_PROFILING_STAGE_TIMERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-stage_timers" PARENT_SCOPE)
# The directory a library outside this one compiles its markers against when it cannot take it from
# this library's usage requirements, because the minimum-footprint profile builds no iclforge::base
# (the AC-4 core, src/ac4/src/core): the stage timers in that profile when they are asked for, else what
# this library's own users see.
if(ICLFORGE_STAGE_TIMERS)
    set(ICLFORGE_PROFILING_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-stage_timers" PARENT_SCOPE)
else()
    set(ICLFORGE_PROFILING_INCLUDE_DIR "${_iclforge_profiling_dir}" PARENT_SCOPE)
endif()

# The minimum-footprint profile compiles the sources it needs itself (src/ac3/minimal.cmake), so no
# library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(base
    SOURCES
        src/cpu_features.cpp
        src/crypto/sha256.cpp
        src/crypto/hmac_sha256.cpp
        src/crypto/signing_key.cpp
    PUBLIC_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/cpu-probe-${ICLFORGE_CPU_PROBE_DIR}"
        "${_iclforge_profiling_dir}"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::fmt_private>")

# getenv (ICLFORGE_SIMD_TIER's override, and the signing key's ICLFORGE_SIGNING_KEY and
# ICLFORGE_SIGNING_KEY_FILE) is flagged by MSVC's CRT (C4996). Scoped to these files rather than
# /wd4996 on the whole target.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(src/cpu_features.cpp src/crypto/signing_key.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
