#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

TEST_CASE("a stream with a signature, or with junk, after its frames is not signed over",
          "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream = synthetic_stream();
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());

    // A second signature over a signed stream: said to be a signature already there.
    const auto again = iclforge::ac4::sign_stream(*signed_stream, f.credentials, f.options());
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().kind == iclforge::ac4::StreamSignatureErrorKind::kAlreadySigned);
    CHECK(again.error().message.find("already carries a content signature") != std::string::npos);

    // Bytes that are not a signature: said to be trailing data, with how many.
    std::vector<std::byte> junk = stream;
    for (int i = 0; i < 40; ++i) {
        junk.push_back(static_cast<std::byte>(i));
    }
    const auto refused = iclforge::ac4::sign_stream(junk, f.credentials, f.options());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == iclforge::ac4::StreamSignatureErrorKind::kTrailingData);
    CHECK(refused.error().message.find("the 40 byte(s) after it are not a content signature") !=
          std::string::npos);
    CHECK(refused.error().message.find("already") == std::string::npos);
}

namespace {

// A syncframe whose size is written with the escape: 0xFFFF in the 16-bit field, then the size in
// three bytes (TS 103 190-2 Annex G). Frames of a few hundred bytes use it in the Dolby streams.
std::vector<std::byte> escaped_syncframe(std::size_t payload, bool crc, std::uint8_t fill) {
    std::vector<std::byte> out = {std::byte{0xAC},
                                  crc ? std::byte{0x41} : std::byte{0x40},
                                  std::byte{0xFF},
                                  std::byte{0xFF},
                                  static_cast<std::byte>(payload >> 16),
                                  static_cast<std::byte>((payload >> 8) & 0xFF),
                                  static_cast<std::byte>(payload & 0xFF)};
    for (std::size_t i = 0; i < payload; ++i) {
        out.push_back(static_cast<std::byte>(fill + i));
    }
    if (crc) {
        out.push_back(std::byte{0});
        out.push_back(std::byte{0});
    }
    return out;
}

// frames, in order, each as a vector, joined
std::vector<std::byte> join(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// An ID3v2.4 tag with a body of `body` bytes (the size is four synchsafe bytes), as HLS packed
// audio puts between its frames.
std::vector<std::byte> id3_tag(std::size_t body) {
    std::vector<std::byte> tag = {std::byte{'I'},
                                  std::byte{'D'},
                                  std::byte{'3'},
                                  std::byte{4},
                                  std::byte{0},
                                  std::byte{0},
                                  static_cast<std::byte>((body >> 21) & 0x7F),
                                  static_cast<std::byte>((body >> 14) & 0x7F),
                                  static_cast<std::byte>((body >> 7) & 0x7F),
                                  static_cast<std::byte>(body & 0x7F)};
    tag.insert(tag.end(), body, std::byte{0x20});
    return tag;
}

}  // namespace

TEST_CASE("the trailer walk follows frame sizes written with the escape", "[ac4][signing]") {
    const Fixture f;
    // Plain and escaped sizes in one stream, both sync words, a size of 330 and one of 970 as in
    // the Dolby streams, and one past 0xFFFF that only the escape can carry.
    const std::vector<std::byte> stream = join(
        {escaped_syncframe(330, true, 1), escaped_syncframe(970, false, 2), syncframe(400, true, 3),
         escaped_syncframe(70000, false, 4), escaped_syncframe(5, true, 5)});
    const auto located = iclforge::ac4::locate_signature_trailer(stream);
    REQUIRE(located.has_value());
    CHECK_FALSE(located->present);
    CHECK(located->stray == 0);
    CHECK(located->framing_ends == stream.size());
    CHECK(iclforge::ac4::scan(stream).frames.size() == 5);

    // A signature after them is found at the byte where the last frame ends, not before.
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());
    const auto trailer = iclforge::ac4::locate_signature_trailer(*signed_stream);
    REQUIRE(trailer.has_value());
    CHECK(trailer->present);
    CHECK(trailer->content_size == stream.size());
    CHECK(trailer->stray == 0);
    CHECK(iclforge::ac4::verify_stream(*signed_stream, f.policy()).verdict ==
          crypto::Verdict::kChecksChainUnanchored);
    CHECK(*iclforge::ac4::strip_signature(*signed_stream) == stream);
    // A walk that read only the 16-bit size would have taken 0xFFFF bytes for the first frame and
    // landed in the middle of the second; this one finds all five and stops at the signature.
    CHECK(iclforge::ac4::scan(*signed_stream).frames.size() == 5);
}

TEST_CASE("a damaged frame makes the rest of the stream a report, never a trailer",
          "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> stream = synthetic_stream();
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
    REQUIRE(signed_stream.has_value());

    // The third frame's sync word damaged: the walk stops there, and the signature after the last
    // frame is not found at that point. Frame 0 is 304 bytes, frame 1 is 321 + 2 (CRC).
    const std::size_t third = (4 + 300) + (4 + 317 + 2);
    REQUIRE(iclforge::ac4::scan(stream).frames[2].raw_ac4_frame.data() ==
            stream.data() + third + 4);
    std::vector<std::byte> damaged = *signed_stream;
    damaged[third] = std::byte{0x00};

    const auto located = iclforge::ac4::locate_signature_trailer(damaged);
    REQUIRE(located.has_value());
    CHECK_FALSE(located->present);
    CHECK(located->framing_ends == third);
    CHECK(located->stray == damaged.size() - third);

    const crypto::VerifyReport report = iclforge::ac4::verify_stream(damaged, f.policy());
    CHECK(report.verdict == crypto::Verdict::kInvalid);
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->message.find("the sync frames stop at byte " + std::to_string(third)) !=
          std::string::npos);
    CHECK_FALSE(iclforge::ac4::summarise_signature(damaged).present);
    CHECK_FALSE(iclforge::ac4::strip_signature(damaged).has_value());
    CHECK_FALSE(iclforge::ac4::signed_content_digest(damaged).has_value());
    CHECK_FALSE(iclforge::ac4::sign_stream(damaged, f.credentials, f.options()).has_value());

    // A byte of a frame's payload damaged instead: the framing holds, the digest does not.
    std::vector<std::byte> payload_damage = *signed_stream;
    payload_damage[third + 100] ^= std::byte{0x40};
    const crypto::VerifyReport digest = iclforge::ac4::verify_stream(payload_damage, f.policy());
    CHECK(digest.verdict == crypto::Verdict::kInvalid);
    REQUIRE(digest.fault.has_value());
    CHECK(digest.fault->code == crypto::SignatureError::kDigestMismatch);

    // A signature that is damaged is still a signature that is damaged, not stray bytes: it
    // begins as one does, so the report says it does not read.
    std::vector<std::byte> bad_signature = *signed_stream;
    bad_signature[stream.size() + 30] ^= std::byte{0x01};
    const auto kept = iclforge::ac4::locate_signature_trailer(bad_signature);
    REQUIRE(kept.has_value());
    CHECK(kept->present);
}

TEST_CASE("an ID3 tag between frames is named, not taken for a signature", "[ac4][signing]") {
    const Fixture f;
    const std::vector<std::byte> tagged =
        join({syncframe(300, true, 1), syncframe(310, true, 2), id3_tag(64),
              syncframe(320, true, 3), syncframe(330, true, 4)});
    const std::size_t tag_at = 4 + 300 + 2 + 4 + 310 + 2;

    const auto located = iclforge::ac4::locate_signature_trailer(tagged);
    REQUIRE(located.has_value());
    CHECK_FALSE(located->present);
    CHECK(located->framing_ends == tag_at);
    CHECK(located->stray == tagged.size() - tag_at);

    // Signing refuses it and says why; so does stripping and hashing. Nothing is signed over the
    // frames before the tag as though they were the stream.
    const auto refused = iclforge::ac4::sign_stream(tagged, f.credentials, f.options());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == iclforge::ac4::StreamSignatureErrorKind::kTrailingData);
    CHECK(refused.error().message.find("ID3v2 tag") != std::string::npos);
    CHECK_FALSE(iclforge::ac4::strip_signature(tagged).has_value());

    // A signature made over the whole file by another tool (the hash of every byte) and appended is
    // not found either: the tag stops the walk and what follows it does not begin as a signature.
    const auto blob = crypto::sign_content(tagged, f.credentials, f.options());
    REQUIRE(blob.has_value());
    std::vector<std::byte> signed_tagged = tagged;
    signed_tagged.insert(signed_tagged.end(), blob->begin(), blob->end());
    const crypto::VerifyReport report = iclforge::ac4::verify_stream(signed_tagged, f.policy());
    CHECK(report.verdict == crypto::Verdict::kInvalid);
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->message.find("ID3v2 tag") != std::string::npos);
    CHECK_FALSE(iclforge::ac4::summarise_signature(signed_tagged).present);
}

// ---- The Dolby kit streams -----------------------------------------------------------------
// Twelve AC-4 elementary streams from Dolby's encoder kit (six of them with every frame's size
// written with the escape) are not in the repository. ICLFORGE_DOLBY_AC4_KIT_STREAMS names a
// directory of them: each .ac4 is signed with the test chain, read back, stripped and checked
// byte for byte; any other file in the directory (an AAC or E-AC-3 stream) is refused as not AC-4.
TEST_CASE("the Dolby kit streams sign, verify and strip", "[ac4][signing][sample]") {
    const char* directory = std::getenv("ICLFORGE_DOLBY_AC4_KIT_STREAMS");
    if (directory == nullptr || *directory == '\0') {
        SKIP("set ICLFORGE_DOLBY_AC4_KIT_STREAMS to a directory of the kit's streams");
    }
    const Fixture f;
    std::size_t signed_count = 0;
    std::size_t refused_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        INFO(entry.path().filename().string());
        const std::vector<std::byte> stream = read_file(entry.path().string());
        if (entry.path().extension() != ".ac4") {
            const auto refused = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().kind == iclforge::ac4::StreamSignatureErrorKind::kNotAnAc4Stream);
            ++refused_count;
            continue;
        }
        // Unsigned: the walk reaches the end, no trailer, nothing stray.
        const auto before = iclforge::ac4::locate_signature_trailer(stream);
        REQUIRE(before.has_value());
        CHECK_FALSE(before->present);
        CHECK(before->stray == 0);
        CHECK(before->framing_ends == stream.size());
        CHECK(iclforge::ac4::verify_stream(stream, f.policy()).verdict ==
              crypto::Verdict::kInvalid);

        const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, f.options());
        REQUIRE(signed_stream.has_value());
        CHECK(std::equal(stream.begin(), stream.end(), signed_stream->begin()));
        const auto after = iclforge::ac4::locate_signature_trailer(*signed_stream);
        REQUIRE(after.has_value());
        CHECK(after->present);
        CHECK(after->content_size == stream.size());
        CHECK(iclforge::ac4::scan(*signed_stream).frames.size() ==
              iclforge::ac4::scan(stream).frames.size());
        const crypto::VerifyReport report =
            iclforge::ac4::verify_stream(*signed_stream, f.policy());
        CHECK_FALSE(report.fault.has_value());
        CHECK(report.verdict == crypto::Verdict::kChecksChainUnanchored);
        CHECK(*iclforge::ac4::strip_signature(*signed_stream) == stream);
        ++signed_count;
    }
    CHECK(signed_count > 0);
    INFO(refused_count << " file(s) that are not AC-4 were refused");
}

TEST_CASE("what a signature says about itself is escaped in the summary", "[ac4][signing]") {
    const Fixture f;
    crypto::SignOptions options = f.options();
    options.metadata.publisher_name = "Pub\x1b[2J\nsigner ok";
    options.metadata.application_name = "App\r\xE2\x80\xAE";
    const std::vector<std::byte> stream = synthetic_stream();
    const auto signed_stream = iclforge::ac4::sign_stream(stream, f.credentials, options);
    REQUIRE(signed_stream.has_value());
    const iclforge::ac4::SignatureSummary summary =
        iclforge::ac4::summarise_signature(*signed_stream);
    REQUIRE(summary.readable);
    for (const std::string& text : {summary.publisher, summary.application, summary.signer,
                                    summary.signed_date, summary.signing_method}) {
        CHECK(std::none_of(text.begin(), text.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u < 0x20 || u == 0x7F;
        }));
    }
    CHECK(summary.publisher == "Pub\\x1B[2J\\x0Asigner ok");
    CHECK(summary.application == "App\\x0D\\u{202E}");
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

    // A couple of stray bytes after the last frame are damage, not a trailer: nothing is signed,
    // stripped or hashed past them, and the report says where the frames stop.
    std::vector<std::byte> stray = stream;
    stray.push_back(std::byte{0x01});
    stray.push_back(std::byte{0x02});
    const auto strays = iclforge::ac4::locate_signature_trailer(stray);
    REQUIRE(strays.has_value());
    CHECK_FALSE(strays->present);
    CHECK(strays->stray == 2);
    CHECK(strays->framing_ends == stream.size());
    CHECK_FALSE(iclforge::ac4::signed_content_digest(stray).has_value());
    CHECK_FALSE(iclforge::ac4::strip_signature(stray).has_value());
    CHECK_FALSE(iclforge::ac4::sign_stream(stray, f.credentials, f.options()).has_value());

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
