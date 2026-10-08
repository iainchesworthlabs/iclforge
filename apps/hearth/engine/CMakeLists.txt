# hearth_engine (planning/hearth-reference-player.md, A3): the player's
# engine, with no Qt in it. The application's window (A5) will be a client of
# this library rather than the place any of it lives, the same split
# apps/crucible/{engine,ui} already keeps - and the same reason: everything
# here can then be tested by iclforge-tests on every CI leg, including the legs
# with no display and no sound card.
add_library(hearth_engine STATIC
    src/ac3_transcoder.cpp
    src/ac4_stream.cpp
    src/decoder_settings.cpp
    src/device_sink.cpp
    src/diagnostic_log.cpp
    src/diagnostics_report.cpp
    src/engine_thread.cpp
    src/media_info.cpp
    src/media_inspector.cpp
    src/network_view.cpp
    src/output_decision.cpp
    src/output_selector.cpp
    src/passthrough_sink.cpp
    src/play_meters.cpp
    src/player.cpp
    src/queue.cpp
    src/session.cpp
    src/settings_model.cpp
    src/stream_decoder.cpp
    src/transport.cpp
    src/unit_reports.cpp)
target_include_directories(hearth_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(hearth_engine PUBLIC cxx_std_23)

# src/native_log_sink.hpp's one function, exactly one platform/<os>/*.cpp
# defining it, no #ifdef selecting between them - the same shape
# apps/crucible/engine/src/platform_services.hpp's own comment states for
# platform_session_monitor(). Windows' is the only one that needs
# <windows.h>, hence the same WIN32_LEAN_AND_MEAN/NOMINMAX pair Crucible's
# own WIN32 block already sets.
if(WIN32)
    target_sources(hearth_engine PRIVATE src/platform/windows/native_log_sink.cpp)
    target_compile_definitions(hearth_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_sources(hearth_engine PRIVATE src/platform/posix/native_log_sink.cpp)
endif()

# The pairing records are a key ring for libs/sendspin's server sessions, which
# a core-only build leaves out, so they are left out with them, and the server
# identity those records are bound to with them. NetworkSinks (discovery and
# pairing, A6) needs the same server half - ServerHost and the computer's own
# mdns backend - plus PairingStore itself. src/network_group_sink.cpp is the same
# reason: it wraps iclforge::sendspin::Group.
if(NOT ICLFORGE_SENDSPIN_CORE_ONLY)
    target_sources(hearth_engine PRIVATE src/pairing_store.cpp src/server_identity.cpp src/network_sinks.cpp
                                            src/network_group_sink.cpp)
    # A Hearth sink's firmware (planning/esp32-ota.md, O5): the board's own HTTP routes over
    # cpp-httplib, configured as libs/sendspin configures it, and the board's own headers for what
    # GET /firmware says and what an image must be (esp-idf/iclforge/include, free of ESP-IDF,
    # as iclforge-tests already reads them), so that this app holds an image to the rules the board
    # holds it to.
    target_sources(hearth_engine PRIVATE src/sink_firmware.cpp src/sink_firmware_view.cpp)
    target_include_directories(hearth_engine PUBLIC "${PROJECT_SOURCE_DIR}/esp-idf/iclforge/include")
    # The opt-in loopback diagnostics endpoint (src/diagnostics_server.hpp): the
    # same httplib already linked in for src/sink_firmware.cpp above, so it adds
    # nothing new to a core-only build to leave out.
    target_sources(hearth_engine PRIVATE src/diagnostics_server.cpp)
    target_link_libraries(hearth_engine PRIVATE iclforge_sendspin_httplib)
endif()

# From apps/shared/media/src, which has no library target: the container facts an item's
# loader hands over (container_input.hpp, whose functions the application's
# loader compiles), probe's JSON stream summary, which the media information
# writes as forge probe does, and Ac4ObjectRenderer (planning/ac4.md, I5),
# the same iclforge::render::LayoutRenderer-based panner forge's own 'decode'
# plays AC-4 objects through, compiled in here so the engine's AC-4 path can
# take it up too.
target_sources(hearth_engine PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/shared/media/src/probe_json.cpp"
    "${PROJECT_SOURCE_DIR}/apps/shared/media/src/ac4_object_render.cpp")
target_include_directories(hearth_engine PUBLIC "${PROJECT_SOURCE_DIR}/apps/shared/media/src")

# iclforge::audio for the output side (PcmOutput, PassthroughSink, the device
# records the output decision reads), iclforge::ac3 for the decoder and the
# renderer, iclforge::ac4 for an AC-4 item's table of contents and iclforge::ac4 for
# its presentations and metadata (probe_json's AC-4 walk), and iclforge::sendspin
# for the JSON writer the media information is written with, the key ring the
# pairing records implement (src/pairing_store.hpp), discovery and pairing
# themselves (src/network_sinks.hpp), and Group, the burst/PCM streaming path a
# network output plays through (src/network_group_sink.hpp). iclforge::ac3 asks
# whether frames carry an authenticity tag.
target_link_libraries(hearth_engine PUBLIC iclforge::audio iclforge::ac3 iclforge::ac4 iclforge::sendspin)
# iclforge::coverage (coverage floors, tools/checks/coverage_report.sh's "apps/hearth" row) instruments
# this target's OWN sources - apps/forge/cli/src's own comment on its matching link explains why the
# instrumented EXECUTABLE that eventually links this library is not enough on its own: --coverage
# is a compile-time flag, so without this a coverage build's .cpp files here would produce no
# .gcno at all and read as no data rather than as low coverage.
target_link_libraries(hearth_engine PRIVATE iclforge::warnings iclforge::fmt iclforge::ac3 iclforge::coverage)
# The engine thread (src/engine_thread.cpp).
find_package(Threads REQUIRED)
target_link_libraries(hearth_engine PUBLIC Threads::Threads)

# The profiling header pair, PUBLIC so the window (ui/, linked PRIVATE below
# it in ../ui/CMakeLists.txt) and hearth_qmltests (which links this same
# library) both resolve every `#include "iclforge/base/detail/profiling.hpp"` they
# carry - apps/crucible/CMakeLists.txt's own crucible_engine target sets up
# the identical pair, for the identical reason (its own comment there has the
# history of why this lives at the engine level rather than per-platform or
# per-executable).
target_link_libraries(hearth_engine PUBLIC iclforge::tracy)
target_include_directories(hearth_engine PUBLIC
    "$<IF:$<BOOL:${ICLFORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/libs/base/variants/profiling-tracy_enabled,${CMAKE_SOURCE_DIR}/libs/base/variants/profiling-tracy_disabled>")
