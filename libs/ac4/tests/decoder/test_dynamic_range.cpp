// What a listener's choice of dynamic range does to the AC-4 decoder
// (iclforge/ac4/decoder/dynamic_range.hpp): standard is the decoder as a board makes it, to the bit;
// late night compresses with a DRC decoder mode the stream configures, at the level the stream's own
// dialnorm gives, so the distance between its loudest and quietest passages shrinks and the level it
// is heard at does not move.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/decoder/dynamic_range.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "sanitized.hpp"

namespace {

using iclforge::base::DynamicRange;

std::vector<std::byte> read_stream(const std::string& leg) {
    const std::filesystem::path path =
        std::filesystem::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

struct Played {
    std::vector<std::vector<float>> channels;
    std::vector<std::size_t> frame_starts;  // each frame's first sample
    int output_changes = 0;                 // times follow_dynamic_range set the output
};

// A board's way of playing a stream: the decoder made with the base output, the choice followed
// after each frame.
Played play(std::span<const std::span<const std::byte>> frames, DynamicRange range) {
    const iclforge::ac4::OutputConfig base{};
    iclforge::ac4::Decoder decoder(iclforge::ac4::DecoderConfig{.syntax = {}, .output = base});
    Played out;
    for (const std::span<const std::byte> frame : frames) {
        const auto decoded = decoder.decode(frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        out.channels.resize(pcm.channels.size());
        out.frame_starts.push_back(out.channels.empty() ? 0 : out.channels[0].size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
        if (iclforge::ac4::follow_dynamic_range(decoder, range, base)) {
            ++out.output_changes;
        }
    }
    return out;
}

// The mean power, in dB, of every 100 ms window over all channels, the silent ones left out: the
// loudest and the quietest of them are the distance late night narrows.
struct Windows {
    double loudest = -200.0;
    double quietest = 200.0;
    double median = 0.0;
    [[nodiscard]] double range() const { return loudest - quietest; }
};

Windows windows_of(const Played& played) {
    constexpr std::size_t kWindow = 4800;
    std::vector<double> levels;
    const std::size_t samples = played.channels.empty() ? 0 : played.channels[0].size();
    for (std::size_t start = 8192; start + kWindow <= samples; start += kWindow) {
        double power = 0.0;
        for (const auto& channel : played.channels) {
            for (std::size_t i = start; i < start + kWindow; ++i) {
                power += static_cast<double>(channel[i]) * static_cast<double>(channel[i]);
            }
        }
        const double db = 10.0 * std::log10(std::max(power / static_cast<double>(kWindow), 1e-30));
        if (db > -70.0) {
            levels.push_back(db);
        }
    }
    Windows out;
    REQUIRE_FALSE(levels.empty());
    for (const double level : levels) {
        out.loudest = std::max(out.loudest, level);
        out.quietest = std::min(out.quietest, level);
    }
    std::ranges::sort(levels);
    out.median = levels[levels.size() / 2];
    return out;
}

iclforge::ac4::PresentationMetadata metadata_with(std::initializer_list<int> mode_ids, std::optional<double> dialnorm) {
    iclforge::ac4::PresentationMetadata metadata;
    metadata.loudness.dialnorm_dbfs = dialnorm;
    iclforge::ac4::DrcInfo drc;
    for (const int id : mode_ids) {
        iclforge::ac4::DrcModeInfo mode;
        mode.id = id;
        drc.modes.push_back(mode);
    }
    metadata.drc = drc;
    return metadata;
}

}  // namespace

TEST_CASE("AC-4 late night: the mode is the strongest speaker mode the stream configures",
          "[dynamic-range][ac4][decoder]") {
    using iclforge::ac4::DrcMode;
    CHECK(iclforge::ac4::late_night_drc_mode(metadata_with({0, 1, 2, 3}, -18.0)) == DrcMode::kPortableSpeakers);
    CHECK(iclforge::ac4::late_night_drc_mode(metadata_with({0, 1, 3}, -18.0)) == DrcMode::kFlatPanelTv);
    CHECK(iclforge::ac4::late_night_drc_mode(metadata_with({0, 3}, -18.0)) == DrcMode::kHomeTheatre);
    // Headphones alone are not a speaker's mode, and the output-level modes (4 to 7) are not asked for.
    CHECK_FALSE(iclforge::ac4::late_night_drc_mode(metadata_with({3}, -18.0)).has_value());
    CHECK_FALSE(iclforge::ac4::late_night_drc_mode(metadata_with({4, 5}, -18.0)).has_value());
    CHECK_FALSE(iclforge::ac4::late_night_drc_mode(iclforge::ac4::PresentationMetadata{}).has_value());
}

TEST_CASE("AC-4 late night: the output is the base until the stream gives a dialnorm and a mode",
          "[dynamic-range][ac4][decoder]") {
    const iclforge::ac4::OutputConfig base{.downmix = iclforge::ac4::DownmixTarget::kStereo, .mix_lfe = false};
    using iclforge::ac4::output_for_dynamic_range;

    // Standard is the base, whatever the stream says.
    const auto standard = output_for_dynamic_range(DynamicRange::kStandard, base, metadata_with({0, 1, 2, 3}, -18.0));
    CHECK_FALSE(standard.output_level_dbfs.has_value());
    CHECK(standard.drc == base.drc);

    // Late night with no dialnorm yet, or no mode to use, is too.
    CHECK_FALSE(output_for_dynamic_range(DynamicRange::kLateNight, base, metadata_with({0, 1, 2, 3}, std::nullopt))
                    .output_level_dbfs.has_value());
    CHECK_FALSE(output_for_dynamic_range(DynamicRange::kLateNight, base, metadata_with({3}, -18.0))
                    .output_level_dbfs.has_value());

    // With both: the stream's own dialnorm is the output level (a gain of exactly 1), the mode is set,
    // and every other field is the base's.
    const auto late = output_for_dynamic_range(DynamicRange::kLateNight, base, metadata_with({0, 1, 2, 3}, -18.25));
    CHECK(late.output_level_dbfs == -18.25);
    CHECK(late.drc == iclforge::ac4::DrcMode::kPortableSpeakers);
    CHECK(late.downmix == base.downmix);
    CHECK(late.mix_lfe == base.mix_lfe);
}

TEST_CASE("AC-4 late night: standard is the decode a board made before the choice existed",
          "[dynamic-range][ac4][decoder]") {
    const std::vector<std::byte> stream = read_stream("ac4-51-drc-ltrt-192");
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    std::vector<std::span<const std::byte>> raw;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        raw.push_back(frame.raw_ac4_frame);
    }
    raw.resize(std::min<std::size_t>(raw.size(), 24));

    // The decoder as a board makes it, with nothing followed.
    iclforge::ac4::Decoder plain(iclforge::ac4::DecoderConfig{});
    const Played standard = play(raw, DynamicRange::kStandard);
    std::vector<std::vector<float>> reference;
    for (const std::span<const std::byte> frame : raw) {
        const auto decoded = plain.decode(frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        reference.resize((*decoded)->channels.size());
        for (std::size_t c = 0; c < (*decoded)->channels.size(); ++c) {
            reference[c].insert(reference[c].end(), (*decoded)->channels[c].begin(), (*decoded)->channels[c].end());
        }
    }
    CHECK(standard.output_changes == 0);
    CHECK(standard.channels == reference);
}

TEST_CASE("AC-4 late night: DEE's 5.1 stream is compressed and heard at the level it was",
          "[dynamic-range][ac4][decoder]") {
    // Made with the home theatre mode as Music light and portable headphones as Speech, and the
    // default profile for flat-panel TV and portable speakers (gen_ac4_baseline.py,
    // ac4-51-drc-ltrt-192); dialnorm -18 dBFS. Fifty 100 ms windows span 15 dB as coded.
    const std::vector<std::byte> stream = read_stream("ac4-51-drc-ltrt-192");
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    std::vector<std::span<const std::byte>> raw;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        raw.push_back(frame.raw_ac4_frame);
    }
    if (iclforge::test::kSanitized) {
        raw.resize(std::min<std::size_t>(raw.size(), 72));
    }

    const Played standard = play(raw, DynamicRange::kStandard);
    const Played late = play(raw, DynamicRange::kLateNight);
    REQUIRE(standard.channels.size() == late.channels.size());
    REQUIRE(standard.channels[0].size() == late.channels[0].size());

    // The first frame plays as coded: the dialnorm that sets the output level comes with it.
    CHECK(late.output_changes == 1);
    const std::size_t first = late.frame_starts.size() > 1 ? late.frame_starts[1] : 0;
    REQUIRE(first > 0);
    for (std::size_t c = 0; c < late.channels.size(); ++c) {
        CHECK(std::equal(standard.channels[c].begin(), standard.channels[c].begin() + static_cast<std::ptrdiff_t>(first),
                         late.channels[c].begin()));
    }

    // Something changed after the first frame, and the level the stream is heard at did not: a 13 dB
    // drop (an output level of -31 dBFS on a stream whose dialnorm is -18) would be the trap.
    CHECK(standard.channels != late.channels);
    const Windows before = windows_of(standard);
    const Windows after = windows_of(late);
    CHECK(std::abs(after.median - before.median) < 3.0);
    CHECK(after.range() <= before.range());
}

namespace {

// Steps of a 997 Hz tone between two levels, two channels, `step_seconds` at each.
std::vector<std::span<const std::byte>> stepped_stream(double dialnorm, double loud, double quiet, double step_seconds,
                                                       int steps, std::vector<iclforge::ac4::EncodedFrame>& keep) {
    const auto step_samples = static_cast<std::size_t>(step_seconds * 48000.0);
    std::vector<std::vector<float>> input(2, std::vector<float>(step_samples * static_cast<std::size_t>(steps)));
    for (std::size_t n = 0; n < input[0].size(); ++n) {
        const double amplitude = (n / step_samples) % 2 == 0 ? loud : quiet;
        const auto x = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(n) / 48000.0));
        input[0][n] = x;
        input[1][n] = x;
    }
    iclforge::ac4::EncoderConfig config;
    config.dialnorm_db = dialnorm;
    config.drc = iclforge::ac4::DrcConfig{.profile = iclforge::ac4::DrcProfile::kFilmStandard};
    auto encoder = iclforge::ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<std::span<const float>> views = {input[0], input[1]};
    auto encoded = encoder->encode(views);
    REQUIRE(encoded.has_value());
    keep.insert(keep.end(), encoded->begin(), encoded->end());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    keep.insert(keep.end(), rest->begin(), rest->end());
    std::vector<std::span<const std::byte>> raw;
    for (const iclforge::ac4::EncodedFrame& frame : keep) {
        raw.emplace_back(frame.raw_ac4_frame);
    }
    return raw;
}

double rms_between_db(const Played& played, double from_s, double to_s) {
    const auto first = static_cast<std::size_t>(from_s * 48000.0);
    const auto last = std::min(static_cast<std::size_t>(to_s * 48000.0), played.channels[0].size());
    REQUIRE(first < last);
    double power = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        power += static_cast<double>(played.channels[0][i]) * static_cast<double>(played.channels[0][i]);
    }
    return 10.0 * std::log10(std::max(power / static_cast<double>(last - first), 1e-30));
}

}  // namespace

TEST_CASE("AC-4 late night: a loud passage comes down, a quiet one comes up, at the stream's own level",
          "[dynamic-range][ac4][decoder]") {
    // Four seconds loud, four quiet, four loud, in a stream that sends Table 161's four modes on the
    // film standard profile at dialnorm -24 dBFS: the settled level of each step is read in its last
    // half second, after the curve's release has run.
    std::vector<iclforge::ac4::EncodedFrame> keep;
    const auto raw = stepped_stream(-24.0, 0.25, 0.0125, 4.0, 3, keep);
    const Played standard = play(raw, DynamicRange::kStandard);
    const Played late = play(raw, DynamicRange::kLateNight);
    REQUIRE(late.output_changes >= 1);

    const double standard_loud = rms_between_db(standard, 3.4, 3.9);
    const double standard_quiet = rms_between_db(standard, 7.4, 7.9);
    const double late_loud = rms_between_db(late, 3.4, 3.9);
    const double late_quiet = rms_between_db(late, 7.4, 7.9);
    INFO("standard " << standard_loud << " / " << standard_quiet << ", late night " << late_loud << " / " << late_quiet);

    // As coded the steps are 26 dB apart.
    CHECK(std::abs((standard_loud - standard_quiet) - 26.0) < 0.5);
    CHECK(late_loud < standard_loud - 1.0);
    CHECK(late_quiet > standard_quiet + 1.0);
    CHECK((late_loud - late_quiet) < (standard_loud - standard_quiet) - 5.0);
}

TEST_CASE("AC-4 late night: a new stream does not start at the last stream's output level",
          "[dynamic-range][ac4][decoder]") {
    const std::vector<std::byte> stream = read_stream("ac4-51-drc-ltrt-192");
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    std::vector<std::span<const std::byte>> raw;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        raw.push_back(frame.raw_ac4_frame);
    }
    raw.resize(std::min<std::size_t>(raw.size(), 12));

    const iclforge::ac4::OutputConfig base{};
    const auto first_frame_of = [&](iclforge::ac4::Decoder& decoder) {
        const auto decoded = decoder.decode(raw[0]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        return (**decoded).channels;
    };

    iclforge::ac4::Decoder fresh(iclforge::ac4::DecoderConfig{.syntax = {}, .output = base});
    const auto reference = first_frame_of(fresh);

    // A stream played in late night leaves the decoder at its dialnorm and mode ...
    iclforge::ac4::Decoder decoder(iclforge::ac4::DecoderConfig{.syntax = {}, .output = base});
    for (const std::span<const std::byte> frame : raw) {
        REQUIRE(decoder.decode(frame).has_value());
        (void)iclforge::ac4::follow_dynamic_range(decoder, DynamicRange::kLateNight, base);
    }
    REQUIRE(decoder.output().output_level_dbfs.has_value());

    // ... and the next stream's first frame is the one a fresh decoder plays, once it is back to the base.
    decoder.reset();
    CHECK(iclforge::ac4::restore_base_output(decoder, base));
    CHECK_FALSE(decoder.output().output_level_dbfs.has_value());
    CHECK(first_frame_of(decoder) == reference);
    // Standard never leaves the base, so there is nothing to restore.
    CHECK_FALSE(iclforge::ac4::restore_base_output(decoder, base));
}

TEST_CASE("AC-4 late night: what the DRC stage costs on the host", "[.][ac4][cost]") {
    // Hidden: a measurement, not a check. DEE's 5.1 stream through the decoder as a board makes it,
    // standard against late night, best of five passes each; the ratio is what planning/hearth-late-night.md
    // quotes, and the board's own figure (S3, C6) is owed to a bench window.
    const std::vector<std::byte> stream = read_stream("ac4-51-drc-ltrt-192");
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    std::vector<std::span<const std::byte>> raw;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        raw.push_back(frame.raw_ac4_frame);
    }
    const auto best_of = [&raw](DynamicRange range) {
        double best = 1e30;
        for (int pass = 0; pass < 5; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            const Played played = play(raw, range);
            const std::chrono::duration<double, std::milli> spent = std::chrono::steady_clock::now() - start;
            best = std::min(best, spent.count());
            REQUIRE_FALSE(played.channels.empty());
        }
        return best;
    };
    const double standard = best_of(DynamicRange::kStandard);
    const double late = best_of(DynamicRange::kLateNight);
    std::printf("AC-4 5.1, %zu frames: standard %.1f ms, late night %.1f ms (x%.3f)\n", raw.size(), standard, late,
                late / standard);
}
