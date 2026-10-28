#pragma once

// A small certificate authority for tests: ephemeral P-256 keys, and X.509 certificates written
// with the shape and the faults a test asks for. Header-only, built on the library's own ECDSA and
// DER encoder so that a test needs no key or certificate file, and nothing here is committed key
// material: every key is derived from a seed string the test chooses, which is not a secret.
//
// The writer is deliberately more permissive than the reader under test. A test builds a chain
// that is wrong in one respect (an expired intermediate, a path length of zero, an unknown critical
// extension, a signature over other bytes) and shows that the reader refuses it for that reason.
// The differential tests (libs/base/tests/interop/) check that what this writes is also what
// OpenSSL reads, so the writer is not marking its own homework.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::test::pki {

using Bytes = std::vector<std::byte>;

inline Bytes bytes_of(std::string_view text) {
    Bytes out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

inline void append(Bytes& to, std::span<const std::byte> from) {
    to.insert(to.end(), from.begin(), from.end());
}

// --- DER writing ----------------------------------------------------------------------------

inline Bytes tlv(std::uint8_t tag, std::span<const std::byte> content) {
    Bytes out;
    out.push_back(static_cast<std::byte>(tag));
    const std::size_t n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<std::byte>(n));
    } else {
        std::vector<std::byte> length;
        for (std::size_t v = n; v != 0; v >>= 8) {
            length.insert(length.begin(), static_cast<std::byte>(v & 0xFF));
        }
        out.push_back(static_cast<std::byte>(0x80 | length.size()));
        append(out, length);
    }
    append(out, content);
    return out;
}

inline Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        append(out, part);
    }
    return out;
}

inline Bytes sequence(std::initializer_list<Bytes> parts) {
    return tlv(0x30, concat(parts));
}
inline Bytes set_of(std::initializer_list<Bytes> parts) {
    return tlv(0x31, concat(parts));
}

// INTEGER from big-endian magnitude bytes: strips redundant leading zeros, then adds one if the
// high bit would read as a sign.
inline Bytes integer(std::span<const std::byte> magnitude) {
    std::size_t skip = 0;
    while (skip + 1 < magnitude.size() && magnitude[skip] == std::byte{0}) {
        ++skip;
    }
    Bytes content;
    if (magnitude.empty() || (std::to_integer<unsigned>(magnitude[skip]) & 0x80u) != 0) {
        content.push_back(std::byte{0});
    }
    append(content, magnitude.subspan(skip));
    return tlv(0x02, content);
}

inline Bytes small_integer(std::uint32_t value) {
    Bytes magnitude;
    for (int shift = 24; shift >= 0; shift -= 8) {
        magnitude.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
    return integer(magnitude);
}

inline Bytes oid(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    std::uint64_t current = 0;
    for (const char c : dotted) {
        if (c == '.') {
            arcs.push_back(current);
            current = 0;
        } else {
            current = current * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    arcs.push_back(current);
    Bytes content;
    // Base 128, most significant group first, the continuation bit set on all but the last.
    const auto base128 = [&](std::uint64_t v) {
        std::array<unsigned, 10> groups{};
        std::size_t n = 0;
        do {
            groups[n++] = static_cast<unsigned>(v & 0x7F);
            v >>= 7;
        } while (v != 0);
        for (std::size_t i = n; i-- > 0;) {
            content.push_back(static_cast<std::byte>(groups[i] | (i != 0 ? 0x80u : 0u)));
        }
    };
    base128(arcs[0] * 40 + arcs[1]);
    for (std::size_t i = 2; i < arcs.size(); ++i) {
        base128(arcs[i]);
    }
    return tlv(0x06, content);
}

inline Bytes utf8(std::string_view text) {
    return tlv(0x0C, bytes_of(text));
}
inline Bytes octet_string(std::span<const std::byte> content) {
    return tlv(0x04, content);
}
inline Bytes boolean(bool value) {
    return tlv(0x01, Bytes{value ? std::byte{0xFF} : std::byte{0x00}});
}

// BIT STRING of whole bytes (no unused bits).
inline Bytes bit_string(std::span<const std::byte> content) {
    Bytes body{std::byte{0}};
    append(body, content);
    return tlv(0x03, body);
}

// Time as a certificate writes it: UTCTime for 1950 to 2049, GeneralizedTime outside.
inline Bytes time_element(std::int64_t unix_seconds) {
    // "YYYY-MM-DDThh:mm:ssZ"
    const std::string iso = iclforge::base::crypto::format_iso8601_utc(unix_seconds);
    std::string digits;
    for (const char c : iso) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        }
    }
    const int year = std::stoi(digits.substr(0, 4));
    if (year >= 1950 && year < 2050) {
        return tlv(0x17, bytes_of(digits.substr(2) + "Z"));
    }
    return tlv(0x18, bytes_of(digits + "Z"));
}

inline std::int64_t at(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
    return *iclforge::base::crypto::unix_time_from_civil(year, month, day, hour, minute, second);
}

// --- Keys -----------------------------------------------------------------------------------

struct KeyPair {
    iclforge::base::crypto::P256PrivateKey key;
    iclforge::base::crypto::P256PublicKey pub;
};

// A key from a seed: SHA-256 of "seed" and a counter, taken as the scalar when it is in range.
// Reproducible, so a failing test fails the same way next time; not secret, so never use it for
// anything but a test.
inline KeyPair key_from_seed(std::string_view seed) {
    for (std::uint32_t counter = 0;; ++counter) {
        const auto digest = iclforge::base::crypto::sha256(
            bytes_of(std::string{seed} + "/" + std::to_string(counter)));
        auto key = iclforge::base::crypto::P256PrivateKey::from_scalar(digest);
        if (key.has_value()) {
            auto pub = key->public_key();
            return KeyPair{std::move(*key), *pub};
        }
    }
}

inline Bytes spki_der(const iclforge::base::crypto::P256PublicKey& key) {
    const auto point = key.uncompressed();
    return sequence(
        {sequence({oid("1.2.840.10045.2.1"), oid("1.2.840.10045.3.1.7")}), bit_string(point)});
}

// ECPrivateKey (RFC 5915). `with_parameters` adds the [0] named curve, `with_public` the [1]
// public key.
inline Bytes sec1_der(const KeyPair& pair, bool with_parameters = true, bool with_public = true) {
    Bytes body = concat({small_integer(1), octet_string(pair.key.scalar())});
    if (with_parameters) {
        append(body, tlv(0xA0, oid("1.2.840.10045.3.1.7")));
    }
    if (with_public) {
        append(body, tlv(0xA1, bit_string(pair.pub.uncompressed())));
    }
    return tlv(0x30, body);
}

// PrivateKeyInfo (RFC 5208) holding an ECPrivateKey without the parameters, as OpenSSL writes it.
inline Bytes pkcs8_der(const KeyPair& pair) {
    return sequence({small_integer(0),
                     sequence({oid("1.2.840.10045.2.1"), oid("1.2.840.10045.3.1.7")}),
                     octet_string(sec1_der(pair, false, true))});
}

// --- Certificates ---------------------------------------------------------------------------

struct Attribute {
    std::string oid;
    std::string value;
    std::uint8_t string_tag = 0x0C;  // UTF8String; 0x13 PrintableString, 0x1E BMPString, ...
};

inline std::vector<Attribute> name_of(std::string_view common_name, std::string_view title = {},
                                      std::string_view serial_number = {}) {
    std::vector<Attribute> out;
    if (!serial_number.empty()) out.push_back({"2.5.4.5", std::string{serial_number}, 0x0C});
    if (!title.empty()) out.push_back({"2.5.4.12", std::string{title}, 0x0C});
    out.push_back({"2.5.4.3", std::string{common_name}, 0x0C});
    return out;
}

inline Bytes name_der(const std::vector<Attribute>& attributes) {
    Bytes body;
    for (const Attribute& a : attributes) {
        append(body, set_of({sequence({oid(a.oid), tlv(a.string_tag, bytes_of(a.value))})}));
    }
    return tlv(0x30, body);
}

struct Extension {
    std::string oid;
    bool critical = false;
    Bytes value;  // the DER inside the OCTET STRING
};

struct CertificateSpec {
    int version = 3;  // 1 or 3
    Bytes serial = {std::byte{0x01}};
    std::vector<Attribute> issuer;
    std::vector<Attribute> subject;
    std::int64_t not_before = 0;
    std::int64_t not_after = 0;
    iclforge::base::crypto::P256PublicKey subject_key;

    // keyUsage as bit numbers (digitalSignature 0, keyCertSign 5); empty for no extension.
    std::vector<int> key_usage_bits;
    bool key_usage_critical = true;
    // basicConstraints: absent unless `ca` is set.
    std::optional<bool> ca;
    std::optional<std::uint32_t> path_length;
    bool basic_constraints_critical = true;
    std::vector<Extension> extra_extensions;

    // Faults for the reader's tests.
    std::optional<Bytes> raw_serial_tlv;  // the whole INTEGER element, replacing `serial`
    Bytes trailing_in_tbs;                // bytes after the last field of the TBS
};

inline Bytes key_usage_value(const std::vector<int>& bits) {
    int highest = 0;
    for (const int b : bits) {
        highest = std::max(highest, b);
    }
    const std::size_t byte_count = static_cast<std::size_t>(highest / 8) + 1;
    Bytes flags(byte_count, std::byte{0});
    for (const int b : bits) {
        flags[static_cast<std::size_t>(b / 8)] |= static_cast<std::byte>(0x80u >> (b % 8));
    }
    // DER drops the trailing zero bits of a named bit string, which the unused-bits count says.
    int unused = 0;
    while (unused < 7 && (std::to_integer<unsigned>(flags.back()) & (1u << unused)) == 0) {
        ++unused;
    }
    Bytes body{static_cast<std::byte>(unused)};
    append(body, flags);
    return tlv(0x03, body);
}

inline Bytes extension_der(const Extension& e) {
    Bytes body = oid(e.oid);
    if (e.critical) {
        append(body, boolean(true));
    }
    append(body, octet_string(e.value));
    return tlv(0x30, body);
}

inline Bytes tbs_der(const CertificateSpec& spec, std::span<const std::byte> signature_algorithm) {
    Bytes body;
    if (spec.version == 3) {
        append(body, tlv(0xA0, small_integer(2)));
    }
    append(body, spec.raw_serial_tlv.has_value() ? *spec.raw_serial_tlv : integer(spec.serial));
    append(body, signature_algorithm);
    append(body, name_der(spec.issuer));
    append(body, sequence({time_element(spec.not_before), time_element(spec.not_after)}));
    append(body, name_der(spec.subject));
    append(body, spki_der(spec.subject_key));
    if (spec.version == 3) {
        Bytes extensions;
        if (spec.ca.has_value()) {
            Bytes bc;
            if (*spec.ca) append(bc, boolean(true));
            if (spec.path_length.has_value()) append(bc, small_integer(*spec.path_length));
            append(extensions,
                   extension_der({"2.5.29.19", spec.basic_constraints_critical, tlv(0x30, bc)}));
        }
        if (!spec.key_usage_bits.empty()) {
            append(extensions, extension_der({"2.5.29.15", spec.key_usage_critical,
                                              key_usage_value(spec.key_usage_bits)}));
        }
        for (const Extension& e : spec.extra_extensions) {
            append(extensions, extension_der(e));
        }
        if (!extensions.empty()) {
            append(body, tlv(0xA3, tlv(0x30, extensions)));
        }
    }
    append(body, spec.trailing_in_tbs);
    return tlv(0x30, body);
}

inline const Bytes& ecdsa_with_sha256() {
    static const Bytes algorithm = sequence({oid("1.2.840.10045.4.3.2")});
    return algorithm;
}

// A certificate signed by `issuer_key`: the to-be-signed part, the algorithm, the DER signature.
inline Bytes build_certificate(const CertificateSpec& spec,
                               const iclforge::base::crypto::P256PrivateKey& issuer_key) {
    const Bytes tbs = tbs_der(spec, ecdsa_with_sha256());
    const auto signature =
        iclforge::base::crypto::ecdsa_sign(issuer_key, iclforge::base::crypto::sha256(tbs));
    const Bytes signature_der = iclforge::base::crypto::encode_ecdsa_der(*signature);
    return sequence({tbs, ecdsa_with_sha256(), bit_string(signature_der)});
}

// --- A three-level authority, shaped like the specification's chain ---------------------------

struct Node {
    KeyPair keys;
    Bytes der;
    iclforge::base::crypto::Certificate cert;
};

struct Authority {
    Node root;
    Node intermediate;
    Node leaf;

    // The chain in wire order, root end first.
    [[nodiscard]] std::vector<iclforge::base::crypto::Certificate> chain() const {
        return {root.cert, intermediate.cert, leaf.cert};
    }
    [[nodiscard]] std::vector<iclforge::base::crypto::Certificate> shuffled_chain() const {
        return {leaf.cert, root.cert, intermediate.cert};
    }
};

struct AuthorityOptions {
    std::string seed = "authority";
    std::int64_t root_not_before = 0;
    std::int64_t root_not_after = 0;
    std::int64_t intermediate_not_before = 0;
    std::int64_t intermediate_not_after = 0;
    std::int64_t leaf_not_before = 0;
    std::int64_t leaf_not_after = 0;
    std::optional<std::uint32_t> root_path_length = std::nullopt;
    std::optional<std::uint32_t> intermediate_path_length = std::nullopt;
    std::vector<std::string> leaf_name = {"ILok", "Code/Content Signer", "1798A2B5D62000000005"};
    Bytes leaf_serial = {std::byte{0x17}, std::byte{0x98}, std::byte{0xA2}, std::byte{0xB5},
                         std::byte{0xD6}, std::byte{0x20}, std::byte{0x00}, std::byte{0x00},
                         std::byte{0x00}, std::byte{0x05}};
    std::vector<int> leaf_key_usage = {0};
    int root_version = 3;  // 1 builds a v1 root, which has no extensions to say it is a CA
    bool intermediate_is_ca = true;
    std::vector<int> intermediate_key_usage = {5, 6};
    std::vector<int> root_key_usage = {5, 6};
};

// A root, an intermediate and a leaf valid from 2024-01-01 to 2034-01-01 unless the options say
// otherwise.
inline iclforge::base::crypto::Certificate parse_or_die(const Bytes& der) {
    auto cert = iclforge::base::crypto::parse_certificate(der);
    if (!cert.has_value()) {
        throw std::runtime_error(
            std::string{"test_pki: a certificate the test built does not parse: "} +
            std::string{iclforge::base::crypto::describe(cert.error())});
    }
    return std::move(*cert);
}

inline Authority make_authority(AuthorityOptions o = {}) {
    const std::int64_t from = at(2024, 1, 1);
    const std::int64_t to = at(2034, 1, 1);
    const auto fill = [&](std::int64_t& value, std::int64_t fallback) {
        if (value == 0) value = fallback;
    };
    fill(o.root_not_before, from);
    fill(o.root_not_after, to);
    fill(o.intermediate_not_before, from);
    fill(o.intermediate_not_after, to);
    fill(o.leaf_not_before, from);
    fill(o.leaf_not_after, to);

    Authority a;
    a.root.keys = key_from_seed(o.seed + "/root");
    a.intermediate.keys = key_from_seed(o.seed + "/intermediate");
    a.leaf.keys = key_from_seed(o.seed + "/leaf");

    CertificateSpec root;
    root.serial = {std::byte{0x01}};
    root.issuer = root.subject = name_of("Test Master CA");
    root.not_before = o.root_not_before;
    root.not_after = o.root_not_after;
    root.subject_key = a.root.keys.pub;
    root.ca = true;
    root.version = o.root_version;
    root.path_length = o.root_path_length;
    root.key_usage_bits = o.root_key_usage;
    a.root.der = build_certificate(root, a.root.keys.key);
    a.root.cert = parse_or_die(a.root.der);

    CertificateSpec mid;
    mid.serial = {std::byte{0x02}};
    mid.issuer = name_of("Test Master CA");
    mid.subject = name_of("Test Signing CA");
    mid.not_before = o.intermediate_not_before;
    mid.not_after = o.intermediate_not_after;
    mid.subject_key = a.intermediate.keys.pub;
    mid.ca = o.intermediate_is_ca ? std::optional<bool>{true} : std::optional<bool>{false};
    mid.path_length = o.intermediate_path_length;
    mid.key_usage_bits = o.intermediate_key_usage;
    a.intermediate.der = build_certificate(mid, a.root.keys.key);
    a.intermediate.cert = parse_or_die(a.intermediate.der);

    CertificateSpec leaf;
    leaf.serial = o.leaf_serial;
    leaf.issuer = name_of("Test Signing CA");
    leaf.subject = name_of(o.leaf_name[0], o.leaf_name[1], o.leaf_name[2]);
    leaf.not_before = o.leaf_not_before;
    leaf.not_after = o.leaf_not_after;
    leaf.subject_key = a.leaf.keys.pub;
    leaf.key_usage_bits = o.leaf_key_usage;
    a.leaf.der = build_certificate(leaf, a.intermediate.keys.key);
    a.leaf.cert = parse_or_die(a.leaf.der);
    return a;
}

}  // namespace iclforge::test::pki
