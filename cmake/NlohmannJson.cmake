# ---------------------------------------------------------------------------
# NlohmannJson.cmake
#
# Defines an INTERFACE target `iclforge::json` wrapping nlohmann/json
# (`nlohmann_json::nlohmann_json`), the JSON library the project file's reader and writer
# (apps/shared/project/src) use in both Forge programs. It reads and writes `.forgeproj`
# documents and nothing else: no codec, no library under libs/ links it.
#
# Resolved the way cmake/Fmt.cmake resolves {fmt}, and for the same reasons: a desktop build gets
# it from vcpkg (vcpkg.json's base "dependencies"); a build with no vcpkg toolchain - the Homebrew
# formula, a distribution package build - takes the FetchContent fallback below, so that the
# command-line tool still configures. It is header-only, so there is nothing to compile and
# nothing to link against.
#
# `ordered_json` is the type the project file uses: an object that keeps its keys in the order
# they were written in, which is what lets a project made by a newer version pass through this one
# unchanged. The library reads and writes numbers without consulting the process's locale.
# ---------------------------------------------------------------------------

# Matches vcpkg's own nlohmann-json at the baseline vcpkg.json pins, so the two routes build
# against the same code. nlohmann's git tags carry a "v" prefix.
set(ICLFORGE_NLOHMANN_JSON_VERSION 3.12.0)

# The oldest a local copy may be. ordered_json (3.9) and the parse call this tree makes are older
# than this; 3.11.0 is a round floor, not a version a build was bisected to.
set(ICLFORGE_NLOHMANN_JSON_MINIMUM_VERSION 3.11.0)

option(ICLFORGE_FETCH_NLOHMANN_JSON
       "Fetch nlohmann/json from source via FetchContent when no local copy is found" ON)

find_package(nlohmann_json ${ICLFORGE_NLOHMANN_JSON_MINIMUM_VERSION} CONFIG QUIET)

if(NOT nlohmann_json_FOUND)
    if(NOT ICLFORGE_FETCH_NLOHMANN_JSON)
        message(FATAL_ERROR
            "nlohmann/json ${ICLFORGE_NLOHMANN_JSON_MINIMUM_VERSION} or newer was not found and "
            "ICLFORGE_FETCH_NLOHMANN_JSON is OFF.\n"
            "Supply it with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
            "or -DCMAKE_PREFIX_PATH=<prefix>, or allow the download by setting "
            "ICLFORGE_FETCH_NLOHMANN_JSON=ON.")
    endif()

    message(STATUS
        "nlohmann/json ${ICLFORGE_NLOHMANN_JSON_MINIMUM_VERSION} or newer not found locally; "
        "fetching v${ICLFORGE_NLOHMANN_JSON_VERSION} "
        "(-DICLFORGE_FETCH_NLOHMANN_JSON=OFF to require a local copy)")

    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG "v${ICLFORGE_NLOHMANN_JSON_VERSION}"
        GIT_SHALLOW TRUE
        # SYSTEM and EXCLUDE_FROM_ALL for the reasons cmake/Fmt.cmake gives for {fmt}.
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(iclforge_json INTERFACE)
add_library(iclforge::json ALIAS iclforge_json)
target_link_libraries(iclforge_json INTERFACE nlohmann_json::nlohmann_json)

# vcpkg's nlohmann_jsonConfig.cmake does not mark its include directory SYSTEM, so a warning
# inside the library's own headers would be promoted to an error by iclforge::warnings' /W4 /WX the
# moment a first-party target links this - the same thing cmake/Fmt.cmake re-marks for {fmt}.
get_target_property(ICLFORGE_JSON_INCLUDE_DIRS nlohmann_json::nlohmann_json
                    INTERFACE_INCLUDE_DIRECTORIES)
if(ICLFORGE_JSON_INCLUDE_DIRS)
    target_include_directories(iclforge_json SYSTEM INTERFACE ${ICLFORGE_JSON_INCLUDE_DIRS})
endif()
