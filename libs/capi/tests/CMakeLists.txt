# iclforge-capi-tests - the C API's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-capi-tests PROJECT capi SOURCES
    test_capi.cpp
    test_capi_ac4_arguments.cpp)

target_link_libraries(iclforge-capi-tests PRIVATE iclforge::c iclforge::ac3 iclforge::ac4)

# The TrueHD C functions are answered by libs/capi/src/truehd.cpp or truehd_absent.cpp by the same
# option, so the test that matches the build is the one compiled. The present one reads the FFmpeg
# fixtures (ICLFORGE_GOLDEN_TRUEHD_DIR) and checks the decode against their SHA-256.
if(ICLFORGE_BUILD_TRUEHD)
    target_sources(iclforge-capi-tests PRIVATE test_capi_truehd.cpp)
    target_link_libraries(iclforge-capi-tests PRIVATE iclforge::base iclforge::json)
else()
    target_sources(iclforge-capi-tests PRIVATE test_capi_truehd_absent.cpp)
endif()
