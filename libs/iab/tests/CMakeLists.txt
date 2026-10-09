# iclforge-iab-tests - the IAB reader's and writer's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-iab-tests PROJECT iab SOURCES
    test_ac3iab.cpp
    test_ac3iab_truncation.cpp
    test_dlc.cpp
    test_iab_writer.cpp
    test_mxf_reader.cpp
    test_mxf_writer.cpp)

target_link_libraries(iclforge-iab-tests PRIVATE iclforge::iab iclforge::ac3)
