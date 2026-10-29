#pragma once

// A small certificate authority for tests: builds X.509 v3 certificates and a four-level chain of
// the shape CS spec §4.1 describes (a root, two intermediates, a leaf) from deterministic keys, so
// that a test can sign, tamper with and expire things without a file, a tool or a network. Every
// name here is obviously a test's own ("ICL Forge Test ..."); nothing imitates an issuer.
//
// The keys derive from a label by hashing it, so they are fixed and public. They protect nothing.
// Header-only, internal to the tests; the oracle for what it builds is OpenSSL
// (tools/checks/check_content_signature_openssl.py parses the same bytes with `openssl x509`).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::test::pki {

using Bytes = std::vector<std::byte>;

inline Bytes tlv(std::uint8_t tag, const Bytes& content) {
    Bytes out;
    out.push_back(static_cast<std::byte>(tag));
    const std::size_t n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<std::byte>(n));
    } else if (n < 0x100) {
        out.push_back(std::byte{0x81});
        out.push_back(static_cast<std::byte>(n));
    } else {
        out.push_back(std::byte{0x82});
        out.push_back(static_cast<std::byte>(n >> 8));
        out.push_back(static_cast<std::byte>(n & 0xFFU));
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

inline Bytes oid(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    std::uint64_t cur = 0;
    for (const char c : dotted) {
        if (c == '.') {
            arcs.push_back(cur);
            cur = 0;
        } else {
            cur = cur * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    arcs.push_back(cur);
    Bytes body;
    const auto put = [&body](std::uint64_t v) {
        std::vector<std::byte> groups;
        groups.insert(groups.begin(), static_cast<std::byte>(v & 0x7FU));
        v >>= 7;
        while (v != 0) {
            groups.insert(groups.begin(), static_cast<std::byte>(0x80U | (v & 0x7FU)));
            v >>= 7;
        }
        body.insert(body.end(), groups.begin(), groups.end());
    };
    put(arcs[0] * 40 + arcs[1]);
    for (std::size_t i = 2; i < arcs.size(); ++i) {
        put(arcs[i]);
    }
    return tlv(0x06, body);
}

inline Bytes text(std::string_view s) {
    return Bytes(reinterpret_cast<const std::byte*>(s.data()),
                 reinterpret_cast<const std::byte*>(s.data()) + s.size());
}
inline Bytes utf8(std::string_view s) {
    return tlv(0x0C, text(s));
}
inline Bytes printable(std::string_view s) {
    return tlv(0x13, text(s));
}
inline Bytes sequence(const Bytes& c) {
    return tlv(0x30, c);
}
inline Bytes set_of(const Bytes& c) {
    return tlv(0x31, c);
}
inline Bytes octets(const Bytes& c) {
    return tlv(0x04, c);
}
inline Bytes boolean(bool v) {
    return tlv(0x01, Bytes{v ? std::byte{0xFF} : std::byte{0x00}});
}

inline Bytes integer_from_bytes(Bytes value) {
    while (value.size() > 1 && value[0] == std::byte{0}) {
        value.erase(value.begin());
    }
    if (value.empty()) {
        value.push_back(std::byte{0});
    }
    if ((std::to_integer<unsigned>(value[0]) & 0x80U) != 0) {
        value.insert(value.begin(), std::byte{0});
    }
    return tlv(0x02, value);
}
inline Bytes integer(std::uint64_t v) {
    Bytes b;
    for (int shift = 56; shift >= 0; shift -= 8) {
        b.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
    }
    return integer_from_bytes(b);
}

inline Bytes bit_string(const Bytes& content, std::uint8_t unused = 0) {
    Bytes body;
    body.push_back(static_cast<std::byte>(unused));
    body.insert(body.end(), content.begin(), content.end());
    return tlv(0x03, body);
}

inline Bytes utc_time(std::int64_t epoch) {
    // YYMMDDHHMMSSZ, valid for 1950..2049, which is where the test certificates sit.
    const std::string iso =
        iclforge::base::crypto::format_iso8601_utc(epoch);  // YYYY-MM-DDTHH:MM:SS
    std::string s = iso.substr(2, 2) + iso.substr(5, 2) + iso.substr(8, 2) + iso.substr(11, 2) +
                    iso.substr(14, 2) + iso.substr(17, 2) + "Z";
    return tlv(0x17, text(s));
}

inline Bytes generalized_time(std::int64_t epoch) {
    const std::string iso = iclforge::base::crypto::format_iso8601_utc(epoch);
    std::string s = iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2) + iso.substr(11, 2) +
                    iso.substr(14, 2) + iso.substr(17, 2) + "Z";
    return tlv(0x18, text(s));
}

inline std::int64_t epoch(std::string_view iso) {
    return *iclforge::base::crypto::parse_iso8601_utc(iso);
}

inline Bytes bytes_from_hex(std::string_view hex) {
    Bytes out;
    int pending = -1;
    for (const char c : hex) {
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        }
        if (v < 0) {
            continue;
        }
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<std::byte>((pending << 4) | v));
            pending = -1;
        }
    }
    return out;
}

// A key from a label: SHA-256 of the label, re-hashed until it is a valid scalar.
inline iclforge::base::crypto::P256PrivateKey key_for(std::string_view label) {
    Bytes seed = text(label);
    for (;;) {
        const auto digest = iclforge::base::crypto::sha256(seed);
        if (auto key = iclforge::base::crypto::P256PrivateKey::from_scalar(digest)) {
            return *key;
        }
        seed.assign(digest.begin(), digest.end());
    }
}

struct Name {
    std::string common_name;
    std::optional<std::string> title;
    std::optional<std::string> serial_number;  // the serialNumber attribute, not the cert serial
    std::string organization = "ICL Forge Test";

    Name() = default;
    explicit Name(std::string cn, std::optional<std::string> t = std::nullopt,
                  std::optional<std::string> sn = std::nullopt)
        : common_name(std::move(cn)), title(std::move(t)), serial_number(std::move(sn)) {}
};

inline Bytes name_der(const Name& n) {
    const auto attribute = [](std::string_view oid_text, const Bytes& value) {
        return set_of(sequence(cat({oid(oid_text), value})));
    };
    Bytes rdns;
    if (n.serial_number) {
        rdns = cat({rdns, attribute("2.5.4.5", printable(*n.serial_number))});
    }
    rdns = cat({rdns, attribute("2.5.4.3", utf8(n.common_name)),
                attribute("2.5.4.10", utf8(n.organization))});
    if (n.title) {
        rdns = cat({rdns, attribute("2.5.4.12", utf8(*n.title))});
    }
    return sequence(rdns);
}

struct CertSpec {
    Name subject;
    std::string serial_hex = "01";
    std::string not_before = "2020-01-01T00:00:00";
    std::string not_after = "2040-01-01T00:00:00";
    std::optional<bool> ca = std::nullopt;  // basicConstraints, critical, when set
    std::optional<std::uint64_t> path_length;
    std::optional<std::uint16_t>
        key_usage;  // RFC 5280 bit 0 = digitalSignature; critical, when set
    bool unknown_critical_extension = false;
    bool unknown_noncritical_extension = false;
    bool generalized_time = false;
    int version = 2;  // v3
};

struct Issued {
    Bytes der;
    Name subject;
    iclforge::base::crypto::P256PrivateKey key;
};

inline Bytes key_usage_der(std::uint16_t bits) {
    // BIT STRING of the named bits, 9 at most (RFC 5280 §4.2.1.3), trailing zero bits dropped.
    std::array<std::uint8_t, 2> octets_{};
    unsigned highest = 0;
    for (unsigned b = 0; b < 9; ++b) {
        if ((bits & (1U << b)) != 0) {
            octets_[b / 8] = static_cast<std::uint8_t>(octets_[b / 8] | (0x80U >> (b % 8)));
            highest = b;
        }
    }
    const std::size_t used_octets = highest / 8 + 1;
    const unsigned unused = static_cast<unsigned>(used_octets * 8 - (highest + 1));
    Bytes content;
    for (std::size_t i = 0; i < used_octets; ++i) {
        content.push_back(static_cast<std::byte>(octets_[i]));
    }
    return bit_string(content, static_cast<std::uint8_t>(unused));
}

inline Bytes extension(std::string_view oid_text, bool critical, const Bytes& value) {
    Bytes body = oid(oid_text);
    if (critical) {
        body = cat({body, boolean(true)});
    }
    body = cat({body, octets(value)});
    return sequence(body);
}

// A certificate for `subject_key`, issued under `issuer_name` and signed with `issuer_key`.
inline Bytes issue(const CertSpec& spec, const iclforge::base::crypto::P256PrivateKey& subject_key,
                   const Name& issuer_name,
                   const iclforge::base::crypto::P256PrivateKey& issuer_key) {
    using namespace iclforge::base::crypto;
    const Bytes signature_algorithm = sequence(oid("1.2.840.10045.4.3.2"));  // ecdsa-with-SHA256
    const P256PublicKey pub = *p256_public_key(subject_key);
    const auto point = p256_public_key_uncompressed(pub);
    const Bytes spki =
        sequence(cat({sequence(cat({oid("1.2.840.10045.2.1"), oid("1.2.840.10045.3.1.7")})),
                      bit_string(Bytes(point.begin(), point.end()))}));
    const auto time = [&spec](std::string_view iso) {
        return spec.generalized_time ? generalized_time(epoch(iso)) : utc_time(epoch(iso));
    };
    Bytes extensions;
    if (spec.ca.has_value()) {
        Bytes bc;
        if (*spec.ca) {
            bc = cat({bc, boolean(true)});
        }
        if (spec.path_length) {
            bc = cat({bc, integer(*spec.path_length)});
        }
        extensions = cat({extensions, extension("2.5.29.19", true, sequence(bc))});
    }
    if (spec.key_usage.has_value()) {
        extensions =
            cat({extensions, extension("2.5.29.15", true, key_usage_der(*spec.key_usage))});
    }
    if (spec.unknown_noncritical_extension) {
        extensions =
            cat({extensions, extension("1.3.6.1.4.1.99999.1", false, octets(Bytes{std::byte{1}}))});
    }
    if (spec.unknown_critical_extension) {
        extensions =
            cat({extensions, extension("1.3.6.1.4.1.99999.2", true, octets(Bytes{std::byte{1}}))});
    }
    Bytes tbs_body =
        cat({tlv(0xA0, integer(static_cast<std::uint64_t>(spec.version))),
             integer_from_bytes(bytes_from_hex(spec.serial_hex)), signature_algorithm,
             name_der(issuer_name), sequence(cat({time(spec.not_before), time(spec.not_after)})),
             name_der(spec.subject), spki});
    if (!extensions.empty()) {
        tbs_body = cat({tbs_body, tlv(0xA3, sequence(extensions))});
    }
    const Bytes tbs = sequence(tbs_body);
    const auto signature = *ecdsa_p256_sign(issuer_key, sha256(tbs));
    const Bytes signature_der = [&] {
        const auto der = ecdsa_signature_to_der(signature);
        return Bytes(der.begin(), der.end());
    }();
    return sequence(cat({tbs, signature_algorithm, bit_string(signature_der)}));
}

// The chain CS spec §4.1 draws: [0] root, [1] master, [2] issuing, [3] content signer, with the
// master's validity ending before the date the tests treat as "today" (so that, like the spec's own
// example chain, an expired intermediate is part of the fixture).
struct TestChain {
    std::array<Bytes, 4> certificates;  // DER, root first
    std::array<Name, 4> names;
    iclforge::base::crypto::P256PrivateKey leaf_key;
    iclforge::base::crypto::P256PrivateKey root_key;
};

inline TestChain make_chain(const std::string& master_not_after = "2025-11-13T01:47:10") {
    TestChain c;
    c.names[0] = Name{"ICL Forge Test Root CA", std::nullopt, "T0001"};
    c.names[1] = Name{"ICL Forge Test Master CA", std::nullopt, "T0002"};
    c.names[2] = Name{"ICL Forge Test Issuing CA", std::nullopt, "T0003"};
    c.names[3] = Name{"ICL Forge Test Content Signer", "Code/Content Signer", "T0004"};
    const auto root_key = key_for("icl-forge-test-root");
    const auto master_key = key_for("icl-forge-test-master");
    const auto issuing_key = key_for("icl-forge-test-issuing");
    c.leaf_key = key_for("icl-forge-test-leaf");
    c.root_key = root_key;

    CertSpec root;
    root.subject = c.names[0];
    root.serial_hex = "0A0001";
    root.not_before = "2010-04-21T23:59:59";
    root.not_after = "2040-04-15T23:59:59";
    root.ca = true;
    root.key_usage = (1U << 0) | (1U << 5) | (1U << 6);
    c.certificates[0] = issue(root, root_key, c.names[0], root_key);

    CertSpec master;
    master.subject = c.names[1];
    master.serial_hex = "0A0002";
    master.not_before = "2015-11-13T01:47:10";
    master.not_after = master_not_after;
    master.ca = true;
    master.key_usage = (1U << 0) | (1U << 5);
    c.certificates[1] = issue(master, master_key, c.names[0], root_key);

    CertSpec issuing;
    issuing.subject = c.names[2];
    issuing.serial_hex = "0A0003";
    issuing.not_before = "2017-01-13T22:46:44";
    issuing.not_after = "2037-01-13T22:46:44";
    issuing.ca = true;
    issuing.key_usage = (1U << 0) | (1U << 5);
    c.certificates[2] = issue(issuing, issuing_key, c.names[1], master_key);

    CertSpec leaf;
    leaf.subject = c.names[3];
    leaf.serial_hex = "0A0004B5D62000000005";
    leaf.not_before = "2022-06-09T21:56:24";
    leaf.not_after = "2042-06-09T21:56:24";
    leaf.ca = true;  // as CS spec §4.5's leaf has it; a verifier must not reject that
    leaf.key_usage = 1U << 0;
    c.certificates[3] = issue(leaf, c.leaf_key, c.names[2], issuing_key);
    return c;
}

}  // namespace iclforge::test::pki
