# iclforge-truehd-tests - the TrueHD and MLP library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-truehd-tests PROJECT truehd SOURCES
    core/test_checks.cpp
    core/test_syntax.cpp
    io/test_access_unit.cpp
    io/test_elementary.cpp
    decoder/test_decoder.cpp
    decoder/test_ffmpeg_fixtures.cpp
    decoder/test_handbuilt.cpp)

target_link_libraries(iclforge-truehd-tests PRIVATE iclforge::truehd iclforge::base_headers iclforge::json)
