# ---------------------------------------------------------------------------
# iclforge::signing - the EMDF Atmos object signer.
#
# Clean-room and dependency-free (SHA-256/HMAC are implemented from FIPS 180-4/
# RFC 2104, not pulled from OpenSSL - the same "no third-party codec deps" rule
# vcpkg.json states). Kept a SEPARATE target from iclforge::ac3, not folded into
# it, so the codec still links nothing to encode/decode: signing is a strictly
# optional step a front end applies to finished frames, and iclforge::ac3 has no
# dependency on it. It reuses the codec's PARSING primitives (BitReader,
# decode_exponents, compute_bit_allocation, the spx helpers) via iclforge::ac3 but
# adds nothing to them.
#
# No embedded key: the signer takes a SigningKey the operator supplies at
# runtime (see signing_key.hpp). This is why the target is committed and built
# by default, unlike the old gitignored quarantine overlay it replaces - there
# is no secret in it to keep out of the tree.
#
# Built and installed as both a static and a shared library by
# iclforge_add_library() (cmake/IclforgeLibrary.cmake). An ordinary C++
# dependent rather than a self-contained C ABI like iclforge::c
# (src/capi/CMakeLists.txt): iclforge_signing_static and iclforge_signing_shared
# link iclforge::ac3_static and iclforge::ac3_shared, rather than always
# embedding one specific variant regardless of BUILD_SHARED_LIBS the way capi
# does.
# ---------------------------------------------------------------------------
iclforge_add_library(signing
    SOURCES
        src/sha256.cpp
        src/hmac_sha256.cpp
        src/signing_key.cpp
        src/emdf_atmos_signer.cpp
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # An ordinary link, not the compile-only one DEPENDS would make: the bare alias is the static
    # codec in a static build, so libiclforge_signing.so carries a copy of what the signer calls of
    # it beside its link to libiclforge_ac3.so. Kept as it was until the signer moves into
    # iclforge::ac3 (planning/consolidation.md, C2).
    LINK_PRIVATE_FIRST iclforge::ac3)
target_link_libraries(iclforge_signing_static PUBLIC iclforge::ac3_static)
target_link_libraries(iclforge_signing_shared PUBLIC iclforge::ac3_shared)

# MSVC flags plain getenv() (signing_key.cpp's env-var fallbacks) as C4996
# "unsafe". The call is POSIX and portable - GCC/Clang raise nothing for it
# under -Wall -Wextra - and the suggested _dupenv_s is a Microsoft CRT
# extension with no NDK/POSIX equivalent, so "fix the code" is not an option
# that stays cross-platform. Scoped to this one compiler's opinion of a
# standard C function, the same way src/ac3 handles its own MSVC-only
# suppressions (see cmake/CompilerWarnings.cmake).
if(MSVC)
    target_compile_options(iclforge_signing_objects PRIVATE /wd4996)
endif()
