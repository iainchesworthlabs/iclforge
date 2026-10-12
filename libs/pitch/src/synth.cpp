#include "iclforge/pitch/synth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "iclforge/pitch/tracker.hpp"

namespace iclforge::pitch {

namespace {

constexpr double kPi = std::numbers::pi;

// xorshift64*: the same numbers on every platform, which <random>'s distributions are not.
class Rng {
   public:
    explicit Rng(std::uint32_t seed)
        : state_(0x9E3779B97F4A7C15ULL ^
                 (static_cast<std::uint64_t>(seed) * 0xBF58476D1CE4E5B9ULL)) {
        if (state_ == 0) {
            state_ = 1;
        }
        for (int i = 0; i < 4; ++i) {
            next();
        }
    }

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    // [0, 1)
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    double gauss() {
        const double u1 = std::max(uniform(), 1e-12);
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }

   private:
    std::uint64_t state_;
};

struct Formant {
    double hz;
    double bandwidth;
};

constexpr std::array<std::array<Formant, 3>, 3> kVowels = {{
    {{{700.0, 90.0}, {1220.0, 100.0}, {2600.0, 140.0}}},  // /a/
    {{{300.0, 60.0}, {2300.0, 110.0}, {3000.0, 150.0}}},  // /i/
    {{{500.0, 80.0}, {900.0, 90.0}, {2500.0, 130.0}}},    // /o/
}};

// A two-pole resonator, gain (1 - r) at its peak.
void resonate(std::vector<double>& x, double hz, double bandwidth, double rate) {
    const double r = std::exp(-kPi * bandwidth / rate);
    const double theta = 2.0 * kPi * hz / rate;
    const double a1 = 2.0 * r * std::cos(theta);
    const double a2 = -r * r;
    const double gain = 1.0 - r;
    double y1 = 0.0;
    double y2 = 0.0;
    for (double& v : x) {
        const double y = gain * v + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        v = y;
    }
}

// x minus its centred box mean of `width` samples, in place, in linear time.
void remove_box_mean(std::vector<double>& x, std::size_t width) {
    if (x.empty() || width < 2) {
        return;
    }
    std::vector<double> prefix(x.size() + 1U, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        prefix[i + 1U] = prefix[i] + x[i];
    }
    const std::size_t half = width / 2U;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t lo = i > half ? i - half : 0U;
        const std::size_t hi = std::min(x.size(), i + half + 1U);
        x[i] -= (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo);
    }
}

std::vector<double> box_mean(const std::vector<double>& x, std::size_t width) {
    std::vector<double> mean(x.size(), 0.0);
    std::vector<double> prefix(x.size() + 1U, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        prefix[i + 1U] = prefix[i] + x[i];
    }
    const std::size_t half = width / 2U;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t lo = i > half ? i - half : 0U;
        const std::size_t hi = std::min(x.size(), i + half + 1U);
        mean[i] = (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo);
    }
    return mean;
}

}  // namespace

Voice synthesize_voice(std::span<const VoiceNote> notes, const VoiceOptions& options) {
    Voice voice;
    const double rate = static_cast<double>(options.sample_rate);
    if (notes.empty() || options.sample_rate == 0) {
        return voice;
    }
    double end_s = 0.0;
    for (const VoiceNote& note : notes) {
        end_s = std::max(end_s, note.start_s + note.duration_s);
    }
    const std::size_t n =
        static_cast<std::size_t>((options.lead_s + end_s + options.tail_s) * rate);
    Rng rng(options.seed);

    // The target pitch (ln Hz) and the note envelope per sample.
    std::vector<double> target(n, 0.0);
    std::vector<double> envelope(n, 0.0);
    std::vector<char> voiced(n, 0);
    for (const VoiceNote& note : notes) {
        const std::size_t a = static_cast<std::size_t>((options.lead_s + note.start_s) * rate);
        const std::size_t b = std::min(
            n, static_cast<std::size_t>((options.lead_s + note.start_s + note.duration_s) * rate));
        if (b <= a) {
            continue;
        }
        const std::size_t ramp = std::min(static_cast<std::size_t>(0.02 * rate), (b - a) / 2U);
        const double ln_hz = std::log(hz_from_midi(note.midi));
        for (std::size_t i = a; i < b; ++i) {
            target[i] = ln_hz;
            voiced[i] = 1;
            double e = 1.0;
            if (ramp > 0 && i - a < ramp) {
                e = static_cast<double>(i - a) / static_cast<double>(ramp);
            } else if (ramp > 0 && b - i <= ramp) {
                e = static_cast<double>(b - i) / static_cast<double>(ramp);
            }
            envelope[i] = e;
        }
    }
    // Hold the previous pitch through gaps so that the glide does not dive towards zero.
    double last = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (voiced[i] != 0) {
            last = target[i];
        } else {
            target[i] = last;
        }
    }
    std::size_t glide =
        std::max<std::size_t>(1U, static_cast<std::size_t>(options.portamento_s * rate));
    std::vector<double> smooth = box_mean(target, glide);

    // Vibrato (ramped in) and a high-passed random walk of jitter.
    std::vector<double> jitter(n, 0.0);
    double walk = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        walk += rng.gauss() * options.jitter * 0.05;
        jitter[i] = walk;
    }
    remove_box_mean(jitter, 400);

    voice.f0.assign(n, 0.0F);
    std::vector<double> f0(n, 0.0);
    const double vibrato = options.vibrato_cents / 1200.0 * std::log(2.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (voiced[i] == 0) {
            continue;
        }
        const double t = static_cast<double>(i) / rate;
        const double ramp = std::clamp(t * 4.0, 0.0, 1.0);
        f0[i] = std::exp(smooth[i] + vibrato * std::sin(2.0 * kPi * options.vibrato_hz * t) * ramp +
                         jitter[i]);
        voice.f0[i] = static_cast<float>(f0[i]);
    }

    // Additive harmonics up to 7.5 kHz, amplitudes falling as h^-1.4, random phases.
    constexpr std::size_t kMaxHarmonics = 80;
    std::array<double, kMaxHarmonics + 1U> phase_offset{};
    for (double& p : phase_offset) {
        p = rng.uniform() * 2.0 * kPi;
    }
    std::vector<double> x(n, 0.0);
    double phi = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (voiced[i] == 0) {
            continue;
        }
        phi += 2.0 * kPi * f0[i] / rate;
        const std::size_t top = std::min(kMaxHarmonics, static_cast<std::size_t>(7500.0 / f0[i]));
        double s = 0.0;
        for (std::size_t h = 1; h <= top; ++h) {
            s += std::pow(static_cast<double>(h), -1.4) *
                 std::sin(static_cast<double>(h) * phi + phase_offset[h]);
        }
        x[i] = s;
    }
    const auto& vowel = kVowels[options.vowel % kVowels.size()];
    for (const Formant& f : vowel) {
        resonate(x, f.hz, f.bandwidth, rate);
    }
    double peak = 0.0;
    for (const double v : x) {
        peak = std::max(peak, std::fabs(v));
    }
    if (peak > 0.0) {
        for (double& v : x) {
            v /= peak;
        }
    }

    // Breath noise, coloured around 1.8 kHz, added to the voiced stretch and shaped by the
    // envelope.
    std::vector<double> breath(n, 0.0);
    for (double& v : breath) {
        v = rng.gauss();
    }
    resonate(breath, 1800.0, 900.0, rate);
    double breath_peak = 0.0;
    for (const double v : breath) {
        breath_peak = std::max(breath_peak, std::fabs(v));
    }
    const double breath_gain =
        std::pow(10.0, options.breath_db / 20.0) / std::max(breath_peak, 1e-12);
    double out_peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = (x[i] + breath_gain * breath[i]) * envelope[i];
        out_peak = std::max(out_peak, std::fabs(x[i]));
    }
    const double scale = out_peak > 0.0 ? options.amplitude / out_peak : 0.0;
    for (double& v : x) {
        v *= scale;
    }

    // Room noise (a low-passed Gaussian) at the requested signal-to-noise ratio over the voiced
    // samples.
    if (options.snr_db) {
        double power = 0.0;
        std::size_t count = 0;
        for (const double v : x) {
            if (std::fabs(v) > 1e-3) {
                power += v * v;
                ++count;
            }
        }
        if (count > 0) {
            power /= static_cast<double>(count);
            std::vector<double> noise(n, 0.0);
            double y = 0.0;
            for (double& v : noise) {
                y = 0.3 * rng.gauss() + 0.7 * y;
                v = y;
            }
            double noise_power = 0.0;
            for (const double v : noise) {
                noise_power += v * v;
            }
            noise_power /= static_cast<double>(n);
            const double wanted = power / std::pow(10.0, *options.snr_db / 10.0);
            const double gain = noise_power > 0.0 ? std::sqrt(wanted / noise_power) : 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                x[i] += gain * noise[i];
            }
        }
    }

    voice.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        voice.samples[i] = static_cast<float>(x[i]);
    }
    return voice;
}

std::vector<VoiceNote> random_melody(double low_midi, double high_midi, std::uint32_t count,
                                     std::uint32_t seed) {
    std::vector<VoiceNote> notes;
    Rng rng(seed);
    constexpr std::array<double, 4> kDurations = {0.25, 0.4, 0.6, 0.9};
    constexpr std::array<double, 4> kGaps = {0.0, 0.0, 0.15, 0.4};
    double t = 0.0;
    double midi = std::floor((low_midi + high_midi) / 2.0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const double d = kDurations[static_cast<std::size_t>(rng.next() % kDurations.size())];
        const double step = static_cast<double>(static_cast<int>(rng.next() % 9U) - 4);
        midi = std::clamp(midi + step, std::ceil(low_midi), std::floor(high_midi));
        notes.push_back({t, d, midi});
        t += d + kGaps[static_cast<std::size_t>(rng.next() % kGaps.size())];
    }
    return notes;
}

}  // namespace iclforge::pitch
