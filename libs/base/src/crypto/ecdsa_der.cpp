#include "iclforge/base/crypto/ecdsa_der.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "der.hpp"

namespace iclforge::base::crypto {
namespace {

void append_integer(std::vector<std::byte>& out, const std::array<std::byte, 32>& value) {
    std::size_t skip = 0;
    while (skip < 31 && value[skip] == std::byte{0}) {
        ++skip;
    }
    const bool pad = (std::to_integer<unsigned>(value[skip]) & 0x80u) != 0;
    out.push_back(static_cast<std::byte>(detail::kTagInteger));
    out.push_back(static_cast<std::byte>(32 - skip + (pad ? 1 : 0)));
    if (pad) {
        out.push_back(std::byte{0});
    }
    out.insert(out.end(), value.begin() + static_cast<std::ptrdiff_t>(skip), value.end());
}

// An INTEGER element's content to 32 bytes, big endian; false if it is not a minimal positive
// integer of at most 256 bits.
bool read_scalar(std::span<const std::byte>& input, std::array<std::byte, 32>& out) {
    const auto element = detail::read_tagged(input, detail::kTagInteger);
    if (!element.has_value() || element->content.empty()) {
        return false;
    }
    std::span<const std::byte> content = element->content;
    if ((std::to_integer<unsigned>(content[0]) & 0x80u) != 0) {
        return false;  // negative
    }
    if (content[0] == std::byte{0}) {
        if (content.size() == 1) {
            out.fill(std::byte{0});  // the value zero; the caller's range check refuses it
            return true;
        }
        // A leading zero byte is only a sign byte, and only when the next byte's high bit needs it.
        if ((std::to_integer<unsigned>(content[1]) & 0x80u) == 0) {
            return false;
        }
        content = content.subspan(1);
    }
    if (content.size() > 32) {
        return false;
    }
    out.fill(std::byte{0});
    std::copy(content.begin(), content.end(),
              out.begin() + static_cast<std::ptrdiff_t>(32 - content.size()));
    return true;
}

}  // namespace

std::vector<std::byte> encode_ecdsa_der(const EcdsaSignature& signature) {
    std::vector<std::byte> body;
    body.reserve(70);
    append_integer(body, signature.r);
    append_integer(body, signature.s);
    std::vector<std::byte> out;
    out.reserve(body.size() + 2);
    out.push_back(static_cast<std::byte>(detail::kTagSequence));
    out.push_back(static_cast<std::byte>(body.size()));  // at most 70: the short form
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<EcdsaSignature> decode_ecdsa_der(std::span<const std::byte> der) {
    std::span<const std::byte> input = der;
    const auto sequence = detail::read_tagged(input, detail::kTagSequence);
    if (!sequence.has_value() || !input.empty()) {
        return std::nullopt;
    }
    std::span<const std::byte> body = sequence->content;
    EcdsaSignature signature;
    if (!read_scalar(body, signature.r) || !read_scalar(body, signature.s) || !body.empty()) {
        return std::nullopt;
    }
    if (!p256_scalar_in_range(signature.r) || !p256_scalar_in_range(signature.s)) {
        return std::nullopt;
    }
    return signature;
}

}  // namespace iclforge::base::crypto
