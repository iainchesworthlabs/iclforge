#pragma once

// A synthetic music scene for the speakers-view options: a few instruments (a kick, a snare, hats, a bass, a pad,
// an arpeggio) and, for an Atmos scene, objects that move through the room (a fly-over, a bee circling the
// listener, rain overhead, a bell that hops), panned onto the slots of whatever layout it is rendered to, then
// measured the way the board measures, window by window.
//
// What each window carries is of three kinds, and the options say which they read:
//   - what the board publishes today (BurstPlayer::status()): peak and RMS per output over 100 ms;
//   - what a three-band meter would add (a decimated one-pole split, the cost of which the options page prices);
//   - what an object seam would add (position, height, level and the per-slot gains of each object).
// The audio is made up, not decoded: it moves the way a groove moves, which is the point, and it is fixed, so
// that every option sees the same music.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iclforge/render/room_plan.hpp"

namespace scene {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr int kRate = 48000;
inline constexpr int kWindow = 4800;  // 100 ms: the board's level window (burst_player.cpp, kLevelWindow)
inline constexpr int kMaxOut = 16;
inline constexpr int kMaxObjects = 12;  // the P4 and P4X's cap (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS)
inline constexpr float kSilent = -120.0F;

struct ObjectState {
    bool active = false;
    // Which object this is, stable from window to window (the order of the objects in a window is not).
    int id = 0;
    // The OAMD room cuboid: x 0 (left) to 1, y 0 (front) to 1, z -1 (floor) to +1 (ceiling).
    float x = 0.5F;
    float y = 0.5F;
    float z = 0.0F;
    float level_db = kSilent;
    // The renderer's gain of this object into each slot (LayoutRenderer::object_gain), power-normalised here.
    std::array<float, kMaxOut> slot_gain{};
};

struct Window {
    int index = 0;
    int outputs = 0;
    // Today.
    std::array<float, kMaxOut> peak_db{};
    std::array<float, kMaxOut> rms_db{};
    // A three-band meter.
    std::array<float, kMaxOut> low_db{};
    std::array<float, kMaxOut> mid_db{};
    std::array<float, kMaxOut> high_db{};
    // An object seam.
    int objects = 0;
    std::array<ObjectState, kMaxObjects> object{};
};

enum class Content : std::uint8_t {
    kStereo,          // PCM from Music Assistant: a stereo programme, whatever the room
    kBed,             // a coded channel-based programme: the bed is the room
    kBedAndObjects,   // a coded programme with objects the board reconstructs
};

namespace detail {

inline float db_power(double power) { return power <= 1e-12 ? kSilent : static_cast<float>(10.0 * std::log10(power)); }
inline float db_amp(double amp) { return amp <= 1e-6 ? kSilent : static_cast<float>(20.0 * std::log10(amp)); }

struct Rng {
    std::uint32_t state = 0x9E3779B9U;
    double next() {  // -1 .. 1
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 2147483648.0 - 1.0;
    }
};

inline double hash01(std::uint32_t n) {
    n ^= n >> 16;
    n *= 0x7FEB352DU;
    n ^= n >> 15;
    n *= 0x846CA68BU;
    n ^= n >> 16;
    return static_cast<double>(n) / 4294967296.0;
}

}  // namespace detail

class Scene {
   public:
    Scene(std::vector<iclforge::render::PlanSpot> spots, Content content) : spots_(std::move(spots)), content_(content) {}

    // The samples of each output in the window `next()` last made: what the board's renderer would have put out
    // (the meters-view prototype takes its spectrum from them).
    [[nodiscard]] const std::array<std::array<float, kWindow>, kMaxOut>& outputs() const { return outputs_; }

    // The window after the last one.
    Window next() {
        Window win;
        win.index = index_;
        win.outputs = static_cast<int>(spots_.size());
        const double t0 = index_ * (static_cast<double>(kWindow) / kRate);
        render_sources(t0);
        place(win, t0);
        measure(win);
        ++index_;
        return win;
    }

   private:
    // The sources, each a mono block of the window.
    enum Source { kKick, kSnare, kHat, kBass, kPadA, kPadB, kLead, kFly, kBee, kRain0, kRain1, kRain2, kBell, kClap, kSources };

    struct SourceBlock {
        std::array<double, kWindow> v{};
        bool active = false;
    };

    // A bar is two seconds at 120 bpm, the pattern is sixteen bars.
    static int bar_of(double t) { return static_cast<int>(std::fmod(t, 32.0) / 2.0); }

    void render_sources(double t0) {
        using detail::Rng;
        for (SourceBlock& b : block_) {
            b.v.fill(0.0);
            b.active = false;
        }
        const int bar = bar_of(t0);
        const bool breakdown = bar == 12 || bar == 13;
        const bool drums = bar >= 2 && !breakdown;
        const bool bass = bar >= 4 && !breakdown;
        const bool lead = bar >= 8;
        const bool atmos = content_ == Content::kBedAndObjects;
        const double cycle0 = std::fmod(t0, 32.0);
        static constexpr double kBassNotes[8] = {55.0, 55.0, 82.4, 55.0, 65.4, 65.4, 98.0, 73.4};
        static constexpr double kChords[4][3] = {{110.0, 130.8, 164.8}, {87.3, 110.0, 130.8}, {130.8, 164.8, 196.0}, {98.0, 123.5, 146.8}};
        static constexpr double kArp[8] = {440.0, 523.3, 659.3, 880.0, 659.3, 523.3, 440.0, 392.0};
        for (int i = 0; i < kWindow; ++i) {
            const double t = t0 + static_cast<double>(i) / kRate;
            const double beat_tau = std::fmod(t, 0.5);
            const double eighth_tau = std::fmod(t, 0.25);
            const double sixteenth_tau = std::fmod(t, 0.125);
            // Kick.
            if (drums) {
                const double phase = 2.0 * kPi * (45.0 * beat_tau + 110.0 * 0.04 * (1.0 - std::exp(-beat_tau / 0.04)));
                block_[kKick].v[i] = 0.95 * std::sin(phase) * std::exp(-beat_tau / 0.16) * (1.0 - std::exp(-beat_tau / 0.002));
                block_[kKick].active = true;
            }
            // Snare on beats 2 and 4, a ghost note before the next bar.
            if (bass) {
                const double bar_t = std::fmod(t, 2.0);
                const bool hit = bar_t >= 0.5 && bar_t < 0.9;
                const bool backbeat = bar_t >= 1.5;
                if (hit || backbeat) {
                    const double tau = hit ? bar_t - 0.5 : bar_t - 1.5;
                    block_[kSnare].v[i] = (0.55 * rng_.next() + 0.35 * std::sin(2.0 * kPi * 185.0 * tau)) * std::exp(-tau / 0.09);
                    block_[kSnare].active = true;
                }
            }
            // A clap behind the listener on the third beat, alternating sides, with a short tail.
            if (bass) {
                const double bar_t = std::fmod(t, 2.0);
                if (bar_t >= 1.0 && bar_t < 1.5) {
                    const double tau = bar_t - 1.0;
                    block_[kClap].v[i] = 0.5 * rng_.next() * std::exp(-tau / 0.12) * (1.0 - std::exp(-tau / 0.002));
                    block_[kClap].active = true;
                }
            }
            // Hats on the eighths, the off-beat louder.
            if (drums) {
                const double noise = rng_.next();
                const double diff = noise - hat_prev_;
                hat_prev_ = noise;
                const bool off_beat = std::fmod(t, 0.5) >= 0.25;
                block_[kHat].v[i] = diff * 0.28 * (off_beat ? 1.0 : 0.6) * std::exp(-eighth_tau / 0.035);
                block_[kHat].active = true;
            }
            // Bass: saw through a one-pole, eighth notes.
            if (bass) {
                const int step = static_cast<int>(t / 0.25) % 8;
                const double f = kBassNotes[step];
                const double saw = 2.0 * (f * t - std::floor(f * t)) - 1.0;
                bass_lp_ += 0.06 * (saw - bass_lp_);
                block_[kBass].v[i] = 0.7 * bass_lp_ * std::exp(-eighth_tau / 0.22) * (1.0 - std::exp(-eighth_tau / 0.006));
                block_[kBass].active = true;
            }
            // Pad: a chord a bar pair, two decorrelated copies.
            {
                const auto& chord = kChords[static_cast<int>(t / 4.0) % 4];
                const double trem = 0.75 + 0.25 * std::sin(2.0 * kPi * 0.17 * t);
                const double swell = bar < 2 ? 1.0 : 0.7;
                double a = 0.0;
                double b = 0.0;
                for (const double f : chord) {
                    a += std::sin(2.0 * kPi * f * t) + 0.35 * std::sin(2.0 * kPi * 2.0 * f * t);
                    b += std::sin(2.0 * kPi * f * 1.004 * t + 1.3) + 0.35 * std::sin(2.0 * kPi * 2.0 * f * 0.997 * t + 0.4);
                }
                block_[kPadA].v[i] = 0.075 * a * trem * swell * (breakdown ? 1.4 : 1.0);
                block_[kPadB].v[i] = 0.075 * b * trem * swell * (breakdown ? 1.4 : 1.0);
                block_[kPadA].active = block_[kPadB].active = true;
            }
            // Arpeggio on sixteenths.
            if (lead) {
                const double f = kArp[static_cast<int>(t / 0.125) % 8];
                block_[kLead].v[i] = (std::sin(2.0 * kPi * f * t) + 0.3 * std::sin(4.0 * kPi * f * t)) * 0.34 *
                                     std::exp(-sixteenth_tau / 0.1) * (1.0 - std::exp(-sixteenth_tau / 0.003));
                block_[kLead].active = true;
            }
            if (atmos) {
                // A fly-over: a rotor, 24 to 28 s into the pattern.
                if (cycle0 >= 24.0 && cycle0 < 28.0) {
                    const double u = (std::fmod(t, 32.0) - 24.0) / 4.0;
                    const double fade = std::sqrt(std::max(0.0, std::sin(kPi * std::clamp(u, 0.0, 1.0))));
                    const double rotor = (0.5 + 0.5 * std::sin(2.0 * kPi * 12.0 * t)) * std::sin(2.0 * kPi * 95.0 * t + 3.0 * std::sin(2.0 * kPi * 7.0 * t));
                    block_[kFly].v[i] = (0.55 * rotor + 0.12 * rng_.next()) * fade;
                    block_[kFly].active = true;
                }
                // A bee circling the listener.
                if (bar >= 8 && bar <= 11) {
                    block_[kBee].v[i] = 0.16 * std::sin(2.0 * kPi * 620.0 * t + 4.0 * std::sin(2.0 * kPi * 6.0 * t)) *
                                        (0.8 + 0.2 * std::sin(2.0 * kPi * 31.0 * t));
                    block_[kBee].active = true;
                }
                // Rain overhead.
                if (bar >= 8) {
                    for (int r = 0; r < 3; ++r) {
                        const double n = rng_.next();
                        rain_prev_[r] = n;
                        block_[kRain0 + r].v[i] = 0.05 * (n - 0.6 * rain_prev_[(r + 1) % 3]) * (0.7 + 0.3 * std::sin(2.0 * kPi * (0.3 + 0.1 * r) * t));
                        block_[kRain0 + r].active = true;
                    }
                }
                // A bell, once a bar, somewhere else each time.
                if (bar >= 2) {
                    const double tau = std::fmod(t, 2.0);
                    block_[kBell].v[i] = 0.2 * (std::sin(2.0 * kPi * 1320.0 * t) + 0.4 * std::sin(2.0 * kPi * 2640.0 * t)) * std::exp(-tau / 0.45);
                    block_[kBell].active = true;
                }
            }
        }
    }

    // Where a source is in the room at time t: azimuth (left positive), elevation, spread, and for an object its
    // OAMD cuboid position.
    struct Place {
        double az = 0.0;
        double el = 0.0;
        double spread = 0.0;
        bool object = false;
        float x = 0.5F;
        float y = 0.5F;
        float z = 0.0F;
        double lfe = 0.0;
    };

    static Place from_cuboid(double x, double y, double z) {
        const double dx = 2.0 * x - 1.0;
        const double dy = 2.0 * y - 1.0;
        Place p;
        p.object = true;
        p.x = static_cast<float>(x);
        p.y = static_cast<float>(y);
        p.z = static_cast<float>(z);
        p.az = std::atan2(-dx, -dy) * 180.0 / kPi;
        p.el = std::clamp(z, -1.0, 1.0) * 75.0;
        const double dist = std::hypot(dx, dy);
        p.spread = std::clamp(1.0 - dist / 0.6, 0.0, 1.0);
        return p;
    }

    Place place_of(int source, double t) const {
        Place p;
        switch (source) {
            case kKick:
            case kBass:
                p.lfe = 0.9;
                p.az = (static_cast<int>(t / 0.5) % 2 == 0) ? 18.0 : -18.0;
                p.spread = 0.25;
                break;
            case kSnare:
                p.spread = 0.25;
                break;
            case kClap:
                p.az = (static_cast<int>(t / 2.0) % 2 == 0) ? 115.0 : -115.0;
                p.spread = 0.1;
                break;
            case kHat:
                p.az = std::fmod(t, 0.5) >= 0.25 ? -30.0 : 30.0;
                break;
            case kPadA:
            case kPadB:
                p.spread = 1.0;
                break;
            case kLead: {
                const double phi = 2.0 * kPi * t / 8.0;
                if (content_ == Content::kBedAndObjects) {
                    return from_cuboid(0.5 + 0.42 * std::cos(phi), 0.5 + 0.42 * std::sin(phi), 0.25 + 0.2 * std::sin(2.0 * kPi * t / 5.0));
                }
                p.az = std::fmod(360.0 * t / 8.0, 360.0);
                break;
            }
            case kFly: {
                const double u = std::clamp((std::fmod(t, 32.0) - 24.0) / 4.0, 0.0, 1.0);
                Place q = from_cuboid(0.0 + 1.0 * u, 1.0 - 1.0 * u, 0.05 + 0.85 * std::sin(kPi * u * 0.5));
                q.lfe = 0.4;
                return q;
            }
            case kBee: {
                const double phi = 2.0 * kPi * 0.45 * t;
                return from_cuboid(0.5 + 0.38 * std::cos(phi), 0.5 + 0.38 * std::sin(phi), 0.1 + 0.08 * std::sin(2.0 * kPi * 1.3 * t));
            }
            case kRain0:
                return from_cuboid(0.25, 0.45, 0.95);
            case kRain1:
                return from_cuboid(0.5, 0.5, 1.0);
            case kRain2:
                return from_cuboid(0.75, 0.45, 0.95);
            case kBell: {
                const std::uint32_t bar = static_cast<std::uint32_t>(t / 2.0);
                return from_cuboid(0.12 + 0.76 * detail::hash01(bar * 3 + 1), 0.12 + 0.76 * detail::hash01(bar * 3 + 2),
                                   0.05 + 0.4 * detail::hash01(bar * 3 + 3));
            }
            default:
                break;
        }
        return p;
    }

    [[nodiscard]] bool is_object(int source) const {
        if (content_ != Content::kBedAndObjects) {
            return false;
        }
        return source == kLead || source == kFly || source == kBee || source == kRain0 || source == kRain1 || source == kRain2 ||
               source == kBell;
    }

    // Gains of a source into the slots: directional with a power law, a constant-power pan for two speakers.
    void pan(const Place& at, std::array<float, kMaxOut>& g) const {
        g.fill(0.0F);
        const std::size_t n = spots_.size();
        std::size_t speakers = 0;
        for (const auto& s : spots_) {
            speakers += (s.connected && !s.lfe) ? 1 : 0;
        }
        if (speakers == 0) {
            return;
        }
        const double rad = kPi / 180.0;
        if (speakers <= 2 || content_ == Content::kStereo) {
            const double lateral = std::clamp(std::sin(at.az * rad), -1.0, 1.0);
            const double a = (lateral * 0.5 + 0.5) * kPi / 2.0;
            const double left = std::sin(a);
            const double right = std::cos(a);
            for (std::size_t i = 0; i < n; ++i) {
                if (!spots_[i].connected || spots_[i].lfe) {
                    continue;
                }
                // A stereo programme on a bigger room reaches only the two front speakers, L and R.
                if (speakers > 2 && spots_[i].name != "L" && spots_[i].name != "R") {
                    continue;
                }
                const double base = spots_[i].azimuth_deg >= 0.0 ? left : right;
                g[i] = static_cast<float>((1.0 - at.spread) * base + at.spread * 0.7071);
            }
            return;
        }
        const double sx = std::cos(at.el * rad) * std::cos(at.az * rad);
        const double sy = std::cos(at.el * rad) * std::sin(at.az * rad);
        const double sz = std::sin(at.el * rad);
        std::array<double, kMaxOut> w{};
        double sum2 = 0.0;
        for (std::size_t i = 0; i < n && i < kMaxOut; ++i) {
            if (!spots_[i].connected || spots_[i].lfe) {
                continue;
            }
            const double ex = spots_[i].elevation_deg * rad;
            const double ax = spots_[i].azimuth_deg * rad;
            const double dot = sx * std::cos(ex) * std::cos(ax) + sy * std::cos(ex) * std::sin(ax) + sz * std::sin(ex);
            const double c = std::max(0.0, dot);
            w[i] = c * c * c * c;
            sum2 += w[i] * w[i];
        }
        const double uniform = 1.0 / std::sqrt(static_cast<double>(speakers));
        for (std::size_t i = 0; i < n && i < kMaxOut; ++i) {
            if (!spots_[i].connected || spots_[i].lfe) {
                continue;
            }
            const double directional = sum2 > 1e-9 ? w[i] / std::sqrt(sum2) : uniform;
            g[i] = static_cast<float>((1.0 - at.spread) * directional + at.spread * uniform);
        }
    }

    void place(Window& win, double t0) {
        const double t_mid = t0 + 0.05;
        for (auto& o : outputs_) {
            o.fill(0.0F);
        }
        const std::size_t n = std::min<std::size_t>(spots_.size(), kMaxOut);
        int lfe_slot = -1;
        for (std::size_t i = 0; i < n; ++i) {
            if (spots_[i].lfe && spots_[i].connected) {
                lfe_slot = static_cast<int>(i);
            }
        }
        win.objects = 0;
        for (int s = 0; s < kSources; ++s) {
            if (!block_[s].active) {
                continue;
            }
            Place at = place_of(s, t_mid);
            std::array<float, kMaxOut> g{};
            if (s == kPadA || s == kPadB) {
                // The decorrelated copies go to alternate slots.
                std::size_t k = 0;
                const double uniform = 1.0 / std::sqrt(std::max(1.0, static_cast<double>(n)));
                for (std::size_t i = 0; i < n; ++i) {
                    if (!spots_[i].connected || spots_[i].lfe) {
                        continue;
                    }
                    g[i] = ((k++ % 2 == 0) == (s == kPadA)) ? static_cast<float>(uniform * 1.4) : 0.0F;
                }
            } else {
                pan(at, g);
            }
            // Stereo programme: a coded surround source is folded; nothing else changes.
            const float master = 0.62F;
            for (std::size_t i = 0; i < n; ++i) {
                if (g[i] == 0.0F) {
                    continue;
                }
                for (int k = 0; k < kWindow; ++k) {
                    outputs_[i][k] += static_cast<float>(block_[s].v[k]) * g[i] * master;
                }
            }
            if (lfe_slot >= 0 && at.lfe > 0.0) {
                for (int k = 0; k < kWindow; ++k) {
                    outputs_[lfe_slot][k] += static_cast<float>(block_[s].v[k] * at.lfe) * master;
                }
            }
            if (is_object(s) && win.objects < kMaxObjects) {
                ObjectState& o = win.object[win.objects++];
                o.active = true;
                o.id = s;
                o.x = at.x;
                o.y = at.y;
                o.z = at.z;
                double sum = 0.0;
                for (int k = 0; k < kWindow; ++k) {
                    sum += block_[s].v[k] * block_[s].v[k];
                }
                o.level_db = detail::db_power(sum / kWindow * master * master);
                double norm = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    norm += static_cast<double>(g[i]) * g[i];
                }
                for (std::size_t i = 0; i < n; ++i) {
                    o.slot_gain[i] = norm > 0.0 ? static_cast<float>(g[i] * g[i] / norm) : 0.0F;
                }
            }
        }
    }

    // Peak and RMS as the board measures; three bands as a decimated one-pole split would: the block summed in
    // fours (a 12 kHz stream, a box filter), two one-poles at about 150 Hz and 1.6 kHz, the three differences
    // squared and summed. The filter states carry from window to window.
    void measure(Window& win) {
        constexpr float kA1 = 0.0754F;  // 1 - exp(-2 pi 150 / 12000)
        constexpr float kA2 = 0.567F;   // 1 - exp(-2 pi 1600 / 12000)
        const std::size_t n = std::min<std::size_t>(spots_.size(), kMaxOut);
        for (std::size_t o = 0; o < n; ++o) {
            const std::array<float, kWindow>& x = outputs_[o];
            float peak = 0.0F;
            double sum = 0.0;
            double sum_low = 0.0;
            double sum_mid = 0.0;
            double sum_high = 0.0;
            for (int k = 0; k < kWindow; k += 4) {
                float d = 0.0F;
                for (int j = 0; j < 4; ++j) {
                    const float v = x[k + j];
                    peak = std::max(peak, std::fabs(v));
                    sum += static_cast<double>(v) * v;
                    d += v;
                }
                d *= 0.25F;
                lp1_[o] += kA1 * (d - lp1_[o]);
                lp2_[o] += kA2 * (d - lp2_[o]);
                const float low = lp1_[o];
                const float mid = lp2_[o] - lp1_[o];
                const float high = d - lp2_[o];
                sum_low += static_cast<double>(low) * low;
                sum_mid += static_cast<double>(mid) * mid;
                sum_high += static_cast<double>(high) * high;
            }
            win.peak_db[o] = detail::db_amp(peak);
            win.rms_db[o] = detail::db_power(sum / kWindow);
            win.low_db[o] = detail::db_power(sum_low / (kWindow / 4));
            win.mid_db[o] = detail::db_power(sum_mid / (kWindow / 4));
            win.high_db[o] = detail::db_power(sum_high / (kWindow / 4));
        }
    }

    std::vector<iclforge::render::PlanSpot> spots_;
    Content content_;
    int index_ = 0;
    detail::Rng rng_;
    double hat_prev_ = 0.0;
    double bass_lp_ = 0.0;
    std::array<double, 3> rain_prev_{};
    std::array<SourceBlock, kSources> block_{};
    std::array<std::array<float, kWindow>, kMaxOut> outputs_{};
    std::array<float, kMaxOut> lp1_{};
    std::array<float, kMaxOut> lp2_{};
};

}  // namespace scene
