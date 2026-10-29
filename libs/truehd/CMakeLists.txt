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
        src/io/access_unit.cpp
        src/io/elementary.cpp
        src/io/major_sync.cpp
    DEPENDS base
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::base_headers>")
