# iclforge-ac3-tests - the AC-3 and E-AC-3 codec's Catch2 tests (cmake/IclforgeTests.cmake).
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _ac3_root)

iclforge_add_test_binary(iclforge-ac3-tests PROJECT ac3 SOURCES
    analysis/test_analysis.cpp
    decoder/test_associated_service.cpp
    decoder/test_associated_service_stream.cpp
    encoder/test_assignment.cpp
    encoder/test_bandwidth.cpp
    oba/test_atmos.cpp
    oba/test_atmos_cbi.cpp
    oba/test_dee_joc_fixture.cpp
    oba/test_joc_7channel.cpp
    oba/test_atmos_motion.cpp
    core/test_bitalloc.cpp
    core/test_fixed32.cpp
    core/test_mdct_fixed.cpp
    decoder/test_block_norm.cpp
    meta/test_bsi.cpp
    encoder/test_bit_reservoir.cpp
    encoder/test_coupling.cpp
    core/test_crc16.cpp
    decoder/test_concealment.cpp
    core/test_crc_mutator.cpp
    decoder/test_decoder.cpp
    decoder/test_diagnostics.cpp
    meta/test_drc.cpp
    encoder/test_eac3.cpp
    encoder/test_eac3_short_syncframes.cpp
    decoder/test_eac3_decoder.cpp
    decoder/test_eac3_programmes.cpp
    decoder/test_eac3_transient_prenoise.cpp
    io/test_elementary.cpp
    io/test_stream_accumulator.cpp
    io/test_metadata_edit.cpp
    io/test_probe.cpp
    emdf/test_emdf.cpp
    encoder/test_encoder.cpp
    encoder/test_enhanced_coupling.cpp
    encoder/test_exp_strategy.cpp
    core/test_exponents.cpp
    test_fmp4.cpp
    core/test_frame.cpp
    decoder/test_latency.cpp
    decoder/test_live_downmix.cpp
    decoder/test_output_stage.cpp
    meta/test_loudness.cpp
    core/test_mantissas.cpp
    core/test_mdct.cpp
    core/test_mdct_fast.cpp
    meta/test_meta_qc.cpp
    meta/test_mixing.cpp
    test_mp4.cpp
    test_mpegts.cpp
    io/test_object_strip.cpp
    quality/test_distortion.cpp
    quality/test_perceptual.cpp
    quality/test_search.cpp
    quality/test_eac3_search.cpp
    oba/test_oba.cpp
    encoder/test_plan.cpp
    verify/test_eac3_selfcheck.cpp
    verify/test_selfcheck.cpp
    verify/test_trace_export.cpp
    signing/test_signing.cpp
    core/test_simd_kernels.cpp
    core/test_tables.cpp
    encoder/test_scalar_math.cpp
    encoder/test_transient.cpp
    decoder/test_transient_prenoise.cpp
    io/test_wav.cpp
    io/test_wav_stream_reader.cpp
    io/test_wav_stream_writer.cpp)

target_link_libraries(iclforge-ac3-tests PRIVATE iclforge::ac3 iclforge::containers)

# -Wnull-dereference false positive, GNU only: "a rewrite really does
# re-stamp the CRCs" hand-flips one bit of an already-encoded frame
# (tampered[6] ^= std::byte{0x20}) to build its negative control, and at -O3
# (Release; not seen at the -debug preset's lower optimization) GCC inlines
# std::byte's operator^= straight into the Catch2 test body and misattributes
# a "null dereference" to the subscript - the same post-inlining
# misattribution cmake/CompilerWarnings.cmake already documents for GCC < 15
# against libstdc++ internals (see its own comment, and gcc.gnu.org/bugzilla
# PR 108757 and neighbours), surfacing here on the CI-pinned GCC 16 instead:
# confirmed on a plain WSL2 GCC 15.2.0 build (config-linux-gcc, not just
# -debug) and, for real, in CI's own build logs (CodeQL, adm-validate, and
# the plain Linux GCC/GCC-arm64 legs all hit it). `tampered` is a full copy
# of a real encoded AC-3 syncframe, always far more than 6 bytes, so nothing
# here is genuinely null-unsafe. Scoped to the file rather than widening
# that project-wide guard, the same reasoning libs/adm/CMakeLists.txt's
# own GCC-15 instance of this category gives for scoping to its one target:
# nothing else in this project has hit it, so silencing it everywhere would
# risk hiding a genuine one elsewhere.
set_source_files_properties(io/test_metadata_edit.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")

# test_bit_reservoir.cpp reaches the encoder's own internal bit_reservoir.hpp
# (ABR's sliding-window accounting), which is plumbing between the encoder's
# translation units rather than library surface - same pattern, same reason.
# test_exp_strategy.cpp includes exp_strategy.hpp, the exponent-run planner the
# two encoder translation units share. It is deliberately not library surface -
# nothing outside libs/ac3/src/encoder/ has any business planning exponent
# runs - but what it decides is a cost judgement, and that is worth holding to
# account on inputs chosen to make the right answer obvious rather than only
# through a whole encode: a library's src/ dir on this target's include path only, the pattern the
# ALSA backend's internal header uses in libs/audio/tests.
target_include_directories(iclforge-ac3-tests PRIVATE "${_ac3_root}/src/encoder")

# test_scalar_math.cpp includes iclforge/base/arithmetic/scalar_math.hpp and
# test_fixed32*.cpp/test_mdct_fixed.cpp include iclforge/base/arithmetic/fixed32.hpp - the
# shared arithmetic target (libs/base) both iclforge::ac3 and iclforge::ac4
# use (planning/ac4.md, decision 31), worth holding to their own accuracy/
# correctness bounds rather than only through the fixture hashes that depend
# on them.
target_link_libraries(iclforge-ac3-tests PRIVATE iclforge::base_headers)
target_include_directories(iclforge-ac3-tests PRIVATE "${_ac3_root}/src/core")
# test_block_norm.cpp includes libs/ac3/src/decoder/block_norm.hpp, the
# fixed-point tier's block exponent, which both decoders share and which is not
# library surface.
target_include_directories(iclforge-ac3-tests PRIVATE "${_ac3_root}/src/decoder")

# test_simd_kernels.cpp (SIMD kernels) needs the ONE architecture directory
# CMake selected in libs/base/CMakeLists.txt, so the test exercises the
# f64x2/i32x4 the library was actually built with rather than a different
# one - same white-box arrangement as libs/ac3/src/encoder just above and the
# ALSA backend's internal header in libs/audio/tests.
target_include_directories(iclforge-ac3-tests PRIVATE "${ICLFORGE_ARCH_INCLUDE_DIR}")

# test_simd_kernels.cpp's CPU-feature-detection cases (the runtime AVX2
# dispatch) need cpu_features.cpp compiled a SECOND time,
# directly into this target, against the same resolved probe directory
# libs/ac3/CMakeLists.txt selected for the library - rather than calling
# into iclforge_ac3_objects's own copy of has_avx2(), which would need
# dllexport/dllimport treatment that would otherwise only matter for the
# config-linux-llvm-shared (BUILD_SHARED_LIBS=ON) leg. Same white-box
# arrangement as libs/ac3/src/encoder above, and unconditional: has_avx2()
# correctly answering "false" on a build with no AVX2 tier is exactly as
# worth testing as it answering "true" where one exists.
target_sources(iclforge-ac3-tests PRIVATE "${ICLFORGE_CPU_FEATURES_SOURCE}")
target_include_directories(iclforge-ac3-tests PRIVATE
    "${ICLFORGE_CPU_FEATURES_INCLUDE_DIR}"
    "${ICLFORGE_CPU_PROBE_INCLUDE_DIR}")
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties("${ICLFORGE_CPU_FEATURES_SOURCE}" PROPERTIES
        COMPILE_OPTIONS "/wd4996")
endif()

# The AVX2 kernel side of the same follow-on: avx2_probe.cpp and
# mdct_avx2.cpp compiled a second time into this target too, same reasoning
# as cpu_features.cpp just above. ICLFORGE_AVX2_INCLUDE_DIR is empty (falsy)
# whenever libs/ac3/CMakeLists.txt did not build the AVX2 tier at all
# (ICLFORGE_AVX2=OFF, or a non-x86_64 target) - the test sources are
# conditional on the exact same condition the library used.
#
# Which of those two worlds the test file is in used to be an
# ICLFORGE_HAVE_AVX2_TIER compile definition it asked about with an #ifdef,
# on the reasoning that tools/checks/check_platform_macros.ps1 scanned src/
# and apps/ only. That check now covers tests/ as well, and the #ifdef was
# costing more than the rule: eleven AVX2 case bodies were not PARSED at all
# on a non-x86_64 leg, so a renamed kernel or a changed signature could only
# be caught by an x86_64 run. So the answer arrives the way every other
# either/or in this repository does - core/avx2/{present,absent}/ each ship
# an avx2_tier.hpp, CMake puts the matching one on the include path, and
# iclforge::test::avx2::kTierCompiled is a constexpr bool the test branches on
# with a plain `if`. The absent variant declares the same eight entry points
# and defines them as unreachable stubs (see its own header), so both arms
# compile everywhere.
if(ICLFORGE_AVX2_INCLUDE_DIR)
    target_sources(iclforge-ac3-tests PRIVATE
        "${ICLFORGE_AVX2_INCLUDE_DIR}/avx2_probe.cpp"
        "${ICLFORGE_AVX2_INCLUDE_DIR}/mdct_avx2.cpp")
    target_include_directories(iclforge-ac3-tests PRIVATE
        "${ICLFORGE_AVX2_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/avx2/present")
    if(ICLFORGE_IS_MSVC_LIKE)
        set_source_files_properties(
            "${ICLFORGE_AVX2_INCLUDE_DIR}/avx2_probe.cpp"
            "${ICLFORGE_AVX2_INCLUDE_DIR}/mdct_avx2.cpp" PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(
            "${ICLFORGE_AVX2_INCLUDE_DIR}/avx2_probe.cpp"
            "${ICLFORGE_AVX2_INCLUDE_DIR}/mdct_avx2.cpp" PROPERTIES
            COMPILE_OPTIONS "-mavx2")
    endif()
else()
    # mdct_avx2.hpp too: libs/ac3/src/internal/avx2/none/mdct_avx2.cpp already
    # gives every kernel it declares a std::unreachable() body inside
    # iclforge_ac3_objects in exactly this configuration, so the absent variant reuses
    # those rather than defining a second set (which is a duplicate-symbol link
    # error, not a harmless overlap). Reaching into src/ by path is the same
    # white-box arrangement this file already uses for
    # libs/ac3/src/{encoder,core,decoder} above.
    target_include_directories(iclforge-ac3-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/core/avx2/absent"
        "${_ac3_root}/src/internal/avx2")
    target_sources(iclforge-ac3-tests PRIVATE core/avx2/absent/avx2_tier.cpp)
endif()

# core/test_simd_kernels.cpp's own AVX2_REQUIRE knob reads std::getenv too -
# same C4996 reasoning as cpu_features.cpp above.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(core/test_simd_kernels.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()

# decoder/test_decoder.cpp's patch_bits: GCC's -O3 VRP mis-derives, only once
# that function is inlined into one specific one-bit call site (count == 1),
# that `frame`'s backing store is null ("source object is likely at address
# zero"), and fails the build under -Werror=array-bounds. `frame` is always a
# real, previously REQUIRE()'d encoded syncframe, dozens of bytes long, at
# every call site - GCC's own known false-positive class for std::vector
# subscripting after aggressive inlining, not a real bounds issue, and the
# same family as the two GCC suppressions cmake/CompilerWarnings.cmake already
# carries (see its -Wnull-dereference and -Warray-bounds entries, which state
# the general shape: post-inlining analysis misattributing an
# optimizer-introduced "impossible" path as a real access).
#
# This was a [[gnu::noinline]] on the function, behind
# `#if defined(__GNUC__) && !defined(__clang__)` - guarded rather than bare
# because MSVC's /W4 /WX treats an unrecognized attribute (C5030) as a hard
# error rather than ignoring it, confirmed in CI. Blocking inlining did remove
# the interprocedural constant-propagation path that triggers the warning, but
# it answered a compiler question in the preprocessor, which is the one thing
# this tree does not do (tools/checks/check_platform_macros.ps1, which now
# covers tests/). Naming the diagnostic here instead says the same thing more
# directly - this warning is a known false positive in this file - and says it
# where the /wd4996 entry above and every other compiler-specific flag in this
# project already live. Clang does not warn here and needs no entry.
#
# Deliberately per-source rather than on the whole target: iclforge::warnings
# already turns -Warray-bounds off tree-wide for GCC >= 16, but this binary is
# also built by the local development loop's older GCC, where it is still on.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(decoder/test_decoder.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds")
endif()

# core/test_crc_mutator.cpp includes libs/ac3/fuzz/crc_mutator.hpp - the re-stamping
# half of the libFuzzer custom mutator (signing-verify fuzz walk). The mutator itself only
# builds under Clang with -fsanitize=fuzzer, but the header's
# restamp_syncframe_crcs is ordinary portable C++ over iclforge::ac3::crc16 and the
# frame-size tables, and whether it produces a crc1 the decoder accepts is
# exactly the kind of thing that should be checked on every platform rather
# than inferred from a coverage number on one. Same shape as the libs/ac3/src
# includes above: a directory outside the public include tree, on this
# target's path only.
target_include_directories(iclforge-ac3-tests PRIVATE "${_ac3_root}/fuzz")

# core/test_bitalloc.cpp, test_mdct.cpp, test_mdct_fast.cpp and meta/test_drc.cpp include the golden
# tables as "<name>_goldens.hpp", from testdata/ (tools/generators writes them there).
target_include_directories(iclforge-ac3-tests PRIVATE "${PROJECT_SOURCE_DIR}/testdata")

# core/test_fixed32_ecpl.cpp calls the fixed-point tier's enhanced-coupling
# functions, which are internal to the library: declared under libs/ac3/src/
# rather than in the installed include tree, and so not exported from
# libiclforge_ac3.so. With BUILD_SHARED_LIBS=ON there is nothing for it to link
# against, and linking the static archive in as well would put a second copy of
# the codec in the executable that the shared pass exists to keep out. It tests
# the tier's arithmetic, not the linkage, and every static leg runs it.
if(NOT BUILD_SHARED_LIBS)
    target_sources(iclforge-ac3-tests PRIVATE core/test_fixed32_ecpl.cpp)
endif()


