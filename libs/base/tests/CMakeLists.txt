# iclforge-base-tests - the foundation library's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-base-tests PROJECT base SOURCES
    test_bit_io.cpp
    test_bitwriter.cpp
    test_crc16.cpp
    test_meters.cpp
    test_crypto.cpp
    test_ecdsa_p256.cpp
    test_x509_chain.cpp
    test_credentials.cpp
    test_expected_payload_padding.cpp
    test_number_parse.cpp
    test_range_for_temporaries.cpp)

target_link_libraries(iclforge-base-tests PRIVATE iclforge::base iclforge::ac3)

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
