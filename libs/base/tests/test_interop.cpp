// The P-256 / ECDSA / X.509 code against OpenSSL, live (interop/p256_interop.py). The script
// makes keys, signatures and chains with OpenSSL; this test reads them with the library under test
// and checks that every verdict agrees; it then writes what the library makes and the script has
// OpenSSL check that. Skipped, with the reason, when Python, the `cryptography` package or the
// openssl command line is not there: the known-answer tests (test_p256.cpp, test_x509.cpp) run
// everywhere, and this one is the check against a second implementation.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "platform/process.hpp"
#include "test_pki.hpp"

namespace {

using namespace iclforge::base::crypto;
namespace pki = iclforge::test::pki;
namespace fs = std::filesystem;
using pki::Bytes;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z, the script's NOW

std::vector<std::byte> from_hex(std::string_view text) {
    const auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    }
    return out;
}

std::string to_hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 0xF]);
    }
    return out;
}

std::vector<std::vector<std::string>> read_rows(const fs::path& path) {
    std::ifstream in{path};
    REQUIRE(in.good());
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words{line};
        std::vector<std::string> row;
        std::string word;
        while (words >> word) {
            row.push_back(word);
        }
        if (!row.empty()) {
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

Bytes read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    Bytes out;
    char c;
    while (in.get(c)) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
}

void write_bytes(const fs::path& path, const Bytes& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

int run_script(const std::string& phase, const fs::path& dir) {
    const std::string command = "\"" + std::string{ICLFORGE_BASE_INTEROP_PYTHON} + "\" \"" +
                                std::string{ICLFORGE_BASE_INTEROP_SCRIPT} + "\" " + phase +
                                " --dir \"" + dir.string() + "\"";
    return iclforge::test::platform::run_shell(command);
}

}  // namespace

namespace {

struct Generated {
    fs::path dir;
    int code = -1;  // the script's exit code for the generate step
};

// OpenSSL's side is made once for all the sections below, because Catch2 runs a test case's
// preamble again for each section and the key generation takes a few seconds.
const Generated& generated() {
    static const Generated result = [] {
        Generated g;
        g.dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("interop_" + iclforge::test::platform::process_id());
        fs::remove_all(g.dir);
        fs::create_directories(g.dir);
        g.code = run_script("generate", g.dir);
        return g;
    }();
    return result;
}

}  // namespace

TEST_CASE("the library agrees with OpenSSL", "[crypto][interop]") {
    if (std::string_view{ICLFORGE_BASE_INTEROP_PYTHON}.empty()) {
        SKIP("no Python 3 interpreter was found at configure time");
    }
    const fs::path& dir = generated().dir;
    if (generated().code == 77) {
        SKIP("python3 'cryptography' or the openssl command line is missing");
    }
    REQUIRE(generated().code == 0);

    SECTION("signatures OpenSSL made verify, and tampered ones do not") {
        const auto valid = read_rows(dir / "theirs_valid.txt");
        const auto invalid = read_rows(dir / "theirs_invalid.txt");
        REQUIRE(valid.size() >= 300);
        REQUIRE(invalid.size() >= 800);
        for (const auto& row : valid) {
            REQUIRE(row.size() == 3);
            const auto key = P256PublicKey::from_uncompressed(from_hex(row[0]));
            REQUIRE(key.has_value());
            const auto digest_bytes = from_hex(row[1]);
            Digest256 digest{};
            std::copy(digest_bytes.begin(), digest_bytes.end(), digest.begin());
            const auto signature = decode_ecdsa_der(from_hex(row[2]));
            REQUIRE(signature.has_value());
            INFO("key " << row[0] << " digest " << row[1]);
            CHECK(ecdsa_verify(*key, digest, *signature));
            // and what we decode encodes back to the bytes OpenSSL wrote
            CHECK(to_hex(encode_ecdsa_der(*signature)) == row[2]);
        }
        for (const auto& row : invalid) {
            REQUIRE(row.size() == 3);
            const auto key = P256PublicKey::from_uncompressed(from_hex(row[0]));
            REQUIRE(key.has_value());
            const auto digest_bytes = from_hex(row[1]);
            Digest256 digest{};
            std::copy(digest_bytes.begin(), digest_bytes.end(), digest.begin());
            const auto signature = decode_ecdsa_der(from_hex(row[2]));
            if (signature.has_value()) {
                INFO("key " << row[0] << " digest " << row[1] << " sig " << row[2]);
                CHECK_FALSE(ecdsa_verify(*key, digest, *signature));
            }
        }
    }

    SECTION("every key format OpenSSL writes loads, and gives the same public key") {
        const auto rows = read_rows(dir / "keys.txt");
        REQUIRE(rows.size() == 7);
        for (const auto& row : rows) {
            const std::string& id = row[0];
            const auto expected = P256PublicKey::from_uncompressed(from_hex(row[1]));
            REQUIRE(expected.has_value());
            std::vector<std::string> files;
            if (id.starts_with("cli_")) {
                files = {id + ".pem"};
            } else {
                files = {"key" + id + "_pkcs8.pem", "key" + id + "_pkcs8.der",
                         "key" + id + "_sec1.pem", "key" + id + "_sec1.der"};
            }
            for (const std::string& file : files) {
                const Bytes content = read_bytes(dir / file);
                const auto key = parse_private_key(content, file);
                INFO(file);
                REQUIRE(key.has_value());
                CHECK(key->public_key() == expected);
            }
            if (!id.starts_with("cli_")) {
                for (const char* suffix : {"_public.pem", "_public.der"}) {
                    const auto keys =
                        parse_public_keys(read_bytes(dir / ("key" + id + suffix)), suffix);
                    REQUIRE(keys.has_value());
                    CHECK((*keys)[0] == *expected);
                }
                for (const char* suffix : {"_pkcs8_encrypted.pem", "_sec1_encrypted.pem"}) {
                    const auto key =
                        parse_private_key(read_bytes(dir / ("key" + id + suffix)), suffix);
                    INFO(suffix);
                    REQUIRE_FALSE(key.has_value());
                    CHECK(key.error().kind == CredentialErrorKind::kEncrypted);
                }
            }
        }
        for (const char* wrong : {"wrong_p384.pem", "wrong_ed25519.pem", "wrong_rsa.pem"}) {
            const auto key = parse_private_key(read_bytes(dir / wrong), wrong);
            INFO(wrong);
            REQUIRE_FALSE(key.has_value());
            CHECK(key.error().kind == CredentialErrorKind::kUnsupported);
        }
    }

    SECTION("chain verdicts match `openssl verify`") {
        const auto rows = read_rows(dir / "chain_verdicts.txt");
        REQUIRE(rows.size() >= 13);
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        for (const auto& row : rows) {
            const std::string& label = row[0];
            const bool openssl_accepts = row[1] == "accept";
            const std::string root_path = (dir / ("chain_" + label + "_root.pem")).string();
            const std::vector<std::string> anchors = {root_path};
            const auto trust = load_trust_store(anchors, {});
            REQUIRE(trust.has_value());

            bool ours_accepts = false;
            std::string reason = "chain accepted";
            auto certificates =
                parse_certificates(read_bytes(dir / ("chain_" + label + "_mid.pem")), label);
            auto leaf =
                parse_certificates(read_bytes(dir / ("chain_" + label + "_leaf.pem")), label);
            if (!certificates.has_value() || !leaf.has_value()) {
                reason =
                    certificates.has_value() ? leaf.error().message : certificates.error().message;
            } else {
                certificates->push_back((*leaf)[0]);
                const auto ordered = order_chain(std::move(*certificates), nullptr, label);
                REQUIRE(ordered.has_value());
                ChainPolicy policy;
                policy.now_unix = kNow;
                const auto verdict = validate_chain(*ordered, *trust, policy);
                ours_accepts = verdict.has_value();
                if (!verdict.has_value()) {
                    reason = std::string{describe(verdict.error().error)};
                }
            }
            INFO(label << ": OpenSSL " << row[1] << ", library: " << reason);
            CHECK(ours_accepts == openssl_accepts);
            (openssl_accepts ? accepted : rejected)++;
        }
        // A test that only ever agreed on "accept", or only on "reject", would not say much.
        CHECK(accepted >= 3);
        CHECK(rejected >= 8);
    }

    SECTION("a certificate OpenSSL wrote reads field for field") {
        std::map<std::string, std::string> expected;
        for (const auto& row : read_rows(dir / "chain_good_expected.txt")) {
            expected[row[0] + " " + row[1]] = row.size() > 2 ? row[2] : "";
        }
        for (const std::string role : {"root", "mid", "leaf"}) {
            const auto certs =
                parse_certificates(read_bytes(dir / ("chain_good_" + role + ".pem")), role);
            REQUIRE(certs.has_value());
            const Certificate& cert = (*certs)[0];
            INFO(role);
            CHECK(cert.serial_hex() == expected[role + " serial"]);
            CHECK(std::to_string(cert.not_before) == expected[role + " not_before"]);
            CHECK(std::to_string(cert.not_after) == expected[role + " not_after"]);
            CHECK(to_hex(cert.public_key.uncompressed()) == expected[role + " pub"]);
            // `openssl x509 -fingerprint` prints the colon-separated uppercase form
            CHECK(cert.sha1_fingerprint() == expected[role + " sha1"]);
            CHECK(cert.sha256_fingerprint() == expected[role + " sha256"]);
            CHECK(cert.version == 3);
        }
        const auto leaf = parse_certificates(read_bytes(dir / "chain_good_leaf.pem"), "leaf");
        REQUIRE(leaf.has_value());
        CHECK((*leaf)[0].subject.find(kOidTitle) == "Code/Content Signer");
        CHECK((*leaf)[0].subject.find(kOidCommonName) == "ILok");
        CHECK((*leaf)[0].subject.find(kOidSerialNumber) == "1798A2B5D62000000005");
        CHECK((*leaf)[0].serial_hex() == "1798A2B5D62000000005");
    }

    SECTION("a version 1 certificate from `openssl x509 -req`") {
        const auto certs = parse_certificates(read_bytes(dir / "v1_root.pem"), "v1");
        REQUIRE(certs.has_value());
        const Certificate& cert = (*certs)[0];
        CHECK(cert.version == 1);
        CHECK(cert.self_issued());
        CHECK_FALSE(cert.basic_constraints.has_value());
        const auto rows = read_rows(dir / "v1_expected.txt");
        CHECK(cert.serial_hex() == rows[0][1]);
        CHECK(to_hex(cert.public_key.uncompressed()) == rows[1][1]);
        // a v1 self-signed certificate is a usable anchor for a chain that is only itself
        TrustStore trust;
        trust.anchors.push_back(TrustAnchor{cert, "v1"});
        ChainPolicy policy;
        // `openssl x509 -req` starts the certificate's validity when it runs, which is after kNow
        // (the script's fixed NOW) on every day from the one the script was written. A time inside
        // the certificate's own dates is what this section means to test.
        policy.now_unix = std::clamp(kNow, cert.not_before + 1, cert.not_after - 1);
        const std::vector<Certificate> chain = {cert};
        CHECK(validate_chain(chain, trust, policy).has_value());
    }

    SECTION("what the library writes is accepted by OpenSSL") {
        // Signatures: 120 keys, edge digests first, then SHA-256 of a counter.
        std::string good;
        std::string bad;
        for (int i = 0; i < 120; ++i) {
            const auto pair = pki::key_from_seed("interop/" + std::to_string(i));
            Digest256 digest{};
            if (i == 0) {
                digest.fill(std::byte{0});
            } else if (i == 1) {
                digest.fill(std::byte{0xFF});
            } else {
                digest = sha256(pki::bytes_of("digest " + std::to_string(i)));
            }
            const auto signature = ecdsa_sign(pair.key, digest);
            REQUIRE(signature.has_value());
            const auto der = encode_ecdsa_der(*signature);
            const std::string pub = to_hex(pair.pub.uncompressed());
            good += pub + " " + to_hex(digest) + " " + to_hex(der) + "\n";
            Digest256 other = digest;
            other[static_cast<std::size_t>(i % 32)] ^= static_cast<std::byte>(1u << (i % 8));
            bad += pub + " " + to_hex(other) + " " + to_hex(der) + "\n";
            EcdsaSignature altered = *signature;
            altered.s[31] ^= std::byte{0x01};
            if (p256_scalar_in_range(altered.s)) {
                bad += pub + " " + to_hex(digest) + " " + to_hex(encode_ecdsa_der(altered)) + "\n";
            }
        }
        write_text(dir / "ours_signatures.txt", good);
        write_text(dir / "ours_tampered.txt", bad);

        // Keys, in the encodings the loaders read and OpenSSL must too.
        std::string key_rows;
        for (const std::string kind : {"a", "b", "c"}) {
            const auto pair = pki::key_from_seed("interop-key/" + kind);
            key_rows += kind + " " + to_hex(pair.pub.uncompressed()) + "\n";
            const std::string base = "ours_key_" + kind;
            write_text(dir / (base + "_pkcs8.pem"),
                       encode_pem("PRIVATE KEY", pki::pkcs8_der(pair)));
            write_bytes(dir / (base + "_pkcs8.der"), pki::pkcs8_der(pair));
            write_text(dir / (base + "_sec1.pem"),
                       encode_pem("EC PRIVATE KEY", pki::sec1_der(pair)));
            write_bytes(dir / (base + "_sec1.der"), pki::sec1_der(pair));
            write_bytes(dir / (base + "_public.der"), pki::spki_der(pair.pub));
        }
        write_text(dir / "ours_keys.txt", key_rows);

        // A chain with the specification's leaf profile.
        const auto authority = pki::make_authority();
        write_text(dir / "ours_root.pem", encode_pem("CERTIFICATE", authority.root.der));
        write_text(dir / "ours_mid.pem", encode_pem("CERTIFICATE", authority.intermediate.der));
        write_text(dir / "ours_leaf.pem", encode_pem("CERTIFICATE", authority.leaf.der));

        const int verified = run_script("verify", dir);
        CHECK(verified == 0);
    }
}
