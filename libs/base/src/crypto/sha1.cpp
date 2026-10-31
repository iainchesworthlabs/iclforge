#include "iclforge/base/crypto/sha1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace iclforge::base::crypto {
namespace {

constexpr std::uint32_t rotl(std::uint32_t v, unsigned n) {
    return (v << n) | (v >> (32U - n));
}

// FIPS 180-4 §6.1.2: one 512-bit block into the five-word state.
void process_block(std::array<std::uint32_t, 5>& h, const std::uint8_t* block) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = (static_cast<std::uint32_t>(block[4 * t]) << 24) |
               (static_cast<std::uint32_t>(block[4 * t + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * t + 2]) << 8) |
               static_cast<std::uint32_t>(block[4 * t + 3]);
    }
    for (std::size_t t = 16; t < 80; ++t) {
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }
    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];
    for (std::size_t t = 0; t < 80; ++t) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999U;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        const std::uint32_t temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::array<std::byte, 20> sha1(std::span<const std::byte> data) {
    std::array<std::uint32_t, 5> h{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    // FIPS 180-4 §5.1.1: append 0x80, zeros to 56 mod 64, then the bit length as a big-endian
    // 64-bit value.
    std::vector<std::uint8_t> padded;
    padded.reserve(data.size() + 72);
    for (const std::byte b : data) {
        padded.push_back(static_cast<std::uint8_t>(b));
    }
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    }
    for (std::size_t off = 0; off < padded.size(); off += 64) {
        process_block(h, padded.data() + off);
    }
    std::array<std::byte, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<std::byte>(h[i] >> 24);
        out[4 * i + 1] = static_cast<std::byte>(h[i] >> 16);
        out[4 * i + 2] = static_cast<std::byte>(h[i] >> 8);
        out[4 * i + 3] = static_cast<std::byte>(h[i]);
    }
    return out;
}

}  // namespace iclforge::base::crypto
