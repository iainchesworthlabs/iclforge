# iclforge-dsp-tests - the signal-processing library's Catch2 tests (cmake/IclforgeTests.cmake).
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _dsp_root)

iclforge_add_test_binary(iclforge-dsp-tests PROJECT dsp SOURCES
    tiered/test_dsp.cpp
    tiered/test_dsp_exact.cpp
    tiered/test_transform_tables.cpp
    tiered/test_portable_math.cpp
    tiered/test_resampler.cpp
    test_dsp_biquad.cpp
    test_dsp_qmf.cpp
    test_dsp_qmf_engine.cpp
    test_dsp_resampler.cpp
    test_fft.cpp)

target_link_libraries(iclforge-dsp-tests PRIVATE iclforge::dsp iclforge::base_headers)

# tiered/ holds the kernels to their bounds at the build's decode scalar, which only the codec that
# calls them builds them at (libs/dsp/CMakeLists.txt): AC-4, whose objects carry them in a static
# build and whose shared library exports none of them, so a shared build compiles them into this
# target a second time (ICLFORGE_AC4_KERNEL_SOURCES, as libs/ac4/tests does).
# tiered/test_dsp_exact.cpp also holds AC-4's A-SPX pre-flattening fit (core/aspx/hf_generator.hpp)
# to the bits of the fit it replaced. Hence AC-4's private root on the include path beside dsp's.
target_include_directories(iclforge-dsp-tests PRIVATE
    "${_dsp_root}/src"
    "${_dsp_root}/variants/decode-scalar-${ICLFORGE_DECODE_SCALAR_TIER}"
    "${PROJECT_SOURCE_DIR}/libs/ac4/src")
if(BUILD_SHARED_LIBS)
    target_sources(iclforge-dsp-tests PRIVATE ${ICLFORGE_AC4_KERNEL_SOURCES})
else()
    target_link_libraries(iclforge-dsp-tests PRIVATE iclforge::ac4)
endif()
