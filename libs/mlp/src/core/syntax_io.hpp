#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/mlp/core/error.hpp"

// The two directions a syntax description is run in. A description is a function template over
// an Io: it names each field once, in order, with its width; SyntaxReader fills the field from the
// bits and SyntaxWriter writes the field's value. A condition that depends on an earlier field
// reads that field's value, which in the writer is the value about to be written, so the two
// directions take the same branches by construction.
//
// A signed field is two's complement in its width. A value the writer cannot represent in its
// width is a configuration error, not a silent truncation: it returns kInvalidConfiguration.

namespace iclforge::mlp::detail {

class SyntaxReader {
   public:
    static constexpr bool kReading = true;

    explicit SyntaxReader(BitReader& reader) noexcept : reader_(reader) {}

    template <typename T>
    bool u(int bits, T& value, std::string_view name) {
        value = static_cast<T>(reader_.read(bits, name));
        return true;
    }

    template <typename T>
    bool s(int bits, T& value, std::string_view name) {
        const std::uint32_t raw = reader_.read(bits, name);
        std::int64_t v = raw;
        if (bits > 0 && ((raw >> (bits - 1)) & 1U) != 0) {
            v -= std::int64_t{1} << bits;
        }
        value = static_cast<T>(v);
        return true;
    }

    bool flag(bool& value, std::string_view name) {
        value = reader_.read_flag(name);
        return true;
    }

    // A presence bit for an optional element: reads the bit and, when it is set, gives the element
    // a value to be filled. Returns whether the element follows.
    template <typename T>
    bool present(std::optional<T>& element, std::string_view name) {
        const bool here = reader_.read_flag(name);
        if (here) {
            element.emplace();
        } else {
            element.reset();
        }
        return here;
    }

    [[nodiscard]] bool overflowed() const noexcept { return reader_.overflowed(); }
    [[nodiscard]] std::size_t position() const noexcept { return reader_.bit_position(); }
    [[nodiscard]] BitReader& bits() noexcept { return reader_; }

   private:
    BitReader& reader_;
};

class SyntaxWriter {
   public:
    static constexpr bool kReading = false;

    explicit SyntaxWriter(BitWriter& writer) noexcept : writer_(writer) {}

    template <typename T>
    bool u(int bits, const T& value, std::string_view name) {
        const auto v = static_cast<std::uint64_t>(value);
        if (bits < 64 && (v >> bits) != 0) {
            failed_ = true;
            return false;
        }
        writer_.write(static_cast<unsigned>(bits), v, name);
        return true;
    }

    template <typename T>
    bool s(int bits, const T& value, std::string_view name) {
        const auto v = static_cast<std::int64_t>(value);
        const std::int64_t lo = -(std::int64_t{1} << (bits - 1));
        const std::int64_t hi = (std::int64_t{1} << (bits - 1)) - 1;
        if (v < lo || v > hi) {
            failed_ = true;
            return false;
        }
        const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
        writer_.write(static_cast<unsigned>(bits), static_cast<std::uint64_t>(v) & mask, name);
        return true;
    }

    bool flag(const bool& value, std::string_view name) {
        writer_.write(1, value ? 1U : 0U, name);
        return true;
    }

    template <typename T>
    bool present(const std::optional<T>& element, std::string_view name) {
        writer_.write(1, element.has_value() ? 1U : 0U, name);
        return element.has_value();
    }

    [[nodiscard]] bool overflowed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t position() const noexcept { return writer_.bit_count(); }
    [[nodiscard]] BitWriter& bits() noexcept { return writer_; }

   private:
    BitWriter& writer_;
    bool failed_ = false;
};

// The value type a description sees for a field: a reference in the reader, a const reference in
// the writer.
template <typename Io, typename T>
using Field = std::conditional_t<Io::kReading, T&, const T&>;

// The failure of a description run: truncation in the reader, an unrepresentable value in the
// writer, else `error`.
template <typename Io>
[[nodiscard]] std::unexpected<Error> fail(const Io& io, Error error) {
    if (io.overflowed()) {
        return std::unexpected(Io::kReading ? Error::kTruncated : Error::kInvalidConfiguration);
    }
    return std::unexpected(error);
}

}  // namespace iclforge::mlp::detail
