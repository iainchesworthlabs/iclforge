# iclforge-base-tests - the foundation library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-base-tests PROJECT base SOURCES
    test_bit_io.cpp
    test_bitwriter.cpp
    test_crc16.cpp
    test_meters.cpp
    test_crypto.cpp)

target_link_libraries(iclforge-base-tests PRIVATE iclforge::base iclforge::ac3)
