#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/ac4/io/content_signature.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;

constexpr const char* kSignedDate = "2024-06-01T12:00:00";

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out;
    for (const char c : raw) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// A syncframe with the given payload: sync word, 16-bit size, payload, and for 0xAC41 a CRC word
// (the CRC value is not checked by scan(), so zero will do for the layout).
std::vector<std::byte> syncframe(std::size_t payload, bool crc, std::uint8_t fill) {
    std::vector<std::byte> out;
    out.push_back(std::byte{0xAC});
    out.push_back(crc ? std::byte{0x41} : std::byte{0x40});
    out.push_back(static_cast<std::byte>(payload >> 8));
    out.push_back(static_cast<std::byte>(payload & 0xFF));
    for (std::size_t i = 0; i < payload; ++i) {
        out.push_back(static_cast<std::byte>(fill + i));
    }
    if (crc) {
        out.push_back(std::byte{0});
        out.push_back(std::byte{0});
    }
    return out;
}

std::vector<std::byte> synthetic_stream() {
    std::vector<std::byte> s;
    for (int i = 0; i < 6; ++i) {
        const auto frame = syncframe(300 + static_cast<std::size_t>(i) * 17, i % 2 == 1,
                                     static_cast<std::uint8_t>(i * 9));
        s.insert(s.end(), frame.begin(), frame.end());
    }
    return s;
}

struct Fixture {
    pki::TestChain chain = pki::make_chain();
    crypto::SigningCredentials credentials;
    std::vector<crypto::Certificate> certs;

    Fixture() {
        for (const auto& der : chain.certificates) {
            certs.push_back(*crypto::parse_certificate(der));
        }
        credentials = *crypto::make_credentials(chain.leaf_key, certs);
    }
    crypto::SignOptions options() const {
        crypto::SignOptions o;
        o.signed_date = kSignedDate;
        o.chain_policy.valid_at = pki::epoch(kSignedDate);
        o.chain_policy.required_leaf_title = std::string(crypto::kDefaultLeafTitle);
        o.nonce.assign(128, std::byte{0x11});
        return o;
    }
    crypto::VerifyPolicy policy() const {
        crypto::VerifyPolicy p;
        p.time_basis = crypto::TimeBasis::kInstant;
        p.instant = pki::epoch("2024-07-01T00:00:00");
        return p;
    }
};

}  // namespace

TEST_CASE("a signed AC-4 stream is the unsigned one with the signature after the last frame",
          "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream = synthetic_stream();
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());

    // CS spec v2.2 §8.3.1: the syncframes are unmodified and the blob follows to the end.
    REQUIRE(signed_stream->size() > stream.size());
    CHECK(std::equal(stream.begin(), stream.end(), signed_stream->begin()));
    const auto located = iclforge::ac4::locate_signature_trailer(*signed_stream);
    REQUIRE(located.has_value());
    CHECK(located->present);
    CHECK(located->content_size == stream.size());
    CHECK(located->blob.size() == signed_stream->size() - stream.size());
    // The blob does not look like a sync word, which is how a syncframe walk knows to stop.
    CHECK(located->blob[0] == std::byte{0x08});

    // The digest the blob holds is SHA-256 of exactly the unsigned stream, worked out here and not
    // by the library's own helper (a boundary one byte out in signing and in verifying alike would
    // round-trip and still be wrong).
    const auto parsed = crypto::parse_content_signature(located->blob);
    REQUIRE(parsed.has_value());
    const std::array<std::byte, 32> expected = crypto::sha256(stream);
    CHECK(std::ranges::any_of(parsed->content_digests, [&expected](const crypto::DigestEntry& d) {
        return std::equal(d.value.begin(), d.value.end(), expected.begin(), expected.end());
    }));

    // The frame walk sees the same frames and stops exactly where the signature starts.
    const auto before = iclforge::ac4::scan(stream);
    const auto after = iclforge::ac4::scan(*signed_stream);
    CHECK(after.frames.size() == before.frames.size());
    REQUIRE(after.stopped_at.has_value());
    CHECK(*after.stopped_at == iclforge::ac4::Error::kLostSync);
    CHECK(after.stopped_at_offset == stream.size());

    // An unsigned stream has no trailer, and its digest is over everything.
    const auto none = iclforge::ac4::locate_signature_trailer(stream);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->present);
    CHECK(none->content_size == stream.size());
    CHECK(*iclforge::ac4::signed_content_digest(stream) == crypto::sha256(stream));
    CHECK(*iclforge::ac4::signed_content_digest(*signed_stream) == crypto::sha256(stream));

    // Stripping gives the original bytes back.
    const auto stripped = iclforge::ac4::strip_signature(*signed_stream);
    REQUIRE(stripped.has_value());
    CHECK(*stripped == stream);
    CHECK(*iclforge::ac4::strip_signature(stream) == stream);
}

TEST_CASE("a signed AC-4 stream verifies, and changes to either part are caught",
          "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream = synthetic_stream();
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());

    const crypto::VerifyReport good = iclforge::ac4::verify_stream(*signed_stream, f.policy());
    REQUIRE_FALSE(good.fault.has_value());
    CHECK(good.verdict == crypto::Verdict::kChecksChainUnanchored);
    const std::vector<crypto::Certificate> anchors = {f.certs[0]};
    crypto::VerifyPolicy anchored = f.policy();
    anchored.trust_anchors = anchors;
    CHECK(iclforge::ac4::verify_stream(*signed_stream, anchored).verdict ==
          crypto::Verdict::kValid);

    // A signature made by the codec-neutral signer over the stream, and appended by hand, verifies:
    // the boundary verify_stream hashes up to is not merely the one sign_stream hashed to.
    const auto blob = crypto::sign_content(stream, f.credentials, f.options());
    REQUIRE(blob.has_value());
    std::vector<std::byte> by_hand = stream;
    by_hand.insert(by_hand.end(), blob->begin(), blob->end());
    CHECK(iclforge::ac4::verify_stream(by_hand, f.policy()).verdict ==
          crypto::Verdict::kChecksChainUnanchored);

    // One bit of audio changed.
    std::vector<std::byte> bad_audio = *signed_stream;
    bad_audio[400] ^= std::byte{0x04};
    const crypto::VerifyReport audio = iclforge::ac4::verify_stream(bad_audio, f.policy());
    CHECK(audio.verdict == crypto::Verdict::kInvalid);
    REQUIRE(audio.fault.has_value());
    CHECK(audio.fault->code == crypto::SignatureError::kDigestMismatch);

    // A frame dropped, or added.
    std::vector<std::byte> shorter(signed_stream->begin() + static_cast<std::ptrdiff_t>(304),
                                   signed_stream->end());
    CHECK(iclforge::ac4::verify_stream(shorter, f.policy()).verdict == crypto::Verdict::kInvalid);
    std::vector<std::byte> longer = syncframe(40, false, 1);
    longer.insert(longer.end(), signed_stream->begin(), signed_stream->end());
    CHECK(iclforge::ac4::verify_stream(longer, f.policy()).fault->code ==
          crypto::SignatureError::kDigestMismatch);

    // One byte of the signature changed.
    std::vector<std::byte> bad_blob = *signed_stream;
    bad_blob[bad_blob.size() - 5] ^= std::byte{0x10};
    CHECK(iclforge::ac4::verify_stream(bad_blob, f.policy()).verdict == crypto::Verdict::kInvalid);

    // The signature cut short, and bytes after it.
    std::vector<std::byte> cut(signed_stream->begin(), signed_stream->end() - 3);
    CHECK(iclforge::ac4::verify_stream(cut, f.policy()).verdict == crypto::Verdict::kInvalid);
    std::vector<std::byte> extended = *signed_stream;
    extended.push_back(std::byte{0});
    CHECK(iclforge::ac4::verify_stream(extended, f.policy()).verdict == crypto::Verdict::kInvalid);
}

TEST_CASE("an unsigned or foreign stream is reported, not mistaken", "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream = synthetic_stream();

    const crypto::VerifyReport unsigned_report = iclforge::ac4::verify_stream(stream, f.policy());
    CHECK(unsigned_report.verdict == crypto::Verdict::kInvalid);
    REQUIRE(unsigned_report.fault.has_value());
    CHECK(unsigned_report.fault->code == crypto::SignatureError::kBadOrMissingSignature);
    CHECK(unsigned_report.fault->message.find("no content signature") != std::string::npos);

    const std::vector<std::byte> foreign(100, std::byte{0x55});
    const auto located = iclforge::ac4::locate_signature_trailer(foreign);
    REQUIRE_FALSE(located.has_value());
    CHECK(located.error().kind == iclforge::ac4::StreamSignatureErrorKind::kNotAnAc4Stream);
    CHECK(iclforge::ac4::verify_stream(foreign, f.policy()).verdict == crypto::Verdict::kInvalid);

    // A last frame cut off is truncation, not a signature.
    std::vector<std::byte> truncated = stream;
    truncated.resize(truncated.size() - 20);
    const auto cut = iclforge::ac4::locate_signature_trailer(truncated);
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().kind == iclforge::ac4::StreamSignatureErrorKind::kTruncated);

    // One to three stray bytes after the last frame are damage, not a trailer.
    std::vector<std::byte> stray = stream;
    stray.push_back(std::byte{0x01});
    stray.push_back(std::byte{0x02});
    CHECK_FALSE(iclforge::ac4::locate_signature_trailer(stray).has_value());

    // Signing refuses a stream that is already signed (it would sign over the old signature), and
    // one that is not AC-4, and reports a signing failure with its code.
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());
    const auto twice = iclforge::ac4::sign_stream(*signed_stream, f.credentials, f.options());
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().kind == iclforge::ac4::StreamSignatureErrorKind::kAlreadySigned);
    CHECK_FALSE(iclforge::ac4::sign_stream(foreign, f.credentials, f.options()).has_value());
    crypto::SigningCredentials empty;
    const auto refused = iclforge::ac4::sign_stream(stream, empty, f.options());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == iclforge::ac4::StreamSignatureErrorKind::kSigning);
    REQUIRE(refused.error().fault.has_value());
    CHECK(refused.error().fault->code == crypto::SignatureError::kCertifierNoPrivateKey);
}

TEST_CASE("a real AC-4 stream signs, plays as it did, and verifies", "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream =
        read_file(std::string(ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR) + "/ac4-stereo-64/dee.ac4");
    const auto before = iclforge::ac4::scan(stream);
    REQUIRE(before.frames.size() > 10);

    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());
    const auto after = iclforge::ac4::scan(*signed_stream);
    // Every frame the unsigned file has is there, byte for byte, and the walk ends at the trailer.
    REQUIRE(after.frames.size() == before.frames.size());
    for (std::size_t i = 0; i < before.frames.size(); ++i) {
        CHECK(after.frames[i].offset == before.frames[i].offset);
        CHECK(
            std::equal(before.frames[i].raw_ac4_frame.begin(), before.frames[i].raw_ac4_frame.end(),
                       after.frames[i].raw_ac4_frame.begin(), after.frames[i].raw_ac4_frame.end()));
    }
    CHECK(iclforge::ac4::verify_stream(*signed_stream, f.policy()).verdict ==
          crypto::Verdict::kChecksChainUnanchored);

    // Summary without a key: what the trailer says about itself.
    const iclforge::ac4::SignatureSummary summary =
        iclforge::ac4::summarise_signature(*signed_stream);
    CHECK(summary.present);
    CHECK(summary.readable);
    CHECK(summary.signed_date == kSignedDate);
    CHECK(summary.signing_method == "pace-ccjf1");
    CHECK(summary.certificates == 4);
    CHECK(summary.signer.find("ICL Forge Test Content Signer") != std::string::npos);
    CHECK(summary.application == "ICL Forge");
    CHECK_FALSE(iclforge::ac4::summarise_signature(stream).present);
}
