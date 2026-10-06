# ---------------------------------------------------------------------------
# iclforge::ac4 - an AC-4 encoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts (planning/ac4.md, phase E1 on).
#
# A peer of src/ac4dec, made the same way by iclforge_add_library()
# (cmake/IclforgeLibrary.cmake). It links iclforge::ac4, since its own header
# describes what it writes with the inspector's Toc and syntax trace, and embeds
# the shared core iclforge::ac4core: the forward transform, the windows and the
# codebooks; both as src/ac4dec does, for the reasons it gives. Its syntax
# writer is a transcription of the tables of its own, separate from the
# decoder's reader. Installed and exported with the inspector, the decoder and
# the core as iclforge::ac4_static and iclforge::ac4_shared
# (cmake/InstallLibrary.cmake).
# ---------------------------------------------------------------------------
iclforge_add_library(ac4enc
    SOURCES
        src/acpl/acpl_encoder.cpp
        src/acpl/acpl_syntax.cpp
        src/ajcc/ajcc_syntax.cpp
        src/ajoc/ajoc_encoder.cpp
        src/ajoc/ajoc_syntax.cpp
        src/ajoc/downmix.cpp
        src/asf/analysis.cpp
        src/asf/coder.cpp
        src/asf/layout.cpp
        src/asf/multichannel.cpp
        src/asf/psycho.cpp
        src/asf/stereo.cpp
        src/aspx/aspx_encoder.cpp
        src/aspx/aspx_syntax.cpp
        src/bit_writer.cpp
        src/encoder.cpp
        src/frame/dialogue.cpp
        src/frame/drc_gains.cpp
        src/frame/frame_writer.cpp
        src/frame/metadata.cpp
        src/frame/sync_frame.cpp
        src/frame/timing.cpp
        src/frame/toc_writer.cpp
        src/oamd/object_metadata.cpp
        src/oamd/oamd_syntax.cpp
    DEPENDS ac4
    BUILD_TREE_DEPENDS
    EMBEDS iclforge::ac4
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src")

# -Wnull-dereference false positive, GNU only: GCC 16 at -O3 (Release) flags
# PortionWriter's copies of its held ObjectCodes, whose std::optional
# members it traces into <optional>'s _M_copy_assign, the libstdc++ category
# cmake/CompilerWarnings.cmake describes. Scoped to the one file.
set_source_files_properties(src/oamd/object_metadata.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")
