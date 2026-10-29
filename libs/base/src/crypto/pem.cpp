#include "iclforge/base/crypto/pem.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iclforge::base::crypto {
namespace {

constexpr std::string_view kBeginPrefix = "-----BEGIN ";
constexpr std::string_view kEndPrefix = "-----END ";
constexpr std::string_view kDashes = "-----";
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    std::vector<std::byte> out;
    // Reserved for the most the text can decode to, so that a key being decoded is never copied
    // to a bigger buffer, which would leave the smaller one freed with the key in it.
    out.reserve(text.size() / 4 * 3 + 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    std::size_t symbols = 0;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) {
            return std::nullopt;  // data after padding
        }
        const int v = sextet(c);
        if (v < 0) {
            return std::nullopt;
        }
        ++symbols;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFFU));
        }
    }
    // RFC 4648 §4: the symbol count plus padding fills whole quanta of four.
    if (padding > 2 || (symbols + static_cast<std::size_t>(padding)) % 4 != 0 || symbols % 4 == 1) {
        return std::nullopt;
    }
    return out;
}

std::string base64_encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t b0 = std::to_integer<std::uint32_t>(data[i]);
        const std::uint32_t b1 =
            i + 1 < data.size() ? std::to_integer<std::uint32_t>(data[i + 1]) : 0;
        const std::uint32_t b2 =
            i + 2 < data.size() ? std::to_integer<std::uint32_t>(data[i + 2]) : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3FU]);
        out.push_back(i + 1 < data.size() ? kAlphabet[(triple >> 6) & 0x3FU] : '=');
        out.push_back(i + 2 < data.size() ? kAlphabet[triple & 0x3FU] : '=');
    }
    return out;
}

std::optional<std::vector<PemBlock>> pem_decode(std::string_view text) {
    std::vector<PemBlock> blocks;
    std::size_t pos = 0;
    while (true) {
        const std::size_t begin = text.find(kBeginPrefix, pos);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t label_start = begin + kBeginPrefix.size();
        const std::size_t label_end = text.find(kDashes, label_start);
        if (label_end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view label = text.substr(label_start, label_end - label_start);
        const std::size_t body_start = label_end + kDashes.size();
        const std::size_t end = text.find(kEndPrefix, body_start);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t end_label_start = end + kEndPrefix.size();
        const std::size_t end_label_end = text.find(kDashes, end_label_start);
        if (end_label_end == std::string_view::npos ||
            text.substr(end_label_start, end_label_end - end_label_start) != label) {
            return std::nullopt;
        }
        // Encapsulated headers ("Proc-Type: 4,ENCRYPTED") are the old encrypted-key form: a body
        // with a colon is not base64, which makes the decode below fail, as it should.
        auto der = base64_decode(text.substr(body_start, end - body_start));
        if (!der.has_value()) {
            return std::nullopt;
        }
        blocks.push_back(PemBlock{std::string(label), std::move(*der)});
        pos = end_label_end + kDashes.size();
    }
    return blocks;
}

std::string pem_encode(std::string_view label, std::span<const std::byte> der) {
    std::string out;
    out += kBeginPrefix;
    out += label;
    out += kDashes;
    out += '\n';
    const std::string body = base64_encode(der);
    for (std::size_t i = 0; i < body.size(); i += 64) {
        out += body.substr(i, 64);
        out += '\n';
    }
    out += kEndPrefix;
    out += label;
    out += kDashes;
    out += '\n';
    return out;
}

}  // namespace iclforge::base::crypto
