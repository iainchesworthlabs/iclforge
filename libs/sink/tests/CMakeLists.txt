# iclforge-sink-tests - the sink library's Catch2 tests (cmake/IclforgeTests.cmake). The sink's
# behaviour is held by hearth-testsink's suites (apps/hearth/engine/tests), through the program's real
# outputs; these are the seam, with outputs that only write down what they were given.
iclforge_add_test_binary(iclforge-sink-tests PROJECT sink SOURCES
    test_sink.cpp
    test_passthrough.cpp)

target_link_libraries(iclforge-sink-tests PRIVATE iclforge::sink iclforge::sink_passthrough iclforge::sendspin
    iclforge::containers)
