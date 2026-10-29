#include "iclforge/containers/iec61937/mat.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace iclforge::containers::iec61937 {

namespace {

// ERRATA M6: the three code words, in the payload's own byte order, as FFmpeg's `spdif` muxer
// writes them for TrueHD. TH spec §21.3 names no code word.
constexpr std::array<std::uint8_t, kMatStartCodeBytes> kStartCode{
    0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
    0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0};
constexpr std::array<std::uint8_t, kMatMiddleCodeBytes> kMiddleCode{
    0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0};
constexpr std::array<std::uint8_t, kMatEndCodeBytes> kEndCode{
    0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11};

constexpr std::size_t kMiddleSlot = 11;
constexpr std::size_t kLastSlot = kMatAccessUnitsPerFrame - 1;
// TH spec §3.1: the access unit's length is the 12 bits after the check nibble, in 16-bit words.
constexpr std::size_t kLengthMask = 0x0FFF;
constexpr std::size_t kMinAccessUnitBytes = 4;

[[nodiscard]] std::size_t slot_start(std::size_t slot) {
    return slot == 0 ? kMatStartCodeBytes : slot * kMatSlotBytes;
}

[[nodiscard]] std::size_t slot_limit(std::size_t slot) {
    if (slot == kMiddleSlot) {
        return kMatMiddleCodeOffset;
    }
    if (slot == kLastSlot) {
        return kMatEndCodeOffset;
    }
    return (slot + 1) * kMatSlotBytes;
}

[[nodiscard]] std::uint8_t u8(std::byte b) {
    return std::to_integer<std::uint8_t>(b);
}

// The access unit's own length from its first two bytes, or 0 when they say none.
[[nodiscard]] std::size_t length_of(std::span<const std::byte> bytes) {
    if (bytes.size() < 2) {
        return 0;
    }
    return ((static_cast<std::size_t>(u8(bytes[0])) << 8U | u8(bytes[1])) & kLengthMask) * 2U;
}

void swap_words(std::span<std::byte> bytes) {
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        std::swap(bytes[i], bytes[i + 1]);
    }
}

void put_bytes(std::span<std::byte> out, std::size_t at, std::span<const std::uint8_t> code) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        out[at + i] = static_cast<std::byte>(code[i]);
    }
}

[[nodiscard]] bool matches(std::span<const std::byte> bytes, std::size_t at,
                           std::span<const std::uint8_t> code) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (u8(bytes[at + i]) != code[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::byte> finish_burst(std::span<const std::byte> payload) {
    std::vector<std::byte> burst(kMatBurstBytes);
    // Pa, Pb, Pc and Pd in natural order; the swap below puts them in the carrier's.
    const std::array<std::uint16_t, 4> preamble{0xF872, 0x4E1F, kMatPc, kMatPd};
    for (std::size_t i = 0; i < preamble.size(); ++i) {
        burst[(2 * i)] = static_cast<std::byte>(preamble[i] >> 8U);
        burst[(2 * i) + 1] = static_cast<std::byte>(preamble[i] & 0xFFU);
    }
    std::copy(payload.begin(), payload.end(),
              burst.begin() + static_cast<std::ptrdiff_t>(kMatPreambleBytes));
    put_bytes(burst, kMatPreambleBytes, kStartCode);
    put_bytes(burst, kMatPreambleBytes + kMatMiddleCodeOffset, kMiddleCode);
    put_bytes(burst, kMatPreambleBytes + kMatEndCodeOffset, kEndCode);
    swap_words(burst);
    return burst;
}

}  // namespace

std::size_t mat_slot_capacity(std::size_t slot) {
    if (slot >= kMatAccessUnitsPerFrame) {
        return 0;
    }
    return slot_limit(slot) - slot_start(slot);
}

std::string_view describe(MatError error) {
    switch (error) {
        case MatError::kNotAnAccessUnit:
            return "not a TrueHD access unit: fewer than four bytes, an odd length, or a length "
                   "field that "
                   "disagrees with the bytes";
        case MatError::kAccessUnitTooLarge:
            return "access unit larger than a MAT slot holds";
        case MatError::kNoSync:
            return "no IEC 61937 preamble where a MAT burst has to start";
        case MatError::kNotMat:
            return "a burst whose data type is not 22";
        case MatError::kTruncatedBurst:
            return "the carrier ends part-way through a MAT burst";
        case MatError::kBadCode:
            return "a MAT start, middle or end code that is not the expected one";
        case MatError::kBadSlot:
            return "an access unit that overruns its MAT slot";
    }
    return "unknown error";
}

std::expected<std::optional<std::vector<std::byte>>, MatError> MatPacker::push(
    std::span<const std::byte> access_unit) {
    if (access_unit.size() < kMinAccessUnitBytes || (access_unit.size() & 1U) != 0 ||
        length_of(access_unit) != access_unit.size()) {
        return std::unexpected(MatError::kNotAnAccessUnit);
    }
    if (access_unit.size() > mat_slot_capacity(held_)) {
        return std::unexpected(MatError::kAccessUnitTooLarge);
    }
    std::copy(access_unit.begin(), access_unit.end(),
              payload_.begin() + static_cast<std::ptrdiff_t>(slot_start(held_)));
    ++held_;
    if (held_ < kMatAccessUnitsPerFrame) {
        return std::optional<std::vector<std::byte>>{};
    }
    std::vector<std::byte> burst = finish_burst(payload_);
    std::fill(payload_.begin(), payload_.end(), std::byte{0});
    held_ = 0;
    return std::optional<std::vector<std::byte>>{std::move(burst)};
}

std::optional<std::vector<std::byte>> MatPacker::flush() {
    if (held_ == 0) {
        return std::nullopt;
    }
    std::vector<std::byte> burst = finish_burst(payload_);
    std::fill(payload_.begin(), payload_.end(), std::byte{0});
    held_ = 0;
    return burst;
}

std::expected<std::vector<std::byte>, MatError> wrap_mat_stream(
    std::span<const std::span<const std::byte>> access_units) {
    MatPacker packer;
    std::vector<std::byte> carrier;
    for (const auto& unit : access_units) {
        auto pushed = packer.push(unit);
        if (!pushed) {
            return std::unexpected(pushed.error());
        }
        const std::optional<std::vector<std::byte>>& burst = *pushed;
        if (burst.has_value()) {
            carrier.insert(carrier.end(), burst->begin(), burst->end());
        }
    }
    if (auto last = packer.flush()) {
        carrier.insert(carrier.end(), last->begin(), last->end());
    }
    return carrier;
}

std::expected<std::vector<std::vector<std::byte>>, MatError> unwrap_mat_stream(
    std::span<const std::byte> carrier) {
    std::vector<std::vector<std::byte>> out;
    if (carrier.empty()) {
        return out;
    }
    // The first burst's Pa and Pb give the word order: F872 4E1F as bytes F8 72 4E 1F is
    // big-endian, as 72 F8 1F 4E little-endian (the order FFmpeg's muxer writes).
    if (carrier.size() < 4) {
        return std::unexpected(MatError::kNoSync);
    }
    const auto head = [&](std::size_t i) { return u8(carrier[i]); };
    bool little_endian = false;
    if (head(0) == 0x72 && head(1) == 0xF8 && head(2) == 0x1F && head(3) == 0x4E) {
        little_endian = true;
    } else if (!(head(0) == 0xF8 && head(1) == 0x72 && head(2) == 0x4E && head(3) == 0x1F)) {
        return std::unexpected(MatError::kNoSync);
    }

    std::vector<std::byte> burst(kMatBurstBytes);
    for (std::size_t pos = 0; pos < carrier.size(); pos += kMatBurstBytes) {
        if (carrier.size() - pos < kMatBurstBytes) {
            return std::unexpected(MatError::kTruncatedBurst);
        }
        std::copy_n(carrier.begin() + static_cast<std::ptrdiff_t>(pos), kMatBurstBytes,
                    burst.begin());
        if (little_endian) {
            swap_words(burst);
        }
        if (u8(burst[0]) != 0xF8 || u8(burst[1]) != 0x72 || u8(burst[2]) != 0x4E ||
            u8(burst[3]) != 0x1F) {
            return std::unexpected(MatError::kNoSync);
        }
        const std::uint16_t pc = static_cast<std::uint16_t>((u8(burst[4]) << 8U) | u8(burst[5]));
        if ((pc & 0x1FU) != (kMatPc & 0x1FU)) {
            return std::unexpected(MatError::kNotMat);
        }
        const std::span<const std::byte> payload{burst.data() + kMatPreambleBytes,
                                                 kMatPayloadBytes};
        if (!matches(payload, 0, kStartCode) ||
            !matches(payload, kMatMiddleCodeOffset, kMiddleCode) ||
            !matches(payload, kMatEndCodeOffset, kEndCode)) {
            return std::unexpected(MatError::kBadCode);
        }
        for (std::size_t slot = 0; slot < kMatAccessUnitsPerFrame; ++slot) {
            const std::size_t start = slot_start(slot);
            const std::size_t length = length_of(payload.subspan(start, 2));
            if (length == 0) {
                continue;  // an empty slot
            }
            if (length < kMinAccessUnitBytes || length > slot_limit(slot) - start) {
                return std::unexpected(MatError::kBadSlot);
            }
            const auto first = payload.begin() + static_cast<std::ptrdiff_t>(start);
            out.emplace_back(first, first + static_cast<std::ptrdiff_t>(length));
        }
    }
    return out;
}

}  // namespace iclforge::containers::iec61937
