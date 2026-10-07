#include "encoder/frame/drc_gains.hpp"

#include <algorithm>
#include <cmath>

#include "tiered/complex.hpp"

namespace iclforge::ac4::detail {
namespace {

constexpr std::size_t kSubbands = dsp::tiered::kQmfSubbands;

}  // namespace

DrcCurve drc_gain_curve(const CurveCodes& codes) noexcept {
    // Table 166: the null band, then each side's section, if it has one, a
    // step of 1 + its level code from the band, and its maximum a step of 1
    // + its code from the section, or from the band without one.
    DrcCurve c;
    c.null_low = -static_cast<double>(codes.lev_nullband_low);
    c.null_high = static_cast<double>(codes.lev_nullband_high);
    c.max_boost_gain = static_cast<double>(codes.gain_max_boost);
    if (codes.nr_boost_sections == 1) {
        c.section_boost_gain = 1.0 + static_cast<double>(codes.gain_section_boost);
        c.section_boost_level = c.null_low - (1.0 + static_cast<double>(codes.lev_section_boost));
    } else {
        c.section_boost_level = c.null_low;
    }
    c.max_boost_level =
        codes.gain_max_boost > 0
            ? c.section_boost_level - (1.0 + static_cast<double>(codes.lev_max_boost))
            : c.null_low;
    c.max_cut_gain = -static_cast<double>(codes.gain_max_cut);
    if (codes.nr_cut_sections == 1) {
        c.section_cut_gain = -(1.0 + static_cast<double>(codes.gain_section_cut));
        c.section_cut_level = c.null_high + 1.0 + static_cast<double>(codes.lev_section_cut);
    } else {
        c.section_cut_level = c.null_high;
    }
    c.max_cut_level = codes.gain_max_cut > 0
                          ? c.section_cut_level + 1.0 + static_cast<double>(codes.lev_max_cut)
                          : c.null_high;
    if (!codes.tc_default) {
        c.attack_ms = 5.0 * codes.tc_attack;
        c.release_ms = 40.0 * codes.tc_release;
        c.attack_fast_ms = 5.0 * codes.tc_attack_fast;
        c.release_fast_ms = 20.0 * codes.tc_release_fast;
        c.adaptive = codes.adaptive_smoothing;
        c.attack_threshold = static_cast<double>(codes.attack_threshold);
        c.release_threshold = static_cast<double>(codes.release_threshold);
    }
    return c;
}

int drc_subframes(int frame_length) noexcept {
    switch (frame_length) {
        case 2048:
            return 8;
        case 1920:
        case 1536:
            return 6;
        case 1024:
            return 4;
        case 960:
        case 768:
            return 3;
        case 512:
            return 2;
        default:
            return 1;  // 384
    }
}

DrcGainEncoder::DrcGainEncoder(const DrcCurve& curve, int gains_config,
                               std::span<const DrcChannel> channels, bool mono_or_stereo,
                               const FrameTiming& timing, int rate_hz, double dialnorm_db)
    : curve_(curve),
      gains_config_(gains_config),
      timing_(timing),
      slot_ms_(64.0 * 1000.0 / static_cast<double>(rate_hz)),
      dialnorm_db_(dialnorm_db),
      analyses_(channels.size()) {
    // Table 168's groups and Table 164's bands, which the gains are sent in.
    groups_ = gains_config > 0 && !mono_or_stereo ? 3 : 1;
    bands_ = gains_config == 2 ? 2 : (gains_config == 3 ? 4 : 1);
    for (const DrcChannel channel : channels) {
        weight_.push_back(
            channel == DrcChannel::kLfe
                ? 0.0
                : (channel == DrcChannel::kSide || channel == DrcChannel::kWide ? kDrcSideWeight
                                                                                  : 1.0));
    }
    k_weight_ = k_weights(static_cast<double>(rate_hz));
    qmf_gain_ = qmf_energy_gain();
}

long long DrcGainEncoder::slots_needed(long long frame) const noexcept {
    return static_cast<long long>(timing_.qmf_slots) * (frame + timing_.control_delay + 1) -
           timing_.hfgen_slots;
}

void DrcGainEncoder::push_slot(std::span<const std::array<double, dsp::tiered::kQmfSubbands>> samples) {
    // The programme's K-weighted power in the slot, as a mean square per
    // sample at full scale 1.0: the slot's 64 samples put sum(QWIN^2) times
    // their power into the subbands.
    double power = 0.0;
    std::array<dsp::tiered::Complex<double>, kSubbands> slot{};
    for (std::size_t c = 0; c < analyses_.size() && c < samples.size(); ++c) {
        analyses_[c].process(samples[c], slot);
        if (weight_[c] == 0.0) {
            continue;
        }
        for (std::size_t k = 0; k < kSubbands; ++k) {
            power += weight_[c] * k_weight_[k] * norm(slot[k]);
        }
    }
    const double p = std::max(power / (static_cast<double>(kSubbands) * qmf_gain_), kDrcPowerFloor);
    pending_.push_back(smoothing_.step(curve_, p, dialnorm_db_, slot_ms_));
    ++slots_;
}

DrcModeGains DrcGainEncoder::gains(long long frame) {
    const int subframes = gains_config_ == 0 ? 1 : drc_subframes(timing_.frame_length);
    const int per = timing_.qmf_slots / subframes;
    const long long first = slots_needed(frame) - timing_.qmf_slots;
    DrcModeGains out;
    out.groups = groups_;
    out.subframes = subframes;
    out.bands = bands_;
    out.gain.assign(static_cast<std::size_t>(groups_ * subframes * bands_), 0);
    for (int sf = 0; sf < subframes; ++sf) {
        // The mean smoothed gain over the subframe's slots, in dB2, for every
        // group and band.
        double sum = 0.0;
        int count = 0;
        for (int n = 0; n < per; ++n) {
            const long long slot = first + static_cast<long long>(sf) * per + n;
            if (slot >= first_ && slot - first_ < static_cast<long long>(pending_.size())) {
                sum += pending_[static_cast<std::size_t>(slot - first_)];
                ++count;
            }
        }
        const int gain = std::clamp(
            static_cast<int>(std::lround(count > 0 ? 6.0 * std::log2(sum / count) : 0.0)), -64, 63);
        for (int g = 0; g < groups_; ++g) {
            for (int b = 0; b < bands_; ++b) {
                out.gain[static_cast<std::size_t>((g * subframes + sf) * bands_ + b)] = gain;
            }
        }
    }
    // Nothing before the next frame's block is read again.
    const long long keep = first + timing_.qmf_slots;
    if (keep > first_) {
        const auto drop = static_cast<std::size_t>(
            std::min<long long>(keep - first_, static_cast<long long>(pending_.size())));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop));
        first_ = keep;
    }
    return out;
}

}  // namespace iclforge::ac4::detail