#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "encoder/encoder_support.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"

// What the channel meaning's tests share (TH spec §14.1): a meaning with a value of its own in
// every field, streams the encoder writes with a major sync every few access units, the major syncs
// of a stream read with their CRC checked, and a comparison of two streams that shows where they
// differ.

namespace iclforge::truehd::testing {

inline constexpr std::size_t kUnitHeader = 4;
inline constexpr std::size_t kMeaningAt = 18;
inline constexpr std::size_t kMeaningBytes = 8;

// Every field set to a value of its own, so that a field written in another's place shows.
inline ChannelMeaning distinct_meaning() {
    ChannelMeaning meaning;
    meaning.heavy_drc_start_up_gain = -9;
    meaning.twoch_control_enabled = true;
    meaning.sixch_control_enabled = false;
    meaning.eightch_control_enabled = true;
    meaning.drc_start_up_gain = 21;
    meaning.twoch_dialogue_norm = 40;
    meaning.twoch_mix_level = 33;
    meaning.sixch_dialogue_norm = 27;
    meaning.sixch_mix_level = 17;
    meaning.sixch_source_format = 5;
    meaning.eightch_dialogue_norm = 24;
    meaning.eightch_mix_level = 49;
    meaning.eightch_source_format = 38;
    return meaning;
}

inline ChannelMeaningEdit edit_of(const ChannelMeaning& meaning) {
    ChannelMeaningEdit edit;
    edit.heavy_drc_start_up_gain = meaning.heavy_drc_start_up_gain;
    edit.drc_start_up_gain = meaning.drc_start_up_gain;
    edit.twoch_control_enabled = meaning.twoch_control_enabled;
    edit.sixch_control_enabled = meaning.sixch_control_enabled;
    edit.eightch_control_enabled = meaning.eightch_control_enabled;
    edit.twoch_dialogue_norm = meaning.twoch_dialogue_norm;
    edit.twoch_mix_level = meaning.twoch_mix_level;
    edit.sixch_dialogue_norm = meaning.sixch_dialogue_norm;
    edit.sixch_mix_level = meaning.sixch_mix_level;
    edit.sixch_source_format = meaning.sixch_source_format;
    edit.eightch_dialogue_norm = meaning.eightch_dialogue_norm;
    edit.eightch_mix_level = meaning.eightch_mix_level;
    edit.eightch_source_format = meaning.eightch_source_format;
    return edit;
}

inline std::vector<std::byte> join(const std::vector<EncodedAccessUnit>& units) {
    std::vector<std::byte> stream;
    for (const EncodedAccessUnit& unit : units) {
        stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
    }
    return stream;
}

inline EncoderConfig config_of(int channels, int restart_units, const ChannelMeaning& meaning) {
    EncoderConfig config;
    config.channels = channels;
    config.restart_interval_units = restart_units;
    config.channel_meaning = meaning;
    return config;
}

// A stream of `channels` channels with a restart (and so a major sync) every `restart_units` units.
inline std::vector<std::byte> encode_stream(int channels, int restart_units,
                                            const ChannelMeaning& meaning, int frames = 9600) {
    const std::vector<std::int32_t> pcm = make_pcm(channels, frames, 24, 11);
    return join(encode_all(config_of(channels, restart_units, meaning), pcm).units);
}

struct Sync {
    std::size_t offset = 0;  // of the sync word
    MajorSync parsed;
};

// Every major sync of the stream, each read with its CRC checked.
inline std::vector<Sync> major_syncs(std::span<const std::byte> stream) {
    std::vector<Sync> out;
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        if (unit->size() >= kUnitHeader + 4 && starts_with_major_sync(unit->subspan(kUnitHeader))) {
            const std::size_t offset =
                static_cast<std::size_t>(unit->data() - stream.data()) + kUnitHeader;
            const auto parsed = parse_major_sync(stream.subspan(offset), true);
            REQUIRE(parsed.has_value());
            out.push_back(Sync{offset, *parsed});
        }
    }
    return out;
}

// The offsets at which two streams of one length differ.
inline std::vector<std::size_t> differences(std::span<const std::byte> a,
                                            std::span<const std::byte> b) {
    REQUIRE(a.size() == b.size());
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            out.push_back(i);
        }
    }
    return out;
}

// True when every difference lies in the channel meaning or the CRC of a major sync of `stream`.
inline bool differences_only_in_meaning_and_crc(std::span<const std::byte> before,
                                                std::span<const std::byte> after) {
    const std::vector<Sync> syncs = major_syncs(after);
    return std::ranges::all_of(differences(before, after), [&](std::size_t at) {
        return std::ranges::any_of(syncs, [&](const Sync& sync) {
            const std::size_t end = sync.offset + sync.parsed.size_bytes;
            const bool in_meaning =
                at >= sync.offset + kMeaningAt && at < sync.offset + kMeaningAt + kMeaningBytes;
            const bool in_crc = at >= end - 2 && at < end;
            return in_meaning || in_crc;
        });
    });
}

inline std::vector<std::int32_t> decode_lenient(std::span<const std::byte> stream) {
    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    Decoder decoder(config);
    std::vector<std::int32_t> pcm;
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        if (decoded && *decoded) {
            pcm.insert(pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
        }
    }
    return pcm;
}

}  // namespace iclforge::truehd::testing
