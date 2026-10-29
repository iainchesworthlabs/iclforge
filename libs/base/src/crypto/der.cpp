#include "iclforge/base/crypto/der.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace iclforge::base::crypto::der {

std::optional<Tlv> read(std::span<const std::byte>& input) {
    if (input.size() < 2) {
        return std::nullopt;
    }
    const std::uint8_t tag = std::to_integer<std::uint8_t>(input[0]);
    if ((tag & 0x1FU) == 0x1FU) {
        return std::nullopt;  // multi-byte tag numbers are not used by anything read here
    }
    const std::uint8_t first = std::to_integer<std::uint8_t>(input[1]);
    std::size_t header = 2;
    std::size_t length = 0;
    if (first < 0x80) {
        length = first;
    } else {
        const std::size_t octets = first & 0x7FU;
        // 0x80 is the indefinite form, which DER forbids (X.690 §10.1). More than four octets would
        // be a length over 4 GiB, which no input here can hold.
        if (octets == 0 || octets > 4 || input.size() < 2 + octets) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < octets; ++i) {
            length = (length << 8) | std::to_integer<std::size_t>(input[2 + i]);
        }
        // Minimal form: no leading zero octet, and not a value the short form could carry.
        if (std::to_integer<std::uint8_t>(input[2]) == 0 || length < 0x80) {
            return std::nullopt;
        }
        header += octets;
    }
    if (length > input.size() - header) {
        return std::nullopt;
    }
    Tlv out;
    out.tag = tag;
    out.content = input.subspan(header, length);
    out.encoded = input.first(header + length);
    input = input.subspan(header + length);
    return out;
}

std::optional<Tlv> read_expecting(std::span<const std::byte>& input, std::uint8_t tag) {
    std::span<const std::byte> probe = input;
    const std::optional<Tlv> tlv = read(probe);
    if (!tlv.has_value() || tlv->tag != tag) {
        return std::nullopt;
    }
    input = probe;
    return tlv;
}

std::optional<std::string> oid_to_string(std::span<const std::byte> content) {
    if (content.empty()) {
        return std::nullopt;
    }
    std::string out;
    std::uint64_t value = 0;
    bool first_arc = true;
    bool in_progress = false;
    for (std::size_t i = 0; i < content.size(); ++i) {
        const std::uint8_t b = std::to_integer<std::uint8_t>(content[i]);
        if (!in_progress && b == 0x80) {
            return std::nullopt;  // a subidentifier does not start with a padding octet
        }
        if (value > (UINT64_MAX >> 7)) {
            return std::nullopt;
        }
        value = (value << 7) | (b & 0x7FU);
        in_progress = true;
        if ((b & 0x80U) != 0) {
            continue;
        }
        if (first_arc) {
            // X.690 §8.19.4: the first two arcs share the first subidentifier.
            const std::uint64_t arc0 = value < 40 ? 0 : (value < 80 ? 1 : 2);
            out += std::to_string(arc0);
            out += '.';
            out += std::to_string(value - arc0 * 40);
            first_arc = false;
        } else {
            out += '.';
            out += std::to_string(value);
        }
        value = 0;
        in_progress = false;
    }
    if (in_progress) {
        return std::nullopt;  // the last subidentifier was cut off
    }
    return out;
}

std::optional<std::uint64_t> small_unsigned(std::span<const std::byte> content) {
    if (content.empty() || content.size() > 9) {
        return std::nullopt;
    }
    if ((std::to_integer<std::uint8_t>(content[0]) & 0x80U) != 0) {
        return std::nullopt;  // negative
    }
    if (content.size() > 1 && content[0] == std::byte{0} &&
        (std::to_integer<std::uint8_t>(content[1]) & 0x80U) == 0) {
        return std::nullopt;  // not minimal
    }
    std::span<const std::byte> digits = content;
    if (digits.size() > 1 && digits[0] == std::byte{0}) {
        digits = digits.subspan(1);
    }
    if (digits.size() > 8) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const std::byte b : digits) {
        value = (value << 8) | std::to_integer<std::uint64_t>(b);
    }
    return value;
}

}  // namespace iclforge::base::crypto::der
