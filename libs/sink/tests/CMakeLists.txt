# iclforge-sink-tests - the sink library's Catch2 tests (cmake/IclforgeTests.cmake). The sink's
# behaviour is held by hearth-testsink's suites (apps/hearth/engine/tests), through the program's real
# outputs; these are the seam, with outputs that only write down what they were given.
iclforge_add_test_binary(iclforge-sink-tests PROJECT sink SOURCES
    test_sink.cpp
    test_passthrough.cpp
    test_end_to_end.cpp)

target_link_libraries(iclforge-sink-tests PRIVATE iclforge::sink iclforge::sink_passthrough iclforge::sink_harness iclforge::sendspin
    iclforge::containers)

# test_end_to_end.cpp names a fixture directory and a scratch directory from the environment with
# std::getenv, which MSVC's C4996 warns of (as the hearth tests' own getenv files are treated).
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(test_end_to_end.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
