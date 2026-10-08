# ---------------------------------------------------------------------------
# iclforge-hearth-tests - Hearth's engine and test sink (cmake/IclforgeTests.cmake), and
# hearth_controller_tests, the small Qt binary beside it. Added by the root CMakeLists.txt under
# ICLFORGE_BUILD_HEARTH, the same shape as the ADM tests.
#
# The engine (apps/hearth/engine) comes first because it needs nothing from
# libs/sendspin's player half: a core-only build still builds and tests it,
# unlike the test sink further down.
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-hearth-tests PROJECT hearth SOURCES
    test_ac3_transcoder.cpp
    test_ac4_engine.cpp
    test_decoder_settings.cpp
    test_device_sink_live.cpp
    test_diagnostics.cpp
    test_engine.cpp
    test_engine_queue_commands.cpp
    test_media_info.cpp
    test_media_info_json.cpp
    test_network_view.cpp
    test_output_decision.cpp
    test_output_selector.cpp
    test_play_meters.cpp
    test_player.cpp
    test_player_bitstream.cpp
    test_queue.cpp
    test_session.cpp
    test_settings_model.cpp
    test_stream_decoder.cpp
    test_transport.cpp)
target_link_libraries(iclforge-hearth-tests PRIVATE hearth_engine)
# hearth_engine does not compile container_input.cpp (apps/hearth/engine/src/session.hpp: the
# application supplies the container readers), so it compiles into this binary, as it does into
# the program, for what test_media_info.cpp and test_player.cpp call and media_info.cpp names.
target_sources(iclforge-hearth-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/apps/shared/media/src/container_input.cpp")
# -Wnull-dereference false positive, GNU only: GCC at -O3 inlines std::operator==(const string&,
# const char*) into a CHECK that compares a title against a literal ("b"), follows it down to
# traits_type::length on the literal's pointer, and reports a null dereference at
# basic_string.h:1165. The full -Wnull-dereference chain names Catch2's decomposer and the
# literal's own type (const char (&)[2]), so the pointer it worries about is a string constant -
# never null. Restructuring the test away from it was tried first and changes nothing, since the
# comparison itself is what gets inlined; scoped to these two files rather than widened, for the
# reason libs/ac3/tests/CMakeLists.txt gives for io/test_metadata_edit.cpp.
set_source_files_properties(test_queue.cpp test_transport.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")
# test_ac4_engine.cpp takes a directory of further AC-4 streams from std::getenv, as
# libs/ac4/tests/decoder/test_api.cpp does - the same C4996 reasoning as that file's.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(test_ac4_engine.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()

# hearth_controller_tests - Catch2 coverage for hearth_controller.cpp's
# QML-translation layer (issue #886, part 2: queue_row(), media_info_to_map()
# and friends - see apps/hearth/ui/tests/test_hearth_controller.cpp's own header for
# the full rationale). A SEPARATE small binary rather than folding into
# iclforge-hearth-tests above: that one is Qt-free by design (see gui_diagnostics.cpp's
# and the Crucible engine sources' own comments elsewhere), and
# hearth_controller.cpp needs Qt Core (QVariantMap/QString) plus a
# QObject-derived class that also touches Qt Gui/Qml - never Quick,
# QuickControls2 or Widgets, and never a live QGuiApplication instance. The test is in
# ../../ui/tests, beside the window's Qt Quick tests, whose CMakeLists.txt is include()d.
# Gated on ICLFORGE_SENDSPIN_CORE_ONLY the same way apps/hearth/CMakeLists.txt
# itself gates add_subdirectory(ui): apps/hearth/ui does not exist at all
# under that build (its own comment: the window "needs more than [the] core"),
# so there is nothing here to compile against. find_package() is called
# directly rather than reusing apps/hearth/ui/CMakeLists.txt's own
# ICLFORGE_HEARTH_UI_QT_FOUND: that PARENT_SCOPE variable stops at
# apps/hearth/CMakeLists.txt's own scope (it is never re-exported past that
# one level), so it does not reach this file's scope as a plain variable read.
if(NOT ICLFORGE_SENDSPIN_CORE_ONLY)
    find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Qml)
    if(Qt6_FOUND)
        add_executable(hearth_controller_tests
            ../../ui/tests/test_hearth_controller.cpp
            "${CMAKE_SOURCE_DIR}/apps/hearth/ui/src/hearth_controller.hpp"
            "${CMAKE_SOURCE_DIR}/apps/hearth/ui/src/item_loader.cpp"
            "${CMAKE_SOURCE_DIR}/apps/hearth/ui/src/item_loader.hpp"
            "${CMAKE_SOURCE_DIR}/apps/shared/media/src/container_input.cpp"
            "${CMAKE_SOURCE_DIR}/apps/shared/media/src/container_input.hpp")
        # hearth_controller.cpp itself is NOT listed as a source: it is
        # #include'd directly by test_hearth_controller.cpp (that file's own
        # header explains why - internal-linkage functions with no header to
        # declare them), so listing it here too would compile it twice and
        # fail at link time with duplicate HearthController symbols. AUTOMOC
        # still needs ON to satisfy the QObject vtable HearthController's
        # methods pull in once compiled (even though no test here ever
        # constructs one) - CMake's dependency scan finds hearth_controller.hpp
        # transitively through the #include chain from the listed .cpp/.hpp
        # sources above.
        set_target_properties(hearth_controller_tests PROPERTIES AUTOMOC ON)
        target_include_directories(hearth_controller_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/apps/hearth/ui/src"
            "${CMAKE_SOURCE_DIR}/apps/shared/media/src")
        # A DEE AC-4 stream, for media_ac4_to_map()'s case.
        target_compile_definitions(hearth_controller_tests PRIVATE
            "ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR=\"${CMAKE_SOURCE_DIR}/tests/golden/external-baseline\"")
        target_link_libraries(hearth_controller_tests PRIVATE
            hearth_engine
            iclforge::containers
            iclforge::warnings iclforge::fmt iclforge::coverage
            Qt6::Core Qt6::Gui Qt6::Qml
            Catch2::Catch2WithMain)
        # Windows only: this target links Qt6 as shared libraries
        # (Qt6Core(d).dll etc.) with nothing of its own to put them next
        # to it, and CMake's global RUNTIME_OUTPUT_DIRECTORY (set at the
        # repo root - every test/CLI executable lands in a flat bin/,
        # windows-native-build-incantation's own finding) puts this exe
        # there alongside hearth.exe, whose own POST_BUILD windeployqt
        # step (apps/hearth/ui/CMakeLists.txt) already deploys exactly
        # those DLLs into that same directory. Depending on hearth
        # here reuses that existing deployment rather than adding a
        # second windeployqt pass - real fix for a real, reproduced
        # failure: catch_discover_tests()'s own POST_BUILD test-listing
        # step launched this exe before any Qt DLL had been deployed
        # anywhere near it and got Windows' STATUS_DLL_NOT_FOUND
        # (0xc0000135), which read as "Error listing tests from
        # executable" with no further detail.
        if(WIN32 AND TARGET hearth)
            add_dependencies(hearth_controller_tests hearth)
        endif()
        # Same rationale as apps/forge/gui/tests/qml.cmake's identical loop:
        # Qt's generated moc output for THIS target compiles under the
        # strict warning set unless excluded.
        get_target_property(ICLFORGE_HEARTH_CTRLTEST_SOURCES hearth_controller_tests SOURCES)
        set(ICLFORGE_HEARTH_CTRLTEST_GENERATED_SOURCES "")
        foreach(source IN LISTS ICLFORGE_HEARTH_CTRLTEST_SOURCES)
            if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
                list(APPEND ICLFORGE_HEARTH_CTRLTEST_GENERATED_SOURCES "${source}")
            endif()
        endforeach()
        if(ICLFORGE_HEARTH_CTRLTEST_GENERATED_SOURCES)
            set_source_files_properties(${ICLFORGE_HEARTH_CTRLTEST_GENERATED_SOURCES}
                PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
        endif()
        catch_discover_tests(hearth_controller_tests ADD_TAGS_AS_LABELS)
    else()
        message(STATUS "Qt6 not found: hearth_controller_tests (hearth_controller.cpp coverage) is skipped")
    endif()
endif()

if(NOT ICLFORGE_SENDSPIN_CORE_ONLY)
    # Hearth's test sink, run in process (apps/hearth/testsink), the server's half of A4's
    # exit with aiosendspin 9.1.1 (tools/sendspin/aiosendspin_exit.py), and A6's own
    # (tools/sendspin/aiosendspin_group_exit.py) - a group with the scripted player as one
    # of its members, played from the app's own Engine.
    target_sources(iclforge-hearth-tests PRIVATE
        test_aiosendspin.cpp
        test_aiosendspin_group.cpp
        test_group.cpp
        test_testsink.cpp
        test_testsink_edges.cpp)
    # The engine's pairing records, its Network page bookkeeping (A6), its
    # NetworkGroupSink (A6, streaming to a group) and issue #874's own
    # exit - a group of two test sinks played from iclforge::hearth::Engine
    # itself - all of which need libs/sendspin's server half, which a
    # core-only build leaves out.
    target_sources(iclforge-hearth-tests PRIVATE
        test_pairing_store.cpp
        test_network_sinks.cpp
        test_network_sinks_rows.cpp
        test_network_live.cpp
        test_server_host_dialling.cpp
        test_server_identity.cpp
        test_network_group_sink.cpp
        test_engine_network_group.cpp
        test_sink_firmware.cpp)
    # A sink's firmware routes against a stand-in board on 127.0.0.1, which is a cpp-httplib
    # server, so compiled on its own with cpp-httplib configured as libs/sendspin configures
    # it - the reason libs/sendspin/tests gives for test_websocket.cpp.
    add_library(iclforge_tests_sink_firmware_board OBJECT test_sink_firmware_board.cpp)
    target_link_libraries(iclforge_tests_sink_firmware_board
        PRIVATE hearth_engine iclforge::sendspin iclforge_sendspin_httplib iclforge::warnings iclforge::coverage Catch2::Catch2)
    target_link_libraries(iclforge-hearth-tests PRIVATE iclforge_tests_sink_firmware_board)
    # The opt-in diagnostics endpoint (diagnostics_server.hpp) against a real bound server -
    # same reason, same isolation as iclforge_tests_sink_firmware_board just above.
    add_library(iclforge_tests_diagnostics_server OBJECT test_diagnostics_server.cpp)
    target_link_libraries(iclforge_tests_diagnostics_server
        PRIVATE hearth_engine iclforge_sendspin_httplib iclforge::warnings iclforge::coverage Catch2::Catch2)
    target_link_libraries(iclforge-hearth-tests PRIVATE iclforge_tests_diagnostics_server)
    target_link_libraries(iclforge-hearth-tests PRIVATE hearth_testsink)
    # test_aiosendspin.cpp takes the player's address from std::getenv - same C4996 reasoning
    # as libs/ac3/tests/core/test_simd_kernels.cpp's. test_aiosendspin_group.cpp's own environment() helper
    # takes the same address the same way, test_network_live.cpp its sinks' names, and
    # test_sink_firmware_board.cpp's live case its board and image.
    if(ICLFORGE_IS_MSVC_LIKE)
        set_source_files_properties(
            test_aiosendspin.cpp test_aiosendspin_group.cpp test_network_live.cpp
            test_sink_firmware_board.cpp
            PROPERTIES COMPILE_OPTIONS "/wd4996")
    endif()
endif()
target_link_libraries(iclforge-hearth-tests PRIVATE iclforge::sendspin)

target_link_libraries(iclforge-hearth-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4)
