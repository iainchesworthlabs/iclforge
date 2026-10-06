# ---------------------------------------------------------------------------
# iclforge::containers - a minimal MPEG-2 Transport Stream muxer.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3 beyond the two stream_type/descriptor choices iclforge::ac3::io::StreamKind
# maps onto. It packetizes PAT + PMT + one PES-wrapped elementary stream into
# 188-byte TS packets, taking every access unit as opaque bytes, so it can be
# reasoned about, tested and reused on its own - the same shape as
# iclforge::containers (src/containers/src/matroska/), which this file mirrors structurally.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library, by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(mpegts
    SOURCES
        src/containers/src/mpegts.cpp
        src/reader.cpp)
