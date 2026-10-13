# ---------------------------------------------------------------------------
# iclforge-theme-tests - Catch2 coverage for apps/shared/theme/src/system_motion*: which printed
# value of a desktop's animation setting means "reduced" (GNOME's gsettings text, KDE's
# AnimationDurationFactor), and the part of SystemMotion that is a decision - the override that
# stands in for the system, and when reducedChanged is and is not emitted. The operating
# system's own answer is a seam per platform (src/platform/<os>/) and is exercised by running the
# programs; the judgement about its text is held here on every platform, whichever one produced
# it.
#
# A SEPARATE small binary, like iclforge-settings-tests (apps/shared/preferences/tests): the other
# test binaries are Qt-free by design, and this needs Qt Core and Qt Gui (SystemMotion listens to
# QGuiApplication when there is one, and none is created here). Built when one of the programs
# that embeds Theme.qml is. Its Qt DLLs are copied beside it from the targets it links, so it
# neither needs a program built first nor waits for one.
# ---------------------------------------------------------------------------
if(ICLFORGE_BUILD_GUI OR ICLFORGE_BUILD_CRUCIBLE OR ICLFORGE_BUILD_HEARTH)
    find_package(Qt6 6.8 QUIET COMPONENTS Core Gui QmlIntegration)
    if(Qt6_FOUND)
        set(_iclforge_theme_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
        if(WIN32)
            set(_iclforge_theme_platform windows)
        elseif(APPLE)
            set(_iclforge_theme_platform macos)
        elseif(LINUX)
            set(_iclforge_theme_platform linux)
        else()
            set(_iclforge_theme_platform other)
        endif()
        add_executable(iclforge-theme-tests
            test_system_motion.cpp
            "${_iclforge_theme_src}/system_motion.cpp"
            "${_iclforge_theme_src}/system_motion.hpp"
            "${_iclforge_theme_src}/system_motion_parse.hpp"
            "${_iclforge_theme_src}/system_motion_query.hpp"
            "${_iclforge_theme_src}/platform/${_iclforge_theme_platform}/system_motion_query.cpp")
        # SystemMotion is a QObject: moc has to see its header.
        set_target_properties(iclforge-theme-tests PROPERTIES AUTOMOC ON)
        target_include_directories(iclforge-theme-tests PRIVATE "${_iclforge_theme_src}")
        target_link_libraries(iclforge-theme-tests PRIVATE
            iclforge::warnings iclforge::coverage
            Qt6::Core Qt6::Gui Qt6::QmlIntegration Catch2::Catch2WithMain)
        if(APPLE)
            target_link_libraries(iclforge-theme-tests PRIVATE "-framework AppKit" objc)
        endif()
        # Windows: Qt is shared libraries and nothing else puts them beside this binary. Before
        # catch_discover_tests below, which runs the binary when it is built.
        if(WIN32)
            add_custom_command(TARGET iclforge-theme-tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:iclforge-theme-tests> $<TARGET_FILE_DIR:iclforge-theme-tests>
                COMMAND_EXPAND_LISTS)
        endif()
        set_property(DIRECTORY APPEND PROPERTY LABELS app-theme)
        catch_discover_tests(iclforge-theme-tests ADD_TAGS_AS_LABELS)
        unset(_iclforge_theme_src)
        unset(_iclforge_theme_platform)
    else()
        message(STATUS "Qt6 not found: iclforge-theme-tests (the system motion setting) is skipped")
    endif()
endif()
