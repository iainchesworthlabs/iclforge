# ---------------------------------------------------------------------------
# Hearth for Android's native build: a THIN wrapper, as apps/demos/android's is, over the repository
# root (planning/hearth-android.md). It does three things:
#
#  1. Puts vcpkg in front of the NDK's toolchain, because libs/sendspin needs the "hearth" feature's
#     five packages (mbedTLS, cpp-httplib, libFLAC, Opus, mdns) and Gradle's own toolchain has none.
#     This is what CMakePresets.json's config-android-arm64 does for a person at a prompt, said
#     again here because AGP, not a preset, is configuring.
#  2. Pulls the repository root in with add_subdirectory(), so iclforge::sink, ::sendspin and
#     ::lyrics are the same targets the desktop test sink links.
#  3. Builds this app's own iclforge_hearth_jni, the one shared object the app loads.
#
# VCPKG_ROOT must name a vcpkg checkout (CI's setup-vcpkg sets it). The ports are built the first
# time and kept in vcpkg's binary cache after; the ports' own build tree is under this build
# directory.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28...4.3)

# --- vcpkg, before project() ---------------------------------------------------------------------
if(NOT DEFINED ENV{VCPKG_ROOT} OR NOT EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "VCPKG_ROOT must name a vcpkg checkout: Hearth for Android links libs/sendspin, "
                        "whose packages come from vcpkg's \"hearth\" feature")
endif()

get_filename_component(ICLFORGE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)

# AGP hands this the NDK's toolchain file. vcpkg's goes first and chains it, so the compiler and the
# sysroot are still the NDK's; a second configure sees vcpkg's file already there and leaves it.
if(NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\.cmake$")
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}" CACHE FILEPATH "" FORCE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "" FORCE)
endif()

# vcpkg builds the ports with its own android toolchain (D:/vcpkg/scripts/toolchains/android.cmake),
# which finds the NDK in the environment and nowhere else, whatever the project's toolchain is.
# AGP names the NDK it pinned as ANDROID_NDK, and that is the one the ports are built with: not the
# runner image's default, which is whichever release the image shipped.
if(DEFINED ANDROID_NDK)
    set(ENV{ANDROID_NDK_HOME} "${ANDROID_NDK}")
endif()

# The triplet follows the ABI AGP is building. Only arm64-v8a is built today (build.gradle.kts).
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(VCPKG_TARGET_TRIPLET "arm64-android" CACHE STRING "" FORCE)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(VCPKG_TARGET_TRIPLET "x64-android" CACHE STRING "" FORCE)
else()
    message(FATAL_ERROR "no vcpkg triplet for ANDROID_ABI=${ANDROID_ABI}")
endif()

# The manifest is the repository's, not this directory's; the feature and the overlays are what
# CMakePresets.json's core fragment sets for every other configuration.
set(VCPKG_MANIFEST_DIR "${ICLFORGE_REPO_ROOT}" CACHE PATH "" FORCE)
set(VCPKG_MANIFEST_FEATURES "hearth" CACHE STRING "" FORCE)
set(VCPKG_OVERLAY_PORTS "${ICLFORGE_REPO_ROOT}/cmake/vcpkg/ports" CACHE PATH "" FORCE)
set(VCPKG_OVERLAY_TRIPLETS "${ICLFORGE_REPO_ROOT}/cmake/vcpkg/triplets" CACHE PATH "" FORCE)
set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed" CACHE PATH "" FORCE)

project(iclforge_hearth_android_jni LANGUAGES CXX)

# --- the repository root --------------------------------------------------------------------------
# Pre-seeded before the root's own option() calls, which leave a cache value that is already there
# alone. Hearth is ON (the default) for libs/sendspin and libs/sink; what this app has no use for is
# the desktop programs and their tests, and Hearth's own applications, whose notices and packaging
# take this wrapper's directory for the repository root (ICLFORGE_BUILD_HEARTH_APPS, the root's).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# NDK r26's libc++ clobbers a std::expected's has-value flag when its payload has tail padding. The
# option has to reach this wrapper's own targets as well as the root's, so it is included here and
# not only by the root (cmake/LibcxxExpectedLayout.cmake).
include("${ICLFORGE_REPO_ROOT}/cmake/LibcxxExpectedLayout.cmake")
set(ICLFORGE_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_GUI OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_CAPI OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_HEARTH ON CACHE BOOL "" FORCE)
# One shared object: the libraries it links are static and end up inside it, so that the APK carries
# libiclforge_hearth_jni.so and libc++_shared.so and not a shared library per library of the root.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ICLFORGE_BUILD_HEARTH_APPS OFF CACHE BOOL "" FORCE)

add_subdirectory("${ICLFORGE_REPO_ROOT}" iclforge_build)

# --- this app's own shared object -----------------------------------------------------------------
add_library(iclforge_hearth_jni SHARED
    audio_link.cpp
    jni_entry.cpp
    native_sink.cpp)

# The loopback harness the instrumented tests call (harness_jni.cpp): the debug build only, so that the
# shipped library carries no test hook and no server half it does not otherwise use.
option(HEARTH_ANDROID_HARNESS "Compile the loopback harness's JNI entry into iclforge_hearth_jni" OFF)
if(HEARTH_ANDROID_HARNESS)
    target_sources(iclforge_hearth_jni PRIVATE harness_jni.cpp)
    target_link_libraries(iclforge_hearth_jni PRIVATE iclforge::sink_harness)
endif()
target_compile_features(iclforge_hearth_jni PRIVATE cxx_std_23)
# The audio backend's registration (android/passthrough_bridge.hpp) is the one header of
# iclforge::audio's src/backend tree this app includes, which the project table exposes for it.
target_include_directories(iclforge_hearth_jni PRIVATE
    "${ICLFORGE_REPO_ROOT}/libs/audio/src/backend")
# log: android/log.h, which native_sink.cpp writes the sink's log through.
target_link_libraries(iclforge_hearth_jni PRIVATE
    iclforge::sink iclforge::sink_passthrough iclforge::audio iclforge::sendspin iclforge::lyrics
    iclforge::warnings log)
