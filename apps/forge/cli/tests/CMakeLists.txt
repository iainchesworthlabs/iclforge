# ---------------------------------------------------------------------------
# iclforge-forge-cli-tests - Forge's command line (cmake/IclforgeTests.cmake). main.cpp compiles
# straight into the forge executable with everything in an anonymous namespace, so there
# is no library surface for parse_options/gather_frame/run_atmos_encode's own
# logic to link against directly - these tests instead run the real, built
# forge binary as a subprocess and inspect what it actually wrote.
#
# Only added when forge itself is being built (ICLFORGE_BUILD_CLI): the
# top-level CMakeLists.txt processes apps/forge/cli before its tests, so the target
# already exists here whenever it is going to.
# ---------------------------------------------------------------------------
# test_cli_live.cpp rides the same gate and the same ICLFORGE_CLI_EXE: it covers
# the device-facing commands (devices/outputs/record/live/monitor), which no
# test touched at all until coverage floors measured commands/audio_io.cpp and
# commands/live_audio.cpp at 0.0% line coverage. Its cases assert what holds
# with or without an endpoint present - see that file's own header - and carry
# the [concurrency] tag, so the ThreadSanitizer leg reaches them through
# `ctest -L concurrency` (ThreadSanitizer leg).
iclforge_add_test_binary(iclforge-forge-cli-tests PROJECT forge-cli SOURCES
    test_cli.cpp test_cli_ac4.cpp test_cli_ac4_decode.cpp test_cli_ac4_encode.cpp
    test_cli_atmos_cbi.cpp test_cli_atmos_encode_ac4.cpp
    test_cli_containers.cpp
    test_cli_live.cpp test_cli_probe.cpp test_cli_stream_tools.cpp
    test_cli_options.cpp test_cli_synth.cpp test_cli_analysis.cpp
    test_cli_encode_paths.cpp test_cli_stream_edges.cpp
    test_cli_inspect_edges.cpp test_cli_atmos_edges.cpp)
# The same device-facing commands with a device to find: software ALSA
# devices (tests/support/alsa_null_device.hpp), so only in the ALSA configuration.
if(ICLFORGE_AUDIO_BACKEND STREQUAL "alsa")
    target_sources(iclforge-forge-cli-tests PRIVATE test_cli_live_alsa.cpp)
endif()
target_compile_definitions(iclforge-forge-cli-tests PRIVATE "ICLFORGE_CLI_EXE=\"$<TARGET_FILE:forge>\"")
# test_cli_probe.cpp probes the committed FFmpeg/DEE-encoded fixtures as
# well as this encoder's own output - they exercise syntax iclforge never
# emits (AHT, spectral extension, and frames this decoder declines
# outright), which is the only way to check that `probe` describes a FORMAT
# rather than this project's dialect of it. Passed as a path for the same
# reason ICLFORGE_GOLDEN_AUDIO_DIR is: ctest runs the binary from the build
# tree, nowhere near the fixtures.
target_compile_definitions(iclforge-forge-cli-tests PRIVATE
    "ICLFORGE_EXTERNAL_BASELINE_DIR=\"${CMAKE_SOURCE_DIR}/tests/golden/external-baseline\"")
# test_cli.cpp decodes one fuzz seed under `quiet`: the DEE-encoded
# E-AC-3 stream a quiet decode was first seen failing on, whose report
# carries infomdat lines that FFmpeg's encode of the same programme does
# not. A path for the same reason ICLFORGE_EXTERNAL_BASELINE_DIR is one.
target_compile_definitions(iclforge-forge-cli-tests PRIVATE
    "ICLFORGE_FUZZ_SEED_DIR=\"${CMAKE_SOURCE_DIR}/libs/ac3/fuzz/seeds\"")
# test_cli_atmos_encode_ac4.cpp calls the steps of the AC-4 encode that Forge's front ends share
# (apps/shared/media), which compile straight into this binary as they do into forge.
target_sources(iclforge-forge-cli-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_encode_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_objects_core.cpp")
target_include_directories(iclforge-forge-cli-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src")

# test_cli_atmos_adm.cpp (ADM BWF reader phase 3, forge's 'atmos-adm' command) needs the ADM option
# on top of the forge binary this directory is only added for: it runs the real, built forge binary
# as a subprocess (ICLFORGE_CLI_EXE) against an ADM fixture, so the command it exercises only exists
# in that binary at all when ICLFORGE_BUILD_ADM was ALSO on for this same configure
# (apps/forge/cli/CMakeLists.txt's own ICLFORGE_BUILD_ADM guard).
if(ICLFORGE_BUILD_ADM)
    target_sources(iclforge-forge-cli-tests PRIVATE test_cli_atmos_adm.cpp test_cli_atmos_iab.cpp
                                                    test_cli_decode_adm.cpp test_cli_atmos_adm_ac4.cpp)
    # These four build ADM and IAB fixtures with the libraries' own headers.
    target_link_libraries(iclforge-forge-cli-tests PRIVATE iclforge::adm)
    if(TARGET iclforge::iab)
        target_link_libraries(iclforge-forge-cli-tests PRIVATE iclforge::iab)
    endif()
endif()

target_link_libraries(iclforge-forge-cli-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4)
