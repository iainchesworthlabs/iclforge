#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/syntax_trace.hpp"

namespace iclforge::base::detail {

// variable_bits(n_bits)'s groups for a value (BitReader::variable_bits). A decoder reads a group,
// and while the continuation bit that follows is set, shifts the value left by n_bits and adds
// 1 << n_bits before reading the next group. So the last group is the value's low n_bits, and each
// group before it is what is left once that one is removed, less one. At most 64 bits of value, so
// at most 64 groups.
struct VariableBits {
    std::array<std::uint64_t, 64> values{};
    std::size_t count = 0;  // values[count - 1] is written first
};

[[nodiscard]] inline VariableBits split_variable_bits(unsigned n_bits,
                                                      std::uint64_t value) noexcept {
    VariableBits groups;
    const std::uint64_t mask = (std::uint64_t{1} << n_bits) - 1;
    groups.values[groups.count++] = value & mask;
    std::uint64_t rest = value >> n_bits;
    while (rest > 0 && groups.count < groups.values.size()) {
        rest -= 1;
        groups.values[groups.count++] = rest & mask;
        rest >>= n_bits;
    }
    return groups;
}

}  // namespace iclforge::base::detail

namespace iclforge {

// The number of bits variable_bits(n_bits) takes for `value`.
[[nodiscard]] inline unsigned variable_bits_width(unsigned n_bits, std::uint64_t value) noexcept {
    return static_cast<unsigned>(base::detail::split_variable_bits(n_bits, value).count) *
           (n_bits + 1);
}

// The MSB-first bit writer every bitstream in the tree is written through, the mirror of
// BitReader: fields are packed most significant bit first, in syntax order (A/52 §5.1, TS 103 190-1
// clause 3.4, ST 2098-2 §5.1).
//
// A write given a name is a syntax element: with a base::SyntaxSink attached, the writer records
// it as a base::SyntaxRecord, in the shape a reader records what it reads, its offset from the
// start of this writer's bits; put(), align() and put_bytes() record nothing.
class BitWriter {
   public:
    BitWriter() noexcept = default;
    // `substream` is the index the records carry.
    explicit BitWriter(int substream, base::SyntaxSink sink = {}) noexcept
        : substream_(substream), sink_(sink) {}

    // A writer that keeps its records instead of sending them, for bits whose place in the unit
    // is not known yet; append() sends them on.
    [[nodiscard]] static BitWriter buffered() {
        BitWriter writer;
        writer.buffering_ = true;
        return writer;
    }

    // Append the low `bits` bits of `value`, MSB first, unrecorded. 0 <= bits <= 64.
    //
    // The bits collect in a 64-bit accumulator and leave it a whole byte at a time, as AC-3's
    // writer packed a frame: a field is a shift, an or and a byte or two pushed. A byte not yet
    // whole is put in bytes() only when they are read.
    void put(std::uint64_t value, int bits) {
        assert(bits >= 0 && bits <= 64);
        assert(bits == 64 || (value >> bits) == 0);
        if (bits > 32) {
            put(value >> 32, bits - 32);
            put(value & 0xFFFFFFFFU, 32);
            return;
        }
        if (padded_) {
            bytes_.pop_back();
            padded_ = false;
        }
        acc_ = (acc_ << bits) | value;
        pending_ += bits;
        while (pending_ >= 8) {
            pending_ -= 8;
            bytes_.push_back(static_cast<std::byte>((acc_ >> pending_) & 0xFF));
        }
        bits_ += static_cast<std::size_t>(bits);
    }

    void put_bit(bool bit) { put(bit ? 1U : 0U, 1); }

    // A fixed-width syntax element of 0 to 64 bits. A zero-width element is not recorded.
    void write(unsigned bits, std::uint64_t value, std::string_view name) {
        if (bits == 0) {
            return;
        }
        const auto offset = bits_;
        put(value, static_cast<int>(bits));
        record(offset, bits, value, name);
    }

    // variable_bits(n_bits) (BitReader::variable_bits): `value` as groups of n_bits, the most
    // significant first, each followed by a continuation bit. One record, with the total width and
    // the value. With `max_groups` above 0, TS 103 420 §5.5.1's variable_bits_max: at most that
    // many groups, the last of them taking what is left of the value modulo its width.
    void write_variable_bits(unsigned n_bits, std::uint64_t value, std::string_view name = {},
                             int max_groups = 0) {
        const auto offset = bits_;
        if (max_groups > 0) {
            int groups = 1;
            std::uint64_t base = 0;
            while (groups < max_groups) {
                const std::uint64_t capacity = std::uint64_t{1} << (static_cast<unsigned>(groups) * n_bits);
                if (value < base + capacity) {
                    break;
                }
                base += capacity;
                ++groups;
            }
            const std::uint64_t encoded = value - base;
            const std::uint64_t mask = (std::uint64_t{1} << n_bits) - 1;
            for (int group = groups - 1; group >= 0; --group) {
                put((encoded >> (static_cast<unsigned>(group) * n_bits)) & mask, static_cast<int>(n_bits));
                put(group == 0 ? 0U : 1U, 1);
            }
        } else {
            const auto groups = base::detail::split_variable_bits(n_bits, value);
            for (std::size_t i = groups.count; i > 0; --i) {
                put(groups.values[i - 1], static_cast<int>(n_bits));
                put(i > 1 ? 1U : 0U, 1);
            }
        }
        record(offset, static_cast<unsigned>(bits_ - offset), value, name);
    }

    // A run of zero bits the syntax does not interpret, recorded as a reader records such a run:
    // one record per 65 535 bits, the last shorter, each valued 0.
    void write_zero_run(std::uint64_t bits, std::string_view name) {
        constexpr std::uint64_t kMaxRecordBits = 65535;
        while (bits > 0) {
            const std::uint64_t width = bits < kMaxRecordBits ? bits : kMaxRecordBits;
            const auto offset = bits_;
            for (std::uint64_t left = width; left > 0;) {
                const int step = left < 64 ? static_cast<int>(left) : 64;
                put(0, step);
                left -= static_cast<std::uint64_t>(step);
            }
            record(offset, static_cast<unsigned>(width), 0, name);
            bits -= width;
        }
    }

    // An element whose bits are not its value, such as an escape code or a Huffman codeword:
    // `raw` in `bits` bits, recorded with `value`.
    void write_as(unsigned bits, std::uint64_t raw, std::uint64_t value, std::string_view name) {
        const auto offset = bits_;
        put(raw, static_cast<int>(bits));
        record(offset, bits, value, name);
    }

    // Zero bits to the next byte boundary of this writer's bits.
    void align() { put(0, static_cast<int>((8U - (bits_ & 7U)) & 7U)); }

    // Aligns, then appends whole bytes.
    void put_bytes(std::span<const std::byte> bytes) {
        align();
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        bits_ += bytes.size() * 8U;
    }

    // Copies another writer's bits after this one's, and sends the records it kept, their offsets
    // moved by where its bits land here.
    void append(const BitWriter& other) {
        const auto start = bits_;
        const std::vector<std::byte>& theirs = other.bytes();
        const std::size_t whole = other.bits_ / 8U;
        for (std::size_t i = 0; i < whole; ++i) {
            put(std::to_integer<unsigned>(theirs[i]), 8);
        }
        if (const auto rest = static_cast<int>(other.bits_ & 7U); rest > 0) {
            put(std::to_integer<unsigned>(theirs[whole]) >> (8 - rest), rest);
        }
        for (base::SyntaxRecord kept : other.kept_) {
            kept.substream = substream_;
            kept.bit_offset += static_cast<std::uint32_t>(start);
            send(kept);
        }
    }

    // Pre-size the output buffer, for a writer that knows its unit's byte count before it starts.
    void reserve(std::size_t bytes) { bytes_.reserve(bytes); }

    // Overwrite a 16-bit big-endian field at a byte offset in what is written already, as a frame's
    // CRC words are once its body is packed.
    void patch_u16(std::size_t byte_offset, std::uint16_t value) {
        assert(byte_offset + 2 <= bytes_.size());
        bytes_[byte_offset] = static_cast<std::byte>(value >> 8);
        bytes_[byte_offset + 1] = static_cast<std::byte>(value & 0xFF);
    }

    [[nodiscard]] std::size_t bit_count() const noexcept { return bits_; }
    [[nodiscard]] std::size_t byte_size() const noexcept { return (bits_ + 7) / 8; }

    // The bytes written so far, the last one zero-padded.
    [[nodiscard]] const std::vector<std::byte>& bytes() const {
        if (pending_ > 0 && !padded_) {
            bytes_.push_back(static_cast<std::byte>((acc_ << (8 - pending_)) & 0xFF));
            padded_ = true;
        }
        return bytes_;
    }

    // Zero-pad to a byte boundary and take the buffer, leaving the writer empty.
    [[nodiscard]] std::vector<std::byte> take() {
        align();
        bits_ = 0;
        acc_ = 0;
        return std::exchange(bytes_, {});
    }

    // Starts over, keeping the substream index and the sink.
    void clear() noexcept {
        bytes_.clear();
        kept_.clear();
        bits_ = 0;
        acc_ = 0;
        pending_ = 0;
        padded_ = false;
    }

    // The records a buffered writer kept.
    [[nodiscard]] std::span<const base::SyntaxRecord> kept() const noexcept { return kept_; }

   private:
    void record(std::size_t offset, unsigned bits, std::uint64_t value, std::string_view name) {
        if (buffering_ || sink_) {
            send(base::SyntaxRecord{substream_, static_cast<std::uint32_t>(offset),
                                    static_cast<std::uint16_t>(bits), value, name});
        }
    }

    void send(const base::SyntaxRecord& record) {
        if (buffering_) {
            kept_.push_back(record);
        } else if (sink_) {
            sink_(record);
        }
    }

    int substream_ = 0;
    base::SyntaxSink sink_{};
    bool buffering_ = false;
    std::vector<base::SyntaxRecord> kept_;
    // The whole bytes, and the last one zero-padded while padded_ (bytes() adds it, put() takes
    // it back): mutable because reading the bytes is what pads them.
    mutable std::vector<std::byte> bytes_;
    mutable bool padded_ = false;
    std::uint64_t acc_ = 0;  // the bits above pending_ are stale and ignored
    int pending_ = 0;        // bits in acc_ not yet in a whole byte, 0 to 7
    std::size_t bits_ = 0;
};

}  // namespace iclforge
