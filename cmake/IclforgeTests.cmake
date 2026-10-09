# ---------------------------------------------------------------------------
# A test binary per project (planning/monorepo.md, (e)): iclforge-<project>-tests, built from the
# project's tests/ directory, and the umbrella target iclforge-tests that builds them all.
#
# ctest names a test by its Catch2 test case, not by its binary, so the names are what one binary
# gave them. ADD_TAGS_AS_LABELS turns each case's tags into ctest labels, which is what lets
# `ctest -L concurrency` select a subset without a second discovery pass, a duplicated registration
# or a name-based -R guess that a renamed test would silently drop out of; `concurrency` is the one
# CMakePresets.json's test-linux-llvm-tsan preset runs (.github/workflows/_build.yml's
# ThreadSanitizer entry, cmake/Sanitizers.cmake). Every other tag becomes a label too, which makes
# `ctest -L encoder` and friends work for free.
#
# The project is a label as well (`ctest -L ac3`), set on the directory rather than on each test:
# catch_discover_tests() writes a case's tags as its LABELS, replacing whatever PROPERTIES gave it,
# and a directory's LABELS are added to those of every test in it.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)
include(Catch)

if(NOT TARGET iclforge-tests)
    add_custom_target(iclforge-tests)
endif()

# iclforge_add_test_binary(<target> [PROJECT <project>] SOURCES <source>...)
function(iclforge_add_test_binary target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROJECT" "SOURCES")
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE
        iclforge::test_support iclforge::warnings iclforge::coverage iclforge::fmt
        Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    # Where every test that writes a file writes it - see tests/support/CMakeLists.txt's
    # ICLFORGE_TEST_SCRATCH_DIR. Each binary's own build directory, so that two binaries never share it.
    target_compile_definitions(${target}
        PRIVATE "ICLFORGE_TEST_SCRATCH_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/scratch\"")
    if(arg_PROJECT)
        set_property(DIRECTORY APPEND PROPERTY LABELS ${arg_PROJECT})
    endif()
    add_dependencies(iclforge-tests ${target})
    catch_discover_tests(${target} ADD_TAGS_AS_LABELS)
endfunction()
