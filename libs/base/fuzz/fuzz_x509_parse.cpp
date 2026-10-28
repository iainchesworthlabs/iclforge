#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/x509.hpp"

// The operator-supplied key material readers in iclforge::base::crypto: the X.509 certificate
// reader and chain validation (src/crypto/x509.cpp), the PEM decoder and the key, certificate and
// public-key loaders (src/crypto/credentials.cpp), and the DER ECDSA signature decoder
// (src/crypto/ecdsa_der.cpp).
//
// These are the parsers that read files and signature bytes the operator did not write
// themselves: a trust anchor handed over by a partner, a chain pulled from a signed stream's
// sidecar (the content signature carries its certificates, and the verifier reads them before it
// has any reason to believe them), a key file from a build system. Every length in DER is
// self-declared, which is the shape of input this harness is for.
//
// Input layout: two bytes of verification time, then the rest is the content.
//     [0 .. 2)    a 16-bit count of days after 2020-01-01, the time chains are judged at
//     [2 .. )     the bytes handed to every reader below, as they are
// The content is not split by kind: a PEM bundle, a DER certificate, a PKCS#8 key and a DER
// signature all come through the same door, and each reader refuses what is not its own. A
// certificate that parses is then taken through chain validation as a one-certificate chain, as
// its own anchor and with a trusted key, with both settings of the expired-intermediate policy.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace iclforge::base::crypto;
    if (size < 2) {
        return 0;
    }
    const std::int64_t days = (static_cast<std::int64_t>(data[0]) << 8) | data[1];
    const std::int64_t now = 1577836800 + days * 86400;  // 2020-01-01T00:00:00Z
    const std::span<const std::byte> content{reinterpret_cast<const std::byte*>(data) + 2,
                                             size - 2};
    const std::string text{reinterpret_cast<const char*>(data) + 2, size - 2};

    (void)decode_pem(text);
    (void)decode_ecdsa_der(content);
    (void)parse_subject_public_key_info(content);
    (void)parse_private_key(content, "fuzz");
    (void)parse_public_keys(content, "fuzz");

    if (const auto certificate = parse_certificate(content)) {
        (void)certificate->serial_hex();
        (void)certificate->sha1_fingerprint();
        (void)certificate->sha256_fingerprint();
        (void)certificate->subject.to_string();
        (void)certificate->issuer.to_string();
        (void)certificate->tbs();

        TrustStore as_anchor;
        as_anchor.anchors.push_back(TrustAnchor{*certificate, "fuzz"});
        TrustStore as_key;
        as_key.keys.push_back(TrustedKey{certificate->public_key, "fuzz"});
        const std::vector<Certificate> chain{*certificate};
        for (const bool relaxed : {false, true}) {
            ChainPolicy policy;
            policy.now_unix = now;
            policy.allow_expired_intermediates = relaxed;
            (void)validate_chain(chain, as_anchor, policy);
            (void)validate_chain(chain, as_key, policy);
        }
    }

    // A bundle: every certificate in the content, put in order and judged as a chain under the
    // first of them. This is the path the chain file of a signing run takes.
    if (auto certificates = parse_certificates(content, "fuzz")) {
        TrustStore trust;
        trust.anchors.push_back(TrustAnchor{certificates->front(), "fuzz"});
        ChainPolicy policy;
        policy.now_unix = now;
        policy.allow_expired_intermediates = (data[0] & 1) != 0;
        if (auto ordered = order_chain(*certificates, nullptr, "fuzz")) {
            (void)validate_chain(*ordered, trust, policy);
        }
        if (auto ordered = order_chain(std::move(*certificates),
                                       &trust.anchors[0].certificate.public_key, "fuzz")) {
            (void)validate_chain(*ordered, trust, policy);
        }
    }
    return 0;
}
