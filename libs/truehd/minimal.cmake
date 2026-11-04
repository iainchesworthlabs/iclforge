# ---------------------------------------------------------------------------
# The minimum-footprint profile's TrueHD archive (ICLFORGE_MINIMAL_TRUEHD, beside the profile's options;
# planning/truehd-parity-esp32-sinks.md): what a decode reads - the CRCs and parities, the format and
# syntax tables, the major sync, the access unit and the decoder - and no encoder, no stream scanner,
# no evolution frame or signing. Included by CMakeLists.txt; iclforge_add_library() makes the static
# archive alone in the profile, with its compile options and code that is not position-independent.
# These are the sources firmware/truehd-probe compiles into its image and measured on the ESP32-C6, -S3
# and -P4.
# ---------------------------------------------------------------------------
iclforge_add_library(truehd
    NO_C4251_SUPPRESSION
    SOURCES
        src/core/crc.cpp
        src/core/format.cpp
        src/core/syntax.cpp
        src/decoder/decoder.cpp
        src/io/access_unit.cpp
        src/io/major_sync.cpp
    # The decoder's zone markers (iclforge/base/detail/profiling.hpp) are the profile's variant of
    # that header, which libs/base names in ICLFORGE_PROFILING_INCLUDE_DIR.
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${ICLFORGE_PROFILING_INCLUDE_DIR}"
    # The profile builds no iclforge::base; the public headers name its bit reader, which is
    # header-only.
    LINK_PUBLIC "$<BUILD_INTERFACE:iclforge::base_headers>")

# -Wno-psabi: GCC's arm-none-eabi and riscv say, once a translation unit and for every std::span
# passed by value, that its passing changed in GCC 7.1 (see libs/ac4/minimal.cmake).
target_compile_options(iclforge_truehd_objects PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>")

# The decoder at -O2 under the profile's ICLFORGE_MINIMAL_HOT_O2, as libs/ac4/minimal.cmake does for its
# hot files: the probe's figures (0.13 of a core for 2.0 at 48 kHz on the ESP32-P4) are -O2.
if(ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties(src/decoder/decoder.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
