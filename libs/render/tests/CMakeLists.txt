# iclforge-render-tests - the renderer's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-render-tests PROJECT render SOURCES
    test_layout.cpp
    test_object_constraints.cpp
    test_object_lfe_timing.cpp
    test_identify.cpp
    test_routing.cpp
    test_serving.cpp
    test_trim_delay.cpp
    test_spatial.cpp)

target_link_libraries(iclforge-render-tests PRIVATE iclforge::render iclforge::ac3)

# test_layout.cpp's Speaker/OutputLayout size assertions: the numbers
# are an ABI fact, written down for MSVC's ABI (which clang-cl shares) and not
# measured anywhere else, so abi/{msvc,unknown}/ each ship a
# speaker_abi.hpp and this puts the matching one on the include path. It was a
# `#if defined(_MSC_VER)` in the test file; see either header for what the
# guard protects and why one ABI's numbers are worth asserting at all.
if(ICLFORGE_IS_MSVC_LIKE)
    target_include_directories(iclforge-render-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/abi/msvc")
else()
    target_include_directories(iclforge-render-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/abi/unknown")
endif()
