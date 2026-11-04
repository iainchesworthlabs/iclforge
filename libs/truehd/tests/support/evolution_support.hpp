#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/core/crc.hpp"

// An evolution frame and the extra data around it, written here from TH spec v3.6 §15.4 and §16.1
// to §16.4 and nothing else: no code of libs/truehd/src/signing and none of the library's reader,
// so a test that signs one of these and compares with what it builds itself is an independent
// check. Keys are made by the test, from a seed, at run time; no key is written into any file.

namespace iclforge::truehd::testing {

struct FramePayloadSpec {
    unsigned id = 11;
    std::vector<std::byte> bytes;
};

struct FrameSpec {
    unsigned key_id = 1;  // 0 to 6: the extension for 7 is not written
    std::vector<FramePayloadSpec> payloads;
    unsigned primary_code = 2;  // 0 to 3: 0, 1, 4 and 16 bytes
    unsigned secondary_code = 0;
    std::vector<std::byte> primary;    // empty: zeros, which is what an unsigned frame holds
    std::vector<std::byte> secondary;  // empty: zeros
    std::size_t zero_fill_bytes = 2;   // zero bytes after the protection
    std::vector<std::byte> trailing;   // bytes after the zero fill: not part of the container
    bool spec36 = false;               // the size is an escape value, not groups with a flag
};

inline constexpr std::array<std::size_t, 4> kTestProtectionBytes{0, 1, 4, 16};

inline std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// evo_frame(): version 0, key_id, the payloads each with the configuration TS 103 420 Table 56
// uses (frame aligned, highest priority), the terminating id 0 and evo_protection().
inline std::vector<std::byte> build_frame(const FrameSpec& spec) {
    BitWriter w;
    w.put(0, 2);
    w.put(spec.key_id, 3);
    for (const FramePayloadSpec& payload : spec.payloads) {
        w.put(payload.id, 5);
        w.put(0, 4);   // no sample offset, duration, group id or codec data
        w.put(0, 1);   // discard_unknown_payload
        w.put(1, 1);   // frame_aligned
        w.put(0, 2);   // create_duplicate, remove_duplicate
        w.put(31, 5);  // priority
        w.put(0, 2);   // processing allowed
        w.put(static_cast<std::uint32_t>(payload.bytes.size()), 8);
        if (!spec.spec36) {
            w.put(0, 1);  // no further group
        }
        for (const std::byte b : payload.bytes) {
            w.put(std::to_integer<std::uint32_t>(b), 8);
        }
    }
    w.put(0, 5);
    w.put(spec.primary_code, 2);
    w.put(spec.secondary_code, 2);
    for (std::size_t i = 0; i < kTestProtectionBytes[spec.primary_code]; ++i) {
        w.put(i < spec.primary.size() ? std::to_integer<std::uint32_t>(spec.primary[i]) : 0U, 8);
    }
    for (std::size_t i = 0; i < kTestProtectionBytes[spec.secondary_code]; ++i) {
        w.put(i < spec.secondary.size() ? std::to_integer<std::uint32_t>(spec.secondary[i]) : 0U,
              8);
    }
    std::vector<std::byte> out = w.take();
    out.insert(out.end(), spec.zero_fill_bytes, std::byte{0});
    out.insert(out.end(), spec.trailing.begin(), spec.trailing.end());
    return out;
}

// The extra data region (TH spec §16.1): header word with its check nibble, evo_reserved and the
// frame length, the frame, `padding` bytes (zero unless the caller sets them) up to the word
// boundary, and the parity byte (§16.4).
inline std::vector<std::byte> build_region(std::span<const std::byte> frame,
                                           std::span<const std::byte> padding = {},
                                           unsigned evo_reserved = 0) {
    const std::size_t content = 4 + frame.size() + padding.size() + 1;
    const std::size_t words = (content + 1) / 2;
    const auto length = static_cast<std::uint32_t>(words - 1);
    const auto word0 = static_cast<std::uint32_t>(
        (std::uint32_t{extra_data_check_nibble(length)} << 12U) | (length & 0xFFFU));
    std::vector<std::byte> out{
        static_cast<std::byte>(word0 >> 8U), static_cast<std::byte>(word0 & 0xFFU),
        static_cast<std::byte>(((evo_reserved & 0xFU) << 4U) | ((frame.size() >> 8U) & 0xFU)),
        static_cast<std::byte>(frame.size() & 0xFFU)};
    out.insert(out.end(), frame.begin(), frame.end());
    out.insert(out.end(), padding.begin(), padding.end());
    out.resize(words * 2, std::byte{0});  // zero up to the word boundary, then the parity byte
    // §16.4: the word of evo_reserved and the frame length, the seed, then the bytes after the
    // framing.
    unsigned accumulator =
        (((evo_reserved & 0xFU) << 12U) | static_cast<unsigned>(frame.size())) ^ 0xA9U;
    for (std::size_t i = 4; i + 1 < out.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(out[i]);
    }
    out.back() = static_cast<std::byte>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
    return out;
}

inline std::vector<std::byte> build_region(const FrameSpec& spec) {
    return build_region(build_frame(spec));
}

// A throw-away key, made from a seed when the test runs.
inline base::crypto::SigningKey make_test_key(std::uint32_t seed, std::size_t size = 32) {
    std::mt19937 generator(seed);
    std::vector<std::byte> bytes(size);
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(generator() & 0xFFU);
    }
    return base::crypto::SigningKey(std::move(bytes));
}

// The tag a correct signer writes: the first `size` bytes of HMAC-SHA-256 over the frame built with
// both protection fields zero.
inline std::vector<std::byte> expected_tag(const base::crypto::SigningKey& key, FrameSpec spec) {
    spec.primary.clear();
    spec.secondary.clear();
    const std::vector<std::byte> message = build_frame(spec);
    const auto digest = base::crypto::hmac_sha256(key.bytes(), message);
    return {digest.begin(),
            digest.begin() + static_cast<std::ptrdiff_t>(kTestProtectionBytes[spec.primary_code])};
}

// `spec` with its primary field set to the correct tag for `key`.
inline FrameSpec signed_spec(const base::crypto::SigningKey& key, FrameSpec spec) {
    spec.primary = expected_tag(key, spec);
    return spec;
}

// A frame with one object audio metadata payload whose bytes depend on `seed`.
inline FrameSpec sample_spec(unsigned seed) {
    FrameSpec spec;
    spec.key_id = seed % 5U;
    FramePayloadSpec payload;
    payload.id = 11;
    for (unsigned i = 0; i < 24; ++i) {
        payload.bytes.push_back(static_cast<std::byte>((seed * 37U + i * 11U + 5U) & 0xFFU));
    }
    spec.payloads.push_back(std::move(payload));
    return spec;
}

// Flip bit `bit` (MSB first) of the evolution payload inside an extra data region.
inline void flip_payload_bit(std::vector<std::byte>& region, std::size_t bit) {
    const std::size_t at = 32U + bit;
    region[at >> 3U] ^= static_cast<std::byte>(0x80U >> (at & 7U));
}

}  // namespace iclforge::truehd::testing
