#pragma once

// The analysis a Meters view would need, written the way the board would run it so that what the preview draws is
// what the board would be given (planning/p4x-meters-view.md):
//
//   the player writes ONE mono mix of what it renders (a sum of the outputs, one integer ring of 48 kHz samples):
//   nothing else is done on the audio path. A panel-side task reads the ring every 50 ms and
//     - for the bands up to 800 Hz, takes the last 171 ms with four samples summed to one (a 12 kHz stream) and
//       runs a 2048-point real FFT: 5.9 Hz a bin, so a third-octave band at 25 Hz is a bin and a half wide;
//     - for the bands from 1 kHz, takes the last 21 ms at 48 kHz and runs a 1024-point real FFT: 47 Hz a bin;
//   each band's power is the power of the bins that fall in it (an edge bin is shared by the overlap).
//
// A real FFT of 2 N points is the family's complex Stockham transform (iclforge/dsp/detail/fft_stockham.hpp, the one
// AC-4 and the MDCT run on the P4, float, single precision) of N points, unpacked: z[n] = x[2n] + i x[2n + 1].

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "iclforge/dsp/detail/fft_stockham.hpp"

namespace meters {

inline constexpr int kRate = 48000;
inline constexpr int kBands = 31;
// ISO 266 third-octave centres, 20 Hz to 20 kHz (the bands of a 31-band graphic equaliser).
inline constexpr std::array<float, kBands> kCentre = {20.0F,   25.0F,   31.5F,   40.0F,   50.0F,   63.0F,    80.0F,    100.0F,
                                                      125.0F,  160.0F,  200.0F,  250.0F,  315.0F,  400.0F,   500.0F,   630.0F,
                                                      800.0F,  1000.0F, 1250.0F, 1600.0F, 2000.0F, 2500.0F,  3150.0F,  4000.0F,
                                                      5000.0F, 6300.0F, 8000.0F, 10000.0F, 12500.0F, 16000.0F, 20000.0F};
// The bands the low path (the 12 kHz stream) answers; the rest are the high path's.
inline constexpr int kLowBands = 17;  // 20 Hz to 800 Hz

inline constexpr std::size_t kLowFrame = 2048;   // at 12 kHz: 171 ms
inline constexpr std::size_t kHighFrame = 1024;  // at 48 kHz: 21 ms
inline constexpr std::size_t kDecimate = 4;
inline constexpr std::size_t kRingLength = kLowFrame * kDecimate;  // 8192 samples at 48 kHz

inline constexpr float kSilentDb = -120.0F;

// The power of the bins of a real frame of `Frame` samples, one-sided, 0 to Frame / 2 inclusive.
template <std::size_t Frame>
class RealPower {
   public:
    static constexpr std::size_t kHalf = Frame / 2;

    RealPower() {
        window_.resize(Frame);
        double sum2 = 0.0;
        for (std::size_t n = 0; n < Frame; ++n) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / Frame);
            window_[n] = static_cast<float>(w);
            sum2 += w * w;
        }
        energy_ = sum2;
        twiddle_.resize(kHalf + 1);
        for (std::size_t k = 0; k <= kHalf; ++k) {
            const double a = 2.0 * std::numbers::pi * static_cast<double>(k) / Frame;
            twiddle_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(-std::sin(a))};
        }
    }

    // frame: Frame samples, oldest first. power: kHalf + 1 values, scaled so that the mean square of the signal in a band
    // is two times the sum of its bins' (Parseval, one-sided): see `band_power`.
    void power(std::span<const float> frame, std::span<float> out) {
        std::array<float, kHalf> re;
        std::array<float, kHalf> im;
        for (std::size_t n = 0; n < kHalf; ++n) {
            re[n] = window_[2 * n] * frame[2 * n];
            im[n] = window_[2 * n + 1] * frame[2 * n + 1];
        }
        iclforge::dsp::fft::stockham_forward<kHalf, float, float>(tables_, std::span<float, kHalf>(re), std::span<float, kHalf>(im));
        for (std::size_t k = 0; k <= kHalf; ++k) {
            const std::size_t a = k % kHalf;
            const std::size_t b = (kHalf - k) % kHalf;
            const float zr = re[a];
            const float zi = im[a];
            const float cr = re[b];
            const float ci = -im[b];
            const float er = 0.5F * (zr + cr);
            const float ei = 0.5F * (zi + ci);
            const float or_ = 0.5F * (zi - ci);
            const float oi = -0.5F * (zr - cr);
            const auto w = twiddle_[k];
            const float xr = er + w.first * or_ - w.second * oi;
            const float xi = ei + w.first * oi + w.second * or_;
            out[k] = xr * xr + xi * xi;
        }
    }

    // Mean-square of the signal that the bins' power adds up to: 2 / (Frame sum w^2) per bin.
    [[nodiscard]] double scale() const { return 2.0 / (static_cast<double>(Frame) * energy_); }

   private:
    iclforge::dsp::fft::StockhamTables<kHalf, float> tables_;
    std::vector<float> window_;
    std::vector<std::pair<float, float>> twiddle_;
    double energy_ = 1.0;
};

// Which bins make up a band, at a frame's bin width: (bin, share of the bin that lies in the band).
struct BandBins {
    std::vector<std::pair<int, float>> bins;
};

[[nodiscard]] inline BandBins bins_of(float lo_hz, float hi_hz, double bin_hz, std::size_t last_bin) {
    BandBins out;
    const int first = std::max(1, static_cast<int>(std::floor(lo_hz / bin_hz + 0.5)) - 1);
    const int last = std::min<int>(static_cast<int>(last_bin) - 1, static_cast<int>(std::ceil(hi_hz / bin_hz + 0.5)) + 1);
    for (int k = first; k <= last; ++k) {
        const double b_lo = (k - 0.5) * bin_hz;
        const double b_hi = (k + 0.5) * bin_hz;
        const double share = (std::min<double>(hi_hz, b_hi) - std::max<double>(lo_hz, b_lo)) / bin_hz;
        if (share > 0.0) {
            out.bins.emplace_back(k, static_cast<float>(share));
        }
    }
    return out;
}

[[nodiscard]] inline float band_low_hz(int band) { return kCentre[static_cast<std::size_t>(band)] * std::pow(2.0F, -1.0F / 6.0F); }
[[nodiscard]] inline float band_high_hz(int band) { return kCentre[static_cast<std::size_t>(band)] * std::pow(2.0F, 1.0F / 6.0F); }

[[nodiscard]] inline float to_db(double mean_square) {
    // A full-scale sine has a mean square of 0.5.
    return mean_square <= 1e-12 ? kSilentDb : static_cast<float>(10.0 * std::log10(mean_square / 0.5));
}

// What is read from the ring each time: the two frames, and their bands.
template <std::size_t HighFrame = kHighFrame>
class AnalyzerOf {
   public:
    AnalyzerOf() : ring_(kRingLength, 0.0F) {
        for (int b = 0; b < kBands; ++b) {
            if (b < kLowBands) {
                low_bins_[static_cast<std::size_t>(b)] = bins_of(band_low_hz(b), band_high_hz(b), static_cast<double>(kRate) / kDecimate / kLowFrame, kLowFrame / 2);
            } else {
                high_bins_[static_cast<std::size_t>(b)] = bins_of(band_low_hz(b), band_high_hz(b), static_cast<double>(kRate) / HighFrame, HighFrame / 2);
            }
        }
    }

    // The mono mix, as the player writes it (floats here; the board's are 16-bit integers).
    void push(std::span<const float> mono) {
        for (const float v : mono) {
            ring_[head_] = v;
            head_ = (head_ + 1) % kRingLength;
        }
    }

    // The levels of the 31 bands, dB below a full-scale sine's power, of the ring as it is.
    void analyze(std::array<float, kBands>& db) {
        std::array<float, kLowFrame> low;
        std::array<float, HighFrame> high;
        // The ring, oldest first, from `head_`.
        const auto at = [this](std::size_t age_from_oldest) { return ring_[(head_ + age_from_oldest) % kRingLength]; };
        for (std::size_t n = 0; n < kLowFrame; ++n) {
            float sum = 0.0F;
            for (std::size_t j = 0; j < kDecimate; ++j) {
                sum += at(n * kDecimate + j);
            }
            low[n] = sum * (1.0F / kDecimate);
        }
        for (std::size_t n = 0; n < HighFrame; ++n) {
            high[n] = at(kRingLength - HighFrame + n);
        }
        low_power_.resize(kLowFrame / 2 + 1);
        high_power_.resize(HighFrame / 2 + 1);
        low_fft_.power(low, low_power_);
        high_fft_.power(high, high_power_);
        for (int b = 0; b < kBands; ++b) {
            const bool is_low = b < kLowBands;
            const BandBins& bins = is_low ? low_bins_[static_cast<std::size_t>(b)] : high_bins_[static_cast<std::size_t>(b)];
            const std::vector<float>& p = is_low ? low_power_ : high_power_;
            double sum = 0.0;
            for (const auto& [k, share] : bins.bins) {
                sum += static_cast<double>(share) * p[static_cast<std::size_t>(k)];
            }
            db[static_cast<std::size_t>(b)] = to_db(sum * (is_low ? low_fft_.scale() : high_fft_.scale()));
        }
    }

   private:
    std::vector<float> ring_;
    std::size_t head_ = 0;
    RealPower<kLowFrame> low_fft_;
    RealPower<HighFrame> high_fft_;
    std::array<BandBins, kBands> low_bins_;
    std::array<BandBins, kBands> high_bins_;
    std::vector<float> low_power_;
    std::vector<float> high_power_;
};

using Analyzer = AnalyzerOf<>;

// What the analysis is held against: one 8192-point FFT of the ring at 48 kHz (5.9 Hz a bin at every band, and the
// price of a 4096-point complex transform).
class Reference {
   public:
    Reference() : ring_(kRingLength, 0.0F) {
        for (int b = 0; b < kBands; ++b) {
            bins_[static_cast<std::size_t>(b)] = bins_of(band_low_hz(b), band_high_hz(b), static_cast<double>(kRate) / kRingLength, kRingLength / 2);
        }
    }
    void push(std::span<const float> mono) {
        for (const float v : mono) {
            ring_[head_] = v;
            head_ = (head_ + 1) % kRingLength;
        }
    }
    void analyze(std::array<float, kBands>& db) {
        std::vector<float> frame(kRingLength);
        for (std::size_t n = 0; n < kRingLength; ++n) {
            frame[n] = ring_[(head_ + n) % kRingLength];
        }
        power_.resize(kRingLength / 2 + 1);
        fft_.power(frame, power_);
        for (int b = 0; b < kBands; ++b) {
            double sum = 0.0;
            for (const auto& [k, share] : bins_[static_cast<std::size_t>(b)].bins) {
                sum += static_cast<double>(share) * power_[static_cast<std::size_t>(k)];
            }
            db[static_cast<std::size_t>(b)] = to_db(sum * fft_.scale());
        }
    }

   private:
    std::vector<float> ring_;
    std::size_t head_ = 0;
    RealPower<kRingLength> fft_;
    std::array<BandBins, kBands> bins_;
    std::vector<float> power_;
};

// The time average of a real-time analyser: each band's power, not its level, follows a one-pole with the time
// constant `tau` (0.1 s is "fast", 1 s "slow"), so that a noise-like band stops flickering by the few degrees of freedom
// of one short frame.
struct PowerAverage {
    std::array<double, kBands> power{};
    bool primed = false;
    void feed(const std::array<float, kBands>& db, float seconds, float tau, std::array<float, kBands>& out) {
        const double a = 1.0 - std::exp(-static_cast<double>(seconds) / tau);
        for (std::size_t b = 0; b < kBands; ++b) {
            const double p = db[b] <= kSilentDb ? 0.0 : 0.5 * std::pow(10.0, db[b] / 10.0);
            power[b] = primed ? power[b] + a * (p - power[b]) : p;
            out[b] = to_db(power[b]);
        }
        primed = true;
    }
};

// --- ballistics ----------------------------------------------------------------------------------------------------

// A bar that rises at once and falls slowly, with a peak that is held and then falls.
struct Ballistic {
    float level = kSilentDb;
    float peak = kSilentDb;
    float hold_left = 0.0F;

    void update(float db, float seconds, float fall_db_per_s, float hold_s, float peak_fall_db_per_s) {
        level = std::max(db, level - fall_db_per_s * seconds);
        if (level >= peak) {
            peak = level;
            hold_left = hold_s;
        } else if (hold_left > 0.0F) {
            hold_left -= seconds;
        } else {
            peak = std::max(level, peak - peak_fall_db_per_s * seconds);
        }
    }
};

}  // namespace meters
