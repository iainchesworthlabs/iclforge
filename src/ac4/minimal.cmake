# ---------------------------------------------------------------------------
# The minimum-footprint profile's AC-4 archive (ICLFORGE_MINIMAL_AC4, beside
# the profile's options; planning/ac4.md, D14b): what a decode reads, and no
# encoder. Included by CMakeLists.txt, which holds the source lists and the
# options every build of the library takes; iclforge_add_library() makes the
# static archive alone in the profile, with its compile options and code that
# is not position-independent.
# ---------------------------------------------------------------------------
iclforge_add_library(ac4
    SOURCES
        ${_ac4_decoder_sources}
        ${_ac4_inspector_sources}
        ${_ac4_kernel_sources}
    PRIVATE_INCLUDES ${_ac4_private_includes}
    LINK_PRIVATE ${_ac4_link_private})

# -Wno-psabi: GCC's arm-none-eabi says, once a translation unit and for every
# std::span passed by value, that its passing changed in GCC 7.1. A note about an
# ABI nothing here links against, hundreds of them in the log of a leg whose
# output is the numbers.
target_compile_options(iclforge_ac4_objects PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>")

# The units a frame's time goes to, under the profile's ICLFORGE_MINIMAL_HOT_O2
# (named for the AC-3 and E-AC-3 decoders' -O2 in src/ac3/minimal.cmake, whose
# switch this is). A size-optimised build is what the profile is; these are the
# files a stage timer showed the optimiser paying for, and it costs flash and
# not SRAM (planning/ac4.md, D14e has what it bought on the ESP32-P4 and what it
# cost).
#
# The kernels of a frame's transform and filter at -O3: on the ESP32-P4's
# in-order core -O3 beat -O2 on these by 2.7 ms a 5.1 frame (it unrolls the
# filter and transform loops and interleaves their independent multiplies, which
# hides the latency of the core's floating-point unit that a rolled loop
# exposes) for 9 KB of flash; the arithmetic is the same operation for
# operation, which the board's PCM hashes checked
# (docs/platforms/bare-metal/esp32-p4.md).
#
# The decoder's (the syntax's Huffman and scale factor reading, the
# reconstruction around the transforms, the stereo and downmix passes) at -O2:
# -O3 gained nothing there and cost 73 KB.
if(ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties(
        src/core/dsp/fft.cpp
        src/core/dsp/mdct.cpp
        src/core/dsp/qmf.cpp
        src/core/dsp/resampler.cpp
        src/core/dsp/synthesis.cpp
        src/core/aspx/hf_generator.cpp
        src/core/acpl/acpl.cpp
        PROPERTIES COMPILE_OPTIONS "-O3")
    set_source_files_properties(
        src/decoder/decoder.cpp
        src/decoder/huffman.cpp
        src/decoder/pcm/asf_reconstruct.cpp
        src/decoder/pcm/aspx.cpp
        src/decoder/pcm/companding.cpp
        src/decoder/pcm/downmix.cpp
        src/decoder/pcm/drc.cpp
        src/decoder/pcm/multichannel.cpp
        src/decoder/pcm/renderer.cpp
        src/decoder/pcm/stereo.cpp
        src/decoder/pcm/substream_pcm.cpp
        src/decoder/syntax/asf.cpp
        src/decoder/syntax/channel_elements.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()
