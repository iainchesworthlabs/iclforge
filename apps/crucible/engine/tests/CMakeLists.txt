# ---------------------------------------------------------------------------
# iclforge-crucible-tests - Crucible's engine core (apps/crucible/engine/src): the slot plan, the
# bed mixer, placement smoothing and the output-mode policy (cmake/IclforgeTests.cmake). All four
# are plain C++ over iclforge::oba types with no platform in them - the platform layer next to
# them is what talks to WASAPI or PipeWire - so they compile into this target on every platform,
# ungated: the rules the window's UI hangs on are worth holding on a CI leg that does not build the
# window itself. The app target (ICLFORGE_BUILD_CRUCIBLE, Windows and Linux) is separate.
#
# The tap pool and the output stage joined them once they were written over
# the AudioDevices seam (apps/crucible/engine/src/audio_devices.hpp): with the
# WASAPI classes behind an interface they are plain C++ over the library's
# codec, packer and decoder, and fake_devices.hpp stands in
# for the machine. platform_audio_devices() itself is declared in
# platform_services.hpp and defined in
# apps/crucible/engine/src/library_devices.cpp, which this target does not
# compile; these tests never call it, and the one place the stage would
# (a null `devices` in its config) is a path the tests do not take, so the
# reference resolves to the stub below on every platform and to the real
# thing in the app.
#
# engine.cpp itself joined them last, with signing_hook.cpp beside it because
# the frame loop owns a SigningHook. It is the same argument one step up: the
# loop is written over the three seams EngineConfig injects (devices,
# sessions, foreground), so with fakes in all three it runs here as plain C++
# on any platform - and until test_engine.cpp nothing in the suite
# constructed an Engine at all, which is how a probe thread that was never
# joined reached a released branch. That file also asserts what the loop does
# about opening taps: the platform seams it would otherwise reach for are the
# ones platform_services_stub.cpp answers, and every case passes fakes
# instead. The one platform piece it does touch is iclforge::audio's DeviceWatcher,
# which is linked below and answers with kNoBackend where there is none; the
# engine notes that and carries on.
#
# test_engine_start.cpp rides the same block for what start()
# answers: that a machine with nothing to play into is refused rather than
# reported as running, which is what the window's status strip prints and
# what the QML suites' "the engine did not start here" skips test for.
#
# test_desktop_entries.cpp and test_translations.cpp are in ../../ui/tests, beside the window's Qt
# Quick tests, but are plain Catch2 over code with no Qt in it (the window's .desktop-entry reader
# is plain C++ over std::filesystem), and ../../ui/tests/CMakeLists.txt is include()d for the Qt
# Quick tests, so this binary takes them from there.
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-crucible-tests PROJECT crucible SOURCES
    test_slots.cpp
    test_bed_mixer.cpp
    test_placement.cpp
    test_output_policy.cpp
    test_tap_pool.cpp
    test_output_stage.cpp
    test_engine_start.cpp
    test_nullsink_position.cpp
    test_platform_seams.cpp
    test_diagnostics.cpp
    ../../ui/tests/test_desktop_entries.cpp
    ../../ui/tests/test_translations.cpp
    test_engine.cpp
    test_engine_commands.cpp
    test_output_stage_edges.cpp
    platform_services_stub.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/slots.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/bed_mixer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/placement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/output_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/tap_pool.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/output_stage.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/signing_hook.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ui/src/desktop_entries.cpp")
# test_engine_commands.cpp saves the signing key's env-var fallbacks with
# std::getenv before clearing them for its own case - same C4996 reasoning as
# libs/base/src/crypto/signing_key.cpp (libs/base/CMakeLists.txt) and
# libs/ac3/tests/core/test_simd_kernels.cpp.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(test_engine_commands.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
# The null-sink driver's position clock (apps/crucible/windows/driver/Source/Main/
# position.h) is a header with no kernel dependencies, kept that way so the
# one piece of the driver that is logic rather than framework plumbing is
# tested here, on every platform, where kernel code could never be. The
# window's .desktop-entry reader (apps/crucible/ui/src/desktop_entries.hpp) rides the
# same target, Qt-free, on every platform.
target_include_directories(iclforge-crucible-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ui/src"
    "${CMAKE_SOURCE_DIR}/apps/crucible/windows/driver/Source/Main")
# The profiling header pair, for engine.cpp's Tracy zones: the same
# generator expression apps/crucible/CMakeLists.txt uses for
# crucible_engine, and for the same reason - the header is PRIVATE to
# iclforge_ac3_objects, so a target that includes it says so itself rather than
# picking it up from iclforge::ac3. Which of the two identically-pathed
# variants is chosen is CMake's decision, never an #ifdef.
target_include_directories(iclforge-crucible-tests PRIVATE
    "$<IF:$<BOOL:${ICLFORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/libs/base/variants/profiling-tracy_enabled,${CMAKE_SOURCE_DIR}/libs/base/variants/profiling-tracy_disabled>")
target_link_libraries(iclforge-crucible-tests PRIVATE iclforge::tracy)

# The Linux Foreground's X11 half has a pure policy (x11_foreground.cpp: when
# a read yields a pid, when a lost display becomes a reason) over an injected
# reader, and the session classification and the process-tree walk beside it
# are headers. None of it needs xcb or a display, so it is tested here with a
# fake reader - on Linux only, the way libs/audio/tests/backend/pipewire rides its
# backend: the files live in a platform directory and are compiled nowhere
# else. Ungated on ICLFORGE_BUILD_CRUCIBLE for the reason the block above
# gives: the rules are worth holding on a leg that never builds the app.
#
# test_session_facts.cpp rides the same block for the same reason, and is why
# proc_facts.hpp exists: the Linux session monitor's /proc readers, its
# per-process fact cache and the two records a refresh builds were all inside
# session_monitor.cpp, which cannot be compiled at all without the PipeWire
# headers - so none of it could be reached from here. Moved into a header
# beside it, none of it needs PipeWire and all of it runs on any Linux
# machine, against a /proc the test writes itself. Unlike x11_foreground.cpp
# below, no file from apps/crucible is added beside it: the header is compiled
# into this case, and the .cpp that includes it stays in crucible_engine
# where libpipewire is.
if(LINUX)
    target_sources(iclforge-crucible-tests PRIVATE
        platform/linux/test_x11_foreground.cpp
        platform/linux/test_session_facts.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/linux/x11_foreground.cpp")
    target_include_directories(iclforge-crucible-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/linux")
endif()

target_link_libraries(iclforge-crucible-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4)

# test_translations.cpp reads the Crucible's six Qt Linguist catalogues as files (no Qt in this
# target), on the same reasoning as the golden data's directories (tests/support/CMakeLists.txt): a
# source-tree path, because ctest runs the binary from the build tree.
target_compile_definitions(iclforge-crucible-tests
    PRIVATE "ICLFORGE_CRUCIBLE_TS_DIR=\"${CMAKE_SOURCE_DIR}/apps/crucible/ui/assets/translations\""
    # The GUI's seven catalogues are held to the same two rules as Crucible's
    # six, from the same parser. They were filled on 2026-09-06 and a rule
    # that holds them there belongs beside the one that holds Crucible's,
    # rather than in a workflow step on one leg.
    PRIVATE "ICLFORGE_GUI_TS_DIR=\"${CMAKE_SOURCE_DIR}/apps/forge/gui/assets/translations\"")
