#pragma once

// Small helpers the crypto tests share: hex in and out, and byte spans over text.

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iclforge::test::crypto {

inline std::vector<std::byte> from_hex(std::string_view hex) {
    std::vector<std::byte> out;
    int pending = -1;
    for (const char c : hex) {
        int v = 0;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue;  // spaces and colons are separators
        }
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<std::byte>((pending << 4) | v));
            pending = -1;
        }
    }
    return out;
}

template <std::size_t N>
std::array<std::byte, N> array_from_hex(std::string_view hex) {
    const std::vector<std::byte> v = from_hex(hex);
    std::array<std::byte, N> out{};
    for (std::size_t i = 0; i < N && i < v.size(); ++i) {
        out[i] = v[i];
    }
    return out;
}

inline std::string to_hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0xFU]);
    }
    return out;
}

inline std::span<const std::byte> text_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace iclforge::test::crypto
