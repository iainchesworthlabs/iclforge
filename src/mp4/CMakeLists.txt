# ---------------------------------------------------------------------------
# iclforge::mp4 - a minimal MP4 (ISOBMFF) container writer, plus fragmented
# MP4/CMAF segmenting and HLS/DASH signaling helpers.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3. It nests ISOBMFF boxes and lays out one audio track's samples,
# taking every frame - and the codec's sample-entry configuration box - as
# opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both this and
# iclforge::ac3, but the codec library never reaches for the container and vice
# versa. Made, as src/matroska is, by iclforge_add_library()
# (cmake/IclforgeLibrary.cmake).
#
# mp4.cpp (mux()) and fragment.cpp (fragment()) share their ISOBMFF
# box-writing plumbing through src/isobmff_detail.hpp; hls.cpp and dash.cpp
# share their segment-duration/bandwidth arithmetic through
# src/manifest_detail.hpp - neither is a public header, both are `inline`
# functions included by more than one .cpp in this same target (see either
# file's own comment).
# ---------------------------------------------------------------------------
iclforge_add_library(mp4
    SOURCES
        src/mp4.cpp
        src/reader.cpp
        src/fragment.cpp
        src/hls.cpp
        src/dash.cpp
    # iclforge::fmt_private compiles {fmt} into hls.cpp and dash.cpp instead of linking it, so
    # iclforge_mp4_static carries no undefined {fmt} reference into a consumer's link - see
    # cmake/Fmt.cmake.
    LINK_PRIVATE "$<BUILD_INTERFACE:iclforge::fmt_private>")
