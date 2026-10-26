#pragma once

// A made-up groove for the Meters view's scenarios (planning/p4x-meters-view.md): a kick, a snare, hats, a bass, a pad and a lead that moves round the room,
// panned onto the outputs of whatever room is playing and written into the tap in blocks of 256 frames, as the player's decode task writes it, up to
// the preview's own clock. Fixed, so that every scenario sees the same music; not a measurement of anything.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/meters_tap.hpp"
#include "iclforge/render/room_plan.hpp"

namespace preview {

class MeterGroove {
   public:
    static constexpr double kRate = 48000.0;
    static constexpr std::size_t kBlock = iclforge::meters::kTapBlockFrames;
    static constexpr double kPi = 3.14159265358979323846;

    // The room that plays, and whether it is a stereo programme (only L and R carry anything).
    void reset(const std::vector<iclforge::render::PlanSpot>& spots, bool stereo_only) {
        spots_ = spots;
        stereo_only_ = stereo_only;
        frames_ = 0;
        started_ = false;
    }

    // Writes blocks up to `now_us` of the preview's clock (the first call starts the groove at that time; a jump of more than a
    // second writes only the last of it: the ring holds 340 ms).
    void feed(iclforge::meters::MeterTap& tap, std::int64_t now_us) {
        if (spots_.empty()) {
            return;
        }
        const double now_s = static_cast<double>(now_us) / 1.0e6;
        if (!started_) {
            started_ = true;
            frames_ = static_cast<std::uint64_t>(std::max(0.0, (now_s - 1.0)) * kRate);
            frames_ -= frames_ % kBlock;
        }
        std::uint64_t target = static_cast<std::uint64_t>(now_s * kRate);
        if (static_cast<double>(target - frames_) > kRate) {
            frames_ = target - static_cast<std::uint64_t>(kRate);
            frames_ -= frames_ % kBlock;
        }
        const std::size_t outputs = std::min(spots_.size(), iclforge::meters::kTapMaxOutputs);
        std::vector<std::vector<float>> data(outputs, std::vector<float>(kBlock));
        std::vector<std::span<float>> views;
        for (auto& d : data) {
            views.emplace_back(d);
        }
        const std::uint32_t live = outputs >= 32 ? ~0U : ((1U << outputs) - 1U);
        while (frames_ + kBlock <= target) {
            render(data, frames_);
            tap.block(std::span<const std::span<float>>(views.data(), views.size()), kBlock, live);
            frames_ += kBlock;
        }
    }

   private:
    struct Source {
        double azimuth = 0.0;
        double elevation = 0.0;
        double spread = 0.0;
        double level = 0.0;
    };

    // How much of a source at (az, el) an output at that speaker's position gets: a cosine to the fourth power round the room, the louder for
    // being nearer, normalised so that a source's power is the same wherever it is.
    void pan(double az_deg, double el_deg, double spread, std::vector<double>& gain) const {
        const double rad = kPi / 180.0;
        gain.assign(spots_.size(), 0.0);
        std::size_t speakers = 0;
        for (const auto& s : spots_) {
            speakers += (s.connected && !s.lfe) ? 1 : 0;
        }
        if (speakers == 0) {
            return;
        }
        if (stereo_only_ || speakers <= 2) {
            const double lateral = std::clamp(std::sin(az_deg * rad), -1.0, 1.0);
            const double a = (lateral * 0.5 + 0.5) * kPi / 2.0;
            for (std::size_t i = 0; i < spots_.size(); ++i) {
                const auto& s = spots_[i];
                if (!s.connected || s.lfe || (stereo_only_ && speakers > 2 && s.name != "L" && s.name != "R")) {
                    continue;
                }
                const double base = s.azimuth_deg >= 0.0 ? std::sin(a) : std::cos(a);
                gain[i] = (1.0 - spread) * base + spread * 0.7071;
            }
            return;
        }
        const double sx = std::cos(el_deg * rad) * std::cos(az_deg * rad);
        const double sy = std::cos(el_deg * rad) * std::sin(az_deg * rad);
        const double sz = std::sin(el_deg * rad);
        double sum2 = 0.0;
        std::vector<double> w(spots_.size(), 0.0);
        for (std::size_t i = 0; i < spots_.size(); ++i) {
            const auto& s = spots_[i];
            if (!s.connected || s.lfe) {
                continue;
            }
            const double ex = s.elevation_deg * rad;
            const double ax = s.azimuth_deg * rad;
            const double dot = sx * std::cos(ex) * std::cos(ax) + sy * std::cos(ex) * std::sin(ax) + sz * std::sin(ex);
            const double c = std::max(0.0, dot);
            w[i] = c * c * c * c;
            sum2 += w[i] * w[i];
        }
        const double uniform = 1.0 / std::sqrt(static_cast<double>(speakers));
        for (std::size_t i = 0; i < spots_.size(); ++i) {
            if (!spots_[i].connected || spots_[i].lfe) {
                continue;
            }
            const double directional = sum2 > 1e-9 ? w[i] / std::sqrt(sum2) : uniform;
            gain[i] = (1.0 - spread) * directional + spread * uniform;
        }
    }

    // A deterministic noise: a counter through a hash, so that a block's samples do not depend on how the blocks were cut.
    static double noise(std::uint64_t n) {
        std::uint64_t x = n * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDULL;
        x ^= x >> 33;
        return static_cast<double>(static_cast<std::int64_t>(x >> 11)) / static_cast<double>(1ULL << 52) - 1.0;
    }

    void render(std::vector<std::vector<float>>& out, std::uint64_t first_frame) const {
        static constexpr double kBassNotes[8] = {55.0, 55.0, 82.4, 55.0, 65.4, 65.4, 98.0, 73.4};
        static constexpr double kChords[4][3] = {{110.0, 130.8, 164.8}, {87.3, 110.0, 130.8}, {130.8, 164.8, 196.0}, {98.0, 123.5, 146.8}};
        static constexpr double kArp[8] = {440.0, 523.3, 659.3, 880.0, 659.3, 523.3, 440.0, 392.0};
        const std::size_t outputs = out.size();
        for (auto& o : out) {
            std::fill(o.begin(), o.end(), 0.0F);
        }
        // Where each part is, at the middle of the block (they move slowly): the gains into the outputs, worked out once a block.
        enum Part { kKick, kSnare, kHat, kBass, kPad, kLead, kParts };
        const double mid = (static_cast<double>(first_frame) + static_cast<double>(kBlock) / 2.0) / kRate;
        const bool off_beat_mid = std::fmod(mid, 0.5) >= 0.25;
        struct Place {
            double az;
            double el;
            double spread;
            double lfe;
        };
        const std::array<Place, kParts> places = {{{0.0, 0.0, 0.25, 0.9},
                                                   {0.0, 0.0, 0.25, 0.0},
                                                   {off_beat_mid ? -30.0 : 30.0, 0.0, 0.0, 0.0},
                                                   {0.0, 0.0, 0.25, 0.9},
                                                   {0.0, 0.0, 1.0, 0.0},
                                                   {std::fmod(360.0 * mid / 8.0, 360.0), 25.0 + 20.0 * std::sin(2.0 * kPi * mid / 5.0), 0.0, 0.0}}};
        std::array<std::vector<double>, kParts> gain;
        for (std::size_t p = 0; p < kParts; ++p) {
            pan(places[p].az, places[p].el, places[p].spread, gain[p]);
            for (std::size_t o = 0; o < outputs; ++o) {
                gain[p][o] = spots_[o].lfe ? places[p].lfe : gain[p][o];
            }
        }
        for (std::size_t k = 0; k < kBlock; ++k) {
            const std::uint64_t n = first_frame + k;
            const double t = static_cast<double>(n) / kRate;
            const double bar_t = std::fmod(t, 2.0);
            const double beat_tau = std::fmod(t, 0.5);
            const double eighth_tau = std::fmod(t, 0.25);
            const double sixteenth_tau = std::fmod(t, 0.125);
            const int bar = static_cast<int>(std::fmod(t, 32.0) / 2.0);
            const bool drums = bar >= 1 && bar != 12 && bar != 13;
            const bool bass_on = bar >= 2 && bar != 12 && bar != 13;
            const bool lead = bar >= 4;
            std::array<double, kParts> v{};
            if (drums) {
                const double phase = 2.0 * kPi * (45.0 * beat_tau + 4.4 * (1.0 - std::exp(-beat_tau / 0.04)));
                v[kKick] = 0.9 * std::sin(phase) * std::exp(-beat_tau / 0.16) * (1.0 - std::exp(-beat_tau / 0.002));
                const bool snare = (bar_t >= 0.5 && bar_t < 0.9) || bar_t >= 1.5;
                if (snare) {
                    const double tau = bar_t >= 1.5 ? bar_t - 1.5 : bar_t - 0.5;
                    v[kSnare] = (0.55 * noise(n) + 0.35 * std::sin(2.0 * kPi * 185.0 * tau)) * std::exp(-tau / 0.09);
                }
                const bool off_beat = std::fmod(t, 0.5) >= 0.25;
                v[kHat] = (noise(n) - noise(n - 1)) * 0.28 * (off_beat ? 1.0 : 0.6) * std::exp(-eighth_tau / 0.035);
            }
            if (bass_on) {
                const double f = kBassNotes[static_cast<int>(t / 0.25) % 8];
                const double saw = 2.0 * (f * t - std::floor(f * t)) - 1.0;
                // A one-pole filter has no closed form: the saw's low part and a sine of the note stand in for it.
                v[kBass] = 0.5 * (0.15 * saw + 0.75 * std::sin(2.0 * kPi * f * t)) * std::exp(-eighth_tau / 0.22) * (1.0 - std::exp(-eighth_tau / 0.006));
            }
            {
                const auto& chord = kChords[static_cast<int>(t / 4.0) % 4];
                double pad = 0.0;
                for (const double f : chord) {
                    pad += std::sin(2.0 * kPi * f * t) + 0.35 * std::sin(4.0 * kPi * f * t);
                }
                v[kPad] = 0.075 * pad * (0.75 + 0.25 * std::sin(2.0 * kPi * 0.17 * t));
            }
            if (lead) {
                const double f = kArp[static_cast<int>(t / 0.125) % 8];
                v[kLead] = (std::sin(2.0 * kPi * f * t) + 0.3 * std::sin(4.0 * kPi * f * t)) * 0.34 * std::exp(-sixteenth_tau / 0.1) * (1.0 - std::exp(-sixteenth_tau / 0.003));
            }
            for (std::size_t o = 0; o < outputs; ++o) {
                double sum = 0.0;
                for (std::size_t p = 0; p < kParts; ++p) {
                    sum += v[p] * gain[p][o];
                }
                out[o][k] = static_cast<float>(sum * 0.62);
            }
        }
    }

    std::vector<iclforge::render::PlanSpot> spots_;
    bool stereo_only_ = false;
    bool started_ = false;
    std::uint64_t frames_ = 0;
};

}  // namespace preview
