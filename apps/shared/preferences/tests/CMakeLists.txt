# ---------------------------------------------------------------------------
# iclforge-settings-tests - Catch2 coverage for apps/shared/preferences/src/settings_migration.cpp,
# the copy of what a program stored under its old names (organisation ac3forge) that forge-gui,
# Hearth and Crucible make at start-up, once, before anything reads a setting: old store only, new
# store only, both, neither, a second start, the directories, and the rule that the old store is
# never written. A SEPARATE small binary, as hearth_controller_tests (apps/hearth/engine/tests) is:
# the other test binaries are Qt-free by design, and this needs Qt Core's QSettings and
# QStandardPaths (and no application object; the tests point QSettings at an INI file in a
# temporary directory through QSettings::setPath, the recipe the Qt Quick suites use). Built when
# one of the programs that calls it is.
# ---------------------------------------------------------------------------
if(ICLFORGE_BUILD_GUI OR ICLFORGE_BUILD_CRUCIBLE OR ICLFORGE_BUILD_HEARTH)
    find_package(Qt6 6.8 QUIET COMPONENTS Core)
    if(Qt6_FOUND)
        add_executable(iclforge-settings-tests
            test_settings_migration.cpp
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/settings_migration.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/settings_migration.hpp")
        target_include_directories(iclforge-settings-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
        target_link_libraries(iclforge-settings-tests PRIVATE
            iclforge::warnings iclforge::coverage Qt6::Core Catch2::Catch2WithMain)
        # Windows: this target links Qt6 as shared libraries and has nothing of its own to put them
        # beside it; the flat bin/ it lands in is where one of the programs' windeployqt steps
        # already puts them, so it builds after one of them (hearth_controller_tests, in
        # apps/hearth/engine/tests, does the same and says why).
        if(WIN32)
            foreach(_iclforge_qt_deployer forge-gui hearth crucible)
                if(TARGET ${_iclforge_qt_deployer})
                    add_dependencies(iclforge-settings-tests ${_iclforge_qt_deployer})
                    break()
                endif()
            endforeach()
            unset(_iclforge_qt_deployer)
        endif()
        set_property(DIRECTORY APPEND PROPERTY LABELS app-preferences)
        catch_discover_tests(iclforge-settings-tests ADD_TAGS_AS_LABELS)
    else()
        message(STATUS "Qt6 not found: iclforge-settings-tests (the settings migration) is skipped")
    endif()
endif()
