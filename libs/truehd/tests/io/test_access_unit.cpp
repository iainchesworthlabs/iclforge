#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string>
#include <cstddef>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "support/stream_builder.hpp"

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

TEST_CASE("a TrueHD major sync written reads back with a good CRC", "[truehd][io]") {
    MajorSync major_sync = stereo_major_sync();
    major_sync.channel_meaning.heavy_drc_start_up_gain = -5;
    major_sync.channel_meaning.drc_start_up_gain = 33;
    major_sync.channel_meaning.twoch_dialogue_norm = 31;
    major_sync.channel_meaning.eightch_source_format = 40;
    auto bytes = write_major_sync(major_sync);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == kMajorSyncBaseBytes);
    CHECK(starts_with_major_sync(*bytes));

    auto read = parse_major_sync(*bytes);
    REQUIRE(read.has_value());
    MajorSync expected = major_sync;
    expected.crc = read->crc;
    expected.channel_meaning_bytes =
        read->channel_meaning_bytes;  // the 64 bits as written, beside their fields
    CHECK(*read == expected);
    CHECK(read->sample_rate() == 48000);
    CHECK(read->eightch_decoder_channel_assignment() == 1);

    (*bytes)[10] ^= std::byte{0x01};
    CHECK(parse_major_sync(*bytes).error().kind == ErrorKind::kBadMajorSyncCrc);
    CHECK(parse_major_sync(*bytes, false).has_value());
}

TEST_CASE("the extension of the channel meaning has a body of length + 1 words (TH spec 14.2)",
          "[truehd][io][no-external-oracle]") {
    // The 16-channel fields: dialogue norm, mix level, channel count, dyn_object_only 0, content
    // description 6 (ISF and dynamic objects), the ISF index and the object count: 29 bits after
    // the 4-bit length. TH spec §14.2 (v3.9) says the body is length + 1 words and not where the
    // nibble sits against the first, so a major sync is built both ways and read both ways; the
    // CRC confirms the size.
    MajorSync base = stereo_major_sync();
    base.extended_substream_info = 2;
    base.channel_meaning.extra_present = true;
    base.num_substreams = 4;
    auto head = write_major_sync(base);  // written without the extension; its last two bytes are the CRC
    REQUIRE(head.has_value());
    // extra_present is the last bit of the channel meaning, the last bit before the CRC.
    for (const bool nibble_in_body : {true, false}) {
        for (const unsigned length : {2U, 5U, 15U}) {
            std::vector<std::byte> bytes(head->begin(), head->end() - 2);
            iclforge::BitWriter extension;
            extension.put(length, 4);
            extension.put(21, 5);   // sixteench_dialogue_norm
            extension.put(33, 6);   // sixteench_mix_level
            extension.put(16, 5);   // sixteench_channel_count
            extension.put(0, 1);    // dyn_object_only
            extension.put(6, 4);    // content_description: ISF and dynamic objects
            extension.put(3, 3);    // sixteench_isf
            extension.put(9, 5);    // sixteench_dynamic_object_count
            // length + 1 words of body; with the nibble in front of them one more word holds it.
            const std::size_t words = length + 1U + (nibble_in_body ? 0U : 1U);
            extension.put(0, static_cast<int>(words * 16U - extension.bit_count()));
            const std::vector<std::byte> body = extension.take();
            bytes.insert(bytes.end(), body.begin(), body.end());
            const std::uint16_t crc = major_sync_crc(bytes);
            bytes.push_back(static_cast<std::byte>(crc >> 8U));
            bytes.push_back(static_cast<std::byte>(crc & 0xFFU));
            CAPTURE(nibble_in_body, length, bytes.size());

            auto read = parse_major_sync(bytes);
            if (!read) {
                UNSCOPED_INFO(std::string(read.error().detail));
            }
            REQUIRE(read.has_value());
            CHECK(read->size_bytes == bytes.size());
            REQUIRE(read->extra.has_value());
            CHECK(read->extra->length == length);
            CHECK(read->extra->body_words() == static_cast<int>(length) + 1);
            CHECK(read->extra->size_from_length);
            CHECK(read->extra->nibble_in_body == nibble_in_body);
            CHECK(read->extra->sixteench_dialogue_norm == 21);
            CHECK(read->extra->sixteench_mix_level == 33);
            CHECK(read->extra->sixteench_channel_count == 16);
            CHECK(read->extra->content_description == 6);
            CHECK(read->extra->isf == 3);
            CHECK(read->extra->dynamic_object_count == 9);
        }
    }
}

TEST_CASE("an MLP major sync keeps the bytes the specification does not describe", "[truehd][io]") {
    MajorSync major_sync;
    major_sync.kind = StreamKind::kMlp;
    major_sync.format_info = 0x2F0F0001;
    major_sync.flags = 0x4000;
    major_sync.channel_meaning_bytes = {0x56, 0x3F, 0, 0, 0x80, 0x80, 0, 0};
    auto bytes = write_major_sync(major_sync);
    REQUIRE(bytes.has_value());
    CHECK(std::to_integer<int>((*bytes)[3]) == 0xBB);
    auto read = parse_major_sync(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->kind == StreamKind::kMlp);
    CHECK(read->channel_meaning_bytes == major_sync.channel_meaning_bytes);
    CHECK(read->group1_bits_code() == 2);
    CHECK(read->mlp_channel_arrangement() == 1);
}

TEST_CASE("a major sync with a wrong signature or a short buffer is refused", "[truehd][io]") {
    auto bytes = write_major_sync(stereo_major_sync());
    REQUIRE(bytes.has_value());
    std::vector<std::byte> broken = *bytes;
    broken[9] = std::byte{0x53};  // 0xB753, v3.0's value for TrueHD
    CHECK(parse_major_sync(broken, false).error().kind == ErrorKind::kBadSignature);
    CHECK(parse_major_sync(std::span<const std::byte>(bytes->data(), 20)).error().kind ==
          ErrorKind::kTruncated);
    broken = *bytes;
    broken[0] = std::byte{0};
    CHECK(parse_major_sync(broken).error().kind == ErrorKind::kBadSync);
}

TEST_CASE("an access unit parses into its header, directory and segment", "[truehd][io]") {
    const std::vector<std::byte> unit = stereo_unit(true, 0x4321);
    auto parsed = parse_access_unit(unit, 0);
    REQUIRE(parsed.has_value());
    CHECK(parsed->length_bytes() == unit.size());
    CHECK(parsed->input_timing == 0x4321);
    REQUIRE(parsed->major_sync.has_value());
    CHECK(parsed->num_substreams == 1);
    CHECK(parsed->entries[0].crc_present);
    CHECK_FALSE(parsed->entries[0].restart_nonexistent);
    CHECK(parsed->segments[0].size() == std::size_t{parsed->entries[0].end_ptr} * 2);
    CHECK_FALSE(parsed->extra_data.has_value());
    CHECK(access_unit_length_bytes(unit) == unit.size());
}

TEST_CASE("an access unit without a major sync needs the substream count", "[truehd][io]") {
    const std::vector<std::byte> unit = stereo_unit(false);
    CHECK(parse_access_unit(unit, 0).error().kind == ErrorKind::kNeedsRestart);
    auto parsed = parse_access_unit(unit, 1);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->major_sync.has_value());
    CHECK(parsed->entries[0].restart_nonexistent);
}

TEST_CASE("the check nibble makes the header and directory XOR to 0xF", "[truehd][io]") {
    std::vector<std::byte> unit = stereo_unit(false);
    unit[0] ^= std::byte{0x10};
    CHECK(parse_access_unit(unit, 1).error().kind == ErrorKind::kBadCheckNibble);
    CHECK(parse_access_unit(unit, 1, false).has_value());
}

TEST_CASE("an end pointer past the unit and a unit past the data are refused", "[truehd][io]") {
    std::vector<std::byte> unit = stereo_unit(false);
    CHECK(parse_access_unit(std::span<const std::byte>(unit).first(unit.size() - 2), 1)
              .error()
              .kind == ErrorKind::kTruncated);
    CHECK(parse_access_unit(std::span<const std::byte>(unit).first(3), 1).error().kind ==
          ErrorKind::kTruncated);

    SubstreamEntry entry;
    entry.end_ptr = 4000;
    const std::array<SubstreamEntry, 1> entries{entry};
    auto prefix = write_access_unit_prefix(std::nullopt, entries, 0, 8);
    REQUIRE(prefix.has_value());
    std::vector<std::byte> bad = *prefix;
    bad.resize(8);
    CHECK(parse_access_unit(bad, 1).error().kind == ErrorKind::kBadLength);
}

TEST_CASE("an entry with an extra word carries the DRC update", "[truehd][io]") {
    SubstreamEntry entry;
    entry.extra_substream_word = true;
    entry.end_ptr = 1;
    entry.drc_gain_update = -100;
    entry.drc_time_update = 5;
    const std::array<SubstreamEntry, 1> entries{entry};
    auto prefix = write_access_unit_prefix(std::nullopt, entries, 7, 10);
    REQUIRE(prefix.has_value());
    CHECK(prefix->size() == 4 + 4);
    std::vector<std::byte> unit = *prefix;
    unit.push_back(std::byte{0});
    unit.push_back(std::byte{0});
    auto parsed = parse_access_unit(unit, 1);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entries[0].extra_substream_word);
    CHECK(parsed->entries[0].drc_gain_update == -100);
    CHECK(parsed->entries[0].drc_time_update == 5);
}

TEST_CASE("bytes after the last segment are extra data, carried and checked", "[truehd][io]") {
    std::vector<std::byte> unit = stereo_unit(false);
    // The extra data: header word (length 2 words), the evolution framing for a 2-byte payload, a
    // pad byte, parity.
    const std::vector<std::byte> extra{std::byte{0x00 | 0x00}, std::byte{0x02}, std::byte{0x00},
                                       std::byte{0x02},        std::byte{0xAB}, std::byte{0xCD},
                                       std::byte{0x00},        std::byte{0x00}};
    // length 2 -> nibble = 0 ^ 0 ^ 2 ^ 0xF = 0xD: the header word's four nibbles XOR to 0xF
    // (TH spec §16.2)
    std::vector<std::byte> framed = extra;
    framed[0] = std::byte{0xD0};
    unsigned accumulator = 0 ^ 2 ^ 0xA9;
    for (std::size_t i = 4; i + 1 < framed.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(framed[i]);
    }
    framed.back() = static_cast<std::byte>(((accumulator >> 8) ^ accumulator) & 0xFF);
    unit.insert(unit.end(), framed.begin(), framed.end());
    // The length word grows by 4 words; the check nibble is recomputed from the header and
    // directory.
    const unsigned words = static_cast<unsigned>(unit.size() / 2);
    unit[0] = static_cast<std::byte>((words >> 8) & 0xF);
    unit[1] = static_cast<std::byte>(words & 0xFF);
    unsigned fold = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        fold ^= std::to_integer<unsigned>(unit[i]);
    }
    const unsigned nibble = 0xF ^ (((fold >> 4) ^ fold) & 0xF);
    unit[0] = static_cast<std::byte>((nibble << 4) | (words >> 8));

    auto parsed = parse_access_unit(unit, 1);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->extra_data.has_value());
    const ExtraData& data = *parsed->extra_data;
    CHECK(data.length == 2);
    CHECK(data.check_ok);
    CHECK(data.framing_ok);
    CHECK(data.evo_frame_byte_length == 2);
    CHECK(data.evolution_payload.size() == 2);
    CHECK(data.parity_ok);
    CHECK(data.bytes.size() == framed.size());
    CHECK_FALSE(data.padding_only);
}

TEST_CASE("an extra data header word is checked to XOR to 0xF and a zero word is padding",
          "[truehd][io]") {
    const auto unit_with = [](std::initializer_list<unsigned> bytes) {
        std::vector<std::byte> unit = stereo_unit(false);
        for (const unsigned b : bytes) {
            unit.push_back(static_cast<std::byte>(b));
        }
        const unsigned words = static_cast<unsigned>(unit.size() / 2);
        unit[0] = static_cast<std::byte>((words >> 8) & 0xF);
        unit[1] = static_cast<std::byte>(words & 0xFF);
        unsigned fold = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            fold ^= std::to_integer<unsigned>(unit[i]);
        }
        unit[0] |= static_cast<std::byte>((0xF ^ (((fold >> 4) ^ fold) & 0xF)) << 4);
        return unit;
    };
    // The nibble of the old formula, the XOR of the length's nibbles alone, is wrong now.
    {
        const std::vector<std::byte> unit = unit_with({0x20, 0x02, 0x00, 0x00, 0x00, 0x00});
        auto parsed = parse_access_unit(unit, 1);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->extra_data.has_value());
        CHECK_FALSE(parsed->extra_data->check_ok);
        CHECK_FALSE(parsed->extra_data->padding_only);
    }
    {
        const std::vector<std::byte> unit = unit_with({0xD0, 0x02, 0x00, 0x00, 0x00, 0x00});
        auto parsed = parse_access_unit(unit, 1);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->extra_data.has_value());
        CHECK(parsed->extra_data->check_ok);
        CHECK_FALSE(parsed->extra_data->padding_only);
    }
    // A zero header word is the padding-only form: exempt, and said so.
    {
        const std::vector<std::byte> unit = unit_with({0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        auto parsed = parse_access_unit(unit, 1);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->extra_data.has_value());
        CHECK(parsed->extra_data->padding_only);
        CHECK(parsed->extra_data->check_ok);
        CHECK(parsed->extra_data->length == 0);
    }
    // A zero nibble with a length, and a zero length with the nibble 0xF, are not padding.
    {
        const std::vector<std::byte> unit = unit_with({0x00, 0x02, 0x00, 0x00, 0x00, 0x00});
        auto parsed = parse_access_unit(unit, 1);
        REQUIRE(parsed->extra_data.has_value());
        CHECK_FALSE(parsed->extra_data->padding_only);
        CHECK_FALSE(parsed->extra_data->check_ok);
        const std::vector<std::byte> full = unit_with({0xF0, 0x00, 0x00, 0x00, 0x00, 0x00});
        auto other = parse_access_unit(full, 1);
        REQUIRE(other->extra_data.has_value());
        CHECK_FALSE(other->extra_data->padding_only);
        CHECK(other->extra_data->check_ok);
    }
}
