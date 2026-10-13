# ---------------------------------------------------------------------------
# iclforge::pitch - a fundamental-frequency tracker for a singing voice, a reference melody as a contour
# of pitch on a grid, and a pitch score of one against the other (planning/karaoke-microphone.md, D7 to
# D10; findings F5).
#
# Standard library only, so that Hearth, the browser build and the sink firmware can all link it: the
# board's microphone path is a later step and wants the same arithmetic. The tracker is the McLeod Pitch
# Method written from the paper; nothing here is third-party code.
#
# Always STATIC and never installed, like iclforge::lyrics: no export annotations, and it is not part of
# the distributed package.
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (libs/ac3/minimal.cmake).
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

add_library(iclforge_pitch STATIC
    src/bleed.cpp
    src/reference.cpp
    src/scorer.cpp
    src/synth.cpp
    src/tracker.cpp)
add_library(iclforge::pitch ALIAS iclforge_pitch)
target_include_directories(iclforge_pitch PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(iclforge_pitch PUBLIC cxx_std_23)
target_link_libraries(iclforge_pitch PRIVATE iclforge::warnings iclforge::coverage)
