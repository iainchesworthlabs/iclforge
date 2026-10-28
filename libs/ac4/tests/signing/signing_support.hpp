#pragma once

// What the AC-4 content signing tests share: a real encoded stream (frames from
// iclforge::ac4::Encoder, not made-up bytes), an ephemeral signing authority (tests/support/
// test_pki.hpp: keys made from a seed at run time; no key is committed), and the verification
// options the cases start from.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

namespace iclforge::test::ac4_signing {

namespace pki = iclforge::test::pki;
namespace crypto = iclforge::base::crypto;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

// A stereo stream of two tones, long enough for several frames: at 48 kHz a frame is 2 048
// samples, and the encoder holds back a frame and a half, so 24 000 samples give about ten.
struct Stream {
    std::vector<ac4::EncodedFrame> encoded;

    [[nodiscard]] std::vector<std::span<const std::byte>> views() const {
        std::vector<std::span<const std::byte>> out;
        out.reserve(encoded.size());
        for (const ac4::EncodedFrame& frame : encoded) {
            out.emplace_back(frame.raw_ac4_frame);
        }
        return out;
    }

    // The elementary stream: every frame in an ac4_syncframe(), with the CRC word or without.
    [[nodiscard]] std::vector<std::byte> elementary(bool crc) const {
        std::vector<std::byte> out;
        for (const ac4::EncodedFrame& frame : encoded) {
            const std::vector<std::byte> sync = ac4::sync_frame(frame.raw_ac4_frame, crc);
            out.insert(out.end(), sync.begin(), sync.end());
        }
        return out;
    }
};

inline std::vector<float> tone(double hz, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            0.4 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return x;
}

inline Stream encode_stream(std::size_t samples = 24000) {
    auto encoder = ac4::Encoder::create(ac4::EncoderConfig{});
    REQUIRE(encoder.has_value());
    const std::vector<float> left = tone(440.0, samples);
    const std::vector<float> right = tone(660.0, samples);
    const std::vector<std::span<const float>> channels{left, right};
    Stream stream;
    auto frames = encoder->encode(channels);
    REQUIRE(frames.has_value());
    stream.encoded = std::move(*frames);
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    stream.encoded.insert(stream.encoded.end(), rest->begin(), rest->end());
    REQUIRE(stream.encoded.size() >= 3);
    return stream;
}

// One encoded stream for the whole binary: encoding takes a moment and every case starts from the
// same frames.
inline const Stream& shared_stream() {
    static const Stream stream = encode_stream();
    return stream;
}

// A signer: the specification-shaped chain from the test PKI, its credentials, and trust in its
// root.
struct Signer {
    pki::Authority authority;
    crypto::SigningCredentials credentials;
    std::int64_t signed_unix = pki::at(2026, 10, 28, 12);

    explicit Signer(pki::AuthorityOptions options = {})
        : authority(pki::make_authority(std::move(options))),
          credentials{authority.leaf.keys.key, authority.chain()} {}

    [[nodiscard]] crypto::TrustStore trust() const {
        crypto::TrustStore store;
        store.anchors.push_back(crypto::TrustAnchor{authority.root.cert, "test root"});
        return store;
    }
};

inline crypto::VerifyOptions verify_options() {
    crypto::VerifyOptions options;
    options.chain.now_unix = kNow;
    return options;
}

// Signs `stream` and returns what a carrier hands back for it.
inline ac4::signing::CarriedContent carried(const Stream& stream,
                                            std::vector<std::byte> signature) {
    ac4::signing::CarriedContent content;
    content.frames = stream.views();
    content.signature = std::move(signature);
    content.signature_present = true;
    return content;
}

}  // namespace iclforge::test::ac4_signing
