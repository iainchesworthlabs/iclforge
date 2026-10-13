# ---------------------------------------------------------------------------
# std::expected under the NDK's libc++ before LLVM 19.
#
# NDK r26's libc++ (LLVM 17) keeps std::expected's has-value flag in the tail padding of a
# trivially copyable payload and then copies the payload, padding and all, over it. A
# std::expected<FrameMetadata, EditError> returned by `return meta;` reports no value, with error 0,
# on a Shield (docs/platforms/android.md, "std::expected and tail padding"). NDK r27 has only half of
# the upstream fix (the flag is set after the value, which cures that, but the type still reuses
# the padding); r28 (clang 19) has both. Nothing before r28 is safe to build this repository with
# as it stands.
#
# This module adds cmake/toolchains/android.libcxx17-expected-layout.h to every C++ translation
# unit, which makes the attribute that causes it a no-op, for an Android build with a Clang older
# than 19. Anything else (every desktop and CI leg, the ESP32 and WASM builds, NDK r28 and later)
# returns at once and compiles exactly what it did.
#
# The include has to reach every target of the build, not only the libraries': an inline function
# that returns a std::expected is compiled into whichever translation units call it, and a call site
# without the shim would lay the type out another way than the library does. So a wrapper project
# that pulls the repository root in with add_subdirectory() (apps/demos/android, apps/hearth/android)
# includes this file itself, before add_subdirectory(): the root's own include then finds the
# guard already set, and the wrapper's own targets get the option as well as the root's.
#
# Delete this file, the header and the three include() lines that name it when the NDK pin reaches
# r28.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT ANDROID OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19)
    return()
endif()

set(ICLFORGE_LIBCXX_EXPECTED_SHIM
    "${CMAKE_CURRENT_LIST_DIR}/toolchains/android.libcxx17-expected-layout.h")

# The shim names a private libc++ header and a private macro. If a point release of the NDK moved
# either, the include would still compile and the build would be wrong again, quietly. So the
# configure step asks the compiler: with the option applied, a payload that has a byte of tail
# padding must not share its storage with the flag. (A function() so the C++ standard and the
# flags it sets for the check stay out of the caller's scope: the wrapper projects have not set a
# standard of their own at this point.)
function(iclforge_check_libcxx_expected_layout)
    include(CheckCXXSourceCompiles)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_REQUIRED_FLAGS "-include \"${ICLFORGE_LIBCXX_EXPECTED_SHIM}\"")
    check_cxx_source_compiles([[
        #include <cstdint>
        #include <expected>
        struct Padded { int a = 0; bool b = false; };
        static_assert(sizeof(Padded) > sizeof(int) + sizeof(bool));
        static_assert(sizeof(std::expected<Padded, std::uint8_t>) > sizeof(Padded),
                      "the has-value flag is still in the payload's tail padding");
        int main() { return 0; }
    ]] ICLFORGE_LIBCXX_EXPECTED_LAYOUT_OK)
    if(NOT ICLFORGE_LIBCXX_EXPECTED_LAYOUT_OK)
        message(FATAL_ERROR
            "cmake/LibcxxExpectedLayout.cmake: with ${ICLFORGE_LIBCXX_EXPECTED_SHIM} applied, "
            "${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_VERSION}) still places std::expected's "
            "has-value flag in its payload's tail padding, which this toolchain's libc++ overwrites. "
            "Use an NDK whose Clang is 19 or later (r28), or see docs/platforms/android.md.")
    endif()
endfunction()

iclforge_check_libcxx_expected_layout()

# SHELL: keeps "-include" and its operand together through CMake's option de-duplication, which
# would otherwise drop a second "-include" a caller added.
add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include \"${ICLFORGE_LIBCXX_EXPECTED_SHIM}\">")
