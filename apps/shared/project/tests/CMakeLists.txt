# ---------------------------------------------------------------------------
# iclforge-app-project-tests - Catch2 coverage for apps/shared/project, the project file's document
# and the command line a project stands for, which the Forge GUI and `forge` share
# (docs/forge/gui/projects.md). It has no library target: its sources compile straight into this
# binary, as they do into each program, and it is Qt-free by design - that is what lets `forge`
# read the same file the window writes.
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-app-project-tests PROJECT app-project SOURCES
    test_project_command.cpp
    test_project_document.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/karaoke_parts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/project_command.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/project_document.cpp")
target_include_directories(iclforge-app-project-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(iclforge-app-project-tests PRIVATE iclforge::ac3 iclforge::json)
# test_project_command.cpp holds the karaoke example's own project to the line its README gives,
# reading it where the example ships it.
target_compile_definitions(iclforge-app-project-tests PRIVATE
    "ICLFORGE_KARAOKE_EXAMPLE_DIR=\"${CMAKE_SOURCE_DIR}/examples/karaoke/auld-lang-syne\"")
