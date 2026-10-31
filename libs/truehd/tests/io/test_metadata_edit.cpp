#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "support/channel_meaning_support.hpp"

// Changing the channel meaning of a finished stream (TH spec §14.1) without touching any other
// byte: every major sync is rewritten and re-stamped, the audio is the same, and an edit that
// cannot be made says why and changes nothing. Provisional, no oracle until the DEE set (ERRATA.md
// questions 51 to 55).

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace fs = std::filesystem;

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("an edit gives the stream the encoder would have written", "[truehd][io][metadata]") {
    const ChannelMeaning meaning = distinct_meaning();
    for (const int channels : {2, 6, 8}) {
        DYNAMIC_SECTION(channels << " channels") {
            std::vector<std::byte> stream = encode_stream(channels, 8, ChannelMeaning{});
            const std::vector<std::byte> original = stream;
            const std::vector<Sync> before = major_syncs(stream);

            const auto summary = edit_channel_meaning(stream, edit_of(meaning));
            REQUIRE(summary.has_value());
            CHECK(summary->major_syncs == before.size());
            CHECK(summary->changed == before.size());
            CHECK(summary->distinct_before == 1);
            CHECK(summary->before == ChannelMeaning{});
            CHECK(summary->after == meaning);
            CHECK(summary->skipped_bytes == 0);

            CHECK(stream == encode_stream(channels, 8, meaning));  // byte for byte
            CHECK(differences_only_in_meaning_and_crc(original, stream));
            CHECK(decode_lenient(stream) == decode_lenient(original));
        }
    }
    SECTION("only the fields named change, in every major sync") {
        std::vector<std::byte> stream = encode_stream(6, 8, meaning);
        ChannelMeaningEdit edit;
        edit.sixch_dialogue_norm = 14;
        edit.sixch_control_enabled = true;
        const auto summary = edit_channel_meaning(stream, edit);
        REQUIRE(summary.has_value());
        ChannelMeaning expected = meaning;
        expected.sixch_dialogue_norm = 14;
        expected.sixch_control_enabled = true;
        for (const Sync& sync : major_syncs(stream)) {
            CHECK(sync.parsed.channel_meaning == expected);
        }
        CHECK(summary->after == expected);
    }
    SECTION("an edit of the values the stream has changes no byte") {
        std::vector<std::byte> stream = encode_stream(6, 8, meaning);
        const std::vector<std::byte> original = stream;
        const auto summary = edit_channel_meaning(stream, edit_of(meaning));
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 0);
        CHECK(stream == original);
    }
    SECTION("an edit with nothing set changes nothing and reports") {
        std::vector<std::byte> stream = encode_stream(2, 8, meaning);
        const std::vector<std::byte> original = stream;
        const auto summary = edit_channel_meaning(stream, ChannelMeaningEdit{});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 0);
        CHECK(summary->before == meaning);
        CHECK(stream == original);
    }
}

TEST_CASE("an edit keeps what is not the channel meaning, and finds every major sync",
          "[truehd][io][metadata]") {
    SECTION("bytes before the first unit and a stream of two meanings") {
        ChannelMeaning first = distinct_meaning();
        ChannelMeaning second = distinct_meaning();
        second.twoch_dialogue_norm = 2;
        second.eightch_mix_level = 1;
        std::vector<std::byte> stream(7, std::byte{0x5A});  // not a unit: left as it is
        const std::vector<std::byte> a = encode_stream(2, 8, first, 4800);
        const std::vector<std::byte> b = encode_stream(2, 8, second, 4800);
        stream.insert(stream.end(), a.begin(), a.end());
        stream.insert(stream.end(), b.begin(), b.end());
        const std::vector<std::byte> original = stream;

        ChannelMeaningEdit edit;
        edit.sixch_dialogue_norm = 3;
        const auto summary = edit_channel_meaning(stream, edit);
        REQUIRE(summary.has_value());
        CHECK(summary->distinct_before == 2);
        CHECK(summary->skipped_bytes >= 7);
        for (std::size_t i = 0; i < 7; ++i) {
            CHECK(stream[i] == std::byte{0x5A});
        }
        std::size_t seen_first = 0;
        std::size_t seen_second = 0;
        for (const Sync& sync : major_syncs(stream)) {
            CHECK(sync.parsed.channel_meaning.sixch_dialogue_norm == 3);
            const bool is_first =
                sync.parsed.channel_meaning.twoch_dialogue_norm == first.twoch_dialogue_norm;
            (is_first ? seen_first : seen_second)++;
        }
        CHECK(seen_first > 0);
        CHECK(seen_second > 0);
        CHECK(differences_only_in_meaning_and_crc(original, stream));
    }
    SECTION("a stream in which a major sync is damaged leaves that unit alone") {
        std::vector<std::byte> stream = encode_stream(2, 8, ChannelMeaning{}, 4800);
        const std::vector<Sync> syncs = major_syncs(stream);
        REQUIRE(syncs.size() > 3);
        stream[syncs[2].offset + 12] ^= std::byte{0x01};  // the CRC no longer holds
        const std::vector<std::byte> original = stream;
        const auto summary = edit_channel_meaning(stream, edit_of(distinct_meaning()));
        REQUIRE(summary.has_value());
        CHECK(summary->skipped_bytes > 0);
        // Everything the reader could not hand out as a unit is as it was.
        CHECK(std::equal(original.begin() + static_cast<std::ptrdiff_t>(syncs[2].offset),
                         original.begin() + static_cast<std::ptrdiff_t>(syncs[2].offset + 28),
                         stream.begin() + static_cast<std::ptrdiff_t>(syncs[2].offset)));
    }
}

TEST_CASE("an edit that cannot be made says why and changes nothing", "[truehd][io][metadata]") {
    SECTION("a value its field cannot hold") {
        std::vector<std::byte> stream = encode_stream(2, 8, ChannelMeaning{}, 4800);
        const std::vector<std::byte> original = stream;
        ChannelMeaningEdit edit;
        edit.twoch_dialogue_norm = 20;  // fits
        edit.sixch_dialogue_norm = 32;  // does not: five bits
        const auto summary = edit_channel_meaning(stream, edit);
        REQUIRE_FALSE(summary.has_value());
        CHECK(summary.error().kind == ErrorKind::kBadParameter);
        CHECK(std::string_view{summary.error().detail}.starts_with("sixch_dialogue_norm"));
        CHECK(stream == original);
    }
    SECTION("bytes with no major sync in them") {
        std::vector<std::byte> junk(300, std::byte{0x11});
        const auto summary = edit_channel_meaning(junk, edit_of(distinct_meaning()));
        REQUIRE_FALSE(summary.has_value());
        CHECK(summary.error().kind == ErrorKind::kBadSync);
    }
    SECTION("an MLP stream, whose bytes after substream_info are not described") {
        const fs::path mlp = fs::path{ICLFORGE_GOLDEN_TRUEHD_DIR} / "mlp_st48_s24.mlp";
        std::vector<std::byte> stream = read_file(mlp);
        const std::vector<std::byte> original = stream;
        const auto summary = edit_channel_meaning(stream, edit_of(distinct_meaning()));
        REQUIRE_FALSE(summary.has_value());
        CHECK(summary.error().kind == ErrorKind::kUnsupported);
        CHECK(stream == original);
    }
}

TEST_CASE("an edit of every committed TrueHD stream changes only the channel meaning and CRCs",
          "[truehd][io][metadata][fixtures]") {
    // FFmpeg's streams (every rate, layout and restart spacing it writes) and the hand-built ones
    // (the 16-channel substream, extra data, noise), whatever their size of major sync.
    const fs::path root{ICLFORGE_GOLDEN_TRUEHD_DIR};
    std::vector<fs::path> files;
    for (const fs::path& dir : {root, root / "handbuilt"}) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".thd") {
                files.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(files);
    REQUIRE(files.size() >= 30);
    const ChannelMeaning meaning = distinct_meaning();
    for (const fs::path& file : files) {
        DYNAMIC_SECTION(file.filename().string()) {
            std::vector<std::byte> stream = read_file(file);
            const std::vector<std::byte> original = stream;
            const auto summary = edit_channel_meaning(stream, edit_of(meaning));
            if (!summary) {
                // A hand-built MLP stream (its bytes after substream_info are not described) or a
                // hand-built stream whose first unit does not read (the manifest's refusals).
                const bool mlp = file.filename().string().starts_with("hb_mlp");
                CHECK(summary.error().kind == (mlp ? ErrorKind::kUnsupported : ErrorKind::kBadSync));
                CHECK(stream == original);
                continue;
            }
            CHECK(summary->changed == summary->major_syncs);
            const std::vector<Sync> before = major_syncs(original);
            const std::vector<Sync> syncs = major_syncs(stream);
            REQUIRE(syncs.size() == summary->major_syncs);
            REQUIRE(before.size() == syncs.size());
            for (std::size_t i = 0; i < syncs.size(); ++i) {
                // The fields named are the edit's; the reserved bits and the extension flag, which
                // an edit does not name, are the stream's.
                ChannelMeaning expected = before[i].parsed.channel_meaning;
                REQUIRE(apply_edit(edit_of(meaning), expected).empty());
                CHECK(syncs[i].parsed.channel_meaning == expected);
                CHECK(syncs[i].parsed.size_bytes == before[i].parsed.size_bytes);
                CHECK(syncs[i].parsed.extra == before[i].parsed.extra);
            }
            CHECK(differences_only_in_meaning_and_crc(original, stream));
            CHECK(decode_lenient(stream) == decode_lenient(original));
        }
    }
}
