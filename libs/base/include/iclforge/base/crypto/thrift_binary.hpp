#pragma once

// Apache Thrift's TBinaryProtocol for structures (the encoding of section 7 of the AC-4 content
// signing specification): big-endian integers, a field header of a type byte and a signed 16-bit
// id, a STOP byte at the end of every struct, a 32-bit length in front of every string and binary,
// and an element type and 32-bit count in front of every list. Written from the specification's
// description of the wire format; no Thrift library is involved. Only what a signature payload
// needs is here: the writer and the reader know i32, string / binary, struct and list, and the
// reader can step over a value of any of the types the specification's table lists.
//
// The reader runs on bytes that came from a file the verifier did not write, so every size it
// meets is checked against what is left of the input before it is believed, and the work it can
// be asked to do is bounded (Limits). A payload that breaks a rule is an error, not a partial
// result.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto::thrift {

// The type codes of the specification's section 7.2. Codes the table does not list (the double,
// the void, the unsigned 64-bit of old Thrift) are refused: this reader's job is to understand what
// a content signature may contain, and a type it has not been told about has no known size.
enum class Type : std::uint8_t {
    kStop = 0,
    kBool = 2,
    kByte = 3,
    kI16 = 6,
    kI32 = 8,
    kI64 = 10,
    kString = 11,  // also binary: the same bytes
    kStruct = 12,
    kMap = 13,
    kSet = 14,
    kList = 15,
};

// True for the codes above, kStop included.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool is_known_type(std::uint8_t code) noexcept;

// --- Writer -------------------------------------------------------------------------------------

// Appends to a vector it does not own. A struct is written as its field headers and values, then
// stop(). The writer does not check that what it is asked to write forms a valid structure: it is
// the encoder half of a round trip that the reader's tests check.
//
// A string or list longer than a signed 32-bit length can say cannot be written; the writer notes
// that (overflowed()) and writes nothing for it, so the caller sees the failure once, at the end,
// and not a payload with a wrapped length.
class ICLFORGE_BASE_EXPORT Writer {
   public:
    explicit Writer(std::vector<std::byte>& out) noexcept : out_(&out) {}

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

    // A field header: the type byte and the id.
    void field(Type type, std::int16_t id);
    // The end of a struct.
    void stop();

    // Values, written after the header of the field that holds them (or as a list element).
    void i32(std::int32_t value);
    void string(std::string_view value);
    void binary(std::span<const std::byte> value);
    void list(Type element, std::size_t count);

    // A header and its value in one call.
    void i32_field(std::int16_t id, std::int32_t value);
    void string_field(std::int16_t id, std::string_view value);
    void binary_field(std::int16_t id, std::span<const std::byte> value);
    void struct_field(std::int16_t id) { field(Type::kStruct, id); }
    void list_field(std::int16_t id, Type element, std::size_t count);

   private:
    void put_u8(std::uint8_t value);
    void put_u16(std::uint16_t value);
    void put_u32(std::uint32_t value);

    std::vector<std::byte>* out_;
    bool overflowed_ = false;
};

// --- Reader -------------------------------------------------------------------------------------

enum class Error : std::uint8_t {
    kTruncated,           // the input ended inside a value
    kUnknownType,         // a type code outside the table, or STOP where a value should be
    kNegativeLength,      // a string, list, set or map length below zero
    kLengthExceedsInput,  // a length, or a count of elements that each take space, beyond the input
    kTooDeep,             // nested deeper than Limits::max_depth
    kTooManyElements,     // more fields and elements than Limits::max_elements
    kTypeMismatch,        // a field the caller knows has another type than it should
    kDuplicateField,      // reported by a caller that meets one field id twice in a struct
    kMissingField,        // reported by a caller whose struct lacks a field it requires
    kTrailingData,        // bytes after the end of the top-level struct
};

[[nodiscard]] ICLFORGE_BASE_EXPORT std::string_view describe(Error error);

struct Limits {
    // Structs and containers inside one another. A content signature nests four deep.
    int max_depth = 16;
    // Field headers plus list elements the reader may meet in the whole input, skipped ones
    // included. A signature payload has about fifty; the bound exists so that a megabyte of
    // single-byte structs cannot be made to cost a million steps.
    std::size_t max_elements = std::size_t{1} << 16;
};

struct FieldHeader {
    Type type = Type::kStop;  // kStop ends the struct
    std::int16_t id = 0;
};

struct ListHeader {
    Type element = Type::kStop;
    // Already checked: not negative, and the input holds at least this many elements of the
    // smallest size the element type can have. Safe to reserve for.
    std::size_t count = 0;
};

class ICLFORGE_BASE_EXPORT Reader {
   public:
    explicit Reader(std::span<const std::byte> input, Limits limits = {}) noexcept
        : input_(input), limits_(limits) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - position_; }

    // Reads a struct from the current position: calls on_field(FieldHeader) for each field until
    // STOP. on_field returns std::expected<bool, Error>: true when it read the field's value, false
    // when it does not know the field, in which case the value is stepped over by its type. The
    // callback reads the value with the functions below and nothing else, and reports a field it
    // knows with the wrong type as Error::kTypeMismatch.
    template <class OnField>
    [[nodiscard]] std::expected<void, Error> read_struct(OnField&& on_field) {
        if (auto entered = enter(); !entered) {
            return entered;
        }
        for (;;) {
            const auto header = read_field_header();
            if (!header) {
                return std::unexpected(header.error());
            }
            if (header->type == Type::kStop) {
                break;
            }
            const std::expected<bool, Error> known = on_field(*header);
            if (!known) {
                return std::unexpected(known.error());
            }
            if (!*known) {
                if (auto skipped = skip(header->type); !skipped) {
                    return skipped;
                }
            }
        }
        leave();
        return {};
    }

    // Values.
    [[nodiscard]] std::expected<std::int32_t, Error> read_i32();
    // A string or binary: a view into the input, valid as long as the input is.
    [[nodiscard]] std::expected<std::span<const std::byte>, Error> read_binary();
    [[nodiscard]] std::expected<ListHeader, Error> read_list_header();

    // Steps over one value of `type`, whatever it holds, within the limits.
    [[nodiscard]] std::expected<void, Error> skip(Type type);

    // After the top-level struct: kTrailingData unless the input is used up.
    [[nodiscard]] std::expected<void, Error> expect_end() const;

   private:
    [[nodiscard]] std::expected<void, Error> enter();
    void leave() noexcept { --depth_; }
    [[nodiscard]] std::expected<FieldHeader, Error> read_field_header();
    [[nodiscard]] std::expected<void, Error> count_element(std::size_t n = 1);
    [[nodiscard]] std::expected<std::span<const std::byte>, Error> take(std::size_t n);
    [[nodiscard]] std::expected<std::uint32_t, Error> take_u32();

    std::span<const std::byte> input_;
    std::size_t position_ = 0;
    Limits limits_;
    int depth_ = 0;
    std::size_t elements_ = 0;
};

}  // namespace iclforge::base::crypto::thrift
