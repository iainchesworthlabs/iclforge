# ---------------------------------------------------------------------------
# iclforge-forge-gui-tests - the Qt-free part of forge-gui's own code (cmake/IclforgeTests.cmake):
# its diagnostics module (src/gui_diagnostics.hpp) and the AC-4 encode settings it builds on.
# They compile straight into this binary, so the rule the diagnostics module exists to hold - the
# support file carries no signing key, no environment value and no byte of a loaded source - is
# checked on every CI leg, including the ones that build no window. The window's Qt Quick suite
# (qml.cmake, include()d from ../CMakeLists.txt) covers the other half, that the controller fills
# the report's fields from what the window is actually showing.
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-forge-gui-tests PROJECT forge-gui SOURCES
    test_gui_diagnostics.cpp
    test_ac4_encode_settings.cpp
    test_karaoke_authoring.cpp
    test_karaoke_outputs.cpp
    test_lfe_pins.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/gui_diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_encode_settings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_presentations.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/karaoke_authoring.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_encode_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_objects_core.cpp"
    # The table of a karaoke stream's parts, which karaoke_authoring.cpp and `forge project` share
    # (it has no dependency, so this binary stays free of the project file's JSON).
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/project/src/karaoke_parts.cpp")
target_include_directories(iclforge-forge-gui-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/project/src")
target_link_libraries(iclforge-forge-gui-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4
        iclforge::lyrics
        iclforge::app_text_encoding)
