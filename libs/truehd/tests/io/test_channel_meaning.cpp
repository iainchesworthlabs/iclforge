#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "support/channel_meaning_support.hpp"

// The channel meaning of TH spec §14.1: packed as the specification lays it out, refused by field
// name when a value does not fit, and written by the encoder into every major sync. Provisional, no
// oracle until the DEE set (ERRATA.md questions 51 to 55): FFmpeg's streams hold zeros, so what
// these tests hold is the layout the specification gives, the checks this library makes of its own
// streams, and that the audio does not change.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

struct Field {
    std::string_view name;
    int low;
    int high;
    std::function<void(ChannelMeaning&, int)> set;
    std::function<void(ChannelMeaningEdit&, int)> edit;
};

const std::vector<Field>& fields() {
    static const std::vector<Field> all{
        {"heavy_drc_start_up_gain", -32, 31,
         [](ChannelMeaning& m, int v) { m.heavy_drc_start_up_gain = static_cast<std::int8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.heavy_drc_start_up_gain = v; }},
        {"drc_start_up_gain", -64, 63,
         [](ChannelMeaning& m, int v) { m.drc_start_up_gain = static_cast<std::int8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.drc_start_up_gain = v; }},
        {"twoch_dialogue_norm", 0, 63,
         [](ChannelMeaning& m, int v) { m.twoch_dialogue_norm = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.twoch_dialogue_norm = v; }},
        {"twoch_mix_level", 0, 63,
         [](ChannelMeaning& m, int v) { m.twoch_mix_level = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.twoch_mix_level = v; }},
        {"sixch_dialogue_norm", 0, 31,
         [](ChannelMeaning& m, int v) { m.sixch_dialogue_norm = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.sixch_dialogue_norm = v; }},
        {"sixch_mix_level", 0, 63,
         [](ChannelMeaning& m, int v) { m.sixch_mix_level = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.sixch_mix_level = v; }},
        {"sixch_source_format", 0, 31,
         [](ChannelMeaning& m, int v) { m.sixch_source_format = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.sixch_source_format = v; }},
        {"eightch_dialogue_norm", 0, 31,
         [](ChannelMeaning& m, int v) { m.eightch_dialogue_norm = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.eightch_dialogue_norm = v; }},
        {"eightch_mix_level", 0, 63,
         [](ChannelMeaning& m, int v) { m.eightch_mix_level = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.eightch_mix_level = v; }},
        {"eightch_source_format", 0, 63,
         [](ChannelMeaning& m, int v) { m.eightch_source_format = static_cast<std::uint8_t>(v); },
         [](ChannelMeaningEdit& e, int v) { e.eightch_source_format = v; }},
    };
    return all;
}

}  // namespace

TEST_CASE("the channel meaning packs into the 64 bits of TH spec 14.1, field by field",
          "[truehd][io][metadata]") {
    // The layout written out again, independently: widths and order from the specification.
    const ChannelMeaning meaning = distinct_meaning();
    std::uint64_t expected = 0;
    int used = 0;
    const auto put = [&](std::uint64_t value, int bits) {
        expected = (expected << bits) | (value & ((std::uint64_t{1} << bits) - 1U));
        used += bits;
    };
    put(static_cast<std::uint64_t>(static_cast<std::int64_t>(meaning.heavy_drc_start_up_gain)), 6);
    put(1, 1);  // twoch_control_enabled
    put(0, 1);  // sixch_control_enabled
    put(1, 1);  // eightch_control_enabled
    put(0, 1);  // reserved
    put(static_cast<std::uint64_t>(static_cast<std::int64_t>(meaning.drc_start_up_gain)), 7);
    put(meaning.twoch_dialogue_norm, 6);
    put(meaning.twoch_mix_level, 6);
    put(meaning.sixch_dialogue_norm, 5);
    put(meaning.sixch_mix_level, 6);
    put(meaning.sixch_source_format, 5);
    put(meaning.eightch_dialogue_norm, 5);
    put(meaning.eightch_mix_level, 6);
    put(meaning.eightch_source_format, 6);
    put(0, 1);  // reserved
    put(0, 1);  // extra_channel_meaning_present
    REQUIRE(used == kChannelMeaningBits);

    const std::array<std::uint8_t, 8> packed = pack_channel_meaning(meaning);
    for (std::size_t i = 0; i < packed.size(); ++i) {
        CHECK(packed[i] == static_cast<std::uint8_t>((expected >> (8 * (7 - i))) & 0xFFU));
    }

    SECTION("and the major sync reads it back") {
        MajorSync major_sync;
        major_sync.format_info = 0x1F000001;
        major_sync.channel_meaning = meaning;
        const auto bytes = write_major_sync(major_sync);
        REQUIRE(bytes.has_value());
        for (std::size_t i = 0; i < packed.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>((*bytes)[kMeaningAt + i]) == packed[i]);
        }
        const auto read = parse_major_sync(*bytes);
        REQUIRE(read.has_value());
        CHECK(read->channel_meaning == meaning);
    }
}

TEST_CASE("a channel meaning field its width cannot hold is refused by name",
          "[truehd][io][metadata]") {
    for (const Field& field : fields()) {
        DYNAMIC_SECTION(field.name) {
            ChannelMeaning meaning;
            // The ends of the range are the field's.
            for (const int value : {field.low, field.high}) {
                field.set(meaning, value);
                CHECK(channel_meaning_problem(meaning).empty());
            }
            // One past either end is not, and the sentence names the field.
            for (const int value : {field.low - 1, field.high + 1}) {
                if (value < 0 && field.low == 0) {
                    continue;  // an unsigned field cannot be handed a negative number
                }
                ChannelMeaning wide;
                field.set(wide, value);
                const std::string_view problem = channel_meaning_problem(wide);
                CHECK(problem.starts_with(field.name));

                MajorSync major_sync;
                major_sync.channel_meaning = wide;
                const auto written = write_major_sync(major_sync);
                REQUIRE_FALSE(written.has_value());
                CHECK(written.error().kind == ErrorKind::kBadParameter);
                CHECK(written.error().detail == problem);

                const auto encoder = Encoder::create(config_of(2, 128, wide));
                REQUIRE_FALSE(encoder.has_value());
                CHECK(encoder.error().kind == EncodeErrorKind::kInvalidConfig);
                CHECK(encoder.error().detail == problem);

                ChannelMeaningEdit edit;
                ChannelMeaning target;
                field.edit(edit, value);
                CHECK(apply_edit(edit, target) == problem);
                CHECK(target == ChannelMeaning{});  // a refused edit sets nothing
            }
        }
    }
    SECTION("the sixteen-channel extension is not written, so the encoder refuses to announce it") {
        ChannelMeaning meaning;
        meaning.extra_present = true;
        const auto encoder = Encoder::create(config_of(2, 128, meaning));
        REQUIRE_FALSE(encoder.has_value());
        CHECK(
            std::string_view{encoder.error().detail}.starts_with("channel_meaning.extra_present"));
    }
}

TEST_CASE("the encoder writes the channel meaning into every major sync and nothing else changes",
          "[truehd][encoder][metadata]") {
    for (const int channels : {2, 6, 8}) {
        DYNAMIC_SECTION(channels << " channels") {
            const ChannelMeaning meaning = distinct_meaning();
            const std::vector<std::byte> plain = encode_stream(channels, 8, ChannelMeaning{});
            const std::vector<std::byte> written = encode_stream(channels, 8, meaning);

            const std::vector<Sync> syncs = major_syncs(written);
            REQUIRE(syncs.size() > 10);
            for (const Sync& sync : syncs) {
                CHECK(sync.parsed.channel_meaning == meaning);
            }
            // The default is what the encoder always wrote, and what FFmpeg's streams hold.
            for (const Sync& sync : major_syncs(plain)) {
                CHECK(sync.parsed.channel_meaning == ChannelMeaning{});
                CHECK(sync.parsed.channel_meaning_bytes == std::array<std::uint8_t, 8>{});
            }
            // The only bytes that differ are the channel meaning and the CRC of each major sync.
            CHECK(differences_only_in_meaning_and_crc(plain, written));
            // And the audio is the same.
            CHECK(decode_lenient(plain) == decode_lenient(written));
        }
    }
}
