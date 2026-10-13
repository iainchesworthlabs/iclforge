# Adds the question "has the operating system been asked to cut back animation?" to a Qt program
# that embeds the family's Theme.qml: SystemMotion, the QML singleton Theme.reducedMotion binds to,
# and the one platform file that knows where each system keeps the answer.
#
#   iclforge_add_system_motion(<target>)
#
# Call it on the executable right after qt_add_executable() and before qt_add_qml_module(), the way
# the program lists apps/shared/theme/src/system_theme.cpp: a QML_ELEMENT is registered only by a
# target that compiles it, so every program and every test binary that embeds Theme.qml needs it,
# and a binary without it fails at run time with "SystemMotion is not defined", not at link time.
#
# One directory per platform under apps/shared/theme/src/platform/, selected here and never by a
# preprocessor conditional (CONTRIBUTING.md, "One subdirectory per platform audio backend"). macOS
# answers from a plain .cpp through the Objective-C runtime, so it adds no OBJCXX language setup to
# the programs that do not already have one; it needs AppKit (every Qt GUI program on macOS has it
# loaded) and libobjc named at link time.
function(iclforge_add_system_motion target)
    set(theme_src "${CMAKE_SOURCE_DIR}/apps/shared/theme/src")
    if(WIN32)
        set(platform windows)
    elseif(APPLE)
        set(platform macos)
    elseif(LINUX)
        set(platform linux)
    else()
        set(platform other)
    endif()
    target_sources(${target} PRIVATE
        "${theme_src}/system_motion.cpp"
        "${theme_src}/system_motion.hpp"
        "${theme_src}/system_motion_parse.hpp"
        "${theme_src}/system_motion_query.hpp"
        "${theme_src}/platform/${platform}/system_motion_query.cpp")
    target_include_directories(${target} PRIVATE "${theme_src}")
    if(APPLE)
        target_link_libraries(${target} PRIVATE "-framework AppKit" objc)
    endif()
endfunction()
