#include "iclforge/base/crypto/credentials.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/der.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::base::crypto {
namespace {

using Unexpected = std::unexpected<CredentialError>;

constexpr std::size_t kMaxCredentialFileBytes = 4U << 20;
// A trust-anchor file may be a system bundle (about 150 certificates); this stops a file of
// thousands from costing a parse of each.
constexpr std::size_t kMaxCertificatesPerFile = 1024;
constexpr std::string_view kOidEcPublicKey = "1.2.840.10045.2.1";
constexpr std::string_view kOidPrime256v1 = "1.2.840.10045.3.1.7";

Unexpected error(CredentialErrorKind kind, std::string message) {
    return Unexpected(CredentialError{kind, std::move(message)});
}

void wipe(std::vector<std::byte>& bytes) {
    volatile std::byte* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p[i] = std::byte{0};
    }
}

std::string_view as_text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::byte> trim_ascii_space(std::span<const std::byte> bytes) {
    const auto is_space = [](std::byte b) {
        return b == std::byte{' '} || b == std::byte{'\t'} || b == std::byte{'\r'} ||
               b == std::byte{'\n'};
    };
    while (!bytes.empty() && is_space(bytes.front())) {
        bytes = bytes.subspan(1);
    }
    while (!bytes.empty() && is_space(bytes.back())) {
        bytes = bytes.first(bytes.size() - 1);
    }
    return bytes;
}

bool oid_is(const der::Tlv& tlv, std::string_view dotted) {
    return tlv.tag == der::kObjectIdentifier && der::oid_to_string(tlv.content) == dotted;
}

// RFC 5915 §3: ECPrivateKey ::= SEQUENCE { version INTEGER (1), privateKey OCTET STRING,
// parameters [0] ECParameters OPTIONAL, publicKey [1] BIT STRING OPTIONAL }. `fields` is the
// SEQUENCE's content after the version.
std::expected<P256PrivateKey, CredentialError> read_sec1_fields(std::span<const std::byte> fields) {
    const auto scalar = der::read_expecting(fields, der::kOctetString);
    if (!scalar || scalar->content.size() != 32) {
        return error(CredentialErrorKind::kMalformed,
                     "the EC private key does not hold a 32-byte scalar");
    }
    std::optional<std::vector<std::byte>> embedded_public;
    while (!fields.empty()) {
        const auto field = der::read(fields);
        if (!field) {
            return error(CredentialErrorKind::kMalformed, "the EC private key is truncated");
        }
        if (field->tag == der::kContext0) {
            std::span<const std::byte> inner = field->content;
            const auto oid = der::read(inner);
            if (!oid || !inner.empty() || !oid_is(*oid, kOidPrime256v1)) {
                return error(CredentialErrorKind::kUnsupported,
                             "the EC private key is not on the P-256 curve (prime256v1)");
            }
        } else if (field->tag == der::kContext1) {
            std::span<const std::byte> inner = field->content;
            const auto bits = der::read_expecting(inner, der::kBitString);
            if (bits && inner.empty() && bits->content.size() == 66 &&
                bits->content[0] == std::byte{0}) {
                embedded_public.emplace(bits->content.begin() + 1, bits->content.end());
            }
        }
    }
    auto key = P256PrivateKey::from_scalar(scalar->content);
    if (!key.has_value()) {
        return error(CredentialErrorKind::kMalformed,
                     "the EC private key is zero or not below the P-256 group order");
    }
    if (embedded_public.has_value()) {
        const auto derived = p256_public_key(*key);
        const auto stated = p256_public_key_from_uncompressed(*embedded_public);
        if (!derived || !stated || !(*derived == *stated)) {
            return error(CredentialErrorKind::kMismatch,
                         "the EC private key file's public key is not the one its scalar gives");
        }
    }
    return *key;
}

std::expected<P256PrivateKey, CredentialError> parse_key_der(std::span<const std::byte> der_bytes) {
    std::span<const std::byte> input = der_bytes;
    const auto outer = der::read_expecting(input, der::kSequence);
    if (!outer || !input.empty()) {
        return error(CredentialErrorKind::kMalformed, "the key is not a DER structure");
    }
    std::span<const std::byte> body = outer->content;
    const auto version = der::read_expecting(body, der::kInteger);
    if (!version) {
        return error(CredentialErrorKind::kMalformed, "the key has no version field");
    }
    const auto v = der::small_unsigned(version->content);
    if (body.empty()) {
        return error(CredentialErrorKind::kMalformed, "the key is truncated");
    }
    if (std::to_integer<std::uint8_t>(body[0]) == der::kOctetString) {
        if (v != std::optional<std::uint64_t>(1)) {
            return error(CredentialErrorKind::kMalformed, "unrecognised EC key version");
        }
        return read_sec1_fields(body);  // SEC 1 / RFC 5915
    }
    if (v != std::optional<std::uint64_t>(0) && v != std::optional<std::uint64_t>(1)) {
        return error(CredentialErrorKind::kMalformed, "unrecognised PKCS#8 key version");
    }
    // RFC 5208 / RFC 5958: PrivateKeyInfo ::= SEQUENCE { version 0, AlgorithmIdentifier,
    // privateKey OCTET STRING containing the algorithm's own key structure }.
    const auto algorithm = der::read_expecting(body, der::kSequence);
    const auto private_key = der::read_expecting(body, der::kOctetString);
    if (!algorithm || !private_key) {
        return error(CredentialErrorKind::kMalformed, "the PKCS#8 key is truncated");
    }
    std::span<const std::byte> algorithm_body = algorithm->content;
    const auto key_oid = der::read(algorithm_body);
    const auto curve_oid = der::read(algorithm_body);
    if (!key_oid || !oid_is(*key_oid, kOidEcPublicKey)) {
        return error(CredentialErrorKind::kUnsupported,
                     "the private key is not an EC key (only EC keys on P-256 are read)");
    }
    if (!curve_oid || !oid_is(*curve_oid, kOidPrime256v1)) {
        return error(CredentialErrorKind::kUnsupported,
                     "the EC private key is not on the P-256 curve (prime256v1)");
    }
    std::span<const std::byte> inner = private_key->content;
    const auto ec = der::read_expecting(inner, der::kSequence);
    if (!ec) {
        return error(CredentialErrorKind::kMalformed, "the PKCS#8 key holds no EC key");
    }
    std::span<const std::byte> ec_body = ec->content;
    const auto ec_version = der::read_expecting(ec_body, der::kInteger);
    if (!ec_version ||
        der::small_unsigned(ec_version->content) != std::optional<std::uint64_t>(1)) {
        return error(CredentialErrorKind::kMalformed, "the PKCS#8 key holds no EC key");
    }
    return read_sec1_fields(ec_body);
}

constexpr std::string_view kEncryptedHint =
    "the private key is encrypted; remove the password first "
    "(openssl pkcs8 -in key.pem -nocrypt, or openssl ec -in key.pem)";

std::expected<P256PrivateKey, CredentialError> parse_key_text(std::span<const std::byte> contents,
                                                              int depth) {
    // A bare scalar is 32 bytes of binary, any of which may be a byte that counts as white space,
    // so its length is tested on the input as it came, before anything is trimmed from it.
    if (contents.size() == 32) {
        if (auto key = P256PrivateKey::from_scalar(contents); key.has_value()) {
            return *key;
        }
    }
    const std::span<const std::byte> trimmed = trim_ascii_space(contents);
    if (trimmed.empty()) {
        return error(CredentialErrorKind::kEmpty, "the key source is empty");
    }
    const std::string_view text = as_text(trimmed);
    if (text.find("-----BEGIN ") != std::string_view::npos) {
        auto blocks = pem_decode(text);
        if (!blocks.has_value()) {
            return error(CredentialErrorKind::kMalformed, "the key's PEM text is damaged");
        }
        for (PemBlock& block : *blocks) {
            if (block.label == "ENCRYPTED PRIVATE KEY") {
                return error(CredentialErrorKind::kUnsupported, std::string(kEncryptedHint));
            }
            if (block.label == "RSA PRIVATE KEY" || block.label == "DSA PRIVATE KEY") {
                return error(CredentialErrorKind::kUnsupported,
                             "the private key is not an EC key (only EC keys on P-256 are read)");
            }
            if (block.label == "PRIVATE KEY" || block.label == "EC PRIVATE KEY") {
                auto key = parse_key_der(block.der);
                for (PemBlock& b : *blocks) {
                    wipe(b.der);
                }
                return key;
            }
        }
        for (PemBlock& b : *blocks) {
            wipe(b.der);
        }
        return error(CredentialErrorKind::kMalformed, "the PEM text holds no private key");
    }
    if (trimmed.size() == 32) {
        auto key = P256PrivateKey::from_scalar(trimmed);
        if (key.has_value()) {
            return *key;
        }
    }
    if (std::to_integer<std::uint8_t>(trimmed[0]) == der::kSequence) {
        return parse_key_der(trimmed);
    }
    if (depth == 0) {
        // The CI form: base64 of one of the above.
        if (auto decoded = base64_decode(text); decoded.has_value() && !decoded->empty()) {
            auto key = parse_key_text(*decoded, depth + 1);
            wipe(*decoded);
            return key;
        }
    }
    return error(CredentialErrorKind::kMalformed,
                 "the key is not PEM, DER, a 32-byte scalar or base64 of one of those");
}

std::expected<std::vector<std::byte>, CredentialError> read_file(const std::string& path) {
    std::ifstream in;
    // Unbuffered, and set before the file is opened (a stream ignores the request after): a
    // stream's own buffer is a block of 4 to 8 KiB that the read fills with the file and that is
    // freed, unwiped, when the stream is closed. For a key file that is the key.
    in.rdbuf()->pubsetbuf(nullptr, 0);
    in.open(path, std::ios::binary);
    if (!in) {
        return error(CredentialErrorKind::kUnreadable, "cannot open '" + path + "'");
    }
    std::vector<std::byte> bytes;
    // A key file is read straight into the vector that is wiped later: no stack buffer and no
    // stream buffer holds a copy of it, and a vector reserved at the file's size is never regrown,
    // which would leave the smaller block freed with the key in it. (A source of unknown size, a
    // pipe, can still regrow.)
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error && file_size <= kMaxCredentialFileBytes) {
        bytes.reserve(static_cast<std::size_t>(file_size) + 1);
    }
    constexpr std::size_t kChunk = 4096;
    for (;;) {
        const std::size_t before = bytes.size();
        const std::size_t room = bytes.capacity() > before ? bytes.capacity() - before : kChunk;
        bytes.resize(before + room);
        in.read(reinterpret_cast<char*>(bytes.data() + before), static_cast<std::streamsize>(room));
        const auto got = static_cast<std::size_t>(in.gcount());
        bytes.resize(before + got);
        if (got == 0) {
            break;
        }
        if (bytes.size() > kMaxCredentialFileBytes) {
            wipe(bytes);
            return error(CredentialErrorKind::kMalformed,
                         "'" + path + "' is larger than a key or certificate file should be");
        }
    }
    if (in.bad()) {
        wipe(bytes);
        return error(CredentialErrorKind::kUnreadable, "cannot read '" + path + "'");
    }
    return bytes;
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

std::expected<P256PrivateKey, CredentialError> parse_p256_private_key(
    std::span<const std::byte> contents) {
    return parse_key_text(contents, 0);
}

std::expected<std::vector<Certificate>, CredentialError> parse_certificates(
    std::span<const std::byte> contents) {
    const std::span<const std::byte> trimmed = trim_ascii_space(contents);
    if (trimmed.empty()) {
        return error(CredentialErrorKind::kEmpty, "the certificate source is empty");
    }
    std::vector<std::vector<std::byte>> ders;
    const std::string_view text = as_text(trimmed);
    if (text.find("-----BEGIN ") != std::string_view::npos) {
        const auto blocks = pem_decode(text);
        if (!blocks.has_value()) {
            return error(CredentialErrorKind::kMalformed, "the certificates' PEM text is damaged");
        }
        for (const PemBlock& block : *blocks) {
            if (block.label == "CERTIFICATE" || block.label == "TRUSTED CERTIFICATE") {
                ders.push_back(block.der);
            }
        }
    } else {
        std::span<const std::byte> rest = trimmed;
        while (!rest.empty()) {
            const auto cert = der::read_expecting(rest, der::kSequence);
            if (!cert) {
                return error(CredentialErrorKind::kMalformed,
                             "the certificates are neither PEM nor DER");
            }
            ders.emplace_back(cert->encoded.begin(), cert->encoded.end());
        }
    }
    if (ders.empty()) {
        return error(CredentialErrorKind::kMalformed, "no certificate found");
    }
    if (ders.size() > kMaxCertificatesPerFile) {
        return error(CredentialErrorKind::kMalformed,
                     "the file holds " + std::to_string(ders.size()) +
                         " certificates and at most " + std::to_string(kMaxCertificatesPerFile) +
                         " are read");
    }
    std::vector<Certificate> out;
    for (std::size_t i = 0; i < ders.size(); ++i) {
        auto parsed = parse_certificate(ders[i]);
        if (!parsed.has_value()) {
            return error(parsed.error() == X509Error::kMalformed
                             ? CredentialErrorKind::kMalformed
                             : CredentialErrorKind::kUnsupported,
                         "certificate " + std::to_string(i) + ": " + to_string(parsed.error()));
        }
        out.push_back(std::move(*parsed));
    }
    return out;
}

std::expected<std::vector<Certificate>, CredentialError> order_chain(
    std::vector<Certificate> certificates) {
    if (certificates.empty()) {
        return error(CredentialErrorKind::kEmpty, "no certificates");
    }
    if (certificates.size() > kMaxChainLength) {
        return error(CredentialErrorKind::kMismatch,
                     "a chain of " + std::to_string(certificates.size()) +
                         " certificates is longer than the " + std::to_string(kMaxChainLength) +
                         " accepted");
    }
    const std::size_t n = certificates.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (certificates[i].der == certificates[j].der) {
                return error(
                    CredentialErrorKind::kMismatch,
                    "the same certificate appears twice (" + certificates[i].subject.text + ")");
            }
        }
    }
    // The leaf is the certificate that issued no other one.
    std::optional<std::size_t> leaf;
    for (std::size_t i = 0; i < n; ++i) {
        bool issues_another = false;
        for (std::size_t j = 0; j < n; ++j) {
            if (j != i && !certificates[j].self_issued() &&
                certificates[j].issuer.der == certificates[i].subject.der) {
                issues_another = true;
            }
        }
        if (!issues_another) {
            if (leaf.has_value()) {
                return error(
                    CredentialErrorKind::kMismatch,
                    "the certificates do not form one chain: more than one issues nothing (" +
                        certificates[*leaf].subject.text + " and " + certificates[i].subject.text +
                        ")");
            }
            leaf = i;
        }
    }
    if (!leaf.has_value()) {
        return error(CredentialErrorKind::kMismatch,
                     "the certificates issue one another in a loop");
    }
    std::vector<Certificate> ordered;
    std::vector<bool> used(n, false);
    std::size_t current = *leaf;
    while (true) {
        used[current] = true;
        ordered.push_back(certificates[current]);
        if (certificates[current].self_issued()) {
            break;
        }
        std::optional<std::size_t> parent;
        for (std::size_t j = 0; j < n; ++j) {
            if (!used[j] && certificates[j].subject.der == certificates[current].issuer.der) {
                parent = j;
                break;
            }
        }
        if (!parent.has_value()) {
            break;  // the issuer is not in the file: the top of what was supplied
        }
        current = *parent;
    }
    if (ordered.size() != n) {
        return error(CredentialErrorKind::kMismatch, "the certificates do not form one chain: " +
                                                         std::to_string(n - ordered.size()) +
                                                         " do not lead to the leaf");
    }
    std::reverse(ordered.begin(), ordered.end());
    return ordered;
}

std::expected<SigningCredentials, CredentialError> make_credentials(
    P256PrivateKey key, std::vector<Certificate> certificates) {
    auto ordered = order_chain(std::move(certificates));
    if (!ordered.has_value()) {
        return std::unexpected(ordered.error());
    }
    const auto derived = p256_public_key(key);
    if (!derived.has_value()) {
        return error(CredentialErrorKind::kMalformed, "the private key is not valid");
    }
    const Certificate& leaf = ordered->back();
    if (!(leaf.public_key == *derived)) {
        return error(CredentialErrorKind::kMismatch,
                     "the private key does not belong to the leaf certificate (" +
                         leaf.subject.text + ", serial " + leaf.serial_hex + ")");
    }
    return SigningCredentials{std::move(key), std::move(*ordered)};
}

std::expected<P256PrivateKey, CredentialError> load_content_key(std::string_view explicit_path) {
    std::string source;
    std::vector<std::byte> bytes;
    if (!explicit_path.empty()) {
        source = std::string(explicit_path);
    } else {
        source = env_value("ICLFORGE_CONTENT_KEY_FILE");
    }
    if (!source.empty()) {
        auto file = read_file(source);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        bytes = std::move(*file);
    } else {
        std::string inline_key = env_value("ICLFORGE_CONTENT_KEY");
        if (inline_key.empty()) {
            return error(
                CredentialErrorKind::kAbsent,
                "no content-key= path, $ICLFORGE_CONTENT_KEY_FILE or $ICLFORGE_CONTENT_KEY "
                "names a private key");
        }
        source = "$ICLFORGE_CONTENT_KEY";
        bytes.reserve(inline_key.size());
        for (const char c : inline_key) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        // This copy of the environment variable is wiped; the process's own environment block,
        // which getenv() points into and which `ps` and /proc can show, is not ours to wipe.
        volatile char* text = inline_key.data();
        for (std::size_t i = 0; i < inline_key.size(); ++i) {
            text[i] = '\0';
        }
    }
    auto key = parse_p256_private_key(bytes);
    wipe(bytes);
    if (!key.has_value()) {
        return error(key.error().kind, source + ": " + key.error().message);
    }
    return key;
}

std::expected<std::vector<Certificate>, CredentialError> load_certificate_files(
    std::span<const std::string> paths) {
    std::vector<Certificate> all;
    for (const std::string& path : paths) {
        auto bytes = read_file(path);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        auto certs = parse_certificates(*bytes);
        if (!certs.has_value()) {
            return error(certs.error().kind, path + ": " + certs.error().message);
        }
        for (Certificate& cert : *certs) {
            all.push_back(std::move(cert));
        }
    }
    return all;
}

std::expected<std::vector<Certificate>, CredentialError> load_content_chain(
    std::span<const std::string> explicit_paths) {
    if (!explicit_paths.empty()) {
        return load_certificate_files(explicit_paths);
    }
    const std::string env = env_value("ICLFORGE_CONTENT_CHAIN_FILE");
    if (env.empty()) {
        return error(CredentialErrorKind::kAbsent,
                     "no content-chain= path or $ICLFORGE_CONTENT_CHAIN_FILE names a certificate "
                     "chain");
    }
    const std::string paths[] = {env};
    return load_certificate_files(paths);
}

}  // namespace iclforge::base::crypto
