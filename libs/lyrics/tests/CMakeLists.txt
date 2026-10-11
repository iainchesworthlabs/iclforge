# iclforge-lyrics-tests - the lyrics library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-lyrics-tests PROJECT lyrics SOURCES
    test_lyrics.cpp)

target_link_libraries(iclforge-lyrics-tests PRIVATE iclforge::lyrics)
