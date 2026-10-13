# hearth-testsink (planning/hearth-reference-player.md, The test sink): libs/sink as a program, with
# outputs that write a WAV file and decode a burst. The sink is libs/sink's; what is here is a static
# library, so the loopback tests in iclforge-hearth-tests can run sinks in-process, and the executable,
# which adds the command line. Never installed or packaged.
add_library(hearth_testsink STATIC src/burst_output.cpp src/sink.cpp src/wav_output.cpp)
target_include_directories(hearth_testsink PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(hearth_testsink PUBLIC cxx_std_23)
# iclforge::ac4 for the extension role's AC-4 bursts (planning/ac4.md, D11): the
# decoder, and the inspector's sync-frame scan in front of it.
target_link_libraries(hearth_testsink PUBLIC iclforge::sink iclforge::sink_decode iclforge::sendspin
                                            iclforge::ac3 iclforge::ac4 iclforge::containers iclforge::lyrics)
# iclforge::coverage: this library's sources are also linked straight into iclforge-hearth-tests (apps/hearth/engine/tests/CMakeLists.txt),
# and need their own instrumentation for the same reason apps/hearth/engine's own link does -
# --coverage is compile-time, not inherited from whatever later links this archive.
target_link_libraries(hearth_testsink PRIVATE iclforge::warnings iclforge::coverage)

add_executable(hearth-testsink src/main.cpp)
target_link_libraries(hearth-testsink PRIVATE hearth_testsink iclforge::warnings)
