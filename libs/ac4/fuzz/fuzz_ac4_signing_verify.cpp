#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/ac4/signing/content_signing.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "test_pki.hpp"

// iclforge::ac4::signing::ElementaryCarrier::read and verify_frames (libs/ac4/src/signing/):
// taking an AC-4 stream and the file beside it apart, hashing the frames it finds, and judging
// the signature against them. This is what `forge decode ... verify-content` runs on a stream
// and a sidecar that arrived from somewhere the verifier does not control, and the carrier walks
// the stream itself (the sync word, the frame_size field with its escape, the optional CRC word)
// with nothing in front of it. Signing is fuzzed too, but only as the oracle below: it runs on
// a stream the operator just produced.
//
// What this harness does not reach. The MP4 `uuid` box carrier is in the forge command line's
// sources (apps/forge/cli/src), because iclforge::ac4 does not use the container library, so a
// libs/ac4 harness cannot link it. The box is found by the MP4 reader, which fuzz_mp4_demux
// presses (user boxes included); picking the box out of what the reader returns is
// apps/forge/cli/tests/test_mp4_signature_carrier.cpp's. The Thrift payload and the chain are
// fuzz_content_signature_verify's and fuzz_x509_parse's (libs/base/fuzz), with edit scripts that
// reach past the signature check; this harness hands the payload on and does not repeat them.
//
// The signer is one fixed authority built when the process starts from a seed: an ephemeral test
// key and nothing else (tests/support/test_pki.hpp). Nothing is read from disk and no key is
// committed. Its root is the only trust anchor.
//
// Input layout:
//     [0]            flags
//                      bit 0   allow_expired_intermediates
//                      bit 1   the "any" leaf profile instead of the specification's
//                      bit 2   tamper: with bit 3, change one bit of the stream after it is signed
//                      bit 3   oracle mode: sign the stream's own frames (see below), and ignore
//                              the sidecar
//                      bits 4-7  with bit 2, which frame and which of its bytes (see oracle())
//     [1 .. 3)       n, the stream's length, big endian, clamped to the bytes that follow
//     [3 .. 3+n)     the stream, handed to ElementaryCarrier as it is
//     [3+n .. )      the sidecar, handed on as it is (unused in oracle mode)
//
// Beyond "does not crash", four properties are checked, each of which aborts if it fails:
//   1. The frames a carrier returns lie inside the bytes it was given, so they are views of the
//      input and the digest is of bytes the caller can see.
//   2. A stream with no sidecar never verifies. (The same stream with the sidecar the input
//      supplies is allowed to: a fuzzer that forged an ECDSA signature would have found something
//      far worse than this harness.)
//   3. Oracle mode: whatever scans to at least one frame, signed by the fixed signer, verifies
//      under the fixed trust, the digest the incremental ContentDigest gives is the one
//      digest_of_frames gives, and a stream with a bit changed after signing does not verify.
//   4. A stream that scans to no frames is refused by sign_frames and by verify_frames.
namespace {

using namespace iclforge::ac4::signing;
namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

struct Fixture {
    pki::Authority authority = pki::make_authority();
    crypto::SigningCredentials credentials{authority.leaf.keys.key, authority.chain()};
    crypto::TrustStore trust;

    Fixture() { trust.anchors.push_back(crypto::TrustAnchor{authority.root.cert, "fuzz root"}); }
};

const Fixture& fixture() {
    static const Fixture f;
    return f;
}

[[noreturn]] void fail() {
    std::abort();
}

// Property 1: every frame is a view into `stream`.
void check_inside(const CarriedContent& content, std::span<const std::byte> stream) {
    const std::byte* const begin = stream.data();
    const std::byte* const end = stream.data() + stream.size();
    for (const std::span<const std::byte> frame : content.frames) {
        if (frame.empty()) {
            continue;
        }
        if (frame.data() < begin || frame.data() + frame.size() > end) {
            fail();
        }
    }
}

// The verifier on whatever it is given, with the leaf profile and the policy the flags choose.
// Returns whether the content verified; a failure is named, which runs error_name() and
// describe() over the code it produced.
bool verify_with_flags(const CarriedContent& content, std::uint8_t flags) {
    const Fixture& f = fixture();
    crypto::VerifyOptions options;
    options.chain.now_unix = kNow;
    options.chain.allow_expired_intermediates = (flags & 1) != 0;
    if ((flags & 2) != 0) {
        options.profile = crypto::LeafProfile::any();
    }
    const auto verified = verify_frames(content, f.trust, options);
    if (!verified) {
        (void)crypto::error_name(verified.error().error);
        (void)crypto::describe(verified.error().error);
        return false;
    }
    // Whatever verifies has a chain and a leaf to describe it.
    if (verified->chain.empty()) {
        fail();
    }
    (void)verified->leaf().subject.to_string();
    (void)verified->acceptance.expired_intermediates.size();
    return true;
}

// The options the oracle expects to succeed under: the defaults, the specification's leaf
// profile included, which is what the fixture's leaf is built to, at a time inside every
// certificate's dates.
bool verify_for_oracle(const CarriedContent& content) {
    const Fixture& f = fixture();
    crypto::VerifyOptions options;
    options.chain.now_unix = kNow;
    return verify_frames(content, f.trust, options).has_value();
}

void oracle(std::span<const std::byte> stream, const CarriedContent& unsigned_content,
            std::uint8_t flags) {
    const Fixture& f = fixture();

    // Property 4, and the case where there is nothing to sign.
    const auto signature =
        sign_frames(unsigned_content.frames, f.credentials, pki::at(2026, 10, 28, 12));
    if (unsigned_content.frames.empty()) {
        if (signature.has_value() || verify_for_oracle(unsigned_content)) {
            fail();
        }
        // A signature over the digest of nothing, which anyone with the key can make, must not
        // make a stream with no frames verify either.
        const auto of_nothing =
            crypto::sign_digest(digest_of_frames({}), f.credentials, pki::at(2026, 10, 28, 12));
        if (!of_nothing.has_value()) {
            fail();
        }
        CarriedContent empty;
        empty.signature = *of_nothing;
        empty.signature_present = true;
        if (verify_for_oracle(empty)) {
            fail();
        }
        return;
    }
    if (!signature.has_value()) {
        fail();
    }

    // The incremental digest and the one-shot digest are the same function of the same frames.
    ContentDigest incremental;
    for (const std::span<const std::byte> frame : unsigned_content.frames) {
        incremental.add_frame(frame);
    }
    if (incremental.frame_count() != unsigned_content.frames.size() ||
        incremental.finish() != digest_of_frames(unsigned_content.frames)) {
        fail();
    }

    // Property 3: the sidecar the signer wrote verifies against the stream it signed.
    const ElementaryCarrier carrier;
    const auto signed_read = carrier.read(stream, *signature);
    if (!signed_read.has_value() || !signed_read->signature_present ||
        !verify_for_oracle(*signed_read)) {
        fail();
    }

    if ((flags & 4) == 0) {
        return;
    }
    // One bit of one frame, in a copy: the frame_size field and the sync word are outside the
    // signed span, and the frame's own bytes are inside it. Bits 4 to 6 of the flags pick which
    // non-empty frame and bit 7 picks its last byte rather than its first, so a digest that
    // leaves out the end of a frame, or a frame, does not slip past.
    std::vector<std::span<const std::byte>> filled;
    for (const std::span<const std::byte> frame : unsigned_content.frames) {
        if (!frame.empty()) {
            filled.push_back(frame);
        }
    }
    if (filled.empty()) {
        return;
    }
    const std::span<const std::byte> victim = filled[((flags >> 4) & 7) % filled.size()];
    const std::size_t offset = static_cast<std::size_t>(victim.data() - stream.data()) +
                               ((flags & 0x80) != 0 ? victim.size() - 1 : 0);
    std::vector<std::byte> changed(stream.begin(), stream.end());
    changed[offset] ^= std::byte{1};
    // The carrier does not check the CRC, so a stream with one frame bit changed still scans, and
    // the signature is what has to refuse it. A stream that no longer scans is refused earlier.
    const auto changed_read = carrier.read(changed, *signature);
    if (changed_read.has_value() && verify_for_oracle(*changed_read)) {
        fail();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 3) {
        return 0;
    }
    const std::uint8_t flags = data[0];
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    const std::size_t available = size - 3;
    const std::size_t declared = (static_cast<std::size_t>(data[1]) << 8) | data[2];
    const std::size_t stream_size = declared < available ? declared : available;
    const std::span<const std::byte> stream{bytes + 3, stream_size};
    const std::span<const std::byte> sidecar{bytes + 3 + stream_size, available - stream_size};
    const ElementaryCarrier carrier;

    // A stream with no sidecar: the carrier's reading of the stream, and what the verifier says
    // of a stream that carries no signature.
    const auto bare = carrier.read(stream, {});
    if (bare.has_value()) {
        check_inside(*bare, stream);
        if (bare->signature_present || verify_with_flags(*bare, flags)) {
            fail();  // property 2
        }
    }

    if ((flags & 8) != 0) {
        if (bare.has_value()) {
            oracle(stream, *bare, flags);
        }
        return 0;
    }

    // The same stream with the sidecar the input supplies, which is the reading `verify-content`
    // makes of a stream and a file that arrived together.
    if (const auto with_sidecar = carrier.read(stream, sidecar)) {
        check_inside(*with_sidecar, stream);
        (void)verify_with_flags(*with_sidecar, flags);
    }
    return 0;
}
