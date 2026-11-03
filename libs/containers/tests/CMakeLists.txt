# iclforge-containers-tests - the container writers' and readers' Catch2 tests
# (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-containers-tests PROJECT containers SOURCES
    iec61937/test_iec61937.cpp
    iec61937/test_iec61937_ac4.cpp
    iec61937/test_iec61937_parts.cpp
    iec61937/test_iec61937_unwrap.cpp
    matroska/test_matroska.cpp
    matroska/test_matroska_reader.cpp
    mp4/test_mp4_reader.cpp
    mpegts/test_mpegts_reader.cpp)

target_link_libraries(iclforge-containers-tests PRIVATE iclforge::containers iclforge::ac3 iclforge::ac4)

# mp4/test_mp4_reader.cpp reads the protected MP4s that are also the MP4 fuzz harness's seeds
# (tools/generators/gen_cenc_reader_fixtures.py makes them). A path, for the reason the CLI tests'
# ICLFORGE_FUZZ_SEED_DIR is one: ctest runs the binary from the build tree.
target_compile_definitions(iclforge-containers-tests PRIVATE
    "ICLFORGE_MP4_SEED_DIR=\"${CMAKE_SOURCE_DIR}/libs/containers/fuzz/seeds/fuzz_mp4_demux\"")

# The IAMF writer and reader (libs/containers/src/iamf) are gated by their own ICLFORGE_BUILD_IAMF
# (default ON, see the root CMakeLists.txt's own option()).
if(ICLFORGE_BUILD_IAMF)
    target_sources(iclforge-containers-tests PRIVATE
        iamf/test_iamf.cpp iamf/test_iamf_container.cpp iamf/test_iamf_objects.cpp
        iamf/test_iamf_coded.cpp iamf/test_iamf_scalable.cpp iamf/test_iamf_sequence.cpp
        iamf/test_iamf_seeds.cpp)
    target_compile_definitions(iclforge-containers-tests PRIVATE
        "ICLFORGE_IAMF_SEED_DIR=\"${CMAKE_SOURCE_DIR}/libs/containers/fuzz/seeds/fuzz_iamf_parse\"")

    # fuzz_iamf_parse's seeds, made by the module's own writers (tools/fuzz/generate-seeds.sh runs it).
    add_executable(iclforge-iamf-seed-tool iamf/iamf_seed_tool.cpp)
    target_link_libraries(iclforge-iamf-seed-tool PRIVATE iclforge::containers iclforge::warnings iclforge::fmt)
    add_dependencies(iclforge-tests iclforge-iamf-seed-tool)
endif()

# iec61937/test_iec61937_ac4.cpp reads std::getenv, for the census directory it packs - the C4996
# reasoning of libs/ac3/tests' cpu_features.cpp. iec61937/test_iec61937_parts.cpp reads it for the
# file its hidden vector-writing case names.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(iec61937/test_iec61937_ac4.cpp iec61937/test_iec61937_parts.cpp
                                PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()

# TrueHD in the containers (planning/truehd.md, phase T2c): the tests read the committed FFmpeg fixtures
# (ICLFORGE_GOLDEN_TRUEHD_DIR) and meet the truehd library only through truehd/truehd_stream.hpp, because the
# library itself knows no codec. iclforge-truehd-container-tool is the same code as a command line, which
# tools/checks/verify_truehd_containers.py runs against FFmpeg.
if(TARGET iclforge::truehd)
    target_sources(iclforge-containers-tests PRIVATE
        iec61937/test_mat.cpp
        matroska/test_matroska_truehd.cpp
        mp4/test_mp4_truehd.cpp
        mpegts/test_mpegts_truehd.cpp)
    target_link_libraries(iclforge-containers-tests PRIVATE iclforge::truehd)

    add_executable(iclforge-truehd-container-tool truehd/truehd_container_tool.cpp)
    target_link_libraries(iclforge-truehd-container-tool
        PRIVATE iclforge::containers iclforge::truehd iclforge::warnings iclforge::fmt)
    target_include_directories(iclforge-truehd-container-tool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(iclforge-tests iclforge-truehd-container-tool)
endif()
