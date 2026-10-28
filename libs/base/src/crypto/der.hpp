#pragma once

// A bounded reader for the subset of ASN.1 DER (ITU-T X.690) that certificates, keys and ECDSA
// signatures use, private to iclforge::base's crypto code. Every length is checked against what
// remains before it is used, so a hostile length cannot make a reader run past its input, and only
// the DER encodings are accepted: a single-byte tag, a definite length in the shortest form.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iclforge::base::crypto::detail {

inline constexpr std::uint8_t kTagBoolean = 0x01;
inline constexpr std::uint8_t kTagInteger = 0x02;
inline constexpr std::uint8_t kTagBitString = 0x03;
inline constexpr std::uint8_t kTagOctetString = 0x04;
inline constexpr std::uint8_t kTagNull = 0x05;
inline constexpr std::uint8_t kTagObjectIdentifier = 0x06;
inline constexpr std::uint8_t kTagUtf8String = 0x0C;
inline constexpr std::uint8_t kTagSequence = 0x30;
inline constexpr std::uint8_t kTagSet = 0x31;
inline constexpr std::uint8_t kTagUtcTime = 0x17;
inline constexpr std::uint8_t kTagGeneralizedTime = 0x18;

struct DerElement {
    std::uint8_t tag = 0;
    std::span<const std::byte> content;
    std::span<const std::byte> whole;  // tag, length and content
};

// Reads one element from the front of `input` and advances it past the element. nullopt, with
// `input` unchanged, for a truncated or non-DER element. A tag number of 31 or more (the
// multi-byte tag form) is refused: nothing in a certificate needs it.
[[nodiscard]] inline std::optional<DerElement> read_element(std::span<const std::byte>& input) {
    if (input.size() < 2) {
        return std::nullopt;
    }
    const std::uint8_t tag = std::to_integer<std::uint8_t>(input[0]);
    if ((tag & 0x1F) == 0x1F) {
        return std::nullopt;
    }
    const std::uint8_t first = std::to_integer<std::uint8_t>(input[1]);
    std::size_t header = 2;
    std::size_t length = 0;
    if (first < 0x80) {
        length = first;
    } else {
        // Long form: 0x80 | n, then n length bytes, big endian. 0x80 alone is the indefinite form
        // (BER), which DER does not have. The first length byte must not be zero and a value below
        // 128 must use the short form, so each length has one encoding.
        const std::size_t count = first & 0x7Fu;
        if (count == 0 || count > 4 || input.size() < 2 + count) {
            return std::nullopt;
        }
        if (std::to_integer<std::uint8_t>(input[2]) == 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < count; ++i) {
            length = (length << 8) | std::to_integer<std::uint8_t>(input[2 + i]);
        }
        if (length < 0x80) {
            return std::nullopt;
        }
        header = 2 + count;
    }
    if (length > input.size() - header) {
        return std::nullopt;
    }
    DerElement element;
    element.tag = tag;
    element.whole = input.first(header + length);
    element.content = input.subspan(header, length);
    input = input.subspan(header + length);
    return element;
}

// The next element when its tag is `tag`; otherwise nullopt and `input` is unchanged.
[[nodiscard]] inline std::optional<DerElement> read_tagged(std::span<const std::byte>& input,
                                                           std::uint8_t tag) {
    std::span<const std::byte> probe = input;
    const auto element = read_element(probe);
    if (!element.has_value() || element->tag != tag) {
        return std::nullopt;
    }
    input = probe;
    return element;
}

// The content of an OBJECT IDENTIFIER as dotted decimal ("1.2.840.10045.2.1"); empty if the
// encoding is not a well-formed OID.
[[nodiscard]] inline std::string oid_to_string(std::span<const std::byte> content) {
    if (content.empty()) {
        return {};
    }
    std::string out;
    std::uint64_t value = 0;
    bool first = true;
    bool in_progress = false;
    for (const std::byte b : content) {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(b);
        if (value > (std::uint64_t{1} << 56)) {
            return {};
        }
        if (!in_progress && byte == 0x80) {
            return {};  // a subidentifier with a redundant leading zero group
        }
        value = (value << 7) | (byte & 0x7Fu);
        in_progress = (byte & 0x80u) != 0;
        if (in_progress) {
            continue;
        }
        if (first) {
            // The first subidentifier packs the first two arcs: 40*x + y, with x in 0..2.
            const std::uint64_t x = value < 80 ? value / 40 : 2;
            out = std::to_string(x) + "." + std::to_string(value - 40 * x);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
        value = 0;
    }
    return in_progress ? std::string{} : out;
}

// True when `content` is exactly the encoding of the OID `dotted` (compared as strings, so only
// the OIDs the callers name need spelling out).
[[nodiscard]] inline bool oid_equals(std::span<const std::byte> content, std::string_view dotted) {
    return oid_to_string(content) == dotted;
}

inline constexpr std::string_view kOidEcPublicKey = "1.2.840.10045.2.1";
inline constexpr std::string_view kOidPrime256v1 = "1.2.840.10045.3.1.7";

enum class EcAlgorithm : std::uint8_t {
    kP256,        // id-ecPublicKey with the named curve prime256v1
    kOtherCurve,  // id-ecPublicKey with another curve, or with implicit or explicit parameters
    kNotEc,       // another algorithm altogether (RSA, Ed25519, ...)
    kMalformed,
};

// Classifies the content of an AlgorithmIdentifier SEQUENCE as RFC 5480 section 2.1.1 writes an
// EC key's: the OID id-ecPublicKey, then the named curve as its parameters. SubjectPublicKeyInfo
// and PKCS#8's PrivateKeyInfo both carry this, and a reader wants to say "that is an RSA key" and
// "that is a P-384 key" in different words.
[[nodiscard]] inline EcAlgorithm classify_ec_algorithm(std::span<const std::byte> content) {
    std::span<const std::byte> input = content;
    const auto algorithm = read_tagged(input, kTagObjectIdentifier);
    if (!algorithm.has_value()) {
        return EcAlgorithm::kMalformed;
    }
    if (oid_to_string(algorithm->content) != kOidEcPublicKey) {
        return EcAlgorithm::kNotEc;
    }
    const auto parameters = read_element(input);
    if (!parameters.has_value() || !input.empty()) {
        return EcAlgorithm::kMalformed;
    }
    if (parameters->tag != kTagObjectIdentifier) {
        return EcAlgorithm::kOtherCurve;  // NULL, or an explicit ECParameters SEQUENCE
    }
    return oid_to_string(parameters->content) == kOidPrime256v1 ? EcAlgorithm::kP256
                                                                : EcAlgorithm::kOtherCurve;
}

}  // namespace iclforge::base::crypto::detail
