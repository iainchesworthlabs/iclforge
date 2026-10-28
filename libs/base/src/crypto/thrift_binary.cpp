#include "iclforge/base/crypto/thrift_binary.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace iclforge::base::crypto::thrift {
namespace {

constexpr std::size_t kMaxLength =
    static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

// The fewest bytes one element of this type takes on the wire. A list that claims more elements
// than the input has room for, at this size, is lying; the check runs before anything is reserved
// or looped over, so a count of two billion in a ten-byte input costs nothing.
constexpr std::size_t min_wire_size(Type type) noexcept {
    switch (type) {
        case Type::kBool:
        case Type::kByte:
        case Type::kStruct:  // the STOP byte of an empty struct
            return 1;
        case Type::kI16:
            return 2;
        case Type::kI32:
        case Type::kString:  // the length
            return 4;
        case Type::kI64:
            return 8;
        case Type::kList:
        case Type::kSet:  // the element type and the count
            return 5;
        case Type::kMap:  // key type, value type and count
            return 6;
        case Type::kStop:
            return 0;
    }
    return 0;
}

// The size of a value of this type when it is a fixed number of bytes, 0 for the types that are
// not (string, struct and the containers).
constexpr std::size_t fixed_wire_size(Type type) noexcept {
    switch (type) {
        case Type::kBool:
        case Type::kByte:
            return 1;
        case Type::kI16:
            return 2;
        case Type::kI32:
            return 4;
        case Type::kI64:
            return 8;
        default:
            return 0;
    }
}

std::uint32_t load_u32(std::span<const std::byte> bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

}  // namespace

bool is_known_type(std::uint8_t code) noexcept {
    switch (code) {
        case 0:
        case 2:
        case 3:
        case 6:
        case 8:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
            return true;
        default:
            return false;
    }
}

std::string_view describe(Error error) {
    switch (error) {
        case Error::kTruncated:
            return "the input ends inside a value";
        case Error::kUnknownType:
            return "a type code the Thrift binary format table of the specification does not list";
        case Error::kNegativeLength:
            return "a negative length";
        case Error::kLengthExceedsInput:
            return "a length or count larger than the remaining input";
        case Error::kTooDeep:
            return "structures nested too deeply";
        case Error::kTooManyElements:
            return "more fields and elements than the reader allows";
        case Error::kTypeMismatch:
            return "a field with a type other than the one its id has";
        case Error::kDuplicateField:
            return "a field that appears twice in one structure";
        case Error::kMissingField:
            return "a required field is missing from a structure";
        case Error::kTrailingData:
            return "bytes after the end of the structure";
    }
    return "unknown error";
}

// --- Writer -------------------------------------------------------------------------------------

void Writer::put_u8(std::uint8_t value) {
    out_->push_back(static_cast<std::byte>(value));
}

void Writer::put_u16(std::uint16_t value) {
    put_u8(static_cast<std::uint8_t>(value >> 8));
    put_u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void Writer::put_u32(std::uint32_t value) {
    put_u8(static_cast<std::uint8_t>(value >> 24));
    put_u8(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    put_u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    put_u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void Writer::field(Type type, std::int16_t id) {
    put_u8(static_cast<std::uint8_t>(type));
    put_u16(static_cast<std::uint16_t>(id));
}

void Writer::stop() {
    put_u8(static_cast<std::uint8_t>(Type::kStop));
}

void Writer::i32(std::int32_t value) {
    put_u32(static_cast<std::uint32_t>(value));
}

void Writer::binary(std::span<const std::byte> value) {
    if (value.size() > kMaxLength) {
        overflowed_ = true;
        return;
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    out_->insert(out_->end(), value.begin(), value.end());
}

void Writer::string(std::string_view value) {
    binary(std::as_bytes(std::span<const char>{value.data(), value.size()}));
}

void Writer::list(Type element, std::size_t count) {
    if (count > kMaxLength) {
        overflowed_ = true;
        return;
    }
    put_u8(static_cast<std::uint8_t>(element));
    put_u32(static_cast<std::uint32_t>(count));
}

void Writer::i32_field(std::int16_t id, std::int32_t value) {
    field(Type::kI32, id);
    i32(value);
}

void Writer::string_field(std::int16_t id, std::string_view value) {
    field(Type::kString, id);
    string(value);
}

void Writer::binary_field(std::int16_t id, std::span<const std::byte> value) {
    field(Type::kString, id);
    binary(value);
}

void Writer::list_field(std::int16_t id, Type element, std::size_t count) {
    field(Type::kList, id);
    list(element, count);
}

// --- Reader -------------------------------------------------------------------------------------

std::expected<std::span<const std::byte>, Error> Reader::take(std::size_t n) {
    if (n > remaining()) {
        return std::unexpected(Error::kTruncated);
    }
    const auto bytes = input_.subspan(position_, n);
    position_ += n;
    return bytes;
}

std::expected<std::uint32_t, Error> Reader::take_u32() {
    const auto bytes = take(4);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return load_u32(*bytes);
}

std::expected<void, Error> Reader::count_element(std::size_t n) {
    // elements_ never exceeds max_elements, so the subtraction cannot wrap.
    if (n > limits_.max_elements - elements_) {
        return std::unexpected(Error::kTooManyElements);
    }
    elements_ += n;
    return {};
}

std::expected<void, Error> Reader::enter() {
    if (depth_ >= limits_.max_depth) {
        return std::unexpected(Error::kTooDeep);
    }
    ++depth_;
    return {};
}

std::expected<FieldHeader, Error> Reader::read_field_header() {
    if (auto counted = count_element(); !counted) {
        return std::unexpected(counted.error());
    }
    const auto type_byte = take(1);
    if (!type_byte) {
        return std::unexpected(type_byte.error());
    }
    const auto code = std::to_integer<std::uint8_t>((*type_byte)[0]);
    if (!is_known_type(code)) {
        return std::unexpected(Error::kUnknownType);
    }
    FieldHeader header;
    header.type = static_cast<Type>(code);
    if (header.type == Type::kStop) {
        return header;
    }
    const auto id = take(2);
    if (!id) {
        return std::unexpected(id.error());
    }
    header.id = static_cast<std::int16_t>(static_cast<std::uint16_t>(
        (std::to_integer<unsigned>((*id)[0]) << 8) | std::to_integer<unsigned>((*id)[1])));
    return header;
}

std::expected<std::int32_t, Error> Reader::read_i32() {
    const auto value = take_u32();
    if (!value) {
        return std::unexpected(value.error());
    }
    return static_cast<std::int32_t>(*value);
}

std::expected<std::span<const std::byte>, Error> Reader::read_binary() {
    if (auto counted = count_element(); !counted) {
        return std::unexpected(counted.error());
    }
    const auto length = read_i32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length < 0) {
        return std::unexpected(Error::kNegativeLength);
    }
    const auto size = static_cast<std::size_t>(*length);
    if (size > remaining()) {
        return std::unexpected(Error::kLengthExceedsInput);
    }
    return take(size);
}

std::expected<ListHeader, Error> Reader::read_list_header() {
    const auto type_byte = take(1);
    if (!type_byte) {
        return std::unexpected(type_byte.error());
    }
    const auto code = std::to_integer<std::uint8_t>((*type_byte)[0]);
    // STOP is a known code but not an element type: nothing has zero bytes on the wire.
    if (!is_known_type(code) || code == 0) {
        return std::unexpected(Error::kUnknownType);
    }
    const auto count = read_i32();
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count < 0) {
        return std::unexpected(Error::kNegativeLength);
    }
    ListHeader header;
    header.element = static_cast<Type>(code);
    header.count = static_cast<std::size_t>(*count);
    // Both factors are below 2^32, so the product cannot overflow a 64-bit size_t. On a 32-bit
    // target the division form is the safe one; it is used everywhere.
    if (header.count > remaining() / min_wire_size(header.element)) {
        return std::unexpected(Error::kLengthExceedsInput);
    }
    return header;
}

std::expected<void, Error> Reader::skip(Type type) {
    if (type == Type::kStop) {
        return std::unexpected(Error::kUnknownType);
    }
    if (auto counted = count_element(); !counted) {
        return counted;
    }
    if (const std::size_t size = fixed_wire_size(type); size != 0) {
        const auto bytes = take(size);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return {};
    }
    switch (type) {
        case Type::kString: {
            // read_binary counts an element of its own; skip has counted this one already, so the
            // length is read directly.
            const auto length = read_i32();
            if (!length) {
                return std::unexpected(length.error());
            }
            if (*length < 0) {
                return std::unexpected(Error::kNegativeLength);
            }
            if (static_cast<std::size_t>(*length) > remaining()) {
                return std::unexpected(Error::kLengthExceedsInput);
            }
            position_ += static_cast<std::size_t>(*length);
            return {};
        }
        case Type::kStruct: {
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
                if (auto skipped = skip(header->type); !skipped) {
                    return skipped;
                }
            }
            leave();
            return {};
        }
        case Type::kList:
        case Type::kSet: {
            const auto header = read_list_header();
            if (!header) {
                return std::unexpected(header.error());
            }
            if (auto entered = enter(); !entered) {
                return entered;
            }
            if (const std::size_t size = fixed_wire_size(header->element); size != 0) {
                // A run of fixed-size values: bound the work by the count, then step over the
                // bytes in one move. read_list_header has already shown they are there.
                if (auto counted = count_element(header->count); !counted) {
                    return counted;
                }
                position_ += header->count * size;
            } else {
                for (std::size_t i = 0; i < header->count; ++i) {
                    if (auto skipped = skip(header->element); !skipped) {
                        return skipped;
                    }
                }
            }
            leave();
            return {};
        }
        case Type::kMap: {
            const auto types = take(2);
            if (!types) {
                return std::unexpected(types.error());
            }
            const auto key_code = std::to_integer<std::uint8_t>((*types)[0]);
            const auto value_code = std::to_integer<std::uint8_t>((*types)[1]);
            if (!is_known_type(key_code) || key_code == 0 || !is_known_type(value_code) ||
                value_code == 0) {
                return std::unexpected(Error::kUnknownType);
            }
            const auto count = read_i32();
            if (!count) {
                return std::unexpected(count.error());
            }
            if (*count < 0) {
                return std::unexpected(Error::kNegativeLength);
            }
            const auto key_type = static_cast<Type>(key_code);
            const auto value_type = static_cast<Type>(value_code);
            const std::size_t pair_size = min_wire_size(key_type) + min_wire_size(value_type);
            const auto pairs = static_cast<std::size_t>(*count);
            if (pairs > remaining() / pair_size) {
                return std::unexpected(Error::kLengthExceedsInput);
            }
            if (auto entered = enter(); !entered) {
                return entered;
            }
            for (std::size_t i = 0; i < pairs; ++i) {
                if (auto skipped = skip(key_type); !skipped) {
                    return skipped;
                }
                if (auto skipped = skip(value_type); !skipped) {
                    return skipped;
                }
            }
            leave();
            return {};
        }
        default:
            return std::unexpected(Error::kUnknownType);
    }
}

std::expected<void, Error> Reader::expect_end() const {
    if (remaining() != 0) {
        return std::unexpected(Error::kTrailingData);
    }
    return {};
}

}  // namespace iclforge::base::crypto::thrift
