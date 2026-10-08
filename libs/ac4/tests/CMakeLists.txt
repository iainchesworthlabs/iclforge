# iclforge-ac4-tests - the AC-4 codec's Catch2 tests (cmake/IclforgeTests.cmake).
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _ac4_root)

iclforge_add_test_binary(iclforge-ac4-tests PROJECT ac4 SOURCES
    core/test_toc.cpp
    core/test_presentation_configs.cpp
    io/test_splitter.cpp
    core/test_toc_syntax.cpp
    core/test_acpl.cpp
    core/test_acpl_exact.cpp
    core/test_ajoc.cpp
    core/test_aspx.cpp
    core/test_mant_exp.cpp
    decoder/test_api.cpp
    decoder/test_decoder.cpp
    decoder/test_frames.cpp
    decoder/test_syntax.cpp
    encoder/test_encoder.cpp
    encoder/test_frame_rates.cpp
    encoder/test_immersive.cpp
    encoder/test_metadata.cpp
    encoder/test_objects.cpp
    encoder/test_presentations.cpp
    encoder/test_rates.cpp)

target_link_libraries(iclforge-ac4-tests PRIVATE iclforge::ac4 iclforge::ac3 iclforge::base_headers)

# decoder/test_{asf,metadata,presentation,channel_elements}.cpp reach the
# AC-4 decoder's syntax functions (libs/ac4/src/decoder/syntax/) to hold each branch of
# the syntax to a hand-built bitstream - the committed streams reach only the
# branches DEE writes - and the other tests in the block below reach its PCM
# functions (libs/ac4/src/decoder/pcm/) and the encoder's syntax writers
# (libs/ac4/src/encoder/). Same pattern as libs/ac3/tests' private headers, with
# one difference: those headers are inline, and these functions are compiled into
# iclforge_ac4_objects under CXX_VISIBILITY_PRESET hidden. With BUILD_SHARED_LIBS=ON,
# iclforge::ac4 is libiclforge_ac4.so, which exports none of them, so these tests link
# only against the static library and are built only when that is what iclforge::ac4
# names. They test the syntax and the arithmetic, not the linkage: every static leg
# runs them, and the shared-libs pass (config-linux-llvm-shared) still runs the tests
# of the public API, test_{decoder,frames,syntax}.cpp and
# test_encoder.cpp, against the real .so file. Linking the static library here
# instead would let those bind to a static copy too, and stop testing the .so at all.
#
# The library's private headers are spelled from libs/ac4/src (core/, decoder/,
# encoder/), with dsp's tiered kernels (libs/dsp/src, tiered/) and their scalar tier's
# iclforge/dsp/tiered/real.hpp beside them. The kernels the decoder and the encoder
# share (libs/ac4/src/core and libs/dsp/src/tiered: the tables and the transforms,
# ICLFORGE_AC4_KERNEL_SOURCES) are compiled into this target a second time in a shared
# build, as libs/ac3/tests compiles cpu_features.cpp: libs/ac4/tests/core/ and
# libs/dsp/tests/tiered/ hold them to their own bounds in every build, and
# libiclforge_ac4.so exports none of them.
target_include_directories(iclforge-ac4-tests PRIVATE
    "${_ac4_root}/src"
    "${PROJECT_SOURCE_DIR}/libs/dsp/src"
    "${PROJECT_SOURCE_DIR}/libs/dsp/variants/decode-scalar-${ICLFORGE_DECODE_SCALAR_TIER}")
if(BUILD_SHARED_LIBS)
    target_sources(iclforge-ac4-tests PRIVATE ${ICLFORGE_AC4_KERNEL_SOURCES})
endif()
if(NOT BUILD_SHARED_LIBS)
    target_sources(iclforge-ac4-tests PRIVATE
        decoder/constructed.cpp
        decoder/hsf.cpp
        decoder/mux.cpp
        decoder/objects.cpp
        decoder/test_acpl.cpp
        decoder/test_acpl_exact.cpp
        decoder/test_asf.cpp
        decoder/test_aspx.cpp
        decoder/test_channel_elements.cpp
        decoder/test_constructed.cpp
        decoder/test_de.cpp
        decoder/test_downmix.cpp
        decoder/test_drc.cpp
        decoder/test_ehfr.cpp
        decoder/test_hsf.cpp
        decoder/test_immersive.cpp
        decoder/test_metadata.cpp
        decoder/test_multichannel.cpp
        decoder/test_noise_fill.cpp
        decoder/test_object_render.cpp
        decoder/test_objects.cpp
        decoder/test_pcm.cpp
        decoder/test_presentation.cpp
        decoder/test_presentations.cpp
        decoder/test_reconstruct_exact.cpp
        decoder/test_renderer.cpp
        decoder/test_ssf.cpp
        encoder/test_acpl.cpp
        encoder/test_aspx.cpp
        encoder/test_multichannel.cpp
        encoder/test_presentations_race.cpp
        encoder/test_writer.cpp
        # decoder/test_object_render.cpp's subject is Forge's shared code (apps/common), compiled
        # in as source; it moves to that code's own tests with it (planning/monorepo.md, C7-2).
        "${PROJECT_SOURCE_DIR}/apps/common/ac4_object_render.cpp")
    target_include_directories(iclforge-ac4-tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/common")
    target_link_libraries(iclforge-ac4-tests PRIVATE iclforge::objects iclforge::render)
endif()

# decoder/test_syntax.cpp reads std::getenv, for its trace and census directories, and so do
# decoder/test_constructed.cpp, test_hsf.cpp, test_objects.cpp and
# test_presentations.cpp, for where to write their streams (and
# test_objects.cpp for Chromium's A-JOC stream, and
# test_object_render.cpp for where to write the streams to listen to),
# decoder/test_api.cpp, for a directory of further streams to decode,
# encoder/test_presentations.cpp, for where to write its streams,
# encoder/test_objects.cpp, for where to write its streams and those to
# listen to, and encoder/test_presentations_race.cpp, for the gold set's -
# the C4996 reasoning of libs/ac3/tests' cpu_features.cpp.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(decoder/test_syntax.cpp
        decoder/test_constructed.cpp decoder/test_objects.cpp decoder/test_presentations.cpp
        decoder/test_api.cpp decoder/test_object_render.cpp decoder/test_hsf.cpp
        encoder/test_objects.cpp encoder/test_presentations.cpp
        encoder/test_presentations_race.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
