#pragma once

// A bounded reader for ASN.1 DER (ITU-T X.690 §8, §10), the subset an X.509 certificate and an EC
// private key use. It reads; it never allocates for a value it returns (a Tlv is two spans into the
// caller's bytes) and never recurses, so a hostile certificate cannot make it run deep or long. The
// callers (x509.cpp, credentials.cpp) walk the structure with it one level at a time.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto::der {

inline constexpr std::uint8_t kBoolean = 0x01;
inline constexpr std::uint8_t kInteger = 0x02;
inline constexpr std::uint8_t kBitString = 0x03;
inline constexpr std::uint8_t kOctetString = 0x04;
inline constexpr std::uint8_t kNull = 0x05;
inline constexpr std::uint8_t kObjectIdentifier = 0x06;
inline constexpr std::uint8_t kUtf8String = 0x0C;
inline constexpr std::uint8_t kPrintableString = 0x13;
inline constexpr std::uint8_t kTeletexString = 0x14;
inline constexpr std::uint8_t kIa5String = 0x16;
inline constexpr std::uint8_t kUtcTime = 0x17;
inline constexpr std::uint8_t kGeneralizedTime = 0x18;
inline constexpr std::uint8_t kBmpString = 0x1E;
inline constexpr std::uint8_t kSequence = 0x30;
inline constexpr std::uint8_t kSet = 0x31;
// Context-specific, constructed: [0], [1], [2], [3] as X.509 uses them.
inline constexpr std::uint8_t kContext0 = 0xA0;
inline constexpr std::uint8_t kContext1 = 0xA1;
inline constexpr std::uint8_t kContext2 = 0xA2;
inline constexpr std::uint8_t kContext3 = 0xA3;

struct Tlv {
    std::uint8_t tag = 0;
    std::span<const std::byte> content;  // the value octets
    std::span<const std::byte> encoded;  // tag, length and value octets, as they appeared
};

// Reads the element at the front of `input` and advances `input` past it. Strict DER: a one-byte
// tag (a tag number of 31 or more is refused), a definite length in the fewest octets (a long form
// that a short form could carry is refused, and so is the indefinite form), and a length that fits
// what remains. nullopt on any violation.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<Tlv> read(std::span<const std::byte>& input);

// Reads one element and requires it to have `tag`.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<Tlv> read_expecting(
    std::span<const std::byte>& input, std::uint8_t tag);

// The dotted form of an OBJECT IDENTIFIER's value octets ("1.2.840.10045.2.1"); nullopt when the
// encoding is not a valid OID (X.690 §8.19).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::string> oid_to_string(
    std::span<const std::byte> content);

// An INTEGER's value as a non-negative number of at most 8 octets, for small fields (a version, a
// path length). nullopt when it is negative, too large, or not minimally encoded.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::uint64_t> small_unsigned(
    std::span<const std::byte> content);

}  // namespace iclforge::base::crypto::der
