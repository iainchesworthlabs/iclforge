# iclforge-audio-tests - the live-audio library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-audio-tests PROJECT audio SOURCES
    test_audio_backend.cpp
    test_live_positions.cpp
    test_monitor_live.cpp
    test_passthrough_live.cpp
    test_pcm_output.cpp
    test_playback_counter.cpp
    test_render_devices.cpp
    test_resampler.cpp
    test_ring_buffer.cpp
    test_spatial_live.cpp
    test_speakers.cpp
    test_watchdog.cpp)

target_link_libraries(iclforge-audio-tests PRIVATE iclforge::audio iclforge::ac3 iclforge::containers)

# ---------------------------------------------------------------------------
# The audio backend's own tests, on the same axis the library selects it on.
#
# libs/audio/CMakeLists.txt compiles exactly one src/backend/<backend>/ directory
# and publishes which one; a backend with device-free logic worth testing gets
# a matching backend/<backend>/ directory here, added only in the build
# that compiled it. So the suite can never test a backend the library does not
# contain, and no test source needs to ask which platform it is on.
#
# test_audio_backend.cpp above is the other half and is unconditional: it holds
# the contract EVERY backend keeps, including the one that refuses everything.
# ---------------------------------------------------------------------------
if(ICLFORGE_AUDIO_BACKEND STREQUAL "alsa")
    target_sources(iclforge-audio-tests PRIVATE
        backend/alsa/test_alsa_device_names.cpp
        backend/alsa/test_alsa_eld_parsing.cpp
        # Software ALSA devices (tests/support/alsa_null_device.hpp): the backend's
        # classes against alsa-lib's null/file/route plugins, so their success paths run
        # without a card.
        test_alsa_null_backend.cpp)
    # The backend's internal header. Not part of the library's public
    # interface and deliberately not installed - it is on the include path for
    # this target only, and only in this configuration.
    target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_BACKEND_DIR}")
elseif(ICLFORGE_AUDIO_BACKEND STREQUAL "pipewire")
    target_sources(iclforge-audio-tests PRIVATE backend/pipewire/test_pipewire_support.cpp)
    target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_BACKEND_DIR}")
    # test_pipewire_support.cpp includes pipewire_support.hpp directly, which
    # in turn includes PipeWire's own headers - the same PkgConfig::PipeWire
    # target libs/audio/CMakeLists.txt linked iclforge_audio against, needed again
    # here since this is a separate translation unit in a separate target.
    target_link_libraries(iclforge-audio-tests PRIVATE PkgConfig::PipeWire)
elseif(ICLFORGE_AUDIO_BACKEND STREQUAL "android")
    # A desktop-hosted test binary never actually selects the android backend (it
    # only exists under NDK cross-compilation, which this target is not) -
    # this branch is future-proofing should that ever change, matching the
    # alsa branch's shape exactly rather than assuming android can never
    # reach here.
    target_sources(iclforge-audio-tests PRIVATE backend/android/test_android_support.cpp)
    target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_BACKEND_DIR}")
elseif(ICLFORGE_AUDIO_BACKEND STREQUAL "macos")
    # Unlike alsa/android above, this branch DOES run on a desktop-hosted
    # test binary for real: libs/audio/CMakeLists.txt selects macos/
    # unconditionally under APPLE, so every macOS CI leg's test build
    # compiles and runs this file.
    target_sources(iclforge-audio-tests PRIVATE backend/macos/test_macos_support.cpp)
    target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_BACKEND_DIR}")
elseif(ICLFORGE_AUDIO_BACKEND STREQUAL "windows")
    # Like macos above, this branch runs on a desktop-hosted test binary for real:
    # libs/audio/CMakeLists.txt selects windows/ unconditionally under WIN32,
    # so every Windows CI leg's test build compiles and runs this file.
    target_sources(iclforge-audio-tests PRIVATE backend/windows/test_windows_support.cpp)
    target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_BACKEND_DIR}")
endif()

# test_live_positions.cpp needs UdpSocket (libs/audio/src/net/udp_socket.hpp)
# to send itself test datagrams over real loopback - internal to iclforge::audio,
# not installed, on the include path the same way a backend's own internal
# header is above (ICLFORGE_AUDIO_NET_DIR, published by libs/audio/CMakeLists.txt
# next to ICLFORGE_AUDIO_BACKEND_DIR). No separate ws2_32 link is needed on
# Windows: iclforge_audio already carries that requirement for any static-library
# consumer, this binary included.
target_include_directories(iclforge-audio-tests PRIVATE "${ICLFORGE_AUDIO_NET_DIR}")

# ../../../apps/shared/media/tests/test_sink_wait.cpp's subject is Forge's shared code (apps/common/sink_wait.hpp, header-only):
# 'play', 'monitor', 'identify' and 'live' wait on their sinks through it, and this holds those
# waits against sinks that stop on cue. It moves to that code's own tests with it
# (planning/monorepo.md, C7-2).
target_sources(iclforge-audio-tests PRIVATE ../../../apps/shared/media/tests/test_sink_wait.cpp)
target_include_directories(iclforge-audio-tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/shared/media/src")
