# ---------------------------------------------------------------------------
# iclforge-app-media-tests - Catch2 coverage for apps/shared/media (cmake/IclforgeTests.cmake), the
# code Forge's two front ends and Hearth's engine share. It has no library target: its sources
# compile straight into this binary, as they do into each program.
#
# RecordingSink (test_recording_sink.cpp) is deliberately Qt-free - std:: and the container
# libraries only, exactly so its streamed-equals-one-shot contract can be held here, in the plain
# Catch2 suite, without a QML engine in the room (Forge GUI's Qt Quick tests cover the interactive
# surfaces; this covers the bytes). The fragmented-MP4 cases exercise Fmp4FolderWriter,
# RecordingSink's own kFmp4 backend, so its source comes along too. Both front ends write their
# takes through them, so the bytes they produce are worth checking in a CLI-only build as well:
# this directory is present whatever ICLFORGE_BUILD_GUI says.
#
# container_input.cpp (container readers (mkv/mp4/ts)) joined the same way for the same reason:
# iclforge::apps::sniff_container/elementary_stream_from_bytes are what
# decode/qc/levels/play/monitor and the GUI's QC/Inspect pickers all call, so
# a sniffing regression belongs in the plain Catch2 suite rather than only in
# the Python encoder-space fuzzer that first found one (REGRESSION_SEEDS'
# 3600083275727211684 in tools/ci/fuzz_encoder_space.py) - test_container_input.cpp covers it
# deterministically and without needing ffmpeg on the machine running it.
# stream_playback.cpp joined for the reason its own header gives: 'monitor'
# and 'spatial' pick their decoder and drain the end-of-stream flush through
# it, and neither command reaches its decode loop without a render device.
# associated_mix.cpp joined for the same reason: 'decode' and 'monitor' choose an associated
# service and pair its units with the main's through it, and 'monitor' cannot get past opening
# a render device on a headless CI leg (test_associated_mix.cpp).
# sink_wait.hpp is header-only: 'play', 'monitor', 'identify' and 'live' wait on their sinks
# through it, and test_sink_wait.cpp holds those waits against sinks that stop on cue. These three
# tests were in libs/ac3/tests and libs/audio/tests until the code they test moved (C7-2).
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-app-media-tests PROJECT app-media SOURCES
    test_ac4_objects_core.cpp
    test_associated_mix.cpp
    test_container_input.cpp
    test_recording_sink.cpp
    test_sink_wait.cpp
    test_stream_playback.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/fmp4_folder_writer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/recording_sink.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/container_input.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/stream_playback.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/associated_mix.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_encode_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_objects_core.cpp")
target_include_directories(iclforge-app-media-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(iclforge-app-media-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4)
