# ---------------------------------------------------------------------------
# iclforge::ac4dec - an AC-4 decoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts.
#
# A peer of src/ac4 (iclforge::ac4, the bitstream inspector) and, like it,
# independent of iclforge::ac3: AC-4 shares no bitstream syntax with AC-3 or
# E-AC-3, and a decoder that linked iclforge::ac3 would bring the whole codec
# into every AC-4 build. It links iclforge::ac4 for the table of contents and the
# presentation and substream group information, which the inspector already
# reads, and adds everything below the substream index table.
#
# Made by iclforge_add_library() (cmake/IclforgeLibrary.cmake), as src/ac4 is,
# and installed and exported with the inspector and the core as
# iclforge::ac4dec_static and iclforge::ac4dec_shared (cmake/InstallLibrary.cmake).
# ---------------------------------------------------------------------------

# Each library links the inspector of its own kind, PUBLIC, since the
# decoder's header names the inspector's Toc and presentation types (DEPENDS),
# and embeds the core (EMBEDS): the static library passes the core on to
# whatever links it, as a link-only dependency, because nothing in an archive
# links it; the shared library carries the part of it that it uses and passes
# on nothing. The OBJECT library takes both for compiling only.
#
# iclforge::arithmetic is the header-only target that carries
# iclforge/arithmetic/scalar_math.hpp, which src/ac4core's dsp/real_functions.hpp
# includes: the float transcendentals that give the same float on every
# platform, called by pcm/aspx.cpp and pcm/companding.cpp through that header
# (src/ac4dec includes from src/ac4core and not from src/arithmetic,
# tools/checks/layering.json; planning/ac4.md, D14a4).
iclforge_add_library(ac4dec
    SOURCES
        src/decoder.cpp
        src/huffman.cpp
        src/pcm/acpl.cpp
        src/pcm/ajcc.cpp
        src/pcm/ajoc.cpp
        src/pcm/isf.cpp
        src/pcm/asf_reconstruct.cpp
        src/pcm/aspx.cpp
        src/pcm/companding.cpp
        src/pcm/de.cpp
        src/pcm/downmix.cpp
        src/pcm/drc.cpp
        src/pcm/immersive.cpp
        src/pcm/mixer.cpp
        src/pcm/multichannel.cpp
        src/pcm/objects.cpp
        src/pcm/renderer.cpp
        src/pcm/routing.cpp
        src/pcm/snf_random.cpp
        src/pcm/stereo.cpp
        src/pcm/substream_pcm.cpp
        src/presentations.cpp
        src/syntax/asf.cpp
        src/syntax/aspx.cpp
        src/syntax/acpl.cpp
        src/syntax/ajcc.cpp
        src/syntax/ajoc.cpp
        src/syntax/channel_elements.cpp
        src/syntax/metadata.cpp
        src/syntax/oamd.cpp
        src/syntax/presentation.cpp
        src/syntax/ssf.cpp
        src/syntax/substream.cpp
    DEPENDS ac4
    BUILD_TREE_DEPENDS
    EMBEDS iclforge::ac4core
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE "$<BUILD_INTERFACE:iclforge::arithmetic>")

# The translation units a frame's time goes to (the syntax's Huffman and scale factor reading, the
# reconstruction around the transforms, the stereo and downmix passes) at -O2 under the profile's
# ICLFORGE_MINIMAL_HOT_O2, as src/ac4core/CMakeLists.txt does for the kernels (at -O3 there); flash
# and not SRAM. planning/ac4.md, D14e has what it bought on the ESP32-P4 and what it cost.
if(ICLFORGE_MINIMAL_DECODER AND ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties(
        src/decoder.cpp
        src/huffman.cpp
        src/pcm/asf_reconstruct.cpp
        src/pcm/aspx.cpp
        src/pcm/companding.cpp
        src/pcm/downmix.cpp
        src/pcm/drc.cpp
        src/pcm/multichannel.cpp
        src/pcm/renderer.cpp
        src/pcm/stereo.cpp
        src/pcm/substream_pcm.cpp
        src/syntax/asf.cpp
        src/syntax/channel_elements.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()
