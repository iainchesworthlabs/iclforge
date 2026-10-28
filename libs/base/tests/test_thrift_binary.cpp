// The Thrift binary reader and writer (thrift_binary.hpp) and the encoding of the content
// signature's structures (content_signature.hpp, the parts about the wire). The byte vectors are
// written out by hand from the specification's section 7: a field is a type byte, a big-endian
// signed 16-bit id and a value; a string is a 32-bit length and its bytes; a list is an element
// type, a 32-bit count and the elements; a struct ends with a zero byte. They were laid out
// independently of the library's writer, so a test that the writer produces them is a test of the
// writer.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"

namespace {

using namespace iclforge::base::crypto;
using Bytes = std::vector<std::byte>;

Bytes B(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Bytes text(std::string_view s) {
    Bytes out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// --- The fixture ------------------------------------------------------------------------------
// A ContentSignature with small values, laid out field by field. The values are not a valid
// signature; this test is about bytes. Field ids 1 to n in the order of the specification's
// section 3.

// clang-format off
const Bytes kFixtureBytes = B({
    // field 1 (certificateChain), type STRUCT
    0x0C, 0x00, 0x01,
    //   field 1 chainStructure, STRING, length 4, "x509"
    0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x78, 0x35, 0x30, 0x39,
    //   field 2 format, STRING, length 3, "der"
    0x0B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x64, 0x65, 0x72,
    //   field 3 pkStandardName, STRING, length 9, "secp256r1"
    0x0B, 0x00, 0x03, 0x00, 0x00, 0x00, 0x09, 0x73, 0x65, 0x63, 0x70, 0x32, 0x35, 0x36, 0x72, 0x31,
    //   field 4 certificates, LIST of STRING, 2 elements: "AB", "C"
    0x0F, 0x00, 0x04, 0x0B, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x41, 0x42, 0x00, 0x00, 0x00, 0x01, 0x43,
    //   STOP
    0x00,
    // field 2 (signerInfo), type STRUCT
    0x0C, 0x00, 0x02,
    //   field 1 signerSerialNumber, "01"
    0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x30, 0x31,
    //   field 2 signatureValue, 2 bytes 30 00
    0x0B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x30, 0x00,
    //   STOP
    0x00,
    // field 3 (signatureValues), type STRUCT
    0x0C, 0x00, 0x03,
    //   field 1 signedInfo, STRUCT
    0x0C, 0x00, 0x01,
    //     field 1 canonicalVersion, I32, 3
    0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
    //     field 2 signedDate, "2026-10-29T00:00:00Z" (20 bytes)
    0x0B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x14, 0x32, 0x30, 0x32, 0x36, 0x2D, 0x31, 0x30, 0x2D, 0x32, 0x39, 0x54, 0x30, 0x30, 0x3A, 0x30, 0x30, 0x3A, 0x30, 0x30, 0x5A,
    //     field 3 digests, LIST of STRUCT, 1 element
    0x0F, 0x00, 0x03, 0x0C, 0x00, 0x00, 0x00, 0x01,
    //       field 1 digestValue, 4 bytes
    0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04,
    //       field 2 reference, "r"
    0x0B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x72,
    //       STOP
    0x00,
    //     STOP (signedInfo)
    0x00,
    //   field 2 signatureValues, STRUCT
    0x0C, 0x00, 0x02,
    //     field 1 digest_data_ref, "a"
    0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x61,
    //     field 2 canonical_method, "b"
    0x0B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x62,
    //     field 3 hash, "c"
    0x0B, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x63,
    //     field 4 signatureMethod, "d"
    0x0B, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x64,
    //     field 5 pkStandardName, "e"
    0x0B, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x65,
    //     field 6 digestMethod, "f"
    0x0B, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x66,
    //     STOP (details)
    0x00,
    //   STOP (signatureValues)
    0x00,
    // STOP (ContentSignature)
    0x00,
});
// clang-format on

ContentSignature fixture() {
    ContentSignature cs;
    cs.certificate_chain.chain_structure = "x509";
    cs.certificate_chain.format = "der";
    cs.certificate_chain.pk_standard_name = "secp256r1";
    cs.certificate_chain.certificates = {text("AB"), text("C")};
    cs.signer_info.signer_serial_number = "01";
    cs.signer_info.signature_value = B({0x30, 0x00});
    cs.signature_values.signed_info.canonical_version = 3;
    cs.signature_values.signed_info.signed_date = "2026-10-29T00:00:00Z";
    cs.signature_values.signed_info.digests = {LsSignedDigestWire{B({1, 2, 3, 4}), "r"}};
    cs.signature_values.signature_values = {"a", "b", "c", "d", "e", "f"};
    return cs;
}

// Offsets into kFixtureBytes of the STOP that ends each struct whose end a test wants to insert
// bytes before: the end of certificateChain (60 bytes from the start), of signerInfo (the 22 bytes
// that follow) and of the whole structure (the last byte).
constexpr std::size_t kChainStop = 59;
constexpr std::size_t kSignerStop = 81;
constexpr std::size_t kTopStop = 204;

Bytes with_inserted(const Bytes& base, std::size_t at, const Bytes& extra) {
    Bytes out(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(at));
    out.insert(out.end(), extra.begin(), extra.end());
    out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(at), base.end());
    return out;
}

void require_equal(const ContentSignature& a, const ContentSignature& b) {
    REQUIRE(a.certificate_chain.chain_structure == b.certificate_chain.chain_structure);
    REQUIRE(a.certificate_chain.format == b.certificate_chain.format);
    REQUIRE(a.certificate_chain.pk_standard_name == b.certificate_chain.pk_standard_name);
    REQUIRE(a.certificate_chain.certificates == b.certificate_chain.certificates);
    REQUIRE(a.signer_info.signer_serial_number == b.signer_info.signer_serial_number);
    REQUIRE(a.signer_info.signature_value == b.signer_info.signature_value);
    REQUIRE(a.signature_values.signed_info.canonical_version ==
            b.signature_values.signed_info.canonical_version);
    REQUIRE(a.signature_values.signed_info.signed_date ==
            b.signature_values.signed_info.signed_date);
    REQUIRE(a.signature_values.signed_info.digests.size() ==
            b.signature_values.signed_info.digests.size());
    for (std::size_t i = 0; i < a.signature_values.signed_info.digests.size(); ++i) {
        REQUIRE(a.signature_values.signed_info.digests[i].digest_value ==
                b.signature_values.signed_info.digests[i].digest_value);
        REQUIRE(a.signature_values.signed_info.digests[i].reference ==
                b.signature_values.signed_info.digests[i].reference);
    }
    const auto& da = a.signature_values.signature_values;
    const auto& db = b.signature_values.signature_values;
    REQUIRE(da.digest_data_ref == db.digest_data_ref);
    REQUIRE(da.canonical_method == db.canonical_method);
    REQUIRE(da.hash == db.hash);
    REQUIRE(da.signature_method == db.signature_method);
    REQUIRE(da.pk_standard_name == db.pk_standard_name);
    REQUIRE(da.digest_method == db.digest_method);
}

// The error a payload is refused with and the text that says why.
struct Refusal {
    ContentSignatureError error;
    std::string detail;
};
Refusal refusal(const Bytes& payload) {
    const auto parsed = parse_content_signature(payload);
    REQUIRE_FALSE(parsed.has_value());
    return {parsed.error().error, parsed.error().detail};
}

bool mentions(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- Reader helpers ---------------------------------------------------------------------------

// A struct reader that knows no field: every field is stepped over by its type. The result is the
// error skipping met, or nothing when the whole struct was read.
std::optional<thrift::Error> skim(const Bytes& input, thrift::Limits limits = {}) {
    thrift::Reader reader(input, limits);
    const auto result = reader.read_struct(
        [](const thrift::FieldHeader&) -> std::expected<bool, thrift::Error> { return false; });
    if (result) {
        return std::nullopt;
    }
    return result.error();
}

// An unknown field of the given id and type, with the value bytes.
Bytes unknown_field(unsigned type, unsigned id, const Bytes& value) {
    return cat({B({type, id >> 8, id & 0xFF}), value});
}

}  // namespace

TEST_CASE("thrift writer: scalar encodings", "[thrift]") {
    Bytes out;
    thrift::Writer w(out);

    SECTION("integers are big endian") {
        w.i32(0x01020304);
        w.i32(-1);
        w.i32(INT32_MIN);
        REQUIRE(out == B({0x01, 0x02, 0x03, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00}));
    }
    SECTION("a field header is a type byte and a signed 16-bit id") {
        w.field(thrift::Type::kI32, 0x0102);
        w.field(thrift::Type::kString, -2);
        w.field(thrift::Type::kStruct, 1);
        REQUIRE(out == B({0x08, 0x01, 0x02, 0x0B, 0xFF, 0xFE, 0x0C, 0x00, 0x01}));
    }
    SECTION("a string has a 32-bit length in front, and an empty one is just the length") {
        w.string("ab");
        w.string("");
        w.binary(B({0xDE, 0xAD}));
        REQUIRE(out == B({0, 0, 0, 2, 'a', 'b', 0, 0, 0, 0, 0, 0, 0, 2, 0xDE, 0xAD}));
    }
    SECTION("a list header is the element type and a 32-bit count") {
        w.list(thrift::Type::kStruct, 3);
        w.list(thrift::Type::kString, 0);
        REQUIRE(out == B({0x0C, 0, 0, 0, 3, 0x0B, 0, 0, 0, 0}));
    }
    SECTION("stop is one zero byte") {
        w.stop();
        REQUIRE(out == B({0}));
    }
    SECTION("the field-and-value helpers write the header and then the value") {
        w.i32_field(7, 3);
        w.string_field(8, "x");
        w.binary_field(9, B({0x01}));
        w.list_field(10, thrift::Type::kI32, 2);
        w.struct_field(11);
        REQUIRE(out == B({0x08, 0x00, 0x07, 0,    0, 0, 3,        //
                          0x0B, 0x00, 0x08, 0,    0, 0, 1, 'x',   //
                          0x0B, 0x00, 0x09, 0,    0, 0, 1, 0x01,  //
                          0x0F, 0x00, 0x0A, 0x08, 0, 0, 0, 2,     //
                          0x0C, 0x00, 0x0B}));
    }
    REQUIRE_FALSE(w.overflowed());
}

TEST_CASE("thrift type table is the specification's", "[thrift]") {
    for (unsigned code = 0; code < 256; ++code) {
        const bool listed = code == 0 || code == 2 || code == 3 || code == 6 || code == 8 ||
                            code == 10 || code == 11 || code == 12 || code == 13 || code == 14 ||
                            code == 15;
        REQUIRE(thrift::is_known_type(static_cast<std::uint8_t>(code)) == listed);
    }
}

TEST_CASE("content signature: the fixture serialises to the hand-laid bytes", "[thrift][wire]") {
    REQUIRE(kFixtureBytes.size() == 205);
    REQUIRE(serialize_content_signature(fixture()) == kFixtureBytes);
}

TEST_CASE("content signature: the hand-laid bytes parse to the fixture", "[thrift][wire]") {
    const auto parsed = parse_content_signature(kFixtureBytes);
    REQUIRE(parsed.has_value());
    require_equal(*parsed, fixture());
}

TEST_CASE("content signature: every truncation of the payload is refused", "[thrift][wire]") {
    // A prefix of a structure is never a complete one, whatever offset it is cut at.
    for (std::size_t n = 0; n < kFixtureBytes.size(); ++n) {
        const Bytes prefix(kFixtureBytes.begin(),
                           kFixtureBytes.begin() + static_cast<std::ptrdiff_t>(n));
        const auto parsed = parse_content_signature(prefix);
        INFO("cut at " << n);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error().error == ContentSignatureError::kBadOrMissingSignature);
    }
}

TEST_CASE("content signature: bytes after the structure are refused", "[thrift][wire]") {
    Bytes payload = kFixtureBytes;
    payload.push_back(std::byte{0});
    const Refusal r = refusal(payload);
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "after the end"));
}

TEST_CASE("content signature: no payload, and one that is too large", "[thrift][wire]") {
    REQUIRE(refusal({}).error == ContentSignatureError::kBadOrMissingSignature);
    const Bytes big(kMaxContentSignatureBytes + 1, std::byte{0});
    const Refusal r = refusal(big);
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "accepts"));
    // The limit is on the size; a payload of exactly the limit is read (and refused for its
    // content, not its size).
    const Bytes at_limit(kMaxContentSignatureBytes, std::byte{0});
    REQUIRE_FALSE(mentions(refusal(at_limit).detail, "accepts"));
}

TEST_CASE("content signature: fields the table does not know are skipped by type",
          "[thrift][wire]") {
    // One unknown field of each type the specification lists, appended to the top-level struct, to
    // the signer info and to the chain: the same signature comes back.
    const Bytes extras = cat({
        unknown_field(2, 100, B({1})),                              // BOOL
        unknown_field(3, 101, B({0x7F})),                           // BYTE
        unknown_field(6, 102, B({0x12, 0x34})),                     // I16
        unknown_field(8, 103, B({0, 0, 0, 9})),                     // I32
        unknown_field(10, 104, B({0, 0, 0, 0, 0, 0, 0, 9})),        // I64
        unknown_field(11, 105, B({0, 0, 0, 3, 'a', 'b', 'c'})),     // STRING
        unknown_field(12, 106, B({0x08, 0, 1, 0, 0, 0, 5, 0x00})),  // STRUCT { i32 } STOP
        unknown_field(13, 107, B({0x0B, 0x08, 0, 0, 0, 1, 0, 0, 0, 1, 'k', 0, 0, 0, 7})),  // MAP
        unknown_field(14, 108, B({0x06, 0, 0, 0, 2, 0, 1, 0, 2})),  // SET of I16
        unknown_field(15, 109,
                      B({0x0B, 0, 0, 0, 2, 0, 0, 0, 1, 'x', 0, 0, 0, 0})),  // LIST of STRING
        unknown_field(15, 110,
                      B({0x0F, 0, 0, 0, 1, 0x08, 0, 0, 0, 1, 0, 0, 0, 4})),  // LIST of LIST
        unknown_field(15, 111, B({0x0C, 0, 0, 0, 2, 0x00, 0x00})),  // LIST of two empty structs
    });
    const ContentSignature expected = fixture();
    for (const std::size_t at : {kChainStop, kSignerStop, kTopStop}) {
        const auto parsed = parse_content_signature(with_inserted(kFixtureBytes, at, extras));
        INFO("inserted before the stop at " << at);
        REQUIRE(parsed.has_value());
        require_equal(*parsed, expected);
    }
    // Unknown fields may come before the known ones too.
    const auto first = parse_content_signature(with_inserted(kFixtureBytes, 0, extras));
    REQUIRE(first.has_value());
    require_equal(*first, expected);
}

TEST_CASE("content signature: a known field of the wrong type is refused, not skipped",
          "[thrift][wire]") {
    // signerSerialNumber (field 1 of SignerInfoWire) written as an I32 instead of a string: an id
    // the table knows is not a field the reader may ignore because its type is not the expected
    // one.
    Bytes payload = B({
        0x0C,
        0x00,
        0x02,  // signerInfo
        0x08,
        0x00,
        0x01,
        0,
        0,
        0,
        1,  // field 1 as an I32
        0x0B,
        0x00,
        0x02,
        0,
        0,
        0,
        0,  // field 2
        0x00,
    });
    Bytes whole = kFixtureBytes;
    // Replace the 22 bytes of the signerInfo struct in the fixture with the above.
    whole.erase(whole.begin() + 60, whole.begin() + 82);
    whole.insert(whole.begin() + 60, payload.begin(), payload.end());
    const Refusal r = refusal(whole);
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "SignerInfoWire"));
    REQUIRE(mentions(r.detail, "type other than"));
}

TEST_CASE("content signature: a field met twice is refused", "[thrift][wire]") {
    // A second signerSerialNumber: two readers could take different ones.
    const Bytes second = cat({B({0x0B, 0x00, 0x01}), B({0, 0, 0, 2}), text("99")});
    const Refusal r = refusal(with_inserted(kFixtureBytes, kSignerStop, second));
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "SignerInfoWire"));
    REQUIRE(mentions(r.detail, "twice"));
}

TEST_CASE("content signature: a missing field is refused", "[thrift][wire]") {
    // The signer info without its signatureValue.
    Bytes whole = kFixtureBytes;
    // signerInfo is 60..82; its second field is the nine bytes before its STOP at 81.
    whole.erase(whole.begin() + 72, whole.begin() + 81);
    const Refusal r = refusal(whole);
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "SignerInfoWire"));
    REQUIRE(mentions(r.detail, "missing"));

    // The whole signerInfo, and the whole signatureValues.
    Bytes no_signer = kFixtureBytes;
    no_signer.erase(no_signer.begin() + 60, no_signer.begin() + 82);
    REQUIRE(mentions(refusal(no_signer).detail, "ContentSignature:"));
    REQUIRE(refusal(B({0x00})).error == ContentSignatureError::kBadOrMissingSignature);
}

TEST_CASE("content signature: a list with the wrong element type is refused", "[thrift][wire]") {
    // certificates written as a list of I32: its header is at offset 40 of the fixture and the
    // element type byte follows the three bytes of the field header.
    Bytes whole = kFixtureBytes;
    REQUIRE(whole[40] == std::byte{0x0F});
    REQUIRE(whole[43] == std::byte{0x0B});
    whole[43] = std::byte{0x08};
    const Refusal r = refusal(whole);
    REQUIRE(r.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(r.detail, "CertificateChainWire"));
}

TEST_CASE("thrift reader: hostile sizes are refused before anything is read or allocated",
          "[thrift]") {
    SECTION("a field header cut short") {
        REQUIRE(skim(B({0x0B})) == thrift::Error::kTruncated);
        REQUIRE(skim(B({0x0B, 0x00})) == thrift::Error::kTruncated);
        REQUIRE(skim(B({})) == thrift::Error::kTruncated);  // no STOP
    }
    SECTION("type codes outside the table") {
        for (const unsigned code : {1u, 4u, 5u, 7u, 9u, 16u, 0x80u, 0xFFu}) {
            INFO("type " << code);
            REQUIRE(skim(B({code, 0, 1})) == thrift::Error::kUnknownType);
        }
    }
    SECTION("a negative string length") {
        REQUIRE(skim(unknown_field(11, 1, B({0xFF, 0xFF, 0xFF, 0xFF}))) ==
                thrift::Error::kNegativeLength);
        REQUIRE(skim(unknown_field(11, 1, B({0x80, 0x00, 0x00, 0x00}))) ==
                thrift::Error::kNegativeLength);
    }
    SECTION("a string longer than the input") {
        REQUIRE(skim(cat({unknown_field(11, 1, B({0, 0, 0, 5, 'a', 'b', 'c'})), B({0})})) ==
                thrift::Error::kLengthExceedsInput);
        REQUIRE(skim(unknown_field(11, 1, B({0x7F, 0xFF, 0xFF, 0xFF}))) ==
                thrift::Error::kLengthExceedsInput);
    }
    SECTION("a list with a negative count") {
        REQUIRE(skim(unknown_field(15, 1, B({0x08, 0xFF, 0xFF, 0xFF, 0xFF}))) ==
                thrift::Error::kNegativeLength);
    }
    SECTION("a list claiming more elements than the input can hold") {
        // Two billion I32s in a dozen bytes: refused from the count and the element size, with no
        // loop and no reservation.
        REQUIRE(skim(cat({unknown_field(15, 1, B({0x08, 0x7F, 0xFF, 0xFF, 0xFF, 0, 0, 0, 1})),
                          B({0})})) == thrift::Error::kLengthExceedsInput);
        // Elements of a size that is not fixed count at their smallest: an empty struct is a byte.
        REQUIRE(skim(cat({unknown_field(15, 1, B({0x0C, 0, 0, 0, 4, 0x00, 0x00})), B({0})})) ==
                thrift::Error::kLengthExceedsInput);
    }
    SECTION("a list element type that cannot be an element") {
        REQUIRE(skim(unknown_field(15, 1, B({0x00, 0, 0, 0, 0}))) == thrift::Error::kUnknownType);
        REQUIRE(skim(unknown_field(15, 1, B({0x63, 0, 0, 0, 0}))) == thrift::Error::kUnknownType);
    }
    SECTION("a map claiming more pairs than the input can hold") {
        REQUIRE(skim(cat({unknown_field(13, 1, B({0x0B, 0x0B, 0x7F, 0xFF, 0xFF, 0xFF})),
                          B({0})})) == thrift::Error::kLengthExceedsInput);
        REQUIRE(skim(unknown_field(13, 1, B({0x0B, 0x0B, 0xFF, 0xFF, 0xFF, 0xFF}))) ==
                thrift::Error::kNegativeLength);
        REQUIRE(skim(unknown_field(13, 1, B({0x00, 0x0B, 0, 0, 0, 0}))) ==
                thrift::Error::kUnknownType);
    }
    SECTION("a fixed-size value cut short") {
        REQUIRE(skim(unknown_field(10, 1, B({0, 0, 0, 0, 0, 0, 0}))) == thrift::Error::kTruncated);
    }
    SECTION("an empty struct, and a well-formed one, are fine") {
        REQUIRE_FALSE(skim(B({0x00})).has_value());
        REQUIRE_FALSE(skim(cat({unknown_field(8, 1, B({0, 0, 0, 1})), B({0})})).has_value());
    }
}

TEST_CASE("thrift reader: nesting is bounded", "[thrift]") {
    // n unknown structs inside one another, then their STOPs, inside the top-level struct. The
    // top-level struct is depth 1, so with the default limit of 16 a nest of 15 is the deepest.
    const auto nest = [](unsigned n) {
        Bytes out;
        for (unsigned i = 0; i < n; ++i) {
            out.insert(out.end(), {std::byte{0x0C}, std::byte{0}, std::byte{1}});
        }
        for (unsigned i = 0; i < n + 1; ++i) {
            out.push_back(std::byte{0});
        }
        return out;
    };
    REQUIRE_FALSE(skim(nest(15)).has_value());
    REQUIRE(skim(nest(16)) == thrift::Error::kTooDeep);
    REQUIRE(skim(nest(5000)) == thrift::Error::kTooDeep);
    REQUIRE_FALSE(skim(nest(3), thrift::Limits{4, 1000}).has_value());
    REQUIRE(skim(nest(4), thrift::Limits{4, 1000}) == thrift::Error::kTooDeep);

    // Lists of lists count as nesting too.
    const auto list_nest = [](unsigned n) {
        Bytes out = B({0x0F, 0, 1});
        for (unsigned i = 0; i < n; ++i) {
            out.insert(out.end(),
                       {std::byte{0x0F}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
        }
        out.insert(out.end(),
                   {std::byte{0x08}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
        out.push_back(std::byte{0});
        return out;
    };
    REQUIRE_FALSE(skim(list_nest(2)).has_value());
    REQUIRE(skim(list_nest(40)) == thrift::Error::kTooDeep);

    // The same through the payload parser: a content signature whose unknown field is a depth bomb.
    const auto bomb = unknown_field(12, 200, nest(2000));
    const auto refused = refusal(with_inserted(kFixtureBytes, kTopStop, bomb));
    REQUIRE(refused.error == ContentSignatureError::kBadOrMissingSignature);
    REQUIRE(mentions(refused.detail, "nested too deeply"));
}

TEST_CASE("thrift reader: the number of fields and elements is bounded", "[thrift]") {
    SECTION("many small fields") {
        // 40,000 unknown bool fields: each costs the reader two steps (the header, the value).
        Bytes input;
        for (int i = 0; i < 40000; ++i) {
            input.insert(input.end(), {std::byte{2}, std::byte{0}, std::byte{1}, std::byte{1}});
        }
        input.push_back(std::byte{0});
        REQUIRE(skim(input) == thrift::Error::kTooManyElements);
        REQUIRE_FALSE(skim(input, thrift::Limits{16, 200000}).has_value());
    }
    SECTION("a long run of bytes in a list") {
        Bytes input = B({0x0F, 0, 1, 0x03, 0, 1, 0x11, 0x70});  // a list of 70,000 BYTEs
        input.resize(input.size() + 70000, std::byte{0});
        input.push_back(std::byte{0});
        REQUIRE(skim(input) == thrift::Error::kTooManyElements);
    }
    SECTION("a small run is fine") {
        Bytes input = B({0x0F, 0, 1, 0x03, 0, 0, 0, 100});
        input.resize(input.size() + 100, std::byte{0});
        input.push_back(std::byte{0});
        REQUIRE_FALSE(skim(input).has_value());
    }
}

TEST_CASE("thrift reader: a callback reads the values it knows", "[thrift]") {
    // { 1: i32 = 258, 2: binary = "hi", 3: list<i32> = [] , 9: i32 (unknown, skipped) } STOP
    const Bytes input = B({0x08, 0, 1, 0,    0, 1, 2,            //
                           0x0B, 0, 2, 0,    0, 0, 2, 'h', 'i',  //
                           0x0F, 0, 3, 0x08, 0, 0, 0, 0,         //
                           0x08, 0, 9, 0,    0, 0, 7,            //
                           0x00});
    thrift::Reader reader(input);
    std::int32_t number = 0;
    std::string word;
    std::size_t list_count = 99;
    thrift::Type list_type = thrift::Type::kStop;
    const auto result = reader.read_struct(
        [&](const thrift::FieldHeader& header) -> std::expected<bool, thrift::Error> {
            switch (header.id) {
                case 1: {
                    const auto v = reader.read_i32();
                    if (!v) return std::unexpected(v.error());
                    number = *v;
                    return true;
                }
                case 2: {
                    const auto v = reader.read_binary();
                    if (!v) return std::unexpected(v.error());
                    word.assign(reinterpret_cast<const char*>(v->data()), v->size());
                    return true;
                }
                case 3: {
                    const auto v = reader.read_list_header();
                    if (!v) return std::unexpected(v.error());
                    list_count = v->count;
                    list_type = v->element;
                    return true;
                }
                default:
                    return false;
            }
        });
    REQUIRE(result.has_value());
    REQUIRE(number == 258);
    REQUIRE(word == "hi");
    REQUIRE(list_count == 0);
    REQUIRE(list_type == thrift::Type::kI32);
    REQUIRE(reader.remaining() == 0);
    REQUIRE(reader.expect_end().has_value());
}

TEST_CASE("thrift reader: read_binary checks the length it is given", "[thrift]") {
    // The same lengths as for a skipped string, reached through the function a parser calls.
    const auto read_one = [](const Bytes& value) {
        const Bytes input = cat({B({0x0B, 0, 1}), value, B({0})});
        thrift::Reader reader(input);
        std::optional<thrift::Error> error;
        const auto result = reader.read_struct(
            [&](const thrift::FieldHeader&) -> std::expected<bool, thrift::Error> {
                const auto bytes = reader.read_binary();
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                return true;
            });
        if (!result) {
            error = result.error();
        }
        return error;
    };
    REQUIRE_FALSE(read_one(B({0, 0, 0, 2, 'o', 'k'})).has_value());
    REQUIRE_FALSE(read_one(B({0, 0, 0, 0})).has_value());
    // Five bytes claimed, four left (three of text and the struct's STOP).
    REQUIRE(read_one(B({0, 0, 0, 5, 'a', 'b', 'c'})) == thrift::Error::kLengthExceedsInput);
    REQUIRE(read_one(B({0x7F, 0xFF, 0xFF, 0xFF})) == thrift::Error::kLengthExceedsInput);
    REQUIRE(read_one(B({0xFF, 0xFF, 0xFF, 0xFF})) == thrift::Error::kNegativeLength);
    REQUIRE(read_one(B({0, 0})) == thrift::Error::kTruncated);
}

TEST_CASE("thrift reader: expect_end reports bytes left over", "[thrift]") {
    const Bytes input = B({0x00, 0x00});
    thrift::Reader reader(input);
    REQUIRE(reader
                .read_struct([](const thrift::FieldHeader&) -> std::expected<bool, thrift::Error> {
                    return false;
                })
                .has_value());
    REQUIRE(reader.remaining() == 1);
    REQUIRE(reader.expect_end().error() == thrift::Error::kTrailingData);
}

TEST_CASE("thrift errors have words", "[thrift]") {
    for (const auto error : {thrift::Error::kTruncated, thrift::Error::kUnknownType,
                             thrift::Error::kNegativeLength, thrift::Error::kLengthExceedsInput,
                             thrift::Error::kTooDeep, thrift::Error::kTooManyElements,
                             thrift::Error::kTypeMismatch, thrift::Error::kDuplicateField,
                             thrift::Error::kMissingField, thrift::Error::kTrailingData}) {
        REQUIRE_FALSE(thrift::describe(error).empty());
        REQUIRE(thrift::describe(error) != "unknown error");
    }
}
