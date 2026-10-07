#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/ac3/meta/loudness.hpp"
#include "iclforge/base/levels.hpp"
#include "iclforge/base/loudness.hpp"
#include "iclforge/base/speaker.hpp"

// planning/consolidation.md's C6 moved the level and loudness meters to base, keyed on a channel
// count, a weight per channel or a list of speakers, and left AC-3's acmod and channel-map forms in
// ac3 as constructors over them. Each base form has to measure exactly what the AC-3 form it
// stands beside does.

namespace {

using iclforge::base::Speaker;

std::vector<std::vector<float>> noise(std::size_t channels, std::size_t samples) {
    std::mt19937 rng(1770);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    std::vector<std::vector<float>> out(channels, std::vector<float>(samples));
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const float gain = 1.0f / static_cast<float>(ch + 1);
        for (auto& sample : out[ch]) {
            sample = gain * dist(rng);
        }
    }
    return out;
}

std::vector<std::span<const float>> spans(const std::vector<std::vector<float>>& channels) {
    return {channels.begin(), channels.end()};
}

}  // namespace

TEST_CASE("base's speaker weights are the channel-map locations' they stand for",
          "[base][meters]") {
    using Location = iclforge::base::Location;
    const std::array<std::pair<Location, Speaker>, 13> pairs{{
        {Location::kLeft, Speaker::kLeft},
        {Location::kRight, Speaker::kRight},
        {Location::kCentre, Speaker::kCentre},
        {Location::kLfe, Speaker::kLfe},
        {Location::kLfe2, Speaker::kLfe2},
        {Location::kLeftSurround, Speaker::kLeftSurround},
        {Location::kRightSurround, Speaker::kRightSurround},
        {Location::kLrs, Speaker::kLeftBack},
        {Location::kRrs, Speaker::kRightBack},
        {Location::kLw, Speaker::kLeftWide},
        {Location::kRw, Speaker::kRightWide},
        {Location::kLc, Speaker::kLeftScreen},
        {Location::kCs, Speaker::kCentreBack},
    }};
    for (const auto& [location, speaker] : pairs) {
        CHECK(iclforge::base::position_weight(speaker) ==
              iclforge::ac3::meta::position_weight(location));
    }
    CHECK(iclforge::base::position_weight(Speaker::kTopFrontLeft) == 1.0);
    CHECK(iclforge::base::position_weight(Speaker::kBottomFrontCentre) == 1.0);
}

TEST_CASE("base's loudness meter over 5.1 speakers is AC-3's 3/2 + LFE meter", "[base][meters]") {
    // A/52 Table 5.8's coded order, LFE last.
    constexpr std::array kSpeakers{Speaker::kLeft,          Speaker::kCentre, Speaker::kRight,
                                   Speaker::kLeftSurround, Speaker::kRightSurround,
                                   Speaker::kLfe};
    iclforge::ac3::meta::LoudnessMeter coded{iclforge::ac3::SampleRate::k48000,
                                             iclforge::ac3::Acmod::k3_2, true};
    iclforge::base::LoudnessMeter by_speaker{48000, kSpeakers};

    const auto signal = noise(kSpeakers.size(), 48000 * 4);
    coded.push(spans(signal));
    by_speaker.push(spans(signal));

    REQUIRE(coded.integrated_lkfs().has_value());
    CHECK(by_speaker.integrated_lkfs() == coded.integrated_lkfs());
    CHECK(by_speaker.momentary_lkfs() == coded.momentary_lkfs());
    CHECK(by_speaker.short_term_lkfs() == coded.short_term_lkfs());
    CHECK(by_speaker.loudness_range() == coded.loudness_range());
    CHECK(by_speaker.true_peak_dbtp() == coded.true_peak_dbtp());
    CHECK(by_speaker.channel_count() == coded.channel_count());
}

TEST_CASE("base's level meter and energy vector are AC-3's for the same channels",
          "[base][meters]") {
    const auto acmod = iclforge::ac3::Acmod::k3_2;
    iclforge::ac3::analysis::LevelMeter coded{acmod, true, 48000};
    iclforge::base::LevelMeter plain{6, 48000};

    const auto signal = noise(6, 4800);
    coded.process(spans(signal));
    plain.process(spans(signal));

    REQUIRE(plain.channel_count() == coded.channel_count());
    for (std::size_t ch = 0; ch < 6; ++ch) {
        CHECK(plain.levels()[ch].peak_db == coded.levels()[ch].peak_db);
        CHECK(plain.levels()[ch].rms_db == coded.levels()[ch].rms_db);
        CHECK(plain.summary()[ch].sum_squares == coded.summary()[ch].sum_squares);
    }

    std::vector<std::optional<double>> azimuths;
    for (int ch = 0; ch < 6; ++ch) {
        azimuths.push_back(iclforge::ac3::analysis::channel_azimuth_deg(acmod, true, ch));
    }
    const auto ours = iclforge::base::energy_vector(plain.levels(), azimuths);
    const auto theirs = iclforge::ac3::analysis::energy_vector(coded.levels(), acmod);
    CHECK(ours.azimuth_deg == theirs.azimuth_deg);
    CHECK(ours.magnitude == theirs.magnitude);
    CHECK(ours.level_db == theirs.level_db);
}
