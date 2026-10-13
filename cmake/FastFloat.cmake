# ---------------------------------------------------------------------------
# FastFloat.cmake
#
# Defines an INTERFACE target `iclforge::fast_float` wrapping fast_float
# (`FastFloat::fast_float`), the library that reads a decimal number into a float or a double with
# the contract of std::from_chars and the exact rounding it asks for. It is what
# iclforge::base::parse_double (libs/base/src/number_parse.cpp) is made of, and nothing else takes
# it: iclforge::base links it PRIVATE through $<BUILD_INTERFACE:...>, the way it takes
# iclforge::fmt_private, so it is compiled into the library and no export graph, package or
# consumer sees it.
#
# Why a library and not the C library's strtod, or std::from_chars itself: strtod reads the
# process locale's decimal point and a C library's own idea of ERANGE; std::from_chars for
# floating point is missing from the NDK's libc++ (r28c included) and from the macOS wheel's
# deployment target (CONTRIBUTING.md, "One exception, for reading rather than writing").
# fast_float is the implementation std::from_chars was specified from, header-only, and the same
# on every toolchain.
#
# Resolved the way cmake/Fmt.cmake and cmake/NlohmannJson.cmake resolve theirs, and for the same
# reasons: a desktop build gets it from vcpkg (vcpkg.json's base "dependencies"); a build with no
# vcpkg toolchain - the Homebrew formula, a distribution package build, the Android app's Gradle
# wrapper, the WebAssembly build - takes the FetchContent fallback below. It is header-only, so
# there is nothing to compile and nothing to link against.
#
# The minimum-footprint profiles (ICLFORGE_MINIMAL_DECODER, ICLFORGE_MINIMAL_ENCODER) do not come
# through here, for the reason cmake/Fmt.cmake gives for itself: they link no third-party library
# and must not make a network dependency at configure time. They compile
# libs/base/src/number_parse_libc.cpp, which answers parse_double with the C library's strtod
# behind the same syntax checks.
# ---------------------------------------------------------------------------

# Matches vcpkg's own fast-float at the baseline vcpkg.json pins, so the two routes build against
# the same code. fast_float's git tags carry a "v" prefix.
set(ICLFORGE_FAST_FLOAT_VERSION 8.2.10)

# The oldest a local copy may be. chars_format::allow_leading_plus and ::skip_white_space, which
# iclforge::base::parse_double's NumberSyntax maps onto, are what this needs; an older copy is not
# used and the pinned version above is fetched instead.
set(ICLFORGE_FAST_FLOAT_MINIMUM_VERSION 8.0.0)

option(ICLFORGE_FETCH_FAST_FLOAT
       "Fetch fast_float from source via FetchContent when no local copy is found" ON)

find_package(FastFloat ${ICLFORGE_FAST_FLOAT_MINIMUM_VERSION} CONFIG QUIET)

if(NOT FastFloat_FOUND)
    if(NOT ICLFORGE_FETCH_FAST_FLOAT)
        message(FATAL_ERROR
            "fast_float ${ICLFORGE_FAST_FLOAT_MINIMUM_VERSION} or newer was not found and "
            "ICLFORGE_FETCH_FAST_FLOAT is OFF.\n"
            "Supply it with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
            "or -DCMAKE_PREFIX_PATH=<prefix>, or allow the download by setting "
            "ICLFORGE_FETCH_FAST_FLOAT=ON.")
    endif()

    message(STATUS
        "fast_float ${ICLFORGE_FAST_FLOAT_MINIMUM_VERSION} or newer not found locally; "
        "fetching v${ICLFORGE_FAST_FLOAT_VERSION} "
        "(-DICLFORGE_FETCH_FAST_FLOAT=OFF to require a local copy)")

    include(FetchContent)
    FetchContent_Declare(fast_float
        GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
        GIT_TAG "v${ICLFORGE_FAST_FLOAT_VERSION}"
        GIT_SHALLOW TRUE
        # SYSTEM and EXCLUDE_FROM_ALL for the reasons cmake/Fmt.cmake gives for {fmt}.
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(fast_float)
endif()

add_library(iclforge_fast_float INTERFACE)
add_library(iclforge::fast_float ALIAS iclforge_fast_float)
target_link_libraries(iclforge_fast_float INTERFACE FastFloat::fast_float)

# vcpkg's FastFloatConfig.cmake does not mark its include directory SYSTEM, so a warning inside
# the library's own headers would be promoted to an error by iclforge::warnings' /W4 /WX the
# moment a first-party target links this - the same thing cmake/NlohmannJson.cmake re-marks.
get_target_property(ICLFORGE_FAST_FLOAT_INCLUDE_DIRS FastFloat::fast_float
                    INTERFACE_INCLUDE_DIRECTORIES)
if(ICLFORGE_FAST_FLOAT_INCLUDE_DIRS)
    target_include_directories(iclforge_fast_float SYSTEM INTERFACE
                               ${ICLFORGE_FAST_FLOAT_INCLUDE_DIRS})
endif()
