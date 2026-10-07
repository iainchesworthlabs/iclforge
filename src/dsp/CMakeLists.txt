# ---------------------------------------------------------------------------
# iclforge::dsp - the signal-processing pieces more than one library uses: the radix-2 FFT kernel, the
# 64-band complex QMF bank, the offline sample-rate converter and the second-order filter sections.
# It knows no codec. AC-3's 512-sample MDCT stays with AC-3.
#
# src/tiered/ holds the kernels written against a decode tier's scalar (iclforge::dsp::tiered:
# AC-4's mixed-radix FFT, MDCT and KBD windows, its QMF banks and synthesis, its sample rate
# converter, and the tables they read; planning/consolidation.md, C5). This library is built at no
# tier, so it lists none of them: the codec that calls them builds them at its tier
# (src/ac4/CMakeLists.txt), with the tier's scalar from variants/decode-scalar-<tier>/. Making the
# two FFTs, the two QMF banks and the two converters one each is a decision of its own, since each
# moves output (planning/consolidation.md, decision 14).
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (src/ac3/minimal.cmake),
# so no library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(dsp
    SOURCES
        src/biquad.cpp
        src/fft.cpp
        src/qmf.cpp
        src/resampler.cpp
    DEPENDS base
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::base_headers>")
