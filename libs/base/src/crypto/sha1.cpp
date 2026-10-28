#include "iclforge/base/crypto/sha1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace iclforge::base::crypto {
namespace {

constexpr std::uint32_t rotl(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void process_block(std::array<std::uint32_t, 5>& h, const std::uint8_t* block) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
               (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];
    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::array<std::byte, 20> sha1(std::span<const std::byte> data) {
    std::array<std::uint32_t, 5> h = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    // FIPS 180-4 §5.1.1: the message, a 1 bit, zeros up to 56 mod 64, and the length in bits as a
    // 64-bit big-endian value. The tail is built in a small buffer so the whole message is not
    // copied.
    const std::size_t whole = data.size() / 64 * 64;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    for (std::size_t offset = 0; offset < whole; offset += 64) {
        process_block(h, bytes + offset);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - whole;
    if (rest != 0) {
        std::memcpy(tail.data(), bytes + whole, rest);
    }
    tail[rest] = 0x80;
    const std::size_t tail_size = rest < 56 ? 64 : 128;
    const std::uint64_t bit_length = std::uint64_t{data.size()} * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
    }
    for (std::size_t offset = 0; offset < tail_size; offset += 64) {
        process_block(h, tail.data() + offset);
    }

    std::array<std::byte, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<std::byte>(h[i] >> (24 - 8 * j));
        }
    }
    return out;
}

}  // namespace iclforge::base::crypto
