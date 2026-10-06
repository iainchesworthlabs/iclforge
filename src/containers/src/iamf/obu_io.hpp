#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/iamf/sequence.hpp"

// Byte and bit level reading and writing for the OBU syntax (AOM IAMF v2.0.0, "Convention"):
// leb128() values, null-terminated UTF-8 strings, and the MSB-first packed fields the position
// parameters use. Internal to src/containers/src/iamf.

namespace iclforge::iamf::detail {

// Appends fields to a byte vector. bits() packs MSB first; every byte-oriented call requires the
// bit position to be byte aligned, which the syntax guarantees at those points.
class Out {
public:
    void u8(std::uint32_t value) { bytes_.push_back(static_cast<std::byte>(value & 0xFFU)); }
    void u16(std::uint32_t value) {
        u8(value >> 8);
        u8(value);
    }
    void s16(std::int32_t value) { u16(static_cast<std::uint32_t>(value) & 0xFFFFU); }
    void u32(std::uint32_t value) {
        u16(value >> 16);
        u16(value & 0xFFFFU);
    }
    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32));
        u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    }
    void fourcc(std::string_view code) {
        for (std::size_t i = 0; i < 4; ++i) {
            u8(i < code.size() ? static_cast<std::uint8_t>(code[i]) : 0U);
        }
    }
    // leb128(): 7 bits per byte, least significant group first, continuation flag in bit 7.
    void leb128(std::uint32_t value) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 0x7FU);
            value >>= 7;
            if (value != 0) {
                byte |= 0x80U;
            }
            u8(byte);
        } while (value != 0);
    }
    // string: the bytes then a terminating 0x00.
    void string(std::string_view text) {
        for (const char c : text) {
            u8(static_cast<std::uint8_t>(c));
        }
        u8(0);
    }
    void bytes(std::span<const std::byte> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }
    // The low `count` bits of `value`, MSB first. The syntax keeps a run of bit fields a whole
    // number of bytes long, so every other call here starts on a byte boundary.
    void bits(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (bit_fill_ == 0) {
                bytes_.push_back(std::byte{0});
            }
            if (((value >> (count - 1 - i)) & 1U) != 0) {
                bytes_.back() |= static_cast<std::byte>(0x80U >> bit_fill_);
            }
            bit_fill_ = (bit_fill_ + 1) % 8;
        }
    }
    [[nodiscard]] const std::vector<std::byte>& data() const { return bytes_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }

private:
    std::vector<std::byte> bytes_;
    unsigned bit_fill_ = 0;  // bits used in the last byte; 0 when it is complete
};

// The number of bytes leb128(value) occupies.
[[nodiscard]] inline std::size_t leb128_size(std::uint32_t value) {
    std::size_t n = 1;
    while (value >= 0x80U) {
        value >>= 7;
        ++n;
    }
    return n;
}

// Reads fields from a span, failing with kTruncated rather than reading past the end.
class Cursor {
public:
    explicit Cursor(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }
    [[nodiscard]] std::size_t position() const { return pos_; }
    [[nodiscard]] bool at_end() const { return pos_ == data_.size(); }
    [[nodiscard]] std::span<const std::byte> rest() const { return data_.subspan(pos_); }

    [[nodiscard]] std::expected<std::uint8_t, Error> u8() {
        if (remaining() < 1) {
            return std::unexpected(Error::kTruncated);
        }
        return std::to_integer<std::uint8_t>(data_[pos_++]);
    }
    [[nodiscard]] std::expected<std::uint16_t, Error> u16() {
        if (remaining() < 2) {
            return std::unexpected(Error::kTruncated);
        }
        const auto v = static_cast<std::uint16_t>((std::to_integer<unsigned>(data_[pos_]) << 8) |
                                                  std::to_integer<unsigned>(data_[pos_ + 1]));
        pos_ += 2;
        return v;
    }
    [[nodiscard]] std::expected<std::int16_t, Error> s16() {
        auto v = u16();
        if (!v.has_value()) {
            return std::unexpected(v.error());
        }
        return static_cast<std::int16_t>(*v);
    }
    [[nodiscard]] std::expected<std::uint32_t, Error> u32() {
        if (remaining() < 4) {
            return std::unexpected(Error::kTruncated);
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v = (v << 8) | std::to_integer<std::uint32_t>(data_[pos_++]);
        }
        return v;
    }
    [[nodiscard]] std::expected<std::uint32_t, Error> leb128() {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            auto byte = u8();
            if (!byte.has_value()) {
                return std::unexpected(byte.error());
            }
            value |= static_cast<std::uint64_t>(*byte & 0x7FU) << (7 * i);
            if ((*byte & 0x80U) == 0) {
                if (value > 0xFFFFFFFFULL) {
                    return std::unexpected(Error::kBadLeb128);
                }
                return static_cast<std::uint32_t>(value);
            }
        }
        return std::unexpected(Error::kBadLeb128);
    }
    [[nodiscard]] std::expected<std::span<const std::byte>, Error> bytes(std::size_t count) {
        if (remaining() < count) {
            return std::unexpected(Error::kTruncated);
        }
        auto span = data_.subspan(pos_, count);
        pos_ += count;
        return span;
    }
    [[nodiscard]] std::expected<std::string, Error> string() {
        std::string out;
        while (true) {
            auto byte = u8();
            if (!byte.has_value()) {
                return std::unexpected(byte.error());
            }
            if (*byte == 0) {
                return out;
            }
            out.push_back(static_cast<char>(*byte));
        }
    }
    // MSB first. After a bits() run the caller must align() before reading whole bytes.
    [[nodiscard]] std::expected<std::uint64_t, Error> bits(unsigned count) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (bit_pos_ == 0) {
                if (remaining() < 1) {
                    return std::unexpected(Error::kTruncated);
                }
                current_ = std::to_integer<std::uint8_t>(data_[pos_]);
            }
            value = (value << 1) | ((current_ >> (7 - bit_pos_)) & 1U);
            if (++bit_pos_ == 8) {
                bit_pos_ = 0;
                ++pos_;
            }
        }
        return value;
    }
    // Discards the rest of the current byte after a bits() run.
    void align() {
        if (bit_pos_ != 0) {
            bit_pos_ = 0;
            ++pos_;
        }
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    unsigned bit_pos_ = 0;
    std::uint8_t current_ = 0;
};

// Sign-extends the low `width` bits of `value`.
[[nodiscard]] inline std::int32_t sign_extend(std::uint64_t value, unsigned width) {
    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    return static_cast<std::int32_t>(static_cast<std::int64_t>((value ^ sign) - sign));
}

}  // namespace iclforge::iamf::detail
