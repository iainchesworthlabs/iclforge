#pragma once

// The operator's own key material for AC-4 content signing: the EC private key, the certificate
// chain that vouches for it, and the certificates or keys the operator trusts when verifying. As
// with SigningKey (signing_key.hpp), none of it is compiled in and none of it is written to disk
// by this code.
//
// Formats. Keys: PEM "PRIVATE KEY" (PKCS#8, RFC 5208), PEM "EC PRIVATE KEY" (SEC 1, RFC 5915), or
// the DER of either. Certificates: PEM "CERTIFICATE" blocks (any number, with any text between
// them) or one DER certificate. Public keys: PEM "PUBLIC KEY" (SubjectPublicKeyInfo), a
// certificate, or the DER of either. An encrypted key is refused with its own error rather than
// asked for a passphrase: a command-line tool that prompts is a tool that hangs in CI, and a
// passphrase on the command line shows in `ps`. Decrypt it with `openssl pkcs8 -nocrypt` or
// `openssl ec` first.
//
// Everything that reads a file or an environment variable gives its errors in words that name the
// source it tried.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// The environment variables key resolution falls back on, in the order load_* tries them. They are
// not the AC-3 signing key's variables (ICLFORGE_SIGNING_KEY*): an HMAC key and an EC key are
// different material and one operator can hold both.
inline constexpr std::string_view kEnvContentSigningKeyFile = "ICLFORGE_CONTENT_SIGNING_KEY_FILE";
inline constexpr std::string_view kEnvContentSigningKey = "ICLFORGE_CONTENT_SIGNING_KEY";
inline constexpr std::string_view kEnvContentSigningChainFile =
    "ICLFORGE_CONTENT_SIGNING_CHAIN_FILE";
inline constexpr std::string_view kEnvContentTrustFile = "ICLFORGE_CONTENT_TRUST_FILE";

enum class CredentialErrorKind : std::uint8_t {
    kAbsent,       // no path and no environment variable: nothing was offered
    kUnreadable,   // a path was given and could not be read
    kEmpty,        // a source resolved and held nothing
    kMalformed,    // it did not parse as what it should be
    kEncrypted,    // a passphrase-protected key
    kUnsupported,  // parses, but is not an EC key on P-256 or similar
    kMismatch,     // the private key is not the leaf certificate's key; or no single leaf
};

struct CredentialError {
    CredentialErrorKind kind = CredentialErrorKind::kMalformed;
    std::string message;  // already names the source
};

// --- PEM (RFC 7468) -----------------------------------------------------------------------

struct PemBlock {
    std::string label;            // "CERTIFICATE", "PRIVATE KEY"
    std::vector<std::byte> data;  // the decoded body
    bool has_headers = false;     // an RFC 1421 header such as Proc-Type: 4,ENCRYPTED was present
};

// Every block in `text`; text outside blocks is ignored. nullopt for a block whose END line is
// missing or names another label, or whose body is not canonical base64. Line endings may be LF or
// CRLF. Zero blocks is an empty vector, not an error.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<PemBlock>> decode_pem(
    std::string_view text);

// One block, 64 characters to the line, LF line endings.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string encode_pem(std::string_view label,
                                                          std::span<const std::byte> data);

// --- Parsing already-read content -----------------------------------------------------------
// `source` is how the content is called in an error: a file name, or the name of a variable.

[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<P256PrivateKey, CredentialError> parse_private_key(
    std::span<const std::byte> content, std::string_view source);

// All the certificates in the content, in the order they appear.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
parse_certificates(std::span<const std::byte> content, std::string_view source);

// The keys the content holds: each "PUBLIC KEY" block and the key of each certificate; or the one
// key of a DER SubjectPublicKeyInfo or certificate.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<P256PublicKey>, CredentialError>
parse_public_keys(std::span<const std::byte> content, std::string_view source);

// Puts certificates in the order of the wire format, the root end first and the leaf last, from a
// set in any order. Certificates that are not on the path from the leaf upward are dropped, and
// repeated ones are counted once.
//   - the leaf is the certificate whose key is `leaf_key`; with none, it is the one certificate
//     that is not the issuer of another (kMismatch if there is not exactly one);
//   - each step up takes the certificate whose subject is the issuer's name and whose key
//     verifies the child's signature, and the walk stops at a self-issued certificate or when no
//     certificate has the name. A chain that stops short of a root is returned as it is: a
//     partial chain is valid when the operator's anchor is one of its issuers (x509.hpp).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
order_chain(std::vector<Certificate> certificates, const P256PublicKey* leaf_key,
            std::string_view source);

// --- Resolving from paths and the environment -----------------------------------------------

// The signer's key and chain, the chain in wire order (root end first, leaf last), with the key
// checked to be the leaf's.
struct SigningCredentials {
    P256PrivateKey key;
    std::vector<Certificate> chain;
};

// Order for the key: `key_path` if non-empty, else $ICLFORGE_CONTENT_SIGNING_KEY_FILE (a path),
// else $ICLFORGE_CONTENT_SIGNING_KEY (the PEM text itself). The chain: `chain_path` if non-empty,
// else $ICLFORGE_CONTENT_SIGNING_CHAIN_FILE. Giving a path for a kind of material means the
// environment is not consulted for that material, so an exported variable cannot replace a file
// the operator named. kAbsent when no source offers the key or the chain.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<P256PrivateKey, CredentialError>
load_content_signing_key(std::string_view key_path);

// The certificates of the chain file in the order the file has them. load_signing_credentials
// orders them.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<Certificate>, CredentialError>
load_content_signing_chain(std::string_view chain_path);

// Both, ordered, and checked: the leaf's public key is the private key's public key
// (kMismatch otherwise), so a chain for another key is found at signing and not at the first
// verifier.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<SigningCredentials, CredentialError>
load_signing_credentials(std::string_view key_path, std::string_view chain_path);

// The trust set for verification. Each path in `anchor_paths` is read as certificates, each of
// which is an anchor; each path in `key_paths` is read as public keys or certificates, each key
// of which is trusted directly. With no path of either kind, $ICLFORGE_CONTENT_TRUST_FILE is read
// (certificates are anchors, "PUBLIC KEY" blocks are trusted keys). kAbsent, never an empty store,
// when nothing was offered: verification against nothing is not a thing to do quietly.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<TrustStore, CredentialError> load_trust_store(
    std::span<const std::string> anchor_paths, std::span<const std::string> key_paths);

}  // namespace iclforge::base::crypto
