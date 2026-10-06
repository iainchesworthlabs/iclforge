# ---------------------------------------------------------------------------
# iclforge::ac4 - the AC-4 codec, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts: the bitstream inspector, the
# decoder and the encoder, one library laid out by AC-3's areas
# (planning/consolidation.md, C1):
#
#   core/     the table of contents and the presentation types (toc.cpp), the
#             tables generated from the ETSI attachments and the transforms both
#             directions compute alike (tables/, dsp/, and a directory per
#             tool's shared kernel), the bit reader and the bit writer
#   io/       the elementary stream (sync frames, the splitter) and the
#             carriage: dac4, the timing a container needs, the codec string,
#             the manifests
#   decoder/  syntax/ reads bits, pcm/ reconstructs
#   encoder/  by tool: asf/, aspx/, acpl/, ajcc/, ajoc/, oamd/, frame/
#
# A peer of iclforge::ac3 and independent of it: AC-4 shares no bitstream
# syntax with AC-3 or E-AC-3. The decoder's reader and the encoder's writer
# stay separate transcriptions of the syntax tables, in opposite directions.
# The core's headers are private (src/core), as the core library's were never
# installed; the public headers are iclforge/ac4/core/, io/, decoder/ and
# encoder/.
#
# Made by iclforge_add_library() (cmake/IclforgeLibrary.cmake). The
# minimum-footprint profile (ICLFORGE_MINIMAL_AC4, planning/ac4.md, D14b)
# builds the decode-only archive from a narrower list of the same sources
# (minimal.cmake), as src/ac3 does; the encoder has no such archive, being
# built for no ESP32 part (decision 34).
# ---------------------------------------------------------------------------

# The decoder: syntax/ reads the substreams, pcm/ reconstructs them, and oba/
# gives object audio metadata its meaning and renders the objects and the
# intermediate spatial format.
set(_ac4_decoder_sources
    src/decoder/decoder.cpp
    src/decoder/huffman.cpp
    src/decoder/pcm/acpl.cpp
    src/decoder/pcm/ajcc.cpp
    src/decoder/pcm/ajoc.cpp
    src/decoder/pcm/asf_reconstruct.cpp
    src/decoder/pcm/aspx.cpp
    src/decoder/pcm/companding.cpp
    src/decoder/pcm/de.cpp
    src/decoder/pcm/downmix.cpp
    src/decoder/pcm/drc.cpp
    src/decoder/pcm/immersive.cpp
    src/decoder/pcm/mixer.cpp
    src/decoder/pcm/multichannel.cpp
    src/decoder/pcm/renderer.cpp
    src/decoder/pcm/routing.cpp
    src/decoder/pcm/snf_random.cpp
    src/decoder/pcm/stereo.cpp
    src/decoder/pcm/substream_pcm.cpp
    src/decoder/presentations.cpp
    src/decoder/syntax/asf.cpp
    src/decoder/syntax/aspx.cpp
    src/decoder/syntax/acpl.cpp
    src/decoder/syntax/ajcc.cpp
    src/decoder/syntax/ajoc.cpp
    src/decoder/syntax/channel_elements.cpp
    src/decoder/syntax/metadata.cpp
    src/decoder/syntax/oamd.cpp
    src/decoder/syntax/presentation.cpp
    src/decoder/syntax/ssf.cpp
    src/decoder/syntax/substream.cpp
    src/oba/isf.cpp
    src/oba/objects.cpp)

# The encoder, and the bit writer its syntax writes through.
set(_ac4_encoder_sources
    src/encoder/acpl/acpl_encoder.cpp
    src/encoder/acpl/acpl_syntax.cpp
    src/encoder/ajcc/ajcc_syntax.cpp
    src/encoder/ajoc/ajoc_encoder.cpp
    src/encoder/ajoc/ajoc_syntax.cpp
    src/encoder/ajoc/downmix.cpp
    src/encoder/asf/analysis.cpp
    src/encoder/asf/coder.cpp
    src/encoder/asf/layout.cpp
    src/encoder/asf/multichannel.cpp
    src/encoder/asf/psycho.cpp
    src/encoder/asf/stereo.cpp
    src/encoder/aspx/aspx_encoder.cpp
    src/encoder/aspx/aspx_syntax.cpp
    src/core/bit_writer.cpp
    src/encoder/encoder.cpp
    src/encoder/frame/dialogue.cpp
    src/encoder/frame/drc_gains.cpp
    src/encoder/frame/frame_writer.cpp
    src/encoder/frame/metadata.cpp
    src/encoder/frame/sync_frame.cpp
    src/encoder/frame/timing.cpp
    src/encoder/frame/toc_writer.cpp
    src/encoder/oamd/object_metadata.cpp
    src/encoder/oamd/oamd_syntax.cpp)

# The table of contents, the elementary stream and the carriage: what was the
# inspector, which the decoder reads the table of contents through
# (parse_raw_frame()).
set(_ac4_inspector_sources
    src/core/toc.cpp
    src/io/carriage.cpp
    src/io/elementary.cpp)

# What the decoder and the encoder compute alike: the tables generated from the
# ETSI attachments, and the transforms (planning/ac4.md, "Where the libraries
# live"). Everything in it is in namespace iclforge::ac4::detail.
set(_ac4_kernel_sources
    src/core/acpl/acpl.cpp
    src/core/ajcc/ajcc.cpp
    src/core/ajoc/ajoc.cpp
    src/core/aspx/frequency_tables.cpp
    src/core/aspx/hf_generator.cpp
    src/core/dsp/fft.cpp
    src/core/dsp/kbd.cpp
    src/core/dsp/mdct.cpp
    src/core/dsp/qmf.cpp
    src/core/dsp/resampler.cpp
    src/core/dsp/synthesis.cpp
    src/core/dsp/transform_tables.cpp
    src/core/tables/huffman_codes.cpp
    src/core/tables/huffman_tables.cpp
    src/core/tables/isf_tables.cpp
    src/core/tables/noise_tables.cpp
    src/core/tables/qmf_tables.cpp
    src/core/tables/sfb_tables.cpp
    src/core/tables/ssf_tables.cpp)

# The scalar the kernels are explicitly instantiated at (planning/ac4.md, D14a:
# "AC-4 joins ICLFORGE_DECODE_SCALAR"): the root's one option for every codec,
# by its tier. "fixed" is iclforge::internal::Fixed32, the ESP32-C6's and -C3's
# (planning/ac4.md, D14d).
#
# ICLFORGE_AC4_ALSO_AT_DOUBLE: the encoder calls straight into a handful of these
# same kernels (Mdct, QmfAnalysis, QmfSynthesis, Resampler,
# ajoc::Reconstruction, aspx::generate_high_band) always at double - decision
# 34 (planning/ac4.md), the encoder never runs on an ESP32 and has no float or
# fixed-point tier of its own, so it does not follow ICLFORGE_DECODE_SCALAR.
# iclforge::ac3 avoids this by compiling its encoder's and decoder's DSP
# separately (ICLFORGE_ENCODE_SCALAR beside ICLFORGE_DECODE_SCALAR,
# src/ac3/CMakeLists.txt); here one set of kernels serves both, so when Real is
# not already double, each of those kernels' .cpp files also explicitly
# instantiates <double>. That is done through a macro the per-tier
# iclforge/ac4/detail/real.hpp defines (variants/decode-scalar-<tier>/, chosen
# just below): it passes its argument through where Real is float and drops it
# where Real is already double (a second explicit instantiation of the same
# specialization would be ill-formed), so no source file asks which build it is
# with a preprocessor conditional (tools/checks/check_platform_macros.ps1's
# rule). The default, double, build is unchanged: one instantiation.
#
# The markers' "iclforge/base/detail/profiling.hpp" is iclforge::base's: nothing
# in an ordinary build, a Tracy zone under ICLFORGE_ENABLE_TRACY, and in the
# minimum-footprint profile under ICLFORGE_STAGE_TIMERS the two functions the
# application supplies. The profile builds no iclforge::base, so the directory
# comes from src/base by variable (ICLFORGE_PROFILING_INCLUDE_DIR).
#
# iclforge::base_headers (src/base): Fixed32, MantExp and the project's own
# float transcendentals, decision 31's "no copies", the same on every platform.
set(_ac4_private_includes
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/variants/decode-scalar-${ICLFORGE_DECODE_SCALAR_TIER}"
    "${ICLFORGE_PROFILING_INCLUDE_DIR}")
set(_ac4_link_private
    "$<BUILD_INTERFACE:iclforge::base_headers>"
    "$<BUILD_INTERFACE:iclforge::tracy>")

# tests/CMakeLists.txt compiles the kernels a second time into iclforge-tests in a shared build, where
# libiclforge_ac4.so exports none of them.
list(TRANSFORM _ac4_kernel_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE ICLFORGE_AC4_KERNEL_SOURCES)
set(ICLFORGE_AC4_KERNEL_SOURCES "${ICLFORGE_AC4_KERNEL_SOURCES}" PARENT_SCOPE)

if(ICLFORGE_MINIMAL_DECODER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/minimal.cmake")
else()
    iclforge_add_library(ac4
        SOURCES
            ${_ac4_decoder_sources}
            ${_ac4_encoder_sources}
            ${_ac4_inspector_sources}
            ${_ac4_kernel_sources}
        PRIVATE_INCLUDES ${_ac4_private_includes}
        LINK_PRIVATE ${_ac4_link_private})
endif()

# -Wdouble-promotion as an error on the float tier (planning/ac4.md, D14a): an
# implicit float-to-double promotion anywhere in the kernels is exactly the
# unmeasured cost a single-precision-FPU target (the ESP32-P4/S3) cannot afford,
# so it is a build failure rather than a silent one. GCC/Clang only - MSVC has no
# equivalent warning, and clang-cl parses MSVC-style arguments so a bare
# -Wdouble-promotion there would need the same /clang: escape the top-level
# CMakeLists.txt's -ffp-contract=off does; the kernels are checked under
# upstream clang-cl too (tu_compile.ps1), so the portable warning set is not
# skipped, only this one MSVC-shaped spelling of it. APPEND, since minimal.cmake
# sets the hot kernels' options before this.
if(ICLFORGE_DECODE_SCALAR_TIER STREQUAL "float32" AND
   (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
    (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT
     CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")))
    set_property(SOURCE ${_ac4_kernel_sources} APPEND PROPERTY
        COMPILE_OPTIONS -Wdouble-promotion -Werror=double-promotion)
endif()

# The float tables of the sample rate converter are built by the compiler, in
# src/core/dsp/resampler.cpp and in no other translation unit and only in a
# build whose scalar is float, and the Q1.30 tables alike only in one whose
# scalar is fixed (planning/ac4.md, D14d) (core/dsp/resampler_design.hpp: the
# three ratios the decoder uses, 1001/960 the long one, 47,094 coefficients each
# designed with a Kaiser window and a sinc, planning/ac4.md, D14a5). A
# compiler's limit on what its constant evaluator may do in one constant
# expression is set for small computations and this is not one: the three tables
# take 6 to 10 million steps in MSVC, 10.6 million in Clang and 76 to 78 million
# operations in GCC 14 and 15 (32 million in GCC 16, which counts them
# differently), and the defaults are 100,000 steps, 1,048,576 steps and
# 33,554,432 operations. Each compiler has its own flag for it, and the limits
# below are three to eleven times what the tables need. The evaluation makes the
# file take 5 to 14 seconds longer to compile, by compiler: clang-cl 5.9 s where
# it took 0.8, GCC 16 6.0 where 0.8, GCC 14 6.1 where 1.1, ESP-IDF's RISC-V GCC
# 15 8.4 where 1.5, Clang 22 8.8 where 1.0 and MSVC 14.7 where 0.7
# (planning/ac4.md, D14a5); a double build pays nothing. The ESP-IDF component
# reaches this file through the root CMakeLists.txt, so the same limits apply
# there. src/core/dsp/transform_tables.cpp narrows the inverse transform's
# tables (core/dsp/transform_tables.hpp) to the tier's scalar the same way:
# about 13,000 doubles, well inside the same limits.
if(ICLFORGE_DECODE_SCALAR_TIER STREQUAL "float32" OR ICLFORGE_DECODE_SCALAR_TIER STREQUAL "fixed32")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_ac4_constexpr_limit "/constexpr:steps67108864")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
           CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl accepts /constexpr:steps and ignores it, with a warning; its own
        # option goes through the /clang: escape, as -ffp-contract=off does.
        set(_ac4_constexpr_limit "/clang:-fconstexpr-steps=67108864")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_ac4_constexpr_limit "-fconstexpr-steps=67108864")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_ac4_constexpr_limit "-fconstexpr-ops-limit=268435456")
    endif()
    if(_ac4_constexpr_limit)
        set_property(SOURCE src/core/dsp/resampler.cpp src/core/dsp/transform_tables.cpp APPEND
            PROPERTY COMPILE_OPTIONS "${_ac4_constexpr_limit}")
    endif()
endif()

# -Wnull-dereference false positive, GNU only: GCC 16 at -O3 (Release) flags
# PortionWriter's copies of its held ObjectCodes, whose std::optional members
# it traces into <optional>'s _M_copy_assign, the libstdc++ category
# cmake/CompilerWarnings.cmake describes. Scoped to the one file.
set_source_files_properties(src/encoder/oamd/object_metadata.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")
