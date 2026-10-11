# ---------------------------------------------------------------------------
# iclforge::lyrics - timed lyrics: an LRC file (UTF-8 or UTF-16) read into lines of timed words, the
# file's place beside a media item, and the stateless cursor that says which line and word a playing
# position is in (planning/karaoke-phase2.md, W13a; planning/karaoke.md, L1 to L6).
#
# Standard library only, so that Hearth's window, Crucible, the sink firmware and the browser can all
# link it. It was apps/hearth/engine/src/lyrics.* until a second program needed it, and a program
# may not use another (tools/checks/projects.json).
#
# Always STATIC and never installed, like iclforge::audio: no export annotations, and it is not part
# of the distributed package.
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (libs/ac3/minimal.cmake),
# which has no use for the lyrics; a sink's player (ICLFORGE_SENDSPIN_PLAYER_ONLY, set by
# firmware/esp-idf/iclforge/CMakeLists.txt) follows an item's words and needs them.
if((ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER) AND NOT ICLFORGE_SENDSPIN_PLAYER_ONLY)
    return()
endif()

# files.cpp is the part that reads a file: kept apart so that a program that only parses text (the sink
# firmware) does not link the stream and file-system code.
add_library(iclforge_lyrics STATIC src/lyrics.cpp src/timing.cpp src/files.cpp)
add_library(iclforge::lyrics ALIAS iclforge_lyrics)
target_include_directories(iclforge_lyrics PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(iclforge_lyrics PUBLIC cxx_std_23)
target_link_libraries(iclforge_lyrics PRIVATE iclforge::warnings iclforge::coverage)
