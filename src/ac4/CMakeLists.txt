# ---------------------------------------------------------------------------
# iclforge::ac4 - an AC-4 sync-frame / TOC / presentation / substream-group
# bitstream inspector.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows
# nothing about AC-3, E-AC-3 or Atmos - AC-4 is a peer codec, not an
# extension of this project's own one, so it gets the same "codec-blind"
# treatment iclforge::mpegts, iclforge::mp4 and iclforge::matroska get. It
# reads TOC/presentation/substream-group framing and reports audio_data and
# metadata() payloads as byte ranges, never decoding them - see
# include/ac4/ac4.hpp for the full scope statement. Installed and exported with
# the decoder and the core (cmake/InstallLibrary.cmake).
#
# iclforge_add_library() (cmake/IclforgeLibrary.cmake) makes the OBJECT library,
# the static and shared wrappers, the aliases and the export header. In the
# minimum-footprint profile (ICLFORGE_MINIMAL_AC4, planning/ac4.md, D14b) it
# makes the static archive alone, with the profile's compile options and code
# that is not position-independent.
# ---------------------------------------------------------------------------
iclforge_add_library(ac4
    SOURCES
        src/ac4.cpp)
