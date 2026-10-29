# iclforge-truehd-tests - the TrueHD and MLP library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-truehd-tests PROJECT truehd SOURCES
    core/test_checks.cpp
    core/test_syntax.cpp
    io/test_access_unit.cpp
    io/test_elementary.cpp
    io/test_elementary_resync.cpp
    decoder/test_decoder.cpp
    decoder/test_decoder_checks.cpp
    decoder/test_ffmpeg_fixtures.cpp
    decoder/test_fixture_features.cpp
    decoder/test_handbuilt.cpp
    encoder/test_coding.cpp
    encoder/test_downmix.cpp
    encoder/test_encoder.cpp
    encoder/test_signals.cpp)

target_link_libraries(iclforge-truehd-tests PRIVATE iclforge::truehd iclforge::base_headers iclforge::json)
# The encoder's coefficient quantiser and the verify comparison are internal (hidden in a shared
# build), so the tests that exercise them compile coding.cpp themselves and see src/.
target_sources(iclforge-truehd-tests PRIVATE ../src/encoder/coding.cpp)
target_include_directories(iclforge-truehd-tests PRIVATE ../src)

# iclforge-truehd-encode-tool - WAV in, TrueHD out, for tools/checks/verify_truehd_encode_interop.py
# and tools/ci/fuzz_truehd_encoder_space.py until the forge encode command replaces it.
add_executable(iclforge-truehd-encode-tool tools/encode_tool.cpp)
target_link_libraries(iclforge-truehd-encode-tool PRIVATE iclforge::truehd iclforge::warnings)
add_dependencies(iclforge-tests iclforge-truehd-encode-tool)
