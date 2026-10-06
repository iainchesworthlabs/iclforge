# ---------------------------------------------------------------------------
# iclforge::matroska - a minimal MKV container writer and reader.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3. It nests EBML elements and lays out clusters, taking every frame
# as opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library, by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(matroska
    SOURCES
        src/matroska.cpp
        src/reader.cpp)
