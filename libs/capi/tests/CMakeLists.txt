# iclforge-capi-tests - the C API's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-capi-tests PROJECT capi SOURCES
    test_capi.cpp
    test_capi_ac4_arguments.cpp)

target_link_libraries(iclforge-capi-tests PRIVATE iclforge::c iclforge::ac3 iclforge::ac4)
