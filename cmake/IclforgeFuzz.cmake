# ---------------------------------------------------------------------------
# libFuzzer harnesses over the libraries' untrusted-input entry points: the compiler they need, the
# instrumentation of the libraries they mutate, and the functions each library's
# fuzz/CMakeLists.txt registers its harnesses with.
#
# libFuzzer (-fsanitize=fuzzer) is an LLVM built-in, unavailable under GCC or
# MSVC, so every harness requires upstream Clang - not clang-cl, whose
# libFuzzer support this project has never exercised. Configure through
# tools/fuzz/run.sh rather than by hand; it sets up a dedicated build/fuzz directory
# with the right compiler and none of ICLFORGE_BUILD_CLI/GUI/TESTS, which this
# needs neither of and which would otherwise drag in vcpkg/Qt/Catch2 for
# nothing the harnesses use.
# ---------------------------------------------------------------------------

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "ICLFORGE_BUILD_FUZZERS needs upstream Clang: libFuzzer (-fsanitize=fuzzer) is an "
        "LLVM built-in, not available under GCC or MSVC. Configure with "
        "-DCMAKE_CXX_COMPILER=clang++, or run tools/fuzz/run.sh, which does this for you.")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "ICLFORGE_BUILD_FUZZERS targets upstream Clang on Linux/macOS (the linux-llvm / "
        "macos-llvm toolchain), not clang-cl: this project has never verified libFuzzer "
        "under the MSVC-ABI frontend, and the fuzzing task this serves asks for Linux-or-"
        "macOS specifically.")
endif()

# ASan+UBSan+SanitizerCoverage on the LIBRARY, not only the harness main: a
# fuzzer only earns its keep against undefined behaviour (the class of bug
# 8386c8f fixed - a malformed differential exponent chain walking the
# reconstruction outside the range it assumed) if the code being mutated is
# instrumented, not just the driver calling it. fuzzer-no-link adds the
# coverage counters libFuzzer's mutation engine reads without pulling in
# libFuzzer's own main, which a library must not carry.
#
# PUBLIC so every harness executable below inherits the same compile AND link
# flags automatically through target_link_libraries(... iclforge::ac3). -fno-
# sanitize-recover=undefined turns a UBSan report into a process abort
# libFuzzer can catch and minimize, rather than a warning it would otherwise
# print and run straight past.
#
# Set on iclforge_ac3_objects, not the iclforge_ac3_static/iclforge_ac3_shared wrapper
# targets: the actual codec sources compile there (see libs/ac3/CMakeLists.txt),
# and PUBLIC on an OBJECT library's usage requirements still propagates
# through PUBLIC target_link_libraries(iclforge_ac3_static PUBLIC iclforge_ac3_objects)
# to whatever finally links iclforge::ac3 - here, each fuzz harness below. That
# includes the EMDF Atmos signer fuzz_signing_verify drives (its frame walk,
# libs/ac3/src/signing/emdf_atmos_signer.cpp), which is a part of iclforge::ac3. The HMAC-SHA-256 it
# computes is iclforge::base's (libs/base/src/crypto), so iclforge_base_objects is instrumented too.
function(iclforge_instrument_for_fuzzing target)
    target_compile_options(${target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined,fuzzer-no-link)
endfunction()

iclforge_instrument_for_fuzzing(iclforge_ac3_objects)
iclforge_instrument_for_fuzzing(iclforge_base_objects)

# The container libraries need the identical treatment: they parse
# self-declared lengths straight off untrusted input, which is exactly the
# class of bug ASan catches and exactly what the harnesses below mutate. An
# uninstrumented library would still crash on a genuine out-of-bounds read,
# but only by luck, and the mutation engine would be walking blind - the
# coverage counters are what steer it into the branches that matter.
#
# Same PUBLIC-on-the-OBJECT-library reasoning as iclforge_ac3_objects above: the
# usage requirements propagate through the static/shared wrappers to
# whatever finally links iclforge::containers - here, each harness. The IAMF parser
# fuzz_iamf_parse mutates is a part of it too.
iclforge_instrument_for_fuzzing(iclforge_containers_objects)

# iclforge_ac4_objects and iclforge_iab_objects, for the same reason: fuzz_ac4_parse and
# fuzz_iab_parse below mutate parsers that take their counts and lengths from
# the file. Each is a library of its own rather than part of iclforge::ac3 (see
# libs/ac4/ and libs/iab/), so nothing iclforge::ac3 carries reaches their
# sources, and until these calls existed both harnesses ran against
# uninstrumented parsers. A harness executable still carries the ASan
# runtime, so a segfault, a timeout or an allocation over -rss_limit_mb
# stopped those runs; an out-of-bounds read or undefined behaviour inside the
# parser did not, and the mutation engine got no coverage from it.
#
# iclforge_adm_objects, which fuzz_adm_parse links, was the last one left out, and
# for a reason the other two did not have: libbw64 is header-only, so
# instrumenting iclforge::adm instruments the libbw64 code it compiles too. UBSan then
# stopped the harness within a few hundred executions inside libbw64's
# UnknownChunk constructor, which takes &data_[0] of an empty vector for a
# zero-length chunk, and does the same in Bw64Reader::read() for a <data> chunk
# of zero length. Both are patched in the dependency itself at populate time
# (libs/adm/patch_libbw64.cmake, wired up in libs/adm/CMakeLists.txt), which
# is what let this call join the set.
#
# An ignorelist scoped to libbw64 was the alternative, and would have hidden
# more than it suppressed: a <fmt > whose channel count and width overflow
# libbw64's uint16_t block alignment gets its read buffer sized from the wrapped
# value and decoded against the real one, which ASan reports as a heap overread
# against iclforge::adm's own read_pcm. That one is guarded in libs/adm/src/adm.cpp,
# and an uninstrumented iclforge::adm reads it as a clean execution.
#
# Each call is guarded on the option that decides whether its library exists,
# and each harness below sits under the same guard, so turning one of those
# options off drops its harness instead of failing the configure.
if(ICLFORGE_BUILD_AC4)
    iclforge_instrument_for_fuzzing(iclforge_ac4_objects)
endif()
if(ICLFORGE_BUILD_IAB)
    iclforge_instrument_for_fuzzing(iclforge_iab_objects)
endif()
if(ICLFORGE_BUILD_ADM)
    iclforge_instrument_for_fuzzing(iclforge_adm_objects)
endif()

add_custom_target(iclforge_fuzzers)

# One executable per untrusted-input entry point. Each adds -fsanitize=fuzzer
# on top of what it already inherited from iclforge::ac3 above: that flag links
# libFuzzer's runtime and its LLVMFuzzerTestOneInput-driven main, which must
# exist exactly once per executable and never inside the library itself.
#
# iclforge::warnings is named explicitly rather than inherited: iclforge links it
# PRIVATE, as every first-party target does, so it governs that library's own
# translation units and stops there. Without this the harness sources would be
# the one corner of the project compiling without the strict set - and a
# harness is exactly where a sloppy size_t-to-int narrowing would quietly
# reshape the input a fuzzer thinks it is passing in.
#
# Anything after `name` is an extra library that harness needs beyond
# iclforge::ac3 - iclforge::iab for fuzz_iab_parse, iclforge::ac4 for
# fuzz_ac4_parse, iclforge::adm for fuzz_adm_parse. Each is a separate target rather than part of iclforge::ac3
# (see its own CMakeLists.txt for why), and none is needed by the harnesses
# that only call into the codec. Naming a library here does not instrument
# it: its OBJECT library needs its own iclforge_instrument_for_fuzzing() call
# above, or the harness links it without coverage counters or sanitizer
# checks.
function(iclforge_add_fuzzer name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE iclforge::ac3 iclforge::warnings ${ARGN})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(iclforge_fuzzers ${name})
endfunction()

# Same, for a harness whose subject is a container library rather than the
# codec: everything about the flags is identical, only the linked library
# differs. Kept as its own function rather than a parameter on the one above
# because every EXISTING harness takes iclforge::ac3 and would gain a noise
# argument for it.
function(iclforge_add_container_fuzzer name library)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE ${library} iclforge::warnings)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(iclforge_fuzzers ${name})
endfunction()

# The harnesses themselves are each library's (libs/<lib>/fuzz/CMakeLists.txt), which the root
# CMakeLists.txt adds after including this file.
