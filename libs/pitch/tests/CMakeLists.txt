# iclforge-pitch-tests - the pitch library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-pitch-tests PROJECT pitch SOURCES
    test_bleed.cpp
    test_reference.cpp
    test_scorer.cpp
    test_tracker.cpp)

target_link_libraries(iclforge-pitch-tests PRIVATE iclforge::pitch)
