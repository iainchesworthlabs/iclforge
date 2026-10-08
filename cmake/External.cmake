# ---------------------------------------------------------------------------
# Third-party code the build fetches rather than finds. The code the tree carries is under
# external/ (planning/monorepo.md, decision 14); vcpkg stays the manager for what it provides.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# Catch2 normally arrives through a package manager - vcpkg's toolchain, a
# distribution package, an explicit CMAKE_PREFIX_PATH. A fresh clone has none
# of those configured, and a test suite that disappears when the local setup is
# missing is worse than one that costs a download, so fall back to fetching it.
# The version matches the vcpkg port so both routes test against the same code.
#
# A macro, so that what it finds and the module path it extends are the caller's: every
# project's test binary reaches catch_discover_tests() through them.
macro(iclforge_find_catch2)
    set(ICLFORGE_CATCH2_VERSION 3.15.3)

    find_package(Catch2 3 CONFIG QUIET)

    if(NOT Catch2_FOUND)
        if(NOT ICLFORGE_FETCH_CATCH2)
            message(FATAL_ERROR
                "Catch2 3 was not found and ICLFORGE_FETCH_CATCH2 is OFF.\n"
                "Supply it with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
                "or -DCMAKE_PREFIX_PATH=<prefix>, allow the download by setting "
                "ICLFORGE_FETCH_CATCH2=ON, or build without tests using "
                "-DICLFORGE_BUILD_TESTS=OFF.")
        endif()

        message(STATUS
            "Catch2 not found locally; fetching v${ICLFORGE_CATCH2_VERSION} "
            "(-DICLFORGE_FETCH_CATCH2=OFF to require a local copy)")

        include(FetchContent)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG "v${ICLFORGE_CATCH2_VERSION}"
            GIT_SHALLOW TRUE
            # SYSTEM keeps Catch2's headers out of reach of /W4 /WX, matching how
            # the installed package presents itself; EXCLUDE_FROM_ALL keeps its
            # install rules out of ours.
            SYSTEM
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(Catch2)

        # catch_discover_tests() lives in Catch2's extras/. The installed package
        # config puts that directory on CMAKE_MODULE_PATH; a source build does not.
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endmacro()
