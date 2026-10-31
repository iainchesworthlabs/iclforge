#include "iclforge/base/crypto/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "crypto/sha256_constants.hpp"
#include "crypto/sha256_hw.hpp"
#include "iclforge/base/detail/sha256_impl.hpp"

namespace iclforge::internal::crypto {
namespace {

constexpr std::uint32_t ror(std::uint32_t x, int r) {
    return (x >> r) | (x << (32 - r));
}

void compress_block(std::uint32_t* h, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] =
            (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
            (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror(w[static_cast<std::size_t>(i - 15)], 7) ^
                                 ror(w[static_cast<std::size_t>(i - 15)], 18) ^
                                 (w[static_cast<std::size_t>(i - 15)] >> 3);
        const std::uint32_t s1 = ror(w[static_cast<std::size_t>(i - 2)], 17) ^
                                 ror(w[static_cast<std::size_t>(i - 2)], 19) ^
                                 (w[static_cast<std::size_t>(i - 2)] >> 10);
        w[static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i - 16)] + s0 +
                                         w[static_cast<std::size_t>(i - 7)] + s1;
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + s1 + ch + kSha256RoundConstants[static_cast<std::size_t>(i)] +
                                 w[static_cast<std::size_t>(i)];
        const std::uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

}  // namespace

void sha256_compress_portable(std::uint32_t* state, const std::uint8_t* data,
                              std::size_t blocks) noexcept {
    for (; blocks != 0; --blocks, data += 64) {
        compress_block(state, data);
    }
}

bool sha256_hardware_supported() noexcept {
    static const bool supported = hw::cpu_reports_sha256();
    return supported;
}

void sha256_compress_hardware(std::uint32_t* state, const std::uint8_t* data,
                              std::size_t blocks) noexcept {
    if (sha256_hardware_supported()) {
        hw::compress(state, data, blocks);
    } else {
        sha256_compress_portable(state, data, blocks);
    }
}

}  // namespace iclforge::internal::crypto

namespace iclforge::base::crypto {
namespace {

constexpr std::array<std::uint32_t, 8> kInitialHash = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

}  // namespace

void Sha256::reset() {
    h_ = kInitialHash;
    total_bytes_ = 0;
    buffered_ = 0;
}

void Sha256::process_blocks(const std::uint8_t* data, std::size_t blocks) {
    // Chosen once per process (see iclforge/base/detail/sha256_impl.hpp); the branch is a load of a
    // cached bool, the same cost the portable path has always paid per block.
    if (internal::crypto::sha256_hardware_supported()) {
        internal::crypto::sha256_compress_hardware(h_.data(), data, blocks);
    } else {
        internal::crypto::sha256_compress_portable(h_.data(), data, blocks);
    }
}

void Sha256::update(std::span<const std::byte> data) {
    total_bytes_ += data.size();
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t len = data.size();
    if (len == 0) {
        return;
    }

    // Top up a partly filled block first.
    if (buffered_ != 0) {
        const std::size_t take = std::min(std::size_t{64} - buffered_, len);
        std::memcpy(buffer_.data() + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ < 64) {
            return;
        }
        process_blocks(buffer_.data(), 1);
        buffered_ = 0;
    }

    // Whole blocks go straight from the caller's buffer, in one call: a kernel that keeps the state
    // in registers across blocks only gets to if it is handed more than one.
    if (len >= 64) {
        const std::size_t blocks = len / 64;
        process_blocks(p, blocks);
        p += blocks * 64;
        len -= blocks * 64;
    }

    if (len != 0) {
        std::memcpy(buffer_.data(), p, len);
        buffered_ = len;
    }
}

void Sha256::finish(std::span<std::byte, 32> out) {
    const std::uint64_t bit_length = total_bytes_ * 8;

    // 0x80, zeros up to 56 mod 64, then the message length in bits as a big-endian 64-bit word
    // (FIPS 180-4 section 5.1.1). A tail with no room for the length takes a block of its own
    // first.
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::memset(buffer_.data() + buffered_, 0, 64 - buffered_);
        process_blocks(buffer_.data(), 1);
        buffered_ = 0;
    }
    std::memset(buffer_.data() + buffered_, 0, 56 - buffered_);
    for (int i = 0; i < 8; ++i) {
        buffer_[static_cast<std::size_t>(56 + i)] =
            static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
    }
    process_blocks(buffer_.data(), 1);

    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i * 4)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 24);
        out[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 16);
        out[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 8);
        out[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)]);
    }
    reset();
}

std::array<std::byte, 32> sha256(std::span<const std::byte> data) {
    Sha256 hash;
    hash.update(data);
    std::array<std::byte, 32> out{};
    hash.finish(out);
    return out;
}

}  // namespace iclforge::base::crypto
