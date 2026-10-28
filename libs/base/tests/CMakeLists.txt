# iclforge-base-tests - the foundation library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-base-tests PROJECT base SOURCES
    test_bit_io.cpp
    test_bitwriter.cpp
    test_crc16.cpp
    test_meters.cpp
    test_crypto.cpp
    test_p256.cpp
    test_sha1.cpp
    test_ecdsa_der.cpp
    test_x509.cpp
    test_credentials.cpp
    test_thrift_binary.cpp
    test_content_signature.cpp
    test_interop.cpp
    test_expected_payload_padding.cpp
    test_number_parse.cpp
    test_range_for_temporaries.cpp)

target_link_libraries(iclforge-base-tests PRIVATE iclforge::base iclforge::ac3)

# test_p256.cpp checks the private field and curve arithmetic directly, not only through the
# public ECDSA functions that use it, so that a wrong constant or a wrong carry is named.
target_include_directories(iclforge-base-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/crypto")

# test_interop.cpp runs interop/p256_interop.py, which has OpenSSL (the openssl command line and
# Python's `cryptography` package) make keys, signatures and chains for the library to read and
# check what the library writes. The interpreter is found here; without one the test is skipped
# with the reason, and the known-answer tests carry the proof by themselves. The script itself
# exits 77, which the test also reads as a skip, when `cryptography` or openssl is missing.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_iclforge_interop_python "${Python3_EXECUTABLE}")
else()
    set(_iclforge_interop_python "")
endif()
target_compile_definitions(iclforge-base-tests PRIVATE
    "ICLFORGE_BASE_INTEROP_PYTHON=\"${_iclforge_interop_python}\""
    "ICLFORGE_BASE_INTEROP_SCRIPT=\"${CMAKE_CURRENT_SOURCE_DIR}/interop/p256_interop.py\"")
unset(_iclforge_interop_python)

# The same table of cases against iclforge::base::parse_double's other implementation,
# src/number_parse_libc.cpp, which the minimum-footprint builds take because they link no third-party
# library (libs/ac3/minimal.cmake). A binary of its own, with that file compiled in, so that the
# library's parse_double (fast_float) is not the one it finds; the archive's own object is not pulled
# in, since this binary already defines the symbol.
iclforge_add_test_binary(iclforge-base-number-parse-libc-tests PROJECT base SOURCES
    test_number_parse.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/number_parse_libc.cpp")
target_link_libraries(iclforge-base-number-parse-libc-tests PRIVATE iclforge::base)
target_compile_definitions(iclforge-base-number-parse-libc-tests PRIVATE ICLFORGE_BASE_STATIC_DEFINE)
