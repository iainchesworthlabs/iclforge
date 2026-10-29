// Read, sign and check the content signature of a raw AC-4 stream.
//
// An AC-4 content signature is one ECDSA P-256 signature over the whole stream, made with the
// operator's own key and certificate chain and written after the last sync frame (the frames are
// not touched, so a signed file plays as the unsigned one). Nothing signs or checks unless this
// code asks it to, and the library holds no key, certificate or fingerprint of any issuer: the
// key and the chain are files the operator supplies at run time. See libs/ac4/ERRATA.md and
// docs/library/signing.md.
//
// Usage: ac4_content_signature <stream.ac4> [<key> <chain> [<anchor>]]
//
//   <stream.ac4>  a raw AC-4 stream, signed or not
//   <key>         the signer's P-256 private key (PEM or DER)
//   <chain>       the certificates that vouch for it: a bundle, root first or in any order
//   <anchor>      a certificate to trust as the top of the chain
//
// With only a stream it reports what the stream carries and what checking it finds (an unsigned
// stream is a report, not an error); ctest runs it that way on a committed stream. With a key and
// a chain it signs the stream in memory and checks the result, against the anchor if one is given.
// A signature that checks out against a chain nothing the operator trusts vouches for is reported
// as unanchored, never as valid.

#include <fmt/printf.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/io/content_signature.hpp"
#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace {

namespace crypto = iclforge::base::crypto;

std::vector<std::byte> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

const char* verdict_name(crypto::Verdict verdict) {
    switch (verdict) {
        case crypto::Verdict::kValid:
            return "valid";
        case crypto::Verdict::kChecksChainUnanchored:
            return "checks out, chain unanchored";
        case crypto::Verdict::kInvalid:
            return "invalid";
    }
    return "?";
}

// What the stream says about its own signature. Nothing here has been verified.
void describe(std::span<const std::byte> stream) {
    const iclforge::ac4::SignatureSummary summary = iclforge::ac4::summarise_signature(stream);
    if (!summary.present) {
        fmt::printf("no content signature after the last sync frame\n");
        return;
    }
    if (!summary.readable) {
        fmt::printf("%zu bytes after the last sync frame, not a content signature: %s\n",
                    summary.blob_size, summary.problem.c_str());
        return;
    }
    fmt::printf(
        "a %zu-byte content signature by %s, dated %s (UTC), %zu certificate(s); unverified\n",
        summary.blob_size, summary.signer.c_str(), summary.signed_date.c_str(),
        summary.certificates);
}

// Checks the stream: every certificate valid now, the leaf titled "Code/Content Signer", and the
// chain anchored at `anchors` when there are any.
crypto::VerifyReport check(std::span<const std::byte> stream,
                           std::span<const crypto::Certificate> anchors) {
    crypto::VerifyPolicy policy;
    policy.trust_anchors = anchors;
    policy.time_basis = crypto::TimeBasis::kNow;  // the spec's default; kSignedDate, kInstant and
                                                  // kIgnore are the alternatives
    policy.now = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    return iclforge::ac4::verify_stream(stream, policy);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4 && argc != 5) {
        fmt::printf("usage: ac4_content_signature <stream.ac4> [<key> <chain> [<anchor>]]\n");
        return 2;
    }
    std::vector<std::byte> stream = read_file(argv[1]);
    if (stream.empty()) {
        fmt::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    describe(stream);

    std::vector<crypto::Certificate> anchors;
    if (argc == 5) {
        const std::vector<std::string> paths{argv[4]};
        auto loaded = crypto::load_certificate_files(paths);
        if (!loaded) {
            fmt::printf("anchor: %s\n", loaded.error().message.c_str());
            return 1;
        }
        anchors = std::move(*loaded);
    }

    if (argc >= 4) {
        // The operator's key and chain; make_credentials() checks the key belongs to the leaf.
        auto key = crypto::load_content_key(argv[2]);
        const std::vector<std::string> chain_paths{argv[3]};
        auto chain = crypto::load_content_chain(chain_paths);
        if (!key || !chain) {
            fmt::printf("credentials: %s\n", (!key ? key.error() : chain.error()).message.c_str());
            return 1;
        }
        auto credentials = crypto::make_credentials(std::move(*key), std::move(*chain));
        if (!credentials) {
            fmt::printf("credentials: %s\n", credentials.error().message.c_str());
            return 1;
        }

        // The date the signature records, and the time the chain is checked at before signing, so
        // that an expired chain is refused here and not after the stream is written.
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        crypto::SignOptions options;
        options.signed_date = crypto::format_iso8601_utc(now);
        options.chain_policy.valid_at = now;
        options.chain_policy.required_leaf_title = std::string(crypto::kDefaultLeafTitle);

        // A stream that already carries a signature is refused rather than signed over; strip it
        // first with iclforge::ac4::strip_signature() to re-sign.
        auto signed_stream = iclforge::ac4::sign_stream(stream, *credentials, options);
        if (!signed_stream) {
            fmt::printf("not signed: %s\n", signed_stream.error().message.c_str());
            return 1;
        }
        fmt::printf("signed: %zu bytes of stream, %zu with the signature\n", stream.size(),
                    signed_stream->size());
        stream = std::move(*signed_stream);
        describe(stream);
    }

    const crypto::VerifyReport report = check(stream, anchors);
    for (const crypto::CheckResult& result : report.checks) {
        fmt::printf("  %s  %s: %s\n", result.ok ? "ok  " : "FAIL", result.name.c_str(),
                    result.detail.c_str());
    }
    fmt::printf("content signature: %s\n", verdict_name(report.verdict));
    // Exit 0 for a stream that checks out and for one that carries nothing to check; the verdict
    // is the program's output, and a caller that must refuse an unsigned or unanchored stream
    // reads it from the report as the forge CLI's verify-content does.
    return 0;
}
