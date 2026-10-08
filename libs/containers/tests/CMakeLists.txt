# iclforge-containers-tests - the container writers' and readers' Catch2 tests
# (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-containers-tests PROJECT containers SOURCES
    iec61937/test_iec61937.cpp
    iec61937/test_iec61937_ac4.cpp
    iec61937/test_iec61937_unwrap.cpp
    matroska/test_matroska.cpp
    matroska/test_matroska_reader.cpp
    mp4/test_mp4_reader.cpp
    mpegts/test_mpegts_reader.cpp)

target_link_libraries(iclforge-containers-tests PRIVATE iclforge::containers iclforge::ac3 iclforge::ac4)

# The IAMF writer and reader (libs/containers/src/iamf) are gated by their own ICLFORGE_BUILD_IAMF
# (default ON, see the root CMakeLists.txt's own option()).
if(ICLFORGE_BUILD_IAMF)
    target_sources(iclforge-containers-tests PRIVATE
        iamf/test_iamf.cpp iamf/test_iamf_container.cpp iamf/test_iamf_objects.cpp
        iamf/test_iamf_sequence.cpp)
endif()

# iec61937/test_iec61937_ac4.cpp reads std::getenv, for the census directory it packs - the C4996
# reasoning of libs/ac3/tests' cpu_features.cpp.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(iec61937/test_iec61937_ac4.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
