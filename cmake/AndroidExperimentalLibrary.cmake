# ---------------------------------------------------------------------------
# std::jthread, std::stop_token, std::stop_source and std::stop_callback on Android.
#
# The NDK's libc++ (r28c: LLVM 19) ships <stop_token> and std::jthread but keeps them behind its
# "experimental library" switch (_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN is defined unless the
# compiler is given -fexperimental-library; r26's libc++ has no <stop_token> at all, which is one
# reason cmake/AndroidNdkMinimum.cmake wants r28). The types are standard C++20 and the Windows,
# ALSA and desktop code already uses them; this is what lets that code, and libs/audio's ring
# buffer test, compile for Android as well.
#
# The switch is Android-only: the NDK's libc++ is what is being worked around. It also exposes the
# other things libc++ 19 still gates (the parallel algorithms and the time zone database); nothing
# here uses them. <format> is not among them in r28c (it is not gated), and the tree uses {fmt} and
# not std::format on purpose (CONTRIBUTING.md, cmake/Fmt.cmake), which nothing here changes. The link
# option is what adds libc++experimental.a, which the compiler driver names itself; the stop token
# types are header-only and do not need it, but a target that instantiated another experimental
# feature would otherwise fail at link, far from the cause.
#
# Measured on a Shield (API 30, NDK r28c): a jthread that loops on its stop_token, a stop_callback
# and request_stop() run correctly against the static and the shared libc++.
#
# Drop this when libc++ stops gating them (check _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN in the
# NDK's __config).
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT ANDROID OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    return()
endif()

add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
add_link_options("$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
