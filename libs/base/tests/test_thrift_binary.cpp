#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"

namespace {

namespace tb = iclforge::base::crypto::thrift;
using iclforge::test::crypto::from_hex;
using iclforge::test::crypto::to_hex;

}  // namespace

TEST_CASE("Thrift binary writer produces the documented bytes", "[crypto][thrift]") {
    tb::Writer w;
    w.field_i32(1, 1);                                    // 08 0001 00000001
    w.field_string(2, "ab");                              // 0b 0002 00000002 6162
    w.field_byte_list(3, from_hex("0102"));               // 0f 0003 03 00000002 0102
    w.field_struct(10);                                   // 0c 000a
    w.field_i32(1, -2);                                   //   08 0001 fffffffe
    w.stop();                                             //   00
    w.field(tb::Type::kMap, 4);                           // 0d 0004
    w.map_header(tb::Type::kString, tb::Type::kList, 1);  // 0b 0f 00000001
    w.string("k");                                        // 00000001 6b
    w.byte_list(from_hex("ff"));                          // 03 00000001 ff
    w.stop();                                             // 00
    CHECK(to_hex(w.bytes()) ==
          "08000100000001"
          "0b00020000000261"
          "62"
          "0f0003030000000201"
          "02"
          "0c000a"
          "08000"
          "1fffffffe"
          "00"
          "0d0004"
          "0b0f00000001"
          "000000016b"
          "0300000001ff"
          "00");
}

TEST_CASE("Thrift binary reader splits a struct and keeps each field's bytes", "[crypto][thrift]") {
    tb::Writer w;
    w.field_i32(1, 7);
    w.field_string(2, "hello");
    w.field_byte_list(3, from_hex("0a0b0c"));
    w.field_struct(4);
    w.field_i32(1, 9);
    w.stop();
    w.field(tb::Type::kList, 5);
    w.list_header(tb::Type::kStruct, 2);
    w.field_i32(1, 1);
    w.stop();
    w.field_i32(1, 2);
    w.stop();
    w.field(tb::Type::kMap, 6);
    w.map_header(tb::Type::kString, tb::Type::kList, 2);
    w.string("a");
    w.byte_list(from_hex("01"));
    w.string("b");
    w.byte_list(from_hex("0203"));
    w.stop();
    const std::vector<std::byte> bytes = w.take();

    std::size_t consumed = 0;
    const auto fields = tb::read_struct(bytes, &consumed);
    REQUIRE(fields.has_value());
    CHECK(consumed == bytes.size());
    REQUIRE(fields->size() == 6);

    CHECK(tb::as_i32(*tb::find(*fields, 1, tb::Type::kI32)) == std::optional<std::int32_t>(7));
    CHECK(tb::as_string(*tb::find(*fields, 2, tb::Type::kString)) ==
          std::optional<std::string>("hello"));
    const auto list = tb::as_byte_list(*tb::find(*fields, 3, tb::Type::kList));
    REQUIRE(list.has_value());
    CHECK(to_hex(*list) == "0a0b0c");
    const auto inner = tb::as_struct(*tb::find(*fields, 4, tb::Type::kStruct));
    REQUIRE(inner.has_value());
    CHECK(tb::as_i32(inner->front()) == std::optional<std::int32_t>(9));
    const auto structs = tb::as_struct_list(*tb::find(*fields, 5, tb::Type::kList));
    REQUIRE(structs.has_value());
    REQUIRE(structs->size() == 2);
    CHECK(to_hex((*structs)[0]) ==
          "080001"
          "00000001"
          "00");
    const auto map = tb::as_string_to_byte_list_map(*tb::find(*fields, 6, tb::Type::kMap));
    REQUIRE(map.has_value());
    REQUIRE(map->size() == 2);
    CHECK((*map)[0].key == "a");
    CHECK(to_hex((*map)[1].value) == "0203");

    // The value of a struct field is its bytes through the STOP: what a digest over it needs.
    const tb::Field* nested = tb::find(*fields, 4, tb::Type::kStruct);
    CHECK(to_hex(nested->value) ==
          "08000100000009"
          "00");

    // The wrong accessor for a field says no instead of misreading it.
    CHECK_FALSE(tb::as_string(*tb::find(*fields, 1, tb::Type::kI32)).has_value());
    CHECK_FALSE(tb::as_i32(*tb::find(*fields, 2, tb::Type::kString)).has_value());
    CHECK(tb::find(*fields, 1, tb::Type::kString) == nullptr);
}

TEST_CASE("Thrift binary reader refuses what is malformed", "[crypto][thrift]") {
    const auto reads = [](const char* hex) { return tb::read_struct(from_hex(hex)).has_value(); };
    CHECK(reads("00"));                             // an empty struct
    CHECK_FALSE(reads(""));                         // no STOP
    CHECK_FALSE(reads("08000100000001"));           // no STOP after a field
    CHECK_FALSE(reads("09000100"));                 // type 9 does not exist
    CHECK_FALSE(reads("0800010000"));               // an i32 cut short
    CHECK_FALSE(reads("0b0001ffffffff00"));         // a string longer than the input
    CHECK_FALSE(reads("0b000100000005616200"));     // length 5, two bytes
    CHECK_FALSE(reads("0f00010300ffffff00"));       // a list claiming 16 million bytes
    CHECK_FALSE(reads("0f00010c7fffffff00"));       // two billion structs in four bytes
    CHECK_FALSE(reads("0d0001 0b0f 7fffffff 00"));  // a map claiming two billion entries
    CHECK_FALSE(
        reads("0d0001 0b0f 00000001 00000001 6b 03 00000005 01 00"));  // its value cut short

    // More values than the reader's budget are refused whatever the lengths say: a list of empty
    // structs costs a byte each, so 4 MB of them is four million values to hold.
    const auto many_structs = [](std::size_t count) {
        std::string hex = "0f0001 0c ";
        char header[16];
        std::snprintf(header, sizeof header, "%08x", static_cast<unsigned>(count));
        hex += header;
        for (std::size_t i = 0; i < count; ++i) {
            hex += "00";
        }
        return hex + "00";
    };
    CHECK(reads(many_structs(100).c_str()));
    CHECK(reads(many_structs(tb::kMaxValues - 8).c_str()));
    CHECK_FALSE(reads(many_structs(tb::kMaxValues + 8).c_str()));
    CHECK_FALSE(reads(many_structs(4'000'000).c_str()));

    // Nesting past kMaxDepth is refused, not followed: 40 structs inside one another.
    std::string deep;
    for (int i = 0; i < 40; ++i) {
        deep += "0c0001";
    }
    deep += "00";
    for (int i = 0; i < 40; ++i) {
        deep += "00";
    }
    CHECK_FALSE(reads(deep.c_str()));
    // 10 deep is fine.
    std::string ok;
    for (int i = 0; i < 10; ++i) {
        ok += "0c0001";
    }
    ok += "00";
    for (int i = 0; i < 10; ++i) {
        ok += "00";
    }
    CHECK(reads(ok.c_str()));

    // No prefix of a valid struct is itself valid, and none crashes the reader.
    tb::Writer w;
    w.field_i32(1, 1);
    w.field_string(2, "text");
    w.field_byte_list(3, from_hex("010203"));
    w.stop();
    const std::vector<std::byte> bytes = w.take();
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        CHECK_FALSE(tb::read_struct(std::span<const std::byte>(bytes).first(cut)).has_value());
    }
}
