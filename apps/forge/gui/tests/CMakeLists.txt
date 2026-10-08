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
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/gui_diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_encode_settings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/ac4_presentations.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_encode_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src/ac4_objects_core.cpp")
target_include_directories(iclforge-forge-gui-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/media/src")
target_link_libraries(iclforge-forge-gui-tests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::containers
        iclforge::ac4)
