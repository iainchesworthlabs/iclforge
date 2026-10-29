#include "iclforge/base/crypto/thrift_binary.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iclforge::base::crypto::thrift {
namespace {

void put_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v >> 8));
    out.push_back(static_cast<std::byte>(v & 0xFFU));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
    }
}

std::uint32_t get_u32(std::span<const std::byte> in) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | std::to_integer<std::uint32_t>(in[i]);
    }
    return v;
}

bool known_type(std::uint8_t t) {
    switch (t) {
        case 2:
        case 3:
        case 4:
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

// How many bytes a value of a fixed-size type takes; 0 for the variable ones.
std::size_t fixed_size(Type t) {
    switch (t) {
        case Type::kBool:
        case Type::kByte:
            return 1;
        case Type::kI16:
            return 2;
        case Type::kI32:
            return 4;
        case Type::kDouble:
        case Type::kI64:
            return 8;
        default:
            return 0;
    }
}

// Advances `pos` past one value of `type`, checking everything on the way.
bool skip_value(std::span<const std::byte> in, std::size_t& pos, Type type, int depth);

bool skip_struct(std::span<const std::byte> in, std::size_t& pos, int depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    for (;;) {
        if (pos >= in.size()) {
            return false;
        }
        const auto t = std::to_integer<std::uint8_t>(in[pos]);
        ++pos;
        if (t == 0) {
            return true;
        }
        if (!known_type(t) || in.size() - pos < 2) {
            return false;
        }
        pos += 2;  // the field id
        if (!skip_value(in, pos, static_cast<Type>(t), depth)) {
            return false;
        }
    }
}

bool skip_value(std::span<const std::byte> in, std::size_t& pos, Type type, int depth) {
    if (const std::size_t n = fixed_size(type); n != 0) {
        if (in.size() - pos < n) {
            return false;
        }
        pos += n;
        return true;
    }
    switch (type) {
        case Type::kString: {
            if (in.size() - pos < 4) {
                return false;
            }
            const std::uint32_t length = get_u32(in.subspan(pos, 4));
            pos += 4;
            if (length > in.size() - pos) {
                return false;
            }
            pos += length;
            return true;
        }
        case Type::kStruct:
            return skip_struct(in, pos, depth + 1);
        case Type::kList:
        case Type::kSet: {
            if (depth + 1 > kMaxDepth || in.size() - pos < 5) {
                return false;
            }
            const auto element = std::to_integer<std::uint8_t>(in[pos]);
            const std::uint32_t count = get_u32(in.subspan(pos + 1, 4));
            pos += 5;
            if (!known_type(element)) {
                return false;
            }
            // Every element takes at least one byte (a struct has its STOP), so a count past the
            // bytes that remain is a lie, and is refused before it can drive a loop.
            if (count > in.size() - pos) {
                return false;
            }
            const Type et = static_cast<Type>(element);
            if (const std::size_t n = fixed_size(et); n != 0) {
                if (static_cast<std::size_t>(count) > (in.size() - pos) / n) {
                    return false;
                }
                pos += static_cast<std::size_t>(count) * n;
                return true;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!skip_value(in, pos, et, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        case Type::kMap: {
            if (depth + 1 > kMaxDepth || in.size() - pos < 6) {
                return false;
            }
            const auto key = std::to_integer<std::uint8_t>(in[pos]);
            const auto value = std::to_integer<std::uint8_t>(in[pos + 1]);
            const std::uint32_t count = get_u32(in.subspan(pos + 2, 4));
            pos += 6;
            if (!known_type(key) || !known_type(value) || count > in.size() - pos) {
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!skip_value(in, pos, static_cast<Type>(key), depth + 1) ||
                    !skip_value(in, pos, static_cast<Type>(value), depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

void Writer::field(Type type, std::int16_t id) {
    out_.push_back(static_cast<std::byte>(type));
    put_u16(out_, static_cast<std::uint16_t>(id));
}

void Writer::i32(std::int32_t value) {
    put_u32(out_, static_cast<std::uint32_t>(value));
}

void Writer::i16(std::int16_t value) {
    put_u16(out_, static_cast<std::uint16_t>(value));
}

void Writer::string(std::string_view value) {
    put_u32(out_, static_cast<std::uint32_t>(value.size()));
    for (const char c : value) {
        out_.push_back(static_cast<std::byte>(c));
    }
}

void Writer::list_header(Type element, std::int32_t count) {
    out_.push_back(static_cast<std::byte>(element));
    put_u32(out_, static_cast<std::uint32_t>(count));
}

void Writer::map_header(Type key, Type value, std::int32_t count) {
    out_.push_back(static_cast<std::byte>(key));
    out_.push_back(static_cast<std::byte>(value));
    put_u32(out_, static_cast<std::uint32_t>(count));
}

void Writer::raw(std::span<const std::byte> bytes) {
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void Writer::field_i32(std::int16_t id, std::int32_t value) {
    field(Type::kI32, id);
    i32(value);
}

void Writer::field_string(std::int16_t id, std::string_view value) {
    field(Type::kString, id);
    string(value);
}

void Writer::byte_list(std::span<const std::byte> bytes) {
    list_header(Type::kByte, static_cast<std::int32_t>(bytes.size()));
    raw(bytes);
}

void Writer::field_byte_list(std::int16_t id, std::span<const std::byte> bytes) {
    field(Type::kList, id);
    byte_list(bytes);
}

std::optional<std::vector<Field>> read_struct(std::span<const std::byte> bytes,
                                              std::size_t* consumed) {
    std::vector<Field> fields;
    std::size_t pos = 0;
    for (;;) {
        if (pos >= bytes.size()) {
            return std::nullopt;
        }
        const auto t = std::to_integer<std::uint8_t>(bytes[pos]);
        ++pos;
        if (t == 0) {
            break;
        }
        if (!known_type(t) || bytes.size() - pos < 2) {
            return std::nullopt;
        }
        Field field;
        field.type = static_cast<Type>(t);
        field.id = static_cast<std::int16_t>((std::to_integer<std::uint16_t>(bytes[pos]) << 8) |
                                             std::to_integer<std::uint16_t>(bytes[pos + 1]));
        pos += 2;
        const std::size_t start = pos;
        // The nesting of the field's own value counts from one: the struct being read is level
        // zero.
        if (!skip_value(bytes, pos, field.type, 1)) {
            return std::nullopt;
        }
        field.value = bytes.subspan(start, pos - start);
        fields.push_back(field);
        if (fields.size() > 4096) {
            return std::nullopt;  // no struct here has more than a few dozen fields
        }
    }
    if (consumed != nullptr) {
        *consumed = pos;
    }
    return fields;
}

const Field* find(std::span<const Field> fields, std::int16_t id, Type type) {
    for (const Field& f : fields) {
        if (f.id == id && f.type == type) {
            return &f;
        }
    }
    return nullptr;
}

std::optional<std::int32_t> as_i32(const Field& field) {
    if (field.type != Type::kI32 || field.value.size() != 4) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(get_u32(field.value));
}

std::optional<std::string> as_string(const Field& field) {
    if (field.type != Type::kString || field.value.size() < 4) {
        return std::nullopt;
    }
    const std::uint32_t length = get_u32(field.value);
    if (length != field.value.size() - 4) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(length);
    for (std::size_t i = 4; i < field.value.size(); ++i) {
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(field.value[i])));
    }
    return out;
}

std::optional<std::span<const std::byte>> as_byte_list(const Field& field) {
    if (field.type != Type::kList || field.value.size() < 5 ||
        field.value[0] != std::byte{static_cast<std::uint8_t>(Type::kByte)}) {
        return std::nullopt;
    }
    const std::uint32_t count = get_u32(field.value.subspan(1, 4));
    if (count != field.value.size() - 5) {
        return std::nullopt;
    }
    return field.value.subspan(5);
}

std::optional<std::vector<Field>> as_struct(const Field& field) {
    if (field.type != Type::kStruct) {
        return std::nullopt;
    }
    std::size_t consumed = 0;
    auto fields = read_struct(field.value, &consumed);
    if (!fields || consumed != field.value.size()) {
        return std::nullopt;
    }
    return fields;
}

std::optional<std::vector<std::span<const std::byte>>> as_struct_list(const Field& field) {
    if (field.type != Type::kList || field.value.size() < 5 ||
        field.value[0] != std::byte{static_cast<std::uint8_t>(Type::kStruct)}) {
        return std::nullopt;
    }
    const std::uint32_t count = get_u32(field.value.subspan(1, 4));
    std::vector<std::span<const std::byte>> out;
    std::size_t pos = 5;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t used = 0;
        const auto fields = read_struct(field.value.subspan(pos), &used);
        if (!fields) {
            return std::nullopt;
        }
        out.push_back(field.value.subspan(pos, used));
        pos += used;
    }
    if (pos != field.value.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<ByteListEntry>> as_string_to_byte_list_map(const Field& field) {
    if (field.type != Type::kMap || field.value.size() < 6 ||
        field.value[0] != std::byte{static_cast<std::uint8_t>(Type::kString)} ||
        field.value[1] != std::byte{static_cast<std::uint8_t>(Type::kList)}) {
        return std::nullopt;
    }
    const std::uint32_t count = get_u32(field.value.subspan(2, 4));
    std::vector<ByteListEntry> out;
    std::size_t pos = 6;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (field.value.size() - pos < 4) {
            return std::nullopt;
        }
        const std::uint32_t key_length = get_u32(field.value.subspan(pos, 4));
        pos += 4;
        if (key_length > field.value.size() - pos) {
            return std::nullopt;
        }
        ByteListEntry entry;
        for (std::size_t k = 0; k < key_length; ++k) {
            entry.key.push_back(
                static_cast<char>(std::to_integer<unsigned char>(field.value[pos + k])));
        }
        pos += key_length;
        if (field.value.size() - pos < 5 ||
            field.value[pos] != std::byte{static_cast<std::uint8_t>(Type::kByte)}) {
            return std::nullopt;
        }
        const std::uint32_t n = get_u32(field.value.subspan(pos + 1, 4));
        pos += 5;
        if (n > field.value.size() - pos) {
            return std::nullopt;
        }
        entry.value = field.value.subspan(pos, n);
        pos += n;
        out.push_back(std::move(entry));
    }
    if (pos != field.value.size()) {
        return std::nullopt;
    }
    return out;
}

}  // namespace iclforge::base::crypto::thrift
