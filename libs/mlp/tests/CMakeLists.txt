# iclforge-mlp-tests - the TrueHD (MLP) codec's Catch2 tests (cmake/IclforgeTests.cmake).
#
# The syntax, CRC and prediction tests reach the library's private headers (libs/mlp/src/core),
# as libs/ac4/tests reaches AC-4's: those functions are compiled into iclforge_mlp_objects with
# hidden visibility, so the tests that call them link the static library and are built only when
# iclforge::mlp names it. The tests of the public API run against whichever library is built.
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _mlp_root)

iclforge_add_test_binary(iclforge-mlp-tests PROJECT mlp SOURCES
    core/test_stream_info.cpp
    decoder/test_ffmpeg_gold.cpp
    encoder/test_round_trip.cpp
    io/test_elementary.cpp)

target_link_libraries(iclforge-mlp-tests PRIVATE iclforge::mlp iclforge::base_headers)
target_include_directories(iclforge-mlp-tests PRIVATE "${_mlp_root}/src")
# The golden masters FFmpeg's encoder wrote (tools/generators/gen_truehd_ffmpeg_gold.py).
target_compile_definitions(iclforge-mlp-tests PRIVATE
    "ICLFORGE_TRUEHD_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/testdata/truehd\"")
if(NOT BUILD_SHARED_LIBS)
    target_sources(iclforge-mlp-tests PRIVATE
        core/test_crc.cpp
        core/test_syntax.cpp)
endif()
