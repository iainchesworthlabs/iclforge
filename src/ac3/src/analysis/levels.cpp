#include "iclforge/ac3/analysis/levels.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/base/levels.hpp"
#include "iclforge/render/spatial.hpp"

namespace iclforge::ac3::analysis {

namespace {

// A/52 Table 5.8, "Channel Array Ordering", one row per acmod. Dual mono is
// two independent programs, so the spec names its channels Ch1/Ch2 rather
// than giving them positions.
constexpr std::array<std::array<std::string_view, 5>, 8> kChannelNames = {{
    {"Ch1", "Ch2"},                  // 0: 1+1
    {"C"},                           // 1: 1/0
    {"L", "R"},                      // 2: 2/0
    {"L", "C", "R"},                 // 3: 3/0
    {"L", "R", "S"},                 // 4: 2/1
    {"L", "C", "R", "S"},            // 5: 3/1
    {"L", "R", "SL", "SR"},          // 6: 2/2
    {"L", "C", "R", "SL", "SR"},     // 7: 3/2
}};

// The directions the spec's channel names imply, on the ITU-R BS.775 ring the
// spatial renderer already pans over. A mono surround sits behind the
// listener; the 3/2 entries come straight from the renderer's geometry so the
// two can never drift apart.
constexpr double kSurroundAzimuth = 180.0;

[[nodiscard]] constexpr std::optional<double> ring_azimuth(Acmod acmod, int index) {
    using iclforge::spatial::kSpeakerAzimuthDeg;
    constexpr std::size_t kL = 0, kC = 1, kR = 2, kSL = 3, kSR = 4;
    const auto at = static_cast<std::size_t>(index);
    switch (acmod) {
        case Acmod::kDualMono: return std::nullopt;
        case Acmod::k1_0: return kSpeakerAzimuthDeg[kC];
        case Acmod::k2_0:
            return at == 0 ? kSpeakerAzimuthDeg[kL] : kSpeakerAzimuthDeg[kR];
        case Acmod::k3_0:
            return kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kC : kR)];
        case Acmod::k2_1:
            return at == 2 ? kSurroundAzimuth
                           : kSpeakerAzimuthDeg[at == 0 ? kL : kR];
        case Acmod::k3_1:
            return at == 3 ? kSurroundAzimuth
                           : kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kC : kR)];
        case Acmod::k2_2:
            return kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kR : (at == 2 ? kSL : kSR))];
        case Acmod::k3_2:
            return kSpeakerAzimuthDeg[at];
    }
    return std::nullopt;
}

}  // namespace

std::string_view channel_name(Acmod acmod, bool lfe, int index) {
    const int fullbw = fullbw_channel_count(acmod);
    if (index < 0 || index >= channel_count(acmod, lfe)) {
        return {};
    }
    if (index == fullbw) {
        return "LFE";
    }
    return kChannelNames[static_cast<std::size_t>(acmod)][static_cast<std::size_t>(index)];
}

std::string_view layout_name(Acmod acmod, bool lfe) {
    // Two static tables rather than a runtime concatenation: the caller gets
    // a view it can hold, and there are only sixteen possible answers.
    constexpr std::array<std::string_view, 8> kPlain = {
        "1+1 dual mono", "1/0 mono", "2/0 stereo", "3/0", "2/1", "3/1", "2/2", "3/2",
    };
    constexpr std::array<std::string_view, 8> kWithLfe = {
        "1+1 dual mono + LFE", "1/0 mono + LFE", "2/0 stereo + LFE", "3/0 + LFE",
        "2/1 + LFE",           "3/1 + LFE",      "2/2 + LFE",        "3/2 + LFE",
    };
    const auto at = static_cast<std::size_t>(acmod);
    return lfe ? kWithLfe[at] : kPlain[at];
}

std::optional<double> channel_azimuth_deg(Acmod acmod, bool lfe, int index) {
    if (index < 0 || index >= channel_count(acmod, lfe)) {
        return std::nullopt;
    }
    if (index >= fullbw_channel_count(acmod)) {
        return std::nullopt;  // the LFE is non-directional by design
    }
    return ring_azimuth(acmod, index);
}

LevelMeter::LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate,
                       const MeterBallistics& ballistics)
    : LevelMeter(acmod, lfe, sample_rate, analysis::channel_count(acmod, lfe), ballistics) {}

LevelMeter::LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate, int channels,
                       const MeterBallistics& ballistics)
    : base::LevelMeter(std::max(channels, analysis::channel_count(acmod, lfe)), sample_rate,
                       ballistics),
      acmod_(acmod),
      lfe_(lfe) {}

Acmod LevelMeter::acmod() const { return acmod_; }
bool LevelMeter::lfe() const { return lfe_; }

SoundfieldVector energy_vector(std::span<const ChannelLevel> levels, Acmod acmod) {
    std::array<std::optional<double>, 5> azimuths{};
    const auto fullbw =
        std::min(static_cast<std::size_t>(fullbw_channel_count(acmod)), azimuths.size());
    for (std::size_t ch = 0; ch < fullbw; ++ch) {
        azimuths[ch] = ring_azimuth(acmod, static_cast<int>(ch));
    }
    return base::energy_vector(levels, std::span(azimuths).first(fullbw));
}

}  // namespace iclforge::ac3::analysis
