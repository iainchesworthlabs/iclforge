# ---------------------------------------------------------------------------
# iclforge::iab - a standalone reader for SMPTE ST 2098-2:2022's Immersive
# Audio Bitstream (IAB), phase 1 of 3 - a
# codec-blind bitstream parser, the same shape iclforge::matroska, iclforge::mp4
# and iclforge::mpegts already use for their own containers: it links nothing
# from iclforge::ac3 and knows nothing about AC-3, E-AC-3 or JOC, so it can be
# reasoned about, tested and reused on its own (see src/mpegts/CMakeLists.txt's
# own header, which this file mirrors structurally).
#
# Unlike iclforge::adm (src/adm), this module has no third-party dependency at
# all - IAB's own element/Plex-coded bitstream needs nothing beyond this
# module's own bit reader (src/bitreader.hpp) - so, like the three container
# writers, it defaults ON rather than OFF, and (also unlike iclforge::adm) has no
# opt-in vcpkg feature of its own yet: it is new in this PR and nothing
# downstream (CLI, GUI, examples) consumes it yet - IAB reader's phase 3
# (`atmos-iab`, mapping onto iclforge::adm) is what will eventually give it
# a consumer. Following iclforge::c's own precedent (see
# packaging/vcpkg-port/iclforge/portfile.cmake's header comment), it is left
# out of the vcpkg port until it has one.
#
# Built and installed as both a static and a shared library, by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(iab
    NO_C4251_SUPPRESSION
    SOURCES
        src/bitreader.cpp
        src/dlc.cpp
        src/dlc_encoder.cpp
        src/iab_writer.cpp
        src/iab_reader.cpp
        src/iab.cpp
        src/mxf_reader.cpp
        src/mxf_writer.cpp)
