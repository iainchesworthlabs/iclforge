# ---------------------------------------------------------------------------
# iclforge::containers - a standalone reader and writer for AOM's IAMF (Immersive Audio
# Model and Formats) v2.0.0: the OBU syntax (IA Sequence Header, Codec Config,
# Audio Element, Mix Presentation, Parameter Block, Audio Frame, Temporal
# Delimiter and Metadata OBUs), the standalone raw OBU stream, and the ISO-BMFF
# encapsulation as a whole file or as movie fragments. It writes `ipcm`
# substreams for channel-based and object-based Audio Elements; E-AC-3 is not in
# IAMF's codec list (Opus, AAC-LC, FLAC, LPCM) and never can be, so the
# channel-based route from E-AC-3 is a decode-then-rewrap bridge. See
# include/iclforge/iamf/iamf.hpp for the full picture.
#
# Codec-blind by design, the same shape iclforge::containers,
# iclforge::containers and iclforge::iab already use for their own formats: it
# links nothing from iclforge::ac3 and knows nothing about AC-3, E-AC-3 or JOC
# (see CONTRIBUTING.md's repository-layout section on what a bare
# `include/iamf/` prefix, with no `ac3/`, means).
#
# Like iclforge::iab (src/iab), this module has no third-party dependency of its
# own - it writes IAMF's OBU/ISOBMFF syntax directly against the published
# spec, the same way iclforge::iab hand-rolls its own bit reader for SMPTE ST
# 2098-2 - so, like the three container writers and iclforge::iab, it defaults ON
# rather than OFF (unlike iclforge::adm, src/adm, whose opt-in/excluded-from-
# install treatment exists specifically because IT vendors third-party
# Boost/libadm code - a reason that does not apply here). It has no opt-in
# vcpkg feature of its own yet, following iclforge::iab's own precedent: new in
# this PR, nothing downstream (CLI, GUI, examples beyond mux_iamf.cpp)
# consumes it yet.
#
# Built and installed as both a static and a shared library, by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(iamf
    SOURCES
        src/container.cpp
        src/containers/src/iamf.cpp
        src/sequence_common.cpp
        src/sequence_read.cpp
        src/sequence_write.cpp)
