# ---------------------------------------------------------------------------
# iclforge-app-cdg-picture-tests - Catch2 coverage for apps/shared/cdg-picture's CdgFrames
# (cmake/IclforgeTests.cmake), the CD+G disc as images by position that Hearth's lyric view,
# Crucible's lyric strip and the Forge GUI's player each hold. It has no library target: its source
# compiles straight into this binary, as it does into each program.
#
# Qt Gui only (QImage), no QGuiApplication and no Qt Quick, so CdgPicture (the item that draws
# the image) and CdgFrameSource are not here: the programs' Qt Quick tests cover them through the
# real controllers (apps/hearth/ui/tests/qml/tst_karaoke_lyrics.qml,
# apps/crucible/ui/tests/qml/tst_lyrics.qml).
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 QUIET COMPONENTS Core Gui)
if(Qt6_FOUND)
    iclforge_add_test_binary(iclforge-app-cdg-picture-tests PROJECT app-cdg-picture SOURCES
        test_cdg_frames.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/cdg_frames.cpp")
    target_include_directories(iclforge-app-cdg-picture-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(iclforge-app-cdg-picture-tests
        PRIVATE
            iclforge::lyrics
            Qt6::Core Qt6::Gui)
    # Windows only: this target links Qt6 as shared libraries and has nothing of its own to put
    # them next to it. CMake's global RUNTIME_OUTPUT_DIRECTORY puts this exe in the flat bin/
    # beside the program whose POST_BUILD windeployqt step already deploys exactly those DLLs, so
    # depending on that program reuses the deployment (apps/hearth/engine/tests/CMakeLists.txt's
    # hearth_controller_tests, whose catch_discover_tests() listing step died with
    # STATUS_DLL_NOT_FOUND without it).
    if(WIN32)
        foreach(_iclforge_deployer IN ITEMS hearth crucible)
            if(TARGET ${_iclforge_deployer})
                add_dependencies(iclforge-app-cdg-picture-tests ${_iclforge_deployer})
                break()
            endif()
        endforeach()
    endif()
else()
    message(STATUS "Qt6 not found: iclforge-app-cdg-picture-tests (apps/shared/cdg-picture) is skipped")
endif()
