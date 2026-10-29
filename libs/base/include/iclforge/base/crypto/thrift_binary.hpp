#pragma once

// Apache Thrift's Binary Protocol for structs (CS spec §7): what the content signature is
// serialised with. Only the wire format is here, not Thrift: a field is a type byte, a signed
// 16-bit id (big-endian) and a value; a struct is its fields followed by a STOP byte. A Writer
// builds one; read_struct() splits one into its fields without interpreting them, leaving each
// field's encoded bytes available so that a caller can hash exactly what it received (the content
// signature digests two structs as they were written, and a verifier that re-serialised them would
// be trusting its own writer to agree with the signer's).
//
// The reader is bounded for hostile input: nesting is capped, every length is checked against the
// bytes that remain before anything is allocated, and a list count cannot exceed those bytes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto::thrift {

// CS spec §7.2 (the TType numbers of Thrift's binary protocol).
enum class Type : std::uint8_t {
    kStop = 0,
    kBool = 2,
    kByte = 3,
    kDouble = 4,
    kI16 = 6,
    kI32 = 8,
    kI64 = 10,
    kString = 11,  // also `binary`
    kStruct = 12,
    kMap = 13,
    kSet = 14,
    kList = 15,
};

// The deepest struct/list/map nesting the reader follows. The content signature nests five levels.
inline constexpr int kMaxDepth = 16;

// The most values (fields, list elements, map keys and values) one read_struct() will walk past. A
// content signature has a few dozen; a count claiming millions, each an empty struct, would
// otherwise cost memory and time in proportion to the input.
inline constexpr std::size_t kMaxValues = 16384;

class ICLFORGE_BASE_EXPORT Writer {
   public:
    // A field header: type and id. The caller writes the value next (or uses the field_* helpers).
    void field(Type type, std::int16_t id);
    void stop() { out_.push_back(std::byte{0}); }

    void i32(std::int32_t value);
    void i16(std::int16_t value);
    void string(std::string_view value);  // 4-byte length, then the bytes
    void list_header(Type element, std::int32_t count);
    void map_header(Type key, Type value, std::int32_t count);
    void raw(std::span<const std::byte> bytes);

    // field + value in one call.
    void field_i32(std::int16_t id, std::int32_t value);
    void field_string(std::int16_t id, std::string_view value);
    // A `list<byte>`: element type BYTE (3), a count, the bytes (as the sample writes digests, the
    // nonce, the certificates and r and s; not a `binary` string).
    void field_byte_list(std::int16_t id, std::span<const std::byte> bytes);
    void byte_list(std::span<const std::byte> bytes);  // header and bytes, for a map value
    // A struct field opens with field(); its fields follow; stop() closes it.
    void field_struct(std::int16_t id) { field(Type::kStruct, id); }

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return out_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

   private:
    std::vector<std::byte> out_;
};

// One field of a struct as it was encoded.
struct Field {
    Type type = Type::kStop;
    std::int16_t id = 0;
    // The value exactly as encoded after the field header: for a struct, its fields and the STOP
    // byte; for a list, its element type, count and elements; for a string, its length and bytes.
    std::span<const std::byte> value;
};

// The fields of the struct whose first field header is at `bytes[0]`, up to and including its STOP,
// which is where `consumed` ends. nullopt for anything that is not a well-formed struct: an unknown
// type, a length past the end, nesting deeper than kMaxDepth, a missing STOP. Field ids may repeat
// or appear in any order (Thrift readers accept that); unknown fields are carried along.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<Field>> read_struct(
    std::span<const std::byte> bytes, std::size_t* consumed = nullptr);

// The first field with `id` and `type`, or nullptr.
[[nodiscard]] ICLFORGE_BASE_EXPORT const Field* find(std::span<const Field> fields, std::int16_t id,
                                                     Type type);

// Interpreters of one field's value; nullopt when the field is not of that shape.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::int32_t> as_i32(const Field& field);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::string> as_string(const Field& field);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::span<const std::byte>> as_byte_list(
    const Field& field);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<Field>> as_struct(const Field& field);
// The elements of a `list<struct>`, each as the bytes of one struct (fields and STOP).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<std::span<const std::byte>>>
as_struct_list(const Field& field);
// A `map<string, list<byte>>`, in the order written.
struct ByteListEntry {
    std::string key;
    std::span<const std::byte> value;
};
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<ByteListEntry>>
as_string_to_byte_list_map(const Field& field);

}  // namespace iclforge::base::crypto::thrift
