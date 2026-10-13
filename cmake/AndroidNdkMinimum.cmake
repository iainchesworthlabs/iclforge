# ---------------------------------------------------------------------------
# The oldest NDK an Android build of this tree may use: r28 (Clang 19, libc++ 19).
#
# An older one builds, and then gets wrong answers on a device with nothing to say so:
#
#  - NDK r26's libc++ (LLVM 17) puts a std::expected's has-value flag in the tail padding of a
#    trivially copyable payload and then copies the payload, padding included, over it, so
#    `return meta;` of a FrameMetadata reports "no value, error 0" and an error whose padding is
#    not zero reports a value (llvm-project#68552 and #70494, android/ndk#1963). r27 has half the
#    fix: the flag is set after the value, but the type still reuses the padding.
#  - r26's std::ranges::binary_search answers true for every value between the first element and
#    the last (llvm-project#61160), which turned every frame between two named AC-4 I-frames into
#    an I-frame.
#  - Clang before 19 ends a range-for's temporaries before its first iteration (P2718R0).
#
# docs/platforms/android.md has the account and what r28c has that r26b had not. The Gradle apps'
# `ndkVersion` and CI's sdkmanager installs pin 28.2.13676358; this is for a person with another
# one in ANDROID_NDK_HOME, who would otherwise find out from a test on the device.
#
# -DICLFORGE_ALLOW_OLD_NDK=ON turns the error into a warning, for reproducing one of the above.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT ANDROID OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19)
    return()
endif()

option(ICLFORGE_ALLOW_OLD_NDK "Build for Android with a Clang older than 19 (NDK before r28) anyway" OFF)

# string(CONCAT) and quoted expansions throughout: the install command below holds a ';', which an
# unquoted list expansion would split on and drop.
string(CONCAT _iclforge_old_ndk_message
    "This Android build uses Clang ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}), older "
    "than the NDK r28 (Clang 19) the tree is pinned to. Its libc++ miscompiles std::expected for "
    "a payload with tail padding, among other things (docs/platforms/android.md, 'std::expected "
    "and tail padding'). Install it with `sdkmanager \"ndk;28.2.13676358\"` and point "
    "ANDROID_NDK_HOME at it.")

if(ICLFORGE_ALLOW_OLD_NDK)
    message(WARNING "${_iclforge_old_ndk_message} ICLFORGE_ALLOW_OLD_NDK=ON: continuing.")
else()
    message(FATAL_ERROR
            "${_iclforge_old_ndk_message} (-DICLFORGE_ALLOW_OLD_NDK=ON to build anyway.)")
endif()
unset(_iclforge_old_ndk_message)
