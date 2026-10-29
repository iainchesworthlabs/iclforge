# iclforge-truehd-tests - the TrueHD and MLP library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-truehd-tests PROJECT truehd SOURCES
    core/test_checks.cpp
    core/test_syntax.cpp
    io/test_access_unit.cpp
    io/test_elementary.cpp
    io/test_elementary_resync.cpp
    io/test_evolution.cpp
    decoder/test_channel_assignment.cpp
    io/test_channel_meaning.cpp
    decoder/test_decoder.cpp
    decoder/test_decoder_checks.cpp
    decoder/test_ffmpeg_fixtures.cpp
    decoder/test_fixture_features.cpp
    decoder/test_handbuilt.cpp
    decoder/test_kernels.cpp
    encoder/test_coded_bits_simd.cpp
    decoder/test_substream3.cpp
    encoder/test_coding.cpp
    encoder/test_downmix.cpp
    encoder/test_encoder.cpp
    encoder/test_signals.cpp
    fuzz/test_truehd_mutator.cpp)

# iclforge::objects only for test_evolution.cpp, which holds this library's reading of the evolution
# frame to the EMDF container reader E-AC-3 streams go through (libs/truehd itself links base alone).
target_link_libraries(iclforge-truehd-tests PRIVATE iclforge::truehd iclforge::base_headers iclforge::json
    iclforge::objects)
# fuzz/test_truehd_mutator.cpp includes libs/truehd/fuzz/truehd_mutator.hpp, the repair half of the
# libFuzzer custom mutator. The mutator itself builds only under Clang with -fsanitize=fuzzer, but
# the header's restamp_truehd_checks is portable C++ over the library's own check routines, and
# whether it writes what a decoder accepts is checked on every platform, not inferred from a
# coverage number on one. Same shape as libs/ac3/tests: a directory outside the public include
# tree, on this target's path only.
target_include_directories(iclforge-truehd-tests PRIVATE ../fuzz)
# The encoder's coefficient quantiser and the verify comparison are internal (hidden in a shared
# build), so the tests that exercise them compile coding.cpp themselves and see src/.
target_sources(iclforge-truehd-tests PRIVATE ../src/encoder/coding.cpp)
target_include_directories(iclforge-truehd-tests PRIVATE ../src)

# The AVX2 kernel of src/internal/avx2, compiled a second time into this binary for the reason
# coding.cpp is: its symbols are hidden in a shared build. ICLFORGE_TRUEHD_AVX2_DIR is empty whenever
# libs/truehd did not build the tier (ICLFORGE_AVX2=OFF, or a target that is not x86-64), and then the
# stub the library compiled in its place is compiled here as well; the test cases skip themselves on
# a CPU or a build without the tier (iclforge::internal::cpu::has_avx2()).
if(ICLFORGE_TRUEHD_AVX2_DIR)
    target_sources(iclforge-truehd-tests PRIVATE "${ICLFORGE_TRUEHD_AVX2_DIR}/coded_bits_avx2.cpp")
    if(ICLFORGE_IS_MSVC_LIKE)
        set_source_files_properties("${ICLFORGE_TRUEHD_AVX2_DIR}/coded_bits_avx2.cpp" PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties("${ICLFORGE_TRUEHD_AVX2_DIR}/coded_bits_avx2.cpp" PROPERTIES
            COMPILE_OPTIONS "-mavx2")
    endif()
else()
    target_sources(iclforge-truehd-tests PRIVATE ../src/internal/avx2/none/coded_bits_avx2.cpp)
endif()

# iclforge-truehd-encode-tool - WAV in, TrueHD out, for tools/checks/verify_truehd_encode_interop.py
# and tools/ci/fuzz_truehd_encoder_space.py until the forge encode command replaces it.
add_executable(iclforge-truehd-encode-tool tools/encode_tool.cpp)
target_link_libraries(iclforge-truehd-encode-tool PRIVATE iclforge::truehd iclforge::warnings)
add_dependencies(iclforge-tests iclforge-truehd-encode-tool)
