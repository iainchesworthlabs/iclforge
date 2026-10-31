# iclforge-sink-tests - the sink library's Catch2 tests (cmake/IclforgeTests.cmake). The sink's
# behaviour is held by hearth-testsink's suites (apps/hearth/engine/tests), through the program's real
# outputs; these are the seam, with outputs that only write down what they were given.
iclforge_add_test_binary(iclforge-sink-tests PROJECT sink SOURCES
    test_sink.cpp
    test_lyric_frame.cpp
    test_lyric_frame_golden.cpp
    test_passthrough.cpp
    test_decoding_output.cpp
    test_decoding_output_truehd.cpp
    test_end_to_end.cpp)

target_link_libraries(iclforge-sink-tests PRIVATE iclforge::sink iclforge::sink_passthrough iclforge::sink_decode iclforge::sink_harness
    iclforge::sendspin iclforge::containers iclforge::ac3 iclforge::render)

# test_lyric_frame_golden.cpp reads the conformance fixtures (lyric-frames/) and the karaoke examples
# under the repository root, by path; the same two directories can be named from the environment, for
# a build whose sources are not where it was configured (an Android device, a copy of the tree).
target_compile_definitions(iclforge-sink-tests PRIVATE
    "ICLFORGE_LYRIC_FRAMES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/lyric-frames\""
    "ICLFORGE_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")

# test_end_to_end.cpp names a fixture directory and a scratch directory from the environment with
# std::getenv, which MSVC's C4996 warns of (as the hearth tests' own getenv files are treated).
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(test_end_to_end.cpp test_lyric_frame_golden.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
