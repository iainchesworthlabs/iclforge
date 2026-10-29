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
    test_thrift_binary.cpp
    test_content_signature.cpp
    test_expected_payload_padding.cpp
    test_number_parse.cpp
    test_range_for_temporaries.cpp)

target_link_libraries(iclforge-base-tests PRIVATE iclforge::base iclforge::ac3)

# test_content_signature.cpp reads std::getenv, for the owner's sample blob it parses when
# ICLFORGE_CONTENT_SIGNATURE_SAMPLE names one - the C4996 reasoning of libs/ac3/tests' cpu_features.cpp.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(test_content_signature.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()

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

# What a private key leaves in freed memory (test_key_residue.cpp). A binary of its own: it replaces
# the global allocation functions to look at each block as it is freed, and no other test should run
# under that.
iclforge_add_test_binary(iclforge-base-key-residue-tests PROJECT base SOURCES
    test_key_residue.cpp)
target_link_libraries(iclforge-base-key-residue-tests PRIVATE iclforge::base)
