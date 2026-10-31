# ---------------------------------------------------------------------------
# iclforge::truehd - the Dolby TrueHD and MLP codec's bitstream and decoder, written from the owner's
# Dolby TrueHD (MLP) Reference Specification (v3.5) and checked against the streams FFmpeg's encoder
# writes and FFmpeg's decoder (planning/truehd.md). The layers are the ones iclforge::ac3 and
# iclforge::ac4 have: core (the constants, the CRCs and parities, the syntax of the restart and block
# headers, the codebooks), io (the major sync, the access unit, the elementary stream) and decoder.
# The encoder, the Atmos substream and the front ends follow in phase T2.
#
# It links iclforge::base for the bit reader and writer and nothing else: no DSP, no objects, no
# platform code, and no third-party code. Built and installed as a static and a shared library by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(truehd
    NO_C4251_SUPPRESSION
    SOURCES
        src/core/crc.cpp
        src/core/format.cpp
        src/core/syntax.cpp
        src/decoder/decoder.cpp
        src/encoder/coding.cpp
        src/encoder/encoder.cpp
        src/io/access_unit.cpp
        src/io/elementary.cpp
        src/io/evolution.cpp
        src/io/major_sync.cpp
        src/io/metadata_edit.cpp
        src/io/scan.cpp
    DEPENDS base
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::base_headers>")

# --- The AVX2 tier ------------------------------------------------------------------------------
# One kernel has a vector body, the encoder's bit count for a block of residuals
# (src/internal/avx2/coded_bits_avx2.cpp), laid out the way libs/ac3/CMakeLists.txt lays out its MDCT
# kernels: the body in a translation unit of its own that is the only one to see an AVX2 compiler flag
# (an AVX2 flag on the codec's other files would compile them for a feature the deployment machine
# might not have, which iclforge/base/detail/cpu_features.hpp's has_avx2() exists to let this project
# avoid), the call site behind has_avx2(), and a stub with the same declarations where there is no
# tier. ICLFORGE_SIMD_RESOLVED, ICLFORGE_AVX2 and ICLFORGE_IS_MSVC_LIKE come from libs/base.
if(ICLFORGE_SIMD_RESOLVED STREQUAL "x86_64" AND ICLFORGE_AVX2)
    add_library(iclforge_truehd_simd_avx2 OBJECT src/internal/avx2/coded_bits_avx2.cpp)
    target_include_directories(iclforge_truehd_simd_avx2 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")
    target_compile_features(iclforge_truehd_simd_avx2 PRIVATE cxx_std_23)
    target_link_libraries(iclforge_truehd_simd_avx2 PRIVATE "$<BUILD_INTERFACE:iclforge::warnings>")
    # /arch:AVX2 for MSVC and clang-cl, -mavx2 for GCC/Clang/AppleClang; never -mfma, so that
    # has_avx2()'s CPUID gate stays the single AVX2 bit.
    if(ICLFORGE_IS_MSVC_LIKE)
        target_compile_options(iclforge_truehd_simd_avx2 PRIVATE /arch:AVX2)
    else()
        target_compile_options(iclforge_truehd_simd_avx2 PRIVATE -mavx2)
    endif()
    set_target_properties(iclforge_truehd_simd_avx2 PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        # LTO is the one mechanism that can hoist AVX2-flagged code into a caller never compiled with
        # the flag: a correctness requirement, not tidiness.
        INTERPROCEDURAL_OPTIMIZATION OFF
        EXPORT_NAME truehd_simd_avx2)
    # The objects go into the static and shared wrappers as sources rather than being linked PUBLIC as
    # libs/ac3 does: a link requirement on a target outside the export set is an error in
    # install(EXPORT truehdTargets), and nothing here needs the target after the link. (Added to
    # iclforge_truehd_objects they would not follow: the wrappers take that target's own objects only.)
    target_sources(iclforge_truehd_static PRIVATE "$<TARGET_OBJECTS:iclforge_truehd_simd_avx2>")
    target_sources(iclforge_truehd_shared PRIVATE "$<TARGET_OBJECTS:iclforge_truehd_simd_avx2>")
    # libs/truehd/tests compiles the same translation unit a second time into its binary.
    set(ICLFORGE_TRUEHD_AVX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2" PARENT_SCOPE)
else()
    target_sources(iclforge_truehd_objects PRIVATE src/internal/avx2/none/coded_bits_avx2.cpp)
endif()
