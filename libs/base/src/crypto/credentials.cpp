#include "iclforge/base/crypto/credentials.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "der.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/secure_zero.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::base::crypto {
namespace {

// A key file, a chain file or a PEM bundle is a few kilobytes. A cap keeps a path that names a
// disc image or a device from being read into memory by mistake.
constexpr std::size_t kMaxFileBytes = 4u << 20;

CredentialError error(CredentialErrorKind kind, std::string message) {
    return CredentialError{kind, std::move(message)};
}

std::unexpected<CredentialError> fail(CredentialErrorKind kind, std::string message) {
    return std::unexpected(error(kind, std::move(message)));
}

// Scrubs a buffer that held key material when the scope ends, on every path out of it.
template <typename T>
struct Scrub {
    T& value;
    ~Scrub() { secure_zero(value); }
    Scrub(const Scrub&) = delete;
    Scrub& operator=(const Scrub&) = delete;
    explicit Scrub(T& v) : value(v) {}
};

struct ScrubBlocks {
    std::vector<PemBlock>& blocks;
    explicit ScrubBlocks(std::vector<PemBlock>& b) : blocks(b) {}
    ScrubBlocks(const ScrubBlocks&) = delete;
    ScrubBlocks& operator=(const ScrubBlocks&) = delete;
    ~ScrubBlocks() {
        for (PemBlock& block : blocks) {
            secure_zero(block.data);
        }
    }
};

std::string_view as_text(std::span<const std::byte> bytes) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool looks_like_pem(std::span<const std::byte> content) {
    return as_text(content).find("-----BEGIN ") != std::string_view::npos;
}

CredentialErrorKind kind_for(X509Error e) {
    switch (e) {
        case X509Error::kUnsupportedVersion:
        case X509Error::kUnsupportedSignature:
        case X509Error::kUnsupportedKey:
        case X509Error::kUnknownCriticalExtension:
            return CredentialErrorKind::kUnsupported;
        default:
            return CredentialErrorKind::kMalformed;
    }
}

// --- base64 ---------------------------------------------------------------------------------

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// RFC 4648 section 4, with the padding required: a length that is a multiple of four, '=' only as
// the last one or two characters.
std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t pad = 0;
    while (pad < 2 && pad < text.size() && text[text.size() - 1 - pad] == '=') {
        ++pad;
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < text.size() - pad; ++i) {
        const int v = sextet(text[i]);
        if (v < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFFu));
        }
    }
    return out;
}

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t left = data.size() - i;
        std::uint32_t group = std::to_integer<std::uint32_t>(data[i]) << 16;
        if (left > 1) group |= std::to_integer<std::uint32_t>(data[i + 1]) << 8;
        if (left > 2) group |= std::to_integer<std::uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(group >> 18) & 63u]);
        out.push_back(kBase64Alphabet[(group >> 12) & 63u]);
        out.push_back(left > 1 ? kBase64Alphabet[(group >> 6) & 63u] : '=');
        out.push_back(left > 2 ? kBase64Alphabet[group & 63u] : '=');
    }
    return out;
}

std::string_view trim(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

// --- Keys -----------------------------------------------------------------------------------

std::expected<P256PrivateKey, CredentialError> key_from_sec1(std::span<const std::byte> der,
                                                             std::string_view source) {
    std::span<const std::byte> input = der;
    const auto sequence = detail::read_tagged(input, detail::kTagSequence);
    if (!sequence.has_value() || !input.empty()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": the EC private key is not a DER SEQUENCE");
    }
    std::span<const std::byte> body = sequence->content;
    const auto version = detail::read_tagged(body, detail::kTagInteger);
    const auto scalar = detail::read_tagged(body, detail::kTagOctetString);
    if (!version.has_value() || version->content.size() != 1 ||
        version->content[0] != std::byte{1} || !scalar.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} +
                        ": not an ECPrivateKey (RFC 5915): version 1 and the "
                        "private key octets are expected");
    }
    // parameters [0]: the named curve, present in SEC1 files and usually absent in PKCS#8 ones.
    if (!body.empty() && std::to_integer<std::uint8_t>(body[0]) == 0xA0) {
        const auto wrapper = detail::read_element(body);
        std::span<const std::byte> inner =
            wrapper ? wrapper->content : std::span<const std::byte>{};
        const auto curve = detail::read_tagged(inner, detail::kTagObjectIdentifier);
        if (!wrapper.has_value() || !curve.has_value() || !inner.empty()) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + ": the curve parameters are malformed");
        }
        if (detail::oid_to_string(curve->content) != detail::kOidPrime256v1) {
            return fail(CredentialErrorKind::kUnsupported,
                        std::string{source} + ": the EC key is on curve " +
                            detail::oid_to_string(curve->content) +
                            ", and only P-256 (prime256v1, 1.2.840.10045.3.1.7) is supported");
        }
    }
    std::optional<std::array<std::byte, 65>> embedded_public;
    if (!body.empty() && std::to_integer<std::uint8_t>(body[0]) == 0xA1) {
        const auto wrapper = detail::read_element(body);
        std::span<const std::byte> inner =
            wrapper ? wrapper->content : std::span<const std::byte>{};
        const auto bits = detail::read_tagged(inner, detail::kTagBitString);
        if (!wrapper.has_value() || !bits.has_value() || !inner.empty() ||
            bits->content.size() != 66 || bits->content[0] != std::byte{0}) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + ": the embedded public key is malformed");
        }
        std::array<std::byte, 65> point{};
        std::copy(bits->content.begin() + 1, bits->content.end(), point.begin());
        embedded_public = point;
    }
    if (!body.empty()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": unexpected data after the EC private key");
    }
    if (scalar->content.size() != 32) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": the private scalar is " +
                        std::to_string(scalar->content.size()) + " bytes, and a P-256 key has 32");
    }
    auto key = P256PrivateKey::from_scalar(scalar->content);
    if (!key.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": the private scalar is not in 1..n-1");
    }
    if (embedded_public.has_value()) {
        // A key file whose public half disagrees with its private half is a file that was edited
        // or assembled from two keys; signing with it would produce signatures nobody can check.
        const auto derived = key->public_key();
        if (!derived.has_value() || derived->uncompressed() != *embedded_public) {
            return fail(
                CredentialErrorKind::kMismatch,
                std::string{source} + ": the public key in the file is not the private key's");
        }
    }
    return std::move(*key);
}

std::expected<P256PrivateKey, CredentialError> key_from_pkcs8(std::span<const std::byte> der,
                                                              std::string_view source) {
    std::span<const std::byte> input = der;
    const auto sequence = detail::read_tagged(input, detail::kTagSequence);
    if (!sequence.has_value() || !input.empty()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": the PKCS#8 key is not a DER SEQUENCE");
    }
    std::span<const std::byte> body = sequence->content;
    const auto version = detail::read_tagged(body, detail::kTagInteger);
    const auto algorithm = detail::read_tagged(body, detail::kTagSequence);
    const auto octets = detail::read_tagged(body, detail::kTagOctetString);
    if (!version.has_value() || version->content.size() != 1 ||
        std::to_integer<unsigned>(version->content[0]) > 1 || !algorithm.has_value() ||
        !octets.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + ": not a PKCS#8 PrivateKeyInfo (RFC 5208)");
    }
    switch (detail::classify_ec_algorithm(algorithm->content)) {
        case detail::EcAlgorithm::kP256:
            break;
        case detail::EcAlgorithm::kNotEc:
            return fail(CredentialErrorKind::kUnsupported,
                        std::string{source} +
                            ": the key is not an EC key (RSA and Ed25519 keys "
                            "cannot sign AC-4 content)");
        case detail::EcAlgorithm::kOtherCurve:
            return fail(CredentialErrorKind::kUnsupported,
                        std::string{source} +
                            ": the EC key is not on P-256 (prime256v1), which the scheme requires");
        case detail::EcAlgorithm::kMalformed:
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + ": the key algorithm is malformed");
    }
    // PKCS#8 version 1 (RFC 5958) may add attributes [0] and a public key [1] (implicit BIT
    // STRING). Neither is needed, so they are read past; anything else is not a key.
    while (!body.empty()) {
        const auto extra = detail::read_element(body);
        if (!extra.has_value() || (extra->tag != 0xA0 && extra->tag != 0x81)) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + ": unexpected data after the PKCS#8 private key");
        }
    }
    return key_from_sec1(octets->content, source);
}

}  // namespace

// --- PEM ------------------------------------------------------------------------------------

std::optional<std::vector<PemBlock>> decode_pem(std::string_view text) {
    std::vector<PemBlock> blocks;
    bool inside = false;
    std::string body;
    PemBlock current;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trim(text.substr(position, end - position));
        position = end + 1;

        if (!inside) {
            constexpr std::string_view kBegin = "-----BEGIN ";
            constexpr std::string_view kDashes = "-----";
            if (line.starts_with(kBegin) && line.ends_with(kDashes) &&
                line.size() >= kBegin.size() + kDashes.size()) {
                current = PemBlock{};
                current.label = std::string{
                    line.substr(kBegin.size(), line.size() - kBegin.size() - kDashes.size())};
                if (current.label.size() > 64) {
                    return std::nullopt;
                }
                body.clear();
                inside = true;
            }
            continue;
        }

        constexpr std::string_view kEnd = "-----END ";
        if (line.starts_with(kEnd)) {
            if (line != std::string{kEnd} + current.label + "-----") {
                return std::nullopt;
            }
            auto data = base64_decode(body);
            if (!data.has_value()) {
                return std::nullopt;
            }
            current.data = std::move(*data);
            blocks.push_back(std::move(current));
            current = PemBlock{};
            inside = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        // RFC 1421 headers ("Proc-Type: 4,ENCRYPTED") come before the body. A colon cannot be
        // base64, so a line with one before any body is a header and a line with one after is
        // an error that base64_decode reports.
        if (body.empty() && line.find(':') != std::string_view::npos) {
            current.has_headers = true;
            continue;
        }
        for (const char c : line) {
            if (c != ' ' && c != '\t') {
                body.push_back(c);
            }
        }
    }
    if (inside) {
        return std::nullopt;
    }
    return blocks;
}

std::string encode_pem(std::string_view label, std::span<const std::byte> data) {
    std::string out = "-----BEGIN " + std::string{label} + "-----\n";
    const std::string encoded = base64_encode(data);
    for (std::size_t i = 0; i < encoded.size(); i += 64) {
        out += encoded.substr(i, 64);
        out += '\n';
    }
    out += "-----END " + std::string{label} + "-----\n";
    return out;
}

// --- Parsing --------------------------------------------------------------------------------

std::expected<P256PrivateKey, CredentialError> parse_private_key(std::span<const std::byte> content,
                                                                 std::string_view source) {
    if (content.empty()) {
        return fail(CredentialErrorKind::kEmpty, std::string{source} + " is empty");
    }
    if (!looks_like_pem(content)) {
        std::span<const std::byte> input = content;
        const auto outer = detail::read_tagged(input, detail::kTagSequence);
        if (!outer.has_value()) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + " is neither PEM nor a DER private key");
        }
        std::span<const std::byte> body = outer->content;
        const auto first = detail::read_element(body);
        const auto second = detail::read_element(body);
        if (!first.has_value() || !second.has_value()) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + " is not a DER private key");
        }
        if (first->tag == detail::kTagSequence) {
            // EncryptedPrivateKeyInfo: { AlgorithmIdentifier, OCTET STRING }.
            return fail(CredentialErrorKind::kEncrypted,
                        std::string{source} +
                            " looks like an encrypted PKCS#8 key; decrypt it first "
                            "(openssl pkcs8 -nocrypt -in KEY -out PLAIN)");
        }
        if (first->tag == detail::kTagInteger && second->tag == detail::kTagSequence) {
            return key_from_pkcs8(content, source);
        }
        if (first->tag == detail::kTagInteger && second->tag == detail::kTagOctetString) {
            return key_from_sec1(content, source);
        }
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " is not a PKCS#8 or SEC 1 private key");
    }

    auto blocks = decode_pem(as_text(content));
    if (!blocks.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " has a PEM block that is not well-formed");
    }
    const ScrubBlocks scrub{*blocks};
    const PemBlock* found = nullptr;
    for (const PemBlock& block : *blocks) {
        const bool is_key = block.label.size() >= 11 && block.label.ends_with("PRIVATE KEY");
        if (!is_key) {
            continue;  // EC PARAMETERS, a certificate in a combined file, and so on
        }
        if (found != nullptr) {
            return fail(CredentialErrorKind::kMalformed,
                        std::string{source} + " holds more than one private key");
        }
        found = &block;
    }
    if (found == nullptr) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " holds no PEM private key block");
    }
    if (found->has_headers || found->label == "ENCRYPTED PRIVATE KEY") {
        return fail(CredentialErrorKind::kEncrypted,
                    std::string{source} +
                        " is an encrypted private key; decrypt it first (openssl pkcs8 -nocrypt "
                        "or openssl ec -in KEY -out PLAIN)");
    }
    if (found->label == "PRIVATE KEY") {
        return key_from_pkcs8(found->data, source);
    }
    if (found->label == "EC PRIVATE KEY") {
        return key_from_sec1(found->data, source);
    }
    return fail(CredentialErrorKind::kUnsupported,
                std::string{source} + " holds a '" + found->label +
                    "' block, and only an EC key on P-256 can sign AC-4 content");
}

std::expected<std::vector<Certificate>, CredentialError> parse_certificates(
    std::span<const std::byte> content, std::string_view source) {
    if (content.empty()) {
        return fail(CredentialErrorKind::kEmpty, std::string{source} + " is empty");
    }
    std::vector<Certificate> out;
    const auto add = [&](std::span<const std::byte> der,
                         std::size_t index) -> std::optional<CredentialError> {
        auto cert = parse_certificate(der);
        if (!cert.has_value()) {
            return error(kind_for(cert.error()), std::string{source} + ": certificate " +
                                                     std::to_string(index + 1) + ": " +
                                                     std::string{describe(cert.error())});
        }
        out.push_back(std::move(*cert));
        return std::nullopt;
    };
    if (!looks_like_pem(content)) {
        if (const auto failure = add(content, 0)) {
            return std::unexpected(*failure);
        }
        return out;
    }
    const auto blocks = decode_pem(as_text(content));
    if (!blocks.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " has a PEM block that is not well-formed");
    }
    for (const PemBlock& block : *blocks) {
        if (block.label == "CERTIFICATE" || block.label == "X509 CERTIFICATE") {
            if (const auto failure = add(block.data, out.size())) {
                return std::unexpected(*failure);
            }
        }
    }
    if (out.empty()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " holds no CERTIFICATE block");
    }
    return out;
}

std::expected<std::vector<P256PublicKey>, CredentialError> parse_public_keys(
    std::span<const std::byte> content, std::string_view source) {
    if (content.empty()) {
        return fail(CredentialErrorKind::kEmpty, std::string{source} + " is empty");
    }
    std::vector<P256PublicKey> out;
    if (!looks_like_pem(content)) {
        const auto spki = parse_subject_public_key_info(content);
        if (spki.has_value()) {
            out.push_back(*spki);
            return out;
        }
        const auto cert = parse_certificate(content);
        if (cert.has_value()) {
            out.push_back(cert->public_key);
            return out;
        }
        // A key and a certificate are told apart by trying; report the error of whichever the
        // content came closest to being.
        const X509Error reported =
            spki.error() == X509Error::kMalformed ? cert.error() : spki.error();
        return fail(kind_for(reported),
                    std::string{source} + ": " + std::string{describe(reported)});
    }
    const auto blocks = decode_pem(as_text(content));
    if (!blocks.has_value()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " has a PEM block that is not well-formed");
    }
    for (const PemBlock& block : *blocks) {
        if (block.label == "PUBLIC KEY") {
            const auto key = parse_subject_public_key_info(block.data);
            if (!key.has_value()) {
                return fail(kind_for(key.error()), std::string{source} + ": public key: " +
                                                       std::string{describe(key.error())});
            }
            out.push_back(*key);
        } else if (block.label == "CERTIFICATE" || block.label == "X509 CERTIFICATE") {
            const auto cert = parse_certificate(block.data);
            if (!cert.has_value()) {
                return fail(kind_for(cert.error()), std::string{source} + ": certificate: " +
                                                        std::string{describe(cert.error())});
            }
            out.push_back(cert->public_key);
        }
    }
    if (out.empty()) {
        return fail(CredentialErrorKind::kMalformed,
                    std::string{source} + " holds no PUBLIC KEY or CERTIFICATE block");
    }
    return out;
}

std::expected<std::vector<Certificate>, CredentialError> order_chain(
    std::vector<Certificate> certificates, const P256PublicKey* leaf_key, std::string_view source) {
    std::vector<Certificate> unique;
    for (Certificate& cert : certificates) {
        const bool seen = std::ranges::any_of(
            unique, [&](const Certificate& other) { return other.der == cert.der; });
        if (!seen) {
            unique.push_back(std::move(cert));
        }
    }
    if (unique.empty()) {
        return fail(CredentialErrorKind::kEmpty, std::string{source} + " holds no certificates");
    }

    const auto issues_another = [&](std::size_t i) {
        for (std::size_t j = 0; j < unique.size(); ++j) {
            if (j != i && unique[j].issuer.der == unique[i].subject.der &&
                !unique[j].self_issued()) {
                return true;
            }
        }
        return false;
    };

    std::optional<std::size_t> leaf;
    if (leaf_key != nullptr) {
        for (std::size_t i = 0; i < unique.size(); ++i) {
            if (unique[i].public_key != *leaf_key) {
                continue;
            }
            // The same key can sit in a certificate and in the cross-certificate of its issuer;
            // the one that issues nothing else is the end of the chain.
            if (!leaf.has_value() || (issues_another(*leaf) && !issues_another(i))) {
                leaf = i;
            }
        }
        if (!leaf.has_value()) {
            return fail(
                CredentialErrorKind::kMismatch,
                std::string{source} + ": no certificate holds the public key of the private key");
        }
    } else {
        std::size_t candidates = 0;
        for (std::size_t i = 0; i < unique.size(); ++i) {
            if (!issues_another(i)) {
                leaf = i;
                ++candidates;
            }
        }
        if (candidates != 1) {
            return fail(CredentialErrorKind::kMismatch,
                        std::string{source} + ": cannot tell which certificate is the leaf (" +
                            std::to_string(candidates) + " certificates issue no other)");
        }
    }

    std::vector<std::size_t> path{*leaf};
    while (!unique[path.back()].self_issued()) {
        const Certificate& child = unique[path.back()];
        std::optional<std::size_t> next;
        for (std::size_t j = 0; j < unique.size(); ++j) {
            if (std::ranges::find(path, j) != path.end() ||
                unique[j].subject.der != child.issuer.der) {
                continue;
            }
            const bool verifies =
                ecdsa_verify(unique[j].public_key, sha256(child.tbs()), child.signature);
            if (verifies) {
                next = j;
                break;
            }
            // A name match whose signature does not verify is kept in case nothing better turns
            // up, so that validation reports the bad signature, not a missing issuer.
            if (!next.has_value()) {
                next = j;
            }
        }
        if (!next.has_value()) {
            break;
        }
        path.push_back(*next);
    }

    std::vector<Certificate> ordered;
    ordered.reserve(path.size());
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        ordered.push_back(std::move(unique[*it]));
    }
    return ordered;
}

// --- Resolution -----------------------------------------------------------------------------

namespace {

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return content;
}

struct Resolved {
    std::string content;  // may hold a private key; scrubbed by the destructor
    std::string source;
    Resolved() = default;
    Resolved(const Resolved&) = delete;
    Resolved& operator=(const Resolved&) = delete;
    Resolved(Resolved&&) noexcept = default;
    Resolved& operator=(Resolved&&) noexcept = default;
    ~Resolved() { secure_zero(content); }
};

std::string environment(std::string_view name) {
    const std::string key{name};
    const char* value = std::getenv(key.c_str());
    return value != nullptr ? std::string{value} : std::string{};
}

std::expected<Resolved, CredentialError> read_path(const std::string& path,
                                                   std::string_view origin) {
    auto content = read_file(path);
    if (!content.has_value()) {
        return fail(CredentialErrorKind::kUnreadable,
                    "cannot read '" + path + "'" + std::string{origin});
    }
    if (content->size() > kMaxFileBytes) {
        secure_zero(*content);
        return fail(CredentialErrorKind::kMalformed,
                    "'" + path + "'" + std::string{origin} + " is larger than " +
                        std::to_string(kMaxFileBytes >> 20) + " MiB");
    }
    Resolved resolved;
    resolved.content = std::move(*content);
    resolved.source = "file '" + path + "'";
    return resolved;
}

// The explicit path, else the variable naming a file, else (for a key) the variable holding the
// text. kAbsent when none is set.
std::expected<Resolved, CredentialError> resolve(std::string_view explicit_path,
                                                 std::string_view env_file,
                                                 std::string_view env_inline) {
    if (!explicit_path.empty()) {
        return read_path(std::string{explicit_path}, "");
    }
    if (const std::string path = environment(env_file); !path.empty()) {
        auto resolved = read_path(path, " (from " + std::string{env_file} + ")");
        if (resolved.has_value()) {
            resolved->source = std::string{env_file} + " ('" + path + "')";
        }
        return resolved;
    }
    if (!env_inline.empty()) {
        if (std::string text = environment(env_inline); !text.empty()) {
            Resolved resolved;
            resolved.content = std::move(text);
            resolved.source = std::string{env_inline};
            return resolved;
        }
    }
    return fail(CredentialErrorKind::kAbsent, "no source offered");
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::expected<P256PrivateKey, CredentialError> load_content_signing_key(std::string_view key_path) {
    auto resolved = resolve(key_path, kEnvContentSigningKeyFile, kEnvContentSigningKey);
    if (!resolved.has_value()) {
        if (resolved.error().kind == CredentialErrorKind::kAbsent) {
            return fail(CredentialErrorKind::kAbsent,
                        "no content signing key provided (a key path, " +
                            std::string{kEnvContentSigningKeyFile} + " or " +
                            std::string{kEnvContentSigningKey} + ")");
        }
        return std::unexpected(resolved.error());
    }
    return parse_private_key(bytes_of(resolved->content), resolved->source);
}

std::expected<std::vector<Certificate>, CredentialError> load_content_signing_chain(
    std::string_view chain_path) {
    auto resolved = resolve(chain_path, kEnvContentSigningChainFile, {});
    if (!resolved.has_value()) {
        if (resolved.error().kind == CredentialErrorKind::kAbsent) {
            return fail(CredentialErrorKind::kAbsent,
                        "no certificate chain provided (a chain path or " +
                            std::string{kEnvContentSigningChainFile} + ")");
        }
        return std::unexpected(resolved.error());
    }
    return parse_certificates(bytes_of(resolved->content), resolved->source);
}

std::expected<SigningCredentials, CredentialError> load_signing_credentials(
    std::string_view key_path, std::string_view chain_path) {
    auto key = load_content_signing_key(key_path);
    if (!key.has_value()) {
        return std::unexpected(key.error());
    }
    auto chain = resolve(chain_path, kEnvContentSigningChainFile, {});
    if (!chain.has_value()) {
        if (chain.error().kind == CredentialErrorKind::kAbsent) {
            return fail(CredentialErrorKind::kAbsent,
                        "no certificate chain provided (a chain path or " +
                            std::string{kEnvContentSigningChainFile} + ")");
        }
        return std::unexpected(chain.error());
    }
    auto certificates = parse_certificates(bytes_of(chain->content), chain->source);
    if (!certificates.has_value()) {
        return std::unexpected(certificates.error());
    }
    const auto public_key = key->public_key();
    if (!public_key.has_value()) {
        return fail(CredentialErrorKind::kMalformed, "the private key is empty");
    }
    auto ordered = order_chain(std::move(*certificates), &*public_key, chain->source);
    if (!ordered.has_value()) {
        return std::unexpected(ordered.error());
    }
    return SigningCredentials{std::move(*key), std::move(*ordered)};
}

namespace {

// Adds what a trust file holds: certificates as anchors and, when `keys_too`, PUBLIC KEY blocks
// as trusted keys. A DER file is one certificate.
std::optional<CredentialError> add_trust_material(TrustStore& store,
                                                  std::span<const std::byte> content,
                                                  const std::string& label, bool mixed) {
    if (content.empty()) {
        return error(CredentialErrorKind::kEmpty, "file '" + label + "' is empty");
    }
    const std::string source = "file '" + label + "'";
    if (mixed && looks_like_pem(content)) {
        const auto blocks = decode_pem(as_text(content));
        if (!blocks.has_value()) {
            return error(CredentialErrorKind::kMalformed,
                         source + " has a PEM block that is not well-formed");
        }
        bool any = false;
        for (const PemBlock& block : *blocks) {
            if (block.label == "CERTIFICATE" || block.label == "X509 CERTIFICATE") {
                auto cert = parse_certificate(block.data);
                if (!cert.has_value()) {
                    return error(kind_for(cert.error()),
                                 source + ": certificate: " + std::string{describe(cert.error())});
                }
                store.anchors.push_back(TrustAnchor{std::move(*cert), label});
                any = true;
            } else if (block.label == "PUBLIC KEY") {
                auto key = parse_subject_public_key_info(block.data);
                if (!key.has_value()) {
                    return error(kind_for(key.error()),
                                 source + ": public key: " + std::string{describe(key.error())});
                }
                store.keys.push_back(TrustedKey{*key, label});
                any = true;
            }
        }
        if (!any) {
            return error(CredentialErrorKind::kMalformed,
                         source + " holds no CERTIFICATE or PUBLIC KEY block");
        }
        return std::nullopt;
    }
    auto certificates = parse_certificates(content, source);
    if (!certificates.has_value()) {
        return certificates.error();
    }
    for (Certificate& cert : *certificates) {
        store.anchors.push_back(TrustAnchor{std::move(cert), label});
    }
    return std::nullopt;
}

}  // namespace

std::expected<TrustStore, CredentialError> load_trust_store(
    std::span<const std::string> anchor_paths, std::span<const std::string> key_paths) {
    TrustStore store;
    for (const std::string& path : anchor_paths) {
        auto resolved = read_path(path, "");
        if (!resolved.has_value()) {
            return std::unexpected(resolved.error());
        }
        if (const auto failure =
                add_trust_material(store, bytes_of(resolved->content), path, false)) {
            return std::unexpected(*failure);
        }
    }
    for (const std::string& path : key_paths) {
        auto resolved = read_path(path, "");
        if (!resolved.has_value()) {
            return std::unexpected(resolved.error());
        }
        auto keys = parse_public_keys(bytes_of(resolved->content), "file '" + path + "'");
        if (!keys.has_value()) {
            return std::unexpected(keys.error());
        }
        for (const P256PublicKey& key : *keys) {
            store.keys.push_back(TrustedKey{key, path});
        }
    }
    if (anchor_paths.empty() && key_paths.empty()) {
        // Only when the operator named nothing: a variable in the environment must not be able to
        // add to a trust set the command line spelled out.
        const std::string path = environment(kEnvContentTrustFile);
        if (path.empty()) {
            return fail(CredentialErrorKind::kAbsent,
                        "no trust anchor or trusted key provided (a trust path or " +
                            std::string{kEnvContentTrustFile} + ")");
        }
        auto resolved = read_path(path, " (from " + std::string{kEnvContentTrustFile} + ")");
        if (!resolved.has_value()) {
            return std::unexpected(resolved.error());
        }
        if (const auto failure =
                add_trust_material(store, bytes_of(resolved->content), path, true)) {
            return std::unexpected(*failure);
        }
    }
    return store;
}

}  // namespace iclforge::base::crypto
