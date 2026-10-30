#pragma once

// The operator's signing credentials: a P-256 private key and the certificate chain that vouches
// for its public half. Both are the operator's own, supplied at run time, never compiled in, in the
// way signing_key.hpp's key is; the library ships machinery and no key, certificate or fingerprint
// of any issuer. Files are read in the forms OpenSSL writes: PEM (PKCS#8 "PRIVATE KEY", SEC 1 "EC
// PRIVATE KEY", "CERTIFICATE"), DER, and for a key a bare 32-byte scalar; and base64 of any of
// those, the form a CI secret takes.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

enum class CredentialErrorKind : std::uint8_t {
    kAbsent,      // no path and no environment variable offered anything: not an error unless asked
    kUnreadable,  // a path was given and could not be read
    kEmpty,       // a source resolved but held nothing
    kMalformed,   // held something that is not a key / certificate in a form read here
    kUnsupported,  // a well-formed key or certificate of a kind not supported (encrypted, RSA, ...)
    kMismatch,     // the key does not belong to the leaf certificate, or the chain does not link
};

struct CredentialError {
    CredentialErrorKind kind = CredentialErrorKind::kMalformed;
    std::string message;  // names the source it tried and what was wrong; never any key bytes
};

// A private key from `contents`. Encrypted PKCS#8 and traditional encrypted PEM are refused with
// the OpenSSL command that removes the password.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<P256PrivateKey, CredentialError>
parse_p256_private_key(std::span<const std::byte> contents);

// Every certificate in `contents`: a PEM bundle, one DER certificate, or DER certificates back to
// back.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
parse_certificates(std::span<const std::byte> contents);

// The certificates reordered root first, leaf last, by following issuer to subject. Fails when they
// do not form one chain (two leaves, a gap, a duplicate) rather than guessing which to use.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
order_chain(std::vector<Certificate> certificates);

struct SigningCredentials {
    P256PrivateKey key;
    std::vector<Certificate> chain;  // root first, leaf last
};

// Orders the chain and checks the key is the leaf's (CS spec §5.1: "a valid ECDSA P-256 private key
// d corresponding to the Content Signer certificate's public key"). The chain's own validity (time,
// anchors, title) is the verifier's policy and is checked there, and by validate_chain() when the
// signer wants to refuse before signing.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<SigningCredentials, CredentialError>
make_credentials(P256PrivateKey key, std::vector<Certificate> certificates);

// Resolution in the style of load_signing_key(): `explicit_path` if non-empty, then the file named
// by $ICLFORGE_CONTENT_KEY_FILE, then $ICLFORGE_CONTENT_KEY inline (a path is preferred: a value
// passed inline shows up in `ps` and shell history).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<P256PrivateKey, CredentialError> load_content_key(
    std::string_view explicit_path);

// The chain from the named files, each a bundle; if none are named, from
// $ICLFORGE_CONTENT_CHAIN_FILE.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
load_certificate_files(std::span<const std::string> paths);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
load_content_chain(std::span<const std::string> explicit_paths);

}  // namespace iclforge::base::crypto
