// Force-included (-include) into every C++ translation unit of an Android build whose NDK is older
// than r28, by cmake/LibcxxExpectedLayout.cmake. Nothing includes it by name.
//
// NDK r26's libc++ is LLVM 17's, and its std::expected<T, E> is laid out as
//
//     _LIBCPP_NO_UNIQUE_ADDRESS union { T val; E unex; } union_;
//     bool has_val_;
//
// With the attribute, has_val_ goes into the tail padding of the union when T has some and is not a
// POD for the purpose of layout (a default member initialiser or a std::optional member is enough).
// The converting constructor then sets has_val_ and std::construct_at()s T by copy, which for a
// trivially copyable T is a copy of sizeof(T) bytes, padding included, over it. A FrameMetadata
// returned from `return value;` reads back has_value() == false and error 0.
//
//   llvm/llvm-project#68552  the flag is overwritten (fixed by #68733, in NDK r27: has_val_ is set
//                            after the value is constructed)
//   llvm/llvm-project#70494  operations on the value overwrite unrelated bytes (fixed by #69673,
//                            in NDK r28 (clang-r530567): expected no longer has tail padding)
//   android/ndk#1963         the NDK's own tracker; the fix did not cherry-pick onto r26
//
// Emptying the attribute puts has_val_ after the union, where it belongs. It changes the layout of
// every libc++ class that carries the attribute (std::expected; the std::ranges views and result
// types; the make_shared control block's allocator; the std::format buffer), and all of those are
// templates that this build instantiates itself, so a translation unit that has the shim and one
// that has not must not be mixed: the module adds it to every target the repository's CMake builds.
//
// This is a bridge, not a policy. Delete it with the module when the NDK pin reaches r28.

#pragma once

// libc++'s configuration header, which defines the macro. Included first so that its own include
// guard keeps it from running again after the redefinition below.
#include <__config>

#undef _LIBCPP_NO_UNIQUE_ADDRESS
#define _LIBCPP_NO_UNIQUE_ADDRESS /* nothing: see above */
