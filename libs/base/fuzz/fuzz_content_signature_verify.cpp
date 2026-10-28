#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

// iclforge::base::crypto::parse_content_signature and verify_digest (src/crypto/
// content_signature.cpp, thrift_binary.cpp): the pace-ccjf1 payload as a verifier meets it, in a
// file or a sidecar that arrived from somewhere the verifier does not control.
//
// Random bytes almost never get past the Thrift reader, and a signature nobody can forge never
// gets past the ECDSA check, so a harness that only hands the payload over would exercise the first
// layer and none of the code after the signature (the date, the digest list). The input therefore
// has two modes, chosen by its first byte, against one fixed signer: an authority built when the
// process starts from a seed, which is an ephemeral test key and nothing else (tests/support/
// test_pki.hpp), with its root as the trust anchor and a payload it signed.
//
// Input layout:
//     [0]        flags
//                  bit 0   allow_expired_intermediates
//                  bit 1   the "any" leaf profile instead of the specification's
//                  bit 2   edit mode (otherwise the rest is the payload as it is)
//                  bit 3   verify against a content digest that differs from the signed one
//     [1 .. )    raw mode:  the payload
//                edit mode: a script of four-byte edits applied to the good payload,
//                           [offset hi][offset lo][operation][value]
//                           operation & 3: 0 xor the byte, 1 set it, 2 delete it, 3 insert `value`
//                           before it. At most 32 edits are applied.
//
// Beyond "does not crash", the harness checks one property: a payload that parses serialises to
// bytes that parse to the same structure.
namespace {

using namespace iclforge::base::crypto;
namespace pki = iclforge::test::pki;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

struct Fixture {
    pki::Authority authority = pki::make_authority();
    Digest256 digest = sha256(pki::bytes_of("fuzz content"));
    std::vector<std::byte> good;
    TrustStore trust;

    Fixture() {
        SigningCredentials credentials{authority.leaf.keys.key, authority.chain()};
        auto payload = sign_digest(digest, credentials, pki::at(2026, 10, 28, 12));
        if (!payload) {
            std::abort();  // the fixture itself is broken: a bug, not an input
        }
        good = std::move(*payload);
        trust.anchors.push_back(TrustAnchor{authority.root.cert, "fuzz root"});
    }
};

const Fixture& fixture() {
    static const Fixture f;
    return f;
}

std::vector<std::byte> edited(const std::vector<std::byte>& good,
                              std::span<const std::uint8_t> script) {
    std::vector<std::byte> out = good;
    for (std::size_t edit = 0; edit < 32 && 4 * (edit + 1) <= script.size(); ++edit) {
        const std::uint8_t* e = script.data() + 4 * edit;
        const std::size_t offset =
            out.empty() ? 0 : ((static_cast<std::size_t>(e[0]) << 8) | e[1]) % out.size();
        const auto value = static_cast<std::byte>(e[3]);
        switch (e[2] & 3) {
            case 0:
                if (!out.empty()) {
                    out[offset] ^= value;
                }
                break;
            case 1:
                if (!out.empty()) {
                    out[offset] = value;
                }
                break;
            case 2:
                if (!out.empty()) {
                    out.erase(out.begin() + static_cast<std::ptrdiff_t>(offset));
                }
                break;
            default:
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(offset), value);
                break;
        }
    }
    return out;
}

bool same(const ContentSignature& a, const ContentSignature& b) {
    const auto& ai = a.signature_values.signed_info;
    const auto& bi = b.signature_values.signed_info;
    if (ai.digests.size() != bi.digests.size()) {
        return false;
    }
    for (std::size_t i = 0; i < ai.digests.size(); ++i) {
        if (ai.digests[i].digest_value != bi.digests[i].digest_value ||
            ai.digests[i].reference != bi.digests[i].reference) {
            return false;
        }
    }
    const auto& ad = a.signature_values.signature_values;
    const auto& bd = b.signature_values.signature_values;
    return a.certificate_chain.chain_structure == b.certificate_chain.chain_structure &&
           a.certificate_chain.format == b.certificate_chain.format &&
           a.certificate_chain.pk_standard_name == b.certificate_chain.pk_standard_name &&
           a.certificate_chain.certificates == b.certificate_chain.certificates &&
           a.signer_info.signer_serial_number == b.signer_info.signer_serial_number &&
           a.signer_info.signature_value == b.signer_info.signature_value &&
           ai.canonical_version == bi.canonical_version && ai.signed_date == bi.signed_date &&
           ad.digest_data_ref == bd.digest_data_ref && ad.canonical_method == bd.canonical_method &&
           ad.hash == bd.hash && ad.signature_method == bd.signature_method &&
           ad.pk_standard_name == bd.pk_standard_name && ad.digest_method == bd.digest_method;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const Fixture& f = fixture();
    const std::uint8_t flags = data[0];
    const std::span<const std::uint8_t> rest{data + 1, size - 1};

    std::vector<std::byte> payload;
    if ((flags & 4) != 0) {
        payload = edited(f.good, rest);
    } else {
        payload.assign(reinterpret_cast<const std::byte*>(rest.data()),
                       reinterpret_cast<const std::byte*>(rest.data()) + rest.size());
    }

    // The parser alone, and its round trip.
    if (const auto parsed = parse_content_signature(payload)) {
        const std::vector<std::byte> again = serialize_content_signature(*parsed);
        const auto reparsed = parse_content_signature(again);
        if (!reparsed || !same(*parsed, *reparsed)) {
            std::abort();
        }
    }

    // The verifier, with the leaf profile and the policy the flags choose.
    VerifyOptions options;
    options.chain.now_unix = kNow;
    options.chain.allow_expired_intermediates = (flags & 1) != 0;
    if ((flags & 2) != 0) {
        options.profile = LeafProfile::any();
    }
    Digest256 digest = f.digest;
    if ((flags & 8) != 0) {
        digest[0] ^= std::byte{1};
    }
    if (const auto verified = verify_digest(payload, digest, f.trust, options)) {
        // Whatever verifies has a chain and a leaf to describe it.
        (void)verified->leaf().subject.to_string();
        (void)verified->acceptance.expired_intermediates.size();
    } else {
        (void)error_name(verified.error().error);
        (void)describe(verified.error().error);
    }
    return 0;
}
