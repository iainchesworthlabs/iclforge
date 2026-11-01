# ---------------------------------------------------------------------------
# iclforge::sink - a Sendspin sink a program runs (planning/hearth-android.md, D3): it listens on a
# WebSocket and advertises _sendspin._tcp, pairs by its pairing PSK and a dynamic or static code,
# admits servers as the specification ranks them, keeps the identity and the pairing records, answers
# the roles a server lists, and hands each stream it is sent to an output the program supplies. Its
# audio output is the program's: hearth-testsink writes WAV files and decodes bursts, and the Android
# app gives a receiver the bytes (apps/hearth/android).
#
# It was apps/hearth/testsink/src/sink.* and store.* until a second program needed it, and a program
# may not use another (tools/checks/projects.json).
#
# Always STATIC and never installed, like iclforge::sendspin: no export annotations, and not part of
# the distributed package. It listens and takes threads, so a board's build (the player half of
# libs/sendspin alone) and a core-only build leave it out (the root CMakeLists.txt).
# ---------------------------------------------------------------------------
add_library(iclforge_sink STATIC src/sink.cpp src/store.cpp src/lyric_frame.cpp src/status_json.cpp)
add_library(iclforge::sink ALIAS iclforge_sink)
target_include_directories(iclforge_sink PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(iclforge_sink PUBLIC cxx_std_23)
target_link_libraries(iclforge_sink PUBLIC iclforge::sendspin iclforge::lyrics)
# iclforge::coverage: the sink's sources are linked into the Hearth tests (apps/hearth/engine/tests),
# which measure them, and --coverage is compile-time, not inherited from whatever later links this
# archive.
target_link_libraries(iclforge_sink PRIVATE iclforge::warnings iclforge::coverage)

# The decoding output (include/iclforge/sink/decoding_output.hpp): a burst output that decodes AC-3,
# E-AC-3 and AC-4 and renders to a speaker layout, and hands the samples to a PcmSink. A target of
# its own for the reason the pass-through output is: a program that only hands bytes on does not
# link three codecs and a renderer.
add_library(iclforge_sink_decode STATIC src/decoding_output.cpp)
add_library(iclforge::sink_decode ALIAS iclforge_sink_decode)
target_link_libraries(iclforge_sink_decode PUBLIC iclforge::sink iclforge::ac3 iclforge::ac4
                                                  iclforge::containers iclforge::objects iclforge::render
                                                  iclforge::truehd)
target_link_libraries(iclforge_sink_decode PRIVATE iclforge::warnings iclforge::coverage)

# The pass-through output (include/iclforge/sink/passthrough.hpp): a burst output that rebuilds each
# Sendspin burst chunk into the IEC 61937 burst it came from and keeps a link alive with null
# bursts. A target of its own, so that a program that decodes its streams (hearth-testsink) or
# writes them down does not link what only a bitstreaming one needs: the burst rebuild
# (iclforge::containers) and the frame header reader (iclforge::ac3).
add_library(iclforge_sink_passthrough STATIC src/passthrough_output.cpp)
add_library(iclforge::sink_passthrough ALIAS iclforge_sink_passthrough)
target_link_libraries(iclforge_sink_passthrough PUBLIC iclforge::sink iclforge::containers iclforge::ac3)
target_link_libraries(iclforge_sink_passthrough PRIVATE iclforge::warnings iclforge::coverage)

# The pass-through path end to end in one process (include/iclforge/sink/loopback_harness.hpp): a
# server session packs a stream into bursts, a sink with the pass-through output over a model device
# is played them over a loopback WebSocket, and what the device was handed is compared with what the
# packer wrote. Its own target for the app's instrumented test and the host tests; nothing ships it
# but the debug build of the Android app.
add_library(iclforge_sink_harness STATIC src/loopback_harness.cpp)
add_library(iclforge::sink_harness ALIAS iclforge_sink_harness)
target_link_libraries(iclforge_sink_harness PUBLIC iclforge::sink iclforge::sink_passthrough iclforge::sendspin
    iclforge::containers iclforge::ac3)
target_link_libraries(iclforge_sink_harness PRIVATE iclforge::warnings iclforge::coverage)
