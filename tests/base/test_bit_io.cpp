#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"

// planning/consolidation.md's C4 made one bit reader and one writer of the six the tree had:
// base's, which AC-3, EMDF and OAMD read and write through, AC-4's decoder's and encoder's, which
// record a syntax trace, and the AC-4 inspector's. Below are those as they were before C4, verbatim
// but for their namespaces, and the random sequences of operations that hold the one reader and the
// one writer to each: the same values, positions, overflow flags, bytes and records.

using iclforge::base::SyntaxRecord;
using iclforge::base::SyntaxSink;

// --- base's reader and writer before C4 ---------------------------------------------------------


namespace old_base {

// MSB-first bit reader, the mirror of BitWriter (A/52 §5.1). Reading past
// the end sets a sticky overflow flag and yields zeros — callers check
// overflowed() once at a suitable boundary instead of guarding every read.
//
// Reads come out of a 64-bit cache refilled a byte at a time, rather than
// one bit per loop iteration as this used to do. A decode reads every
// mantissa, exponent group and GAQ codeword through here — some fifty
// thousand fields a frame for a 5.1 stream — and the bit-at-a-time form
// cost about eight cycles per BIT of each: on an ESP32-S3 that was most of
// the mantissa stage of a decode that had nothing else left in it
// (docs/platforms/bare-metal/esp32-s3.md). The values, the position and the overflow
// behaviour are exactly what they were; only the number of instructions
// between them changed.
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t read(int bits) {
        assert(bits >= 0 && bits <= 32);
        if (bits == 0) {
            return 0;
        }
        if (cached_bits_ < bits) {
            refill(bits);
        }
        // The cache holds cached_bits_ valid bits in its low bits, the
        // oldest highest; the field is its top `bits` of them. refill()
        // guarantees cached_bits_ is in [bits, 64] here, so shift is in
        // [0, 63] - spelled out for the analyzer, which otherwise explores
        // an infeasible cached_bits_ >= 64 + bits == 0 path and flags the
        // shift below as unbounded.
        const int shift = cached_bits_ - bits;
        assert(shift >= 0 && shift < 64);
        const auto value = static_cast<std::uint32_t>((cache_ >> shift) & mask(bits));
        cached_bits_ = shift;
        cache_ &= (std::uint64_t{1} << shift) - 1;
        position_ += static_cast<std::size_t>(bits);
        return value;
    }

    [[nodiscard]] std::uint32_t read_bit() { return read(1); }

    void skip(std::size_t bits) {
        position_ += bits;
        if (position_ > data_.size() * 8) {
            overflowed_ = true;
        }
        // The cache no longer describes the position; drop it and let the
        // next read rebuild it from the byte the position now lands in.
        cache_ = 0;
        cached_bits_ = 0;
        next_byte_ = position_ >> 3;
        pending_skip_ = static_cast<int>(position_ & 7);
    }

    [[nodiscard]] std::size_t bit_position() const { return position_; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }

private:
    [[nodiscard]] static constexpr std::uint64_t mask(int bits) {
        return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    }

    // Make at least `need` bits available. Whole bytes are appended while
    // there is room for them and data to take them from; when the data runs
    // out the cache is padded with zero bits instead, and the read that
    // needed them is the one that has overflowed - the same zeros, and the
    // same flag, that the bit-at-a-time reader produced past the end.
    void refill(int need) {
        while (cached_bits_ + 8 <= 64 && next_byte_ < data_.size()) {
            cache_ = (cache_ << 8) | std::to_integer<std::uint64_t>(data_[next_byte_]);
            cached_bits_ += 8;
            ++next_byte_;
            if (pending_skip_ != 0) {
                // A skip() left the position mid-byte: the bits of this byte
                // before it were consumed already.
                cached_bits_ -= pending_skip_;
                cache_ &= mask(cached_bits_);
                pending_skip_ = 0;
            }
        }
        if (cached_bits_ < need) {
            const int pad = need - cached_bits_;
            cache_ <<= pad;
            cached_bits_ += pad;
            overflowed_ = true;
        }
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    std::size_t next_byte_ = 0;  // the first byte not yet in the cache
    std::uint64_t cache_ = 0;
    int cached_bits_ = 0;
    int pending_skip_ = 0;  // bits of the next cached byte already consumed by skip()
    bool overflowed_ = false;
};

}  // namespace old_base


namespace old_base {

// MSB-first bit packer for AC-3 syntax elements. A/52 §5.1: fields are packed
// into the bit stream most-significant-bit first, in syntax order.
class BitWriter {
public:
    // Append the low `bits` bits of `value`, MSB first. 0 <= bits <= 32.
    void put(std::uint32_t value, int bits) {
        assert(bits >= 0 && bits <= 32);
        assert(bits == 32 || value < (std::uint64_t{1} << bits));
        acc_ = (acc_ << bits) | value;
        pending_ += bits;
        while (pending_ >= 8) {
            pending_ -= 8;
            bytes_.push_back(static_cast<std::byte>((acc_ >> pending_) & 0xFF));
        }
    }

    void put_bit(bool bit) { put(bit ? 1u : 0u, 1); }

    // Pre-size the output buffer. Every CBR pack site knows its frame's
    // exact byte count before emitting a single field, so put()'s
    // one-byte-at-a-time growth (~11 geometric reallocations for a full
    // syncframe) is pure waste there.
    void reserve(std::size_t bytes) { bytes_.reserve(bytes); }

    // Zero-pad to the next byte boundary. Syncframes are an integral number of
    // 16-bit words, so a fully packed frame always ends byte-aligned; this is
    // for tests and partial assemblies.
    void byte_align() {
        if (pending_ != 0) {
            put(0, 8 - pending_);
        }
    }

    [[nodiscard]] std::size_t bit_count() const {
        return bytes_.size() * 8 + static_cast<std::size_t>(pending_);
    }

    // Overwrite a 16-bit big-endian field at a byte offset in already-emitted
    // output. Used to patch the two CRC words after the frame body is packed
    // (crc1 sits at byte offset 2, immediately after the sync word).
    void patch_u16(std::size_t byte_offset, std::uint16_t value) {
        assert(byte_offset + 2 <= bytes_.size());
        bytes_[byte_offset] = static_cast<std::byte>(value >> 8);
        bytes_[byte_offset + 1] = static_cast<std::byte>(value & 0xFF);
    }

    // View of the fully emitted bytes; only valid when byte-aligned.
    [[nodiscard]] std::span<const std::byte> bytes() const {
        assert(pending_ == 0);
        return bytes_;
    }

    // Zero-pad to a byte boundary and take the buffer, leaving the writer empty.
    [[nodiscard]] std::vector<std::byte> take() {
        byte_align();
        return std::exchange(bytes_, {});
    }

private:
    std::vector<std::byte> bytes_;
    std::uint64_t acc_ = 0;  // bits above `pending_` are stale and ignored
    int pending_ = 0;
};

}  // namespace old_base
namespace old_base {
namespace {
std::uint32_t read_variable_bits(old_base::BitReader& r, int group_bits) {
    std::uint32_t value = 0;
    while (true) {
        value += r.read(static_cast<int>(group_bits));
        if (r.read_bit() == 0) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

[[nodiscard]] std::uint32_t read_variable_bits_max(old_base::BitReader& r, int group_bits,
                                                   int max_groups) {
    std::uint32_t value = 0;
    for (int group = 1;; ++group) {
        value += r.read(group_bits);
        const bool read_more = r.read_bit() != 0;
        if (!read_more || group >= max_groups) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

struct VarBitsShape {
    int groups = 1;
    std::uint64_t offset = 0;
};

[[nodiscard]] VarBitsShape variable_bits_shape(std::uint32_t value, int group_bits) {
    assert(group_bits > 0 && group_bits <= 11);
    VarBitsShape shape;
    while (true) {
        const std::uint64_t capacity = std::uint64_t{1} << (shape.groups * group_bits);
        if (value < shape.offset + capacity) {
            return shape;
        }
        shape.offset += capacity;
        ++shape.groups;
    }
}


void put_variable_bits(old_base::BitWriter& w, std::uint32_t value, int group_bits) {
    const auto [groups, offset] = variable_bits_shape(value, group_bits);
    const auto encoded = static_cast<std::uint64_t>(value) - offset;
    for (int group = groups - 1; group >= 0; --group) {
        const auto shift = group * group_bits;
        w.put(static_cast<std::uint32_t>((encoded >> shift) &
                                         ((std::uint64_t{1} << group_bits) - 1)),
              group_bits);
        w.put(group == 0 ? 0u : 1u, 1);  // read_more
    }
}

void put_variable_bits_max(old_base::BitWriter& w, std::uint32_t value, int group_bits,
                           int max_groups) {
    int groups = 1;
    std::uint64_t offset = 0;
    while (groups < max_groups) {
        const std::uint64_t capacity = std::uint64_t{1} << (groups * group_bits);
        if (value < offset + capacity) {
            break;
        }
        offset += capacity;
        ++groups;
    }
    const auto encoded = static_cast<std::uint64_t>(value) - offset;
    for (int group = groups - 1; group >= 0; --group) {
        w.put(static_cast<std::uint32_t>((encoded >> (group * group_bits)) &
                                         ((std::uint64_t{1} << group_bits) - 1)),
              group_bits);
        w.put(group == 0 ? 0u : 1u, 1);  // read_more
    }
}

}  // namespace
}  // namespace old_base

// --- AC-4's reader and writer before C4 ---------------------------------------------------------



// The bit reader every syntax function reads through. MSB first, as both
// parts read (Part 1 clause 3.4). Reading past the end does not throw and
// does not read out of bounds: it returns zeros and sets a sticky flag, which
// each syntax function checks at the points where carrying on would loop on
// a count read from nothing.
//
// The bits are read through a cache of 8 bytes, as a 64-bit word: a peek of up
// to 32 bits is a shift of it, and the word is loaded again only when the
// position has moved 4 bytes on. The bytes past the end of the data are zeros in
// it, which is what a read past the end returns.
//
// Every read that corresponds to a syntax element names that element, and
// when a SyntaxSink is attached the reader emits one SyntaxRecord for it (see
// SyntaxRecord in ac4/syntax.hpp for what counts as one element). Reads
// that are not syntax elements - byte_align, fill bits, skipped bytes - go
// through skip() and align(), which emit nothing.

namespace old_ac4 {

class BitReader {
   public:
    BitReader(std::span<const std::byte> data, int substream, SyntaxSink sink) noexcept
        : data_(data), substream_(substream), sink_(sink) {}

    // A fixed-width syntax element of up to 32 bits.
    std::uint32_t read(int bits, std::string_view name) noexcept {
        const std::size_t start = pos_;
        const std::uint32_t value = peek_raw(bits);
        advance(bits);
        emit(start, bits, value, name);
        return value;
    }

    bool read_flag(std::string_view name) noexcept { return read(1, name) != 0; }

    // Part 1 clause 4.2.2's variable_bits(n_bits), recorded as one element.
    // The text sets no limit on how many groups follow one another, so none
    // is set here: the loop ends at a clear b_read_more or at the end of the
    // substream. A value too large for 64 bits is recorded, and returned,
    // modulo 2^64. The caller gets all 64 bits: the sizes built from this are
    // compared against the substream rather than looped over, so handing back
    // the low 32 would let a value of 2^32 or more pass a check its true size
    // fails.
    std::uint64_t variable_bits(int n_bits, std::string_view name) noexcept {
        const std::size_t start = pos_;
        std::uint64_t value = 0;
        while (true) {
            value += peek_raw(n_bits);
            advance(n_bits);
            const bool more = peek_raw(1) != 0;
            advance(1);
            if (!more || overflow_) {
                break;
            }
            value <<= n_bits;
            value += std::uint64_t{1} << n_bits;
        }
        emit_element(start, pos_, value, name);
        return value;
    }

    // A field whose width the stream sets and whose bits the syntax does not
    // interpret (add_data, extensions_bits, drc2_bits): one record of the
    // whole width, valued at its last 64 bits. A record's width is 16 bits,
    // so a run longer than 65535 bits is recorded as consecutive 65535-bit
    // records, the last shorter.
    void read_run(std::uint64_t bits, std::string_view name) noexcept {
        while (bits > 0) {
            const std::uint64_t width = bits < kMaxRecordBits ? bits : kMaxRecordBits;
            const std::size_t start = pos_;
            std::uint64_t value = 0;
            for (std::uint64_t i = 0; i < width; ++i) {
                value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
            }
            advance_bits(static_cast<std::size_t>(width));
            emit(start, static_cast<int>(width), value, name);
            bits -= width;
        }
    }

    // Unrecorded bits: alignment, fill, reserved payload skipped over.
    void skip(std::size_t bits) noexcept { advance_bits(bits); }

    void align() noexcept { advance_bits((8 - (pos_ % 8)) % 8); }

    // The peeks and the recorded-read helpers below exist for the Huffman and
    // escape decoders, which decide a width before they know it.
    [[nodiscard]] std::uint32_t peek_raw(int bits) const noexcept {
        if (bits <= 0) {
            return 0;
        }
        if (bits > 32) {
            return peek_slow(bits);
        }
        return static_cast<std::uint32_t>(window() >> static_cast<unsigned>(64 - bits));
    }

    void consume(int bits) noexcept { advance(bits); }

    // A zero-width element (a sign-bit group with no nonzero lines) is not a
    // record: it occupies no bits, and the Python transcription skips it too.
    // Nor is an element that runs past the end of the substream: the reader
    // hands back zeros for it and the syntax function fails at its next
    // check, so the trace ends with the last element the substream holds.
    void emit(std::size_t start, int bits, std::uint64_t value, std::string_view name) const {
        if (sink_ && bits > 0 && start + static_cast<std::size_t>(bits) <= size_bits()) {
            sink_(SyntaxRecord{substream_, static_cast<std::uint32_t>(start),
                               static_cast<std::uint16_t>(bits), value, name});
        }
    }

    // One element, whatever its width. A record's width is 16 bits, so an
    // element wider than 65535 bits is recorded as consecutive 65535-bit
    // records, the last shorter, each valued at its own last 64 bits - the
    // shape read_run() uses, which the Python transcription follows too. Only
    // variable_bits() can reach that width, and a value of 65536 bits of
    // continuation groups carries nothing worth recording as a number.
    void emit_element(std::size_t start, std::size_t end, std::uint64_t value,
                      std::string_view name) const {
        const std::size_t total = end - start;
        if (total <= kMaxRecordBits) {
            emit(start, static_cast<int>(total), value, name);
            return;
        }
        for (std::size_t at = start; at < end;) {
            const std::size_t width = std::min<std::size_t>(kMaxRecordBits, end - at);
            std::uint64_t chunk = 0;
            for (std::size_t i = 0; i < width; ++i) {
                chunk = (chunk << 1U) | bit_at(at + i);
            }
            emit(at, static_cast<int>(width), chunk, name);
            at += width;
        }
    }

    // The bit at an absolute position, zero past the end of the data. For a decoder that reads
    // ahead of what its syntax consumes (the speech spectral frontend's arithmetic decoder,
    // which reads 30 bits before it knows how many it needs): reading does not move the reader.
    [[nodiscard]] std::uint32_t bit(std::size_t index) const noexcept { return bit_at(index); }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size_bits() const noexcept { return data_.size() * 8U; }
    [[nodiscard]] std::size_t remaining_bits() const noexcept {
        return pos_ >= size_bits() ? 0 : size_bits() - pos_;
    }
    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] int substream() const noexcept { return substream_; }
    [[nodiscard]] SyntaxSink sink() const noexcept { return sink_; }

    // Moves to an absolute bit position inside the data, as when metadata()
    // is reached through audio_size rather than by reading audio_data().
    void seek(std::size_t bit_position) noexcept {
        if (bit_position > size_bits()) {
            overflow_ = true;
            pos_ = size_bits();
        } else {
            pos_ = bit_position;
        }
    }

   private:
    static constexpr std::uint64_t kMaxRecordBits = 65535;

    // The 64 bits from the current position on, the first in the top bit, zeros
    // past the end of the data. The cache holds the 8 bytes from cache_byte_ on; the
    // position is inside its first 4 bytes, so at least 33 of the bits are real.
    [[nodiscard]] std::uint64_t window() const noexcept {
        const std::size_t byte = pos_ >> 3U;
        if (byte < cache_byte_ || byte - cache_byte_ > 3U) {
            load(byte);
        }
        return cache_ << (pos_ - cache_byte_ * 8U);
    }

    void load(std::size_t byte) const noexcept {
        cache_byte_ = byte;
        std::uint64_t word = 0;
        if (byte + 8U <= data_.size()) {
            std::memcpy(&word, data_.data() + byte, sizeof word);
            if constexpr (std::endian::native == std::endian::little) {
                word = std::byteswap(word);
            }
        } else {
            for (std::size_t i = 0; i < 8U; ++i) {
                word = (word << 8U) |
                       (byte + i < data_.size() ? static_cast<std::uint64_t>(data_[byte + i]) : 0U);
            }
        }
        cache_ = word;
    }

    // A peek wider than the word holds, bit by bit: the low 32 bits of the value.
    [[nodiscard]] std::uint32_t peek_slow(int bits) const noexcept {
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
        }
        return value;
    }

    [[nodiscard]] std::uint32_t bit_at(std::size_t bit) const noexcept {
        if (bit >= size_bits()) {
            return 0;
        }
        const auto byte = static_cast<std::uint32_t>(data_[bit / 8U]);
        return (byte >> (7U - static_cast<unsigned>(bit % 8U))) & 1U;
    }

    void advance(int bits) noexcept { advance_bits(static_cast<std::size_t>(bits)); }

    void advance_bits(std::size_t bits) noexcept {
        pos_ += bits;
        if (pos_ > size_bits()) {
            overflow_ = true;
        }
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    int substream_ = 0;
    SyntaxSink sink_{};
    bool overflow_ = false;
    // The cache of window(): the 8 bytes from cache_byte_ on, as a big-endian word.
    // The data does not change, so it needs no invalidating; it is mutable because a
    // peek fills it.
    mutable std::uint64_t cache_ = 0;
    mutable std::size_t cache_byte_ = std::numeric_limits<std::size_t>::max();
};

}  // namespace old_ac4



// Writes one substream's (or the table of contents') bits, MSB first, and
// records each syntax element it writes as an iclforge::ac4::SyntaxRecord, in the shape
// the decoder's reader records what it reads (ac4/syntax.hpp): offsets from
// the start of this writer's bits, one record per element, none for
// byte_align or fill_bits.

namespace old_ac4 {

class BitWriter {
   public:
    // `substream` is the index the records carry; the table of contents'
    // writer takes a null sink, since no trace records the table of contents.
    explicit BitWriter(int substream = 0, SyntaxSink sink = {}) noexcept;

    // A writer that keeps its records instead of sending them, for bits whose
    // place in the substream is not known yet; append() sends them on.
    [[nodiscard]] static BitWriter buffered();

    // A fixed-width field. `bits` is 0 to 64; the value's bits above it must
    // be zero.
    void write(unsigned bits, std::uint64_t value, std::string_view name);

    // Part 1 clause 4.2.2's variable_bits(n_bits): `value` as groups of
    // n_bits, the most significant first, each followed by a continuation bit.
    // One record, with the total width and the value.
    void write_variable_bits(unsigned n_bits, std::uint64_t value, std::string_view name);


    // Bits nothing records: fill_bits and padding.
    void write_unrecorded(unsigned bits, std::uint64_t value);

    // A run of zero bits the syntax does not interpret (add_data, skip_data,
    // add_table_data), recorded as a decoder records such a run: one record
    // per 65 535 bits, the last shorter, each valued 0.
    void write_zero_run(std::uint64_t bits, std::string_view name);

    // An element whose bits are not its value, such as ext_code: `raw` in
    // `bits` bits, recorded with `value`.
    void write_as(unsigned bits, std::uint64_t raw, std::uint64_t value, std::string_view name);

    // byte_align: zero bits to the next byte boundary of this writer's bits.
    void align();

    // Copies another writer's bits after this one's, and sends the records it
    // kept, their offsets moved by where its bits land here.
    void append(const BitWriter& other);

    [[nodiscard]] std::size_t bit_position() const noexcept { return bits_; }
    [[nodiscard]] std::size_t byte_size() const noexcept { return (bits_ + 7) / 8; }

    // The bytes written so far, the last one zero-padded.
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

    // Starts over, keeping the substream index and the sink.
    void clear() noexcept;

    // The records a buffered writer kept.
    [[nodiscard]] std::span<const SyntaxRecord> kept() const noexcept { return kept_; }

   private:
    void put(unsigned bits, std::uint64_t value);

    void emit(const SyntaxRecord& record);

    int substream_ = 0;
    SyntaxSink sink_{};
    bool buffering_ = false;
    std::vector<SyntaxRecord> kept_;
    std::vector<std::byte> bytes_;
    std::size_t bits_ = 0;
};

// The number of bits variable_bits(n_bits) takes for `value`.
[[nodiscard]] unsigned variable_bits_width(unsigned n_bits, std::uint64_t value) noexcept;

}  // namespace old_ac4


namespace old_ac4 {
namespace {

// Part 1 clause 4.2.2: a decoder reads a group, and while the continuation
// bit that follows is set, shifts the value left by n_bits and adds 1 << n_bits
// before reading the next group. So the last group is the value's low n_bits,
// and each group before it is what is left once that one is removed, less
// one. At most 64 bits of value, so at most 64 groups.
struct Groups {
    std::array<std::uint64_t, 64> values{};
    std::size_t count = 0;  // values[count - 1] is written first
};

[[nodiscard]] Groups split(unsigned n_bits, std::uint64_t value) noexcept {
    Groups groups;
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

}  // namespace

inline unsigned variable_bits_width(unsigned n_bits, std::uint64_t value) noexcept {
    return static_cast<unsigned>(split(n_bits, value).count) * (n_bits + 1);
}

inline BitWriter::BitWriter(int substream, SyntaxSink sink) noexcept
    : substream_(substream), sink_(sink) {}

inline BitWriter BitWriter::buffered() {
    BitWriter writer;
    writer.buffering_ = true;
    return writer;
}

inline void BitWriter::clear() noexcept {
    bytes_.clear();
    kept_.clear();
    bits_ = 0;
}

inline void BitWriter::emit(const SyntaxRecord& record) {
    if (buffering_) {
        kept_.push_back(record);
    } else if (sink_) {
        sink_(record);
    }
}

inline void BitWriter::append(const BitWriter& other) {
    const auto start = bits_;
    for (std::size_t i = 0; i < other.bits_; ++i) {
        const auto byte = std::to_integer<unsigned>(other.bytes_[i / 8]);
        put(1, (byte >> (7 - (i % 8))) & 1U);
    }
    for (SyntaxRecord record : other.kept_) {
        record.substream = substream_;
        record.bit_offset += static_cast<std::uint32_t>(start);
        emit(record);
    }
}

inline void BitWriter::put(unsigned bits, std::uint64_t value) {
    for (unsigned i = bits; i > 0; --i) {
        const auto bit = static_cast<unsigned>((value >> (i - 1)) & 1U);
        if (bits_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit != 0) {
            const auto shift = static_cast<unsigned>(7 - (bits_ % 8));
            bytes_.back() |= static_cast<std::byte>(1U << shift);
        }
        ++bits_;
    }
}

inline void BitWriter::write(unsigned bits, std::uint64_t value, std::string_view name) {
    // A zero-width element, such as the sign bits of a codeword whose lines
    // are all zero, is not recorded (docs/verification.md).
    if (bits == 0) {
        return;
    }
    const auto offset = bits_;
    put(bits, value);
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset),
                      static_cast<std::uint16_t>(bits), value, name});
}

inline void BitWriter::write_variable_bits(unsigned n_bits, std::uint64_t value,
                                           std::string_view name) {
    const auto offset = bits_;
    const Groups groups = split(n_bits, value);
    for (std::size_t i = groups.count; i > 0; --i) {
        put(n_bits, groups.values[i - 1]);
        put(1, i > 1 ? 1U : 0U);
    }
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset),
                      static_cast<std::uint16_t>(bits_ - offset), value, name});
}

inline void BitWriter::write_unrecorded(unsigned bits, std::uint64_t value) {
    put(bits, value);
}

inline void BitWriter::write_zero_run(std::uint64_t bits, std::string_view name) {
    constexpr std::uint64_t kMaxRecordBits = 65535;
    while (bits > 0) {
        const std::uint64_t width = bits < kMaxRecordBits ? bits : kMaxRecordBits;
        const auto offset = bits_;
        for (std::uint64_t i = 0; i < width; ++i) {
            put(1, 0);
        }
        emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset),
                          static_cast<std::uint16_t>(width), 0, name});
        bits -= width;
    }
}

inline void BitWriter::write_as(unsigned bits, std::uint64_t raw, std::uint64_t value,
                                std::string_view name) {
    const auto offset = bits_;
    put(bits, raw);
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset),
                      static_cast<std::uint16_t>(bits), value, name});
}

inline void BitWriter::align() {
    while (bits_ % 8 != 0) {
        put(1, 0);
    }
}

}  // namespace old_ac4
// --- the AC-4 inspector's reader before C4 ------------------------------------------------------
namespace old_toc {
enum class Error { kTruncated, kOther };
namespace {
class Reader {
   public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t bits(int n) {
        std::uint32_t value = 0;
        for (int i = 0; i < n; ++i) {
            value = (value << 1) | read_bit();
        }
        return value;
    }

    void byte_align() { position_ = (position_ + 7) & ~std::size_t{7}; }

    // Reads and discards n bits - every call site below that consumes a
    // reserved/unused field rather than a value it goes on to use.
    void skip(int n) { (void)bits(n); }

    // Discards n bytes by moving the read position, for a byte count the
    // stream chose: presentation_config_ext_info's n_skip_bytes, which
    // variable_bits() lets reach 2^32. skip(8 * n) overflowed int on such a
    // count, and would then have walked the phantom bits past the end of the
    // data one at a time. A count past the end marks the reader overflowed,
    // as reading those bits would have.
    void skip_bytes(std::uint32_t n) {
        const std::uint64_t end = std::uint64_t{data_.size()} * 8;
        const std::uint64_t target = std::uint64_t{position_} + std::uint64_t{n} * 8;
        if (target <= end) {
            position_ = static_cast<std::size_t>(target);
            return;
        }
        overflowed_ = true;
        if (position_ < end) {
            position_ = static_cast<std::size_t>(end);
        }
    }

    // Bit-granular twin of skip_bytes(), for a bit count the stream chose
    // (oamd_common_data()'s add_data, after trim()/bed_render_info()/
    // headphone() spend some of add_data_bytes*8) rather than a whole byte
    // count - same 64-bit-safe arithmetic, same reasoning.
    void skip_bits(std::uint64_t n) {
        const std::uint64_t end = std::uint64_t{data_.size()} * 8;
        const std::uint64_t target = std::uint64_t{position_} + n;
        if (target <= end) {
            position_ = static_cast<std::size_t>(target);
            return;
        }
        overflowed_ = true;
        if (position_ < end) {
            position_ = static_cast<std::size_t>(end);
        }
    }

    void fail(Error error) {
        if (!error_) {
            error_ = error;
        }
    }

    [[nodiscard]] std::optional<Error> error() const {
        // Truncation wins even over an explicit fail() call made afterwards:
        // once real data has run out, every subsequent read returns a
        // phantom 0, so any "logical" refusal a parse_* helper derives from
        // one of those phantom bits (e.g. a b_channel_coded that reads as 0
        // only because it ran off the end) is itself meaningless and would
        // misreport the actual cause as something more specific than it is.
        if (overflowed_) {
            return Error::kTruncated;
        }
        if (error_) {
            return error_;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t bit_position() const { return position_; }

   private:
    [[nodiscard]] std::uint32_t read_bit() {
        const std::size_t byte_index = position_ >> 3;
        if (byte_index >= data_.size()) {
            overflowed_ = true;
            ++position_;
            return 0;
        }
        const auto bit =
            (std::to_integer<std::uint32_t>(data_[byte_index]) >> (7 - (position_ & 7))) & 1u;
        ++position_;
        return bit;
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    bool overflowed_ = false;
    std::optional<Error> error_;
};

// Table 3 (§4.2.2): a value sent as groups of n_bits, MSB group first, each
// followed by a continuation bit.
std::uint32_t variable_bits(Reader& r, int n_bits) {
    std::uint32_t value = 0;
    while (true) {
        value += r.bits(n_bits);
        if (!r.bits(1)) {
            return value;
        }
        value <<= n_bits;
        value += (1u << n_bits);
    }
}

}  // namespace
}  // namespace old_toc

namespace {

std::vector<std::byte> random_bytes(std::mt19937& rng, std::size_t max_size) {
    std::vector<std::byte> data(std::uniform_int_distribution<std::size_t>(0, max_size)(rng));
    for (auto& b : data) {
        b = static_cast<std::byte>(std::uniform_int_distribution<int>(0, 255)(rng));
    }
    return data;
}

int pick(std::mt19937& rng, int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

bool same(const std::vector<SyntaxRecord>& a, const std::vector<SyntaxRecord>& b) {
    return std::ranges::equal(a, b, [](const SyntaxRecord& x, const SyntaxRecord& y) {
        return x.substream == y.substream && x.bit_offset == y.bit_offset && x.bits == y.bits &&
               x.value == y.value && x.name == y.name;
    });
}

}  // namespace

TEST_CASE("the bit reader reads as base's reader did before C4", "[base][bitreader]") {
    std::mt19937 rng(20261006);
    for (int trial = 0; trial < 4000; ++trial) {
        const auto data = random_bytes(rng, 24);
        old_base::BitReader before(data);
        iclforge::BitReader after(data);
        for (int op = 0; op < 40; ++op) {
            switch (pick(rng, 0, 4)) {
                case 0: {
                    const int bits = pick(rng, 0, 32);
                    REQUIRE(before.read(bits) == after.read(bits));
                    break;
                }
                case 1:
                    REQUIRE(before.read_bit() == after.read_bit());
                    break;
                case 2: {
                    const auto bits = static_cast<std::size_t>(pick(rng, 0, 70));
                    before.skip(bits);
                    after.skip(bits);
                    break;
                }
                case 3: {
                    const int n = pick(rng, 1, 11);
                    REQUIRE(old_base::read_variable_bits(before, n) ==
                            static_cast<std::uint32_t>(after.variable_bits(n)));
                    break;
                }
                default: {
                    const int n = pick(rng, 1, 8);
                    const int groups = pick(rng, 1, 4);
                    REQUIRE(old_base::read_variable_bits_max(before, n, groups) ==
                            static_cast<std::uint32_t>(after.variable_bits(n, {}, groups)));
                    break;
                }
            }
            REQUIRE(before.bit_position() == after.bit_position());
            REQUIRE(before.overflowed() == after.overflowed());
        }
    }
}

TEST_CASE("the bit reader reads and records as AC-4's reader did before C4", "[base][bitreader]") {
    std::mt19937 rng(103190);
    static constexpr std::array<std::string_view, 3> kNames = {"a", "b_flag", "c"};
    for (int trial = 0; trial < 4000; ++trial) {
        const auto data = random_bytes(rng, 40);
        std::vector<SyntaxRecord> got_before;
        std::vector<SyntaxRecord> got_after;
        auto keep_before = [&](const SyntaxRecord& r) { got_before.push_back(r); };
        auto keep_after = [&](const SyntaxRecord& r) { got_after.push_back(r); };
        const int substream = pick(rng, 0, 3);
        old_ac4::BitReader before(data, substream, SyntaxSink(keep_before));
        iclforge::BitReader after(data, substream, SyntaxSink(keep_after));
        for (int op = 0; op < 40; ++op) {
            const std::string_view name = kNames[static_cast<std::size_t>(pick(rng, 0, 2))];
            switch (pick(rng, 0, 10)) {
                case 0: {
                    const int bits = pick(rng, 0, 32);
                    REQUIRE(before.read(bits, name) == after.read(bits, name));
                    break;
                }
                case 1:
                    REQUIRE(before.read_flag(name) == after.read_flag(name));
                    break;
                case 2: {
                    const int n = pick(rng, 1, 11);
                    REQUIRE(before.variable_bits(n, name) == after.variable_bits(n, name));
                    break;
                }
                case 3: {
                    const auto bits = static_cast<std::uint64_t>(pick(rng, 0, 100));
                    before.read_run(bits, name);
                    after.read_run(bits, name);
                    break;
                }
                case 4: {
                    const auto bits = static_cast<std::size_t>(pick(rng, 0, 70));
                    before.skip(bits);
                    after.skip(bits);
                    break;
                }
                case 5:
                    before.align();
                    after.align();
                    break;
                case 6: {
                    const int bits = pick(rng, 0, 40);
                    REQUIRE(before.peek_raw(bits) == after.peek(bits));
                    break;
                }
                case 7: {
                    const int bits = pick(rng, 0, 20);
                    before.consume(bits);
                    after.consume(bits);
                    break;
                }
                case 8: {
                    const auto at = static_cast<std::size_t>(pick(rng, 0, 330));
                    REQUIRE(before.bit(at) == after.bit(at));
                    break;
                }
                case 9: {
                    const auto at = static_cast<std::size_t>(pick(rng, 0, 330));
                    before.seek(at);
                    after.seek(at);
                    break;
                }
                default: {
                    const auto start = before.position();
                    const int bits = pick(rng, 0, 20);
                    before.emit(start, bits, 7, name);
                    after.record(start, bits, 7, name);
                    break;
                }
            }
            REQUIRE(before.position() == after.bit_position());
            REQUIRE(before.overflow() == after.overflowed());
            REQUIRE(before.remaining_bits() == after.remaining_bits());
            REQUIRE(before.size_bits() == after.size_bits());
        }
        REQUIRE(same(got_before, got_after));
    }
}

TEST_CASE("the bit reader reads as the AC-4 inspector's reader did before C4",
          "[base][bitreader]") {
    std::mt19937 rng(1031901);
    for (int trial = 0; trial < 4000; ++trial) {
        const auto data = random_bytes(rng, 24);
        old_toc::Reader before(data);
        iclforge::BitReader after(data);
        for (int op = 0; op < 40; ++op) {
            switch (pick(rng, 0, 4)) {
                case 0: {
                    const int bits = pick(rng, 0, 40);
                    REQUIRE(before.bits(bits) == after.read(bits));
                    break;
                }
                case 1: {
                    const int bits = pick(rng, 0, 40);
                    before.skip(bits);
                    after.skip(static_cast<std::size_t>(bits));
                    break;
                }
                case 2:
                    before.byte_align();
                    after.align();
                    break;
                case 3: {
                    const int n = pick(rng, 1, 5);
                    REQUIRE(old_toc::variable_bits(before, n) ==
                            static_cast<std::uint32_t>(after.variable_bits(n)));
                    break;
                }
                default: {
                    // The inspector's skip past the end stopped its reader at the end; a skip
                    // past the end overflows both, and an overflowed parse is a truncated one
                    // whatever the position.
                    const auto n = static_cast<std::uint64_t>(pick(rng, 0, 120));
                    before.skip_bits(n);
                    after.skip(static_cast<std::size_t>(n));
                    break;
                }
            }
            REQUIRE(before.error().has_value() == after.overflowed());
            if (!after.overflowed()) {
                REQUIRE(before.bit_position() == after.bit_position());
            }
        }
    }
}

TEST_CASE("the bit writer writes as base's writer did before C4", "[base][bitwriter]") {
    std::mt19937 rng(5201);
    for (int trial = 0; trial < 4000; ++trial) {
        old_base::BitWriter before;
        iclforge::BitWriter after;
        for (int op = 0; op < 40; ++op) {
            switch (pick(rng, 0, 4)) {
                case 0: {
                    const int bits = pick(rng, 0, 32);
                    const std::uint64_t value =
                        bits == 0 ? 0 : (std::uint64_t{rng()} & ((std::uint64_t{1} << bits) - 1));
                    before.put(static_cast<std::uint32_t>(value), bits);
                    after.put(value, bits);
                    break;
                }
                case 1: {
                    const bool bit = (rng() & 1U) != 0;
                    before.put_bit(bit);
                    after.put_bit(bit);
                    break;
                }
                case 2:
                    before.byte_align();
                    after.align();
                    break;
                case 3: {
                    const auto value = static_cast<std::uint32_t>(rng() >> pick(rng, 0, 31));
                    const int n = pick(rng, 1, 11);
                    old_base::put_variable_bits(before, value, n);
                    after.write_variable_bits(static_cast<unsigned>(n), value);
                    break;
                }
                default: {
                    const auto value = static_cast<std::uint32_t>(rng() >> pick(rng, 16, 31));
                    const int n = pick(rng, 1, 8);
                    const int groups = pick(rng, 4, 6);
                    old_base::put_variable_bits_max(before, value, n, groups);
                    after.write_variable_bits(static_cast<unsigned>(n), value, {}, groups);
                    break;
                }
            }
            REQUIRE(before.bit_count() == after.bit_count());
        }
        REQUIRE(before.take() == after.take());
    }
}

TEST_CASE("the bit writer writes and records as AC-4's writer did before C4", "[base][bitwriter]") {
    std::mt19937 rng(4220);
    static constexpr std::array<std::string_view, 3> kNames = {"x", "y", "z"};
    for (int trial = 0; trial < 3000; ++trial) {
        std::vector<SyntaxRecord> got_before;
        std::vector<SyntaxRecord> got_after;
        auto keep_before = [&](const SyntaxRecord& r) { got_before.push_back(r); };
        auto keep_after = [&](const SyntaxRecord& r) { got_after.push_back(r); };
        const int substream = pick(rng, 0, 3);
        old_ac4::BitWriter before(substream, SyntaxSink(keep_before));
        iclforge::BitWriter after(substream, SyntaxSink(keep_after));
        old_ac4::BitWriter before_buffered = old_ac4::BitWriter::buffered();
        iclforge::BitWriter after_buffered = iclforge::BitWriter::buffered();
        for (int op = 0; op < 30; ++op) {
            const std::string_view name = kNames[static_cast<std::size_t>(pick(rng, 0, 2))];
            const bool into_buffer = (rng() & 3U) == 0;
            old_ac4::BitWriter& b = into_buffer ? before_buffered : before;
            iclforge::BitWriter& a = into_buffer ? after_buffered : after;
            switch (pick(rng, 0, 7)) {
                case 0: {
                    const auto bits = static_cast<unsigned>(pick(rng, 0, 64));
                    const std::uint64_t value = bits == 64 ? (std::uint64_t{rng()} << 32 | rng())
                                                           : ((std::uint64_t{rng()} << 32 | rng()) &
                                                              ((std::uint64_t{1} << bits) - 1));
                    b.write(bits, value, name);
                    a.write(bits, value, name);
                    break;
                }
                case 1: {
                    const std::uint64_t value =
                        (std::uint64_t{rng()} << 32 | rng()) >> pick(rng, 0, 63);
                    const auto n = static_cast<unsigned>(pick(rng, 1, 11));
                    b.write_variable_bits(n, value, name);
                    a.write_variable_bits(n, value, name);
                    REQUIRE(old_ac4::variable_bits_width(n, value) ==
                            iclforge::variable_bits_width(n, value));
                    break;
                }
                case 2: {
                    const auto bits = static_cast<unsigned>(pick(rng, 0, 32));
                    const std::uint64_t value =
                        bits == 0 ? 0 : (rng() & ((std::uint64_t{1} << bits) - 1));
                    b.write_unrecorded(bits, value);
                    a.put(value, static_cast<int>(bits));
                    break;
                }
                case 3: {
                    const auto bits = static_cast<std::uint64_t>(
                        pick(rng, 0, 70000) * (pick(rng, 0, 9) == 0 ? 1 : 0) + pick(rng, 0, 40));
                    b.write_zero_run(bits, name);
                    a.write_zero_run(bits, name);
                    break;
                }
                case 4: {
                    const auto bits = static_cast<unsigned>(pick(rng, 1, 24));
                    const std::uint64_t raw = rng() & ((std::uint64_t{1} << bits) - 1);
                    b.write_as(bits, raw, raw + 3, name);
                    a.write_as(bits, raw, raw + 3, name);
                    break;
                }
                case 5:
                    b.align();
                    a.align();
                    break;
                case 6:
                    before.append(before_buffered);
                    after.append(after_buffered);
                    before_buffered.clear();
                    after_buffered.clear();
                    break;
                default:
                    if ((rng() & 7U) == 0) {
                        before.clear();
                        after.clear();
                    }
                    break;
            }
            REQUIRE(before.bit_position() == after.bit_count());
            REQUIRE(before.byte_size() == after.byte_size());
            REQUIRE(before.bytes() == after.bytes());
            REQUIRE(before_buffered.bytes() == after_buffered.bytes());
            REQUIRE(same({before_buffered.kept().begin(), before_buffered.kept().end()},
                         {after_buffered.kept().begin(), after_buffered.kept().end()}));
        }
        REQUIRE(same(got_before, got_after));
    }
}
