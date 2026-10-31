# hearth-player (docs/hearth/player.md): libs/sink's sink as a program that plays through the
# computer's output device. The library is what iclforge-hearth-tests links, so the loopback and
# scheduling tests run without a device; the executable adds the command line. Never installed
# or packaged.
add_library(hearth_player STATIC src/frame_device.cpp src/player.cpp src/scheduled_sink.cpp)
target_include_directories(hearth_player PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(hearth_player PUBLIC cxx_std_23)
target_link_libraries(hearth_player PUBLIC iclforge::sink iclforge::sink_decode iclforge::sendspin iclforge::audio
                                          iclforge::ac3 iclforge::render fmt::fmt)
# iclforge::coverage: these sources are also linked straight into iclforge-hearth-tests, for the
# reason apps/hearth/testsink/CMakeLists.txt gives.
# player.cpp reads the computer's name and the user's data directory from the environment with
# std::getenv - the C4996 reasoning of apps/hearth/engine/tests/CMakeLists.txt's.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(src/player.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
target_link_libraries(hearth_player PRIVATE iclforge::warnings iclforge::coverage)

add_executable(hearth-player src/main.cpp)
target_link_libraries(hearth-player PRIVATE hearth_player iclforge::warnings)
