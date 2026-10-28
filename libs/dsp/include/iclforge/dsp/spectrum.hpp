#pragma once

// A third-octave spectrum of a mono signal, for a meter: 31 bands from 20 Hz to 20 kHz (the ISO 266 series of a 31-band
// graphic equaliser), each the level of the signal in it in dB below a full-scale sine's power, from the newest 8,192 samples of a
// 48 kHz stream (171 ms), as 16-bit integers: what a player can put in a ring for nothing (planning/p4x-meters-view.md).
//
// It is the family's own FFT (iclforge/dsp/detail/fft_stockham.hpp, the passes AC-4 and the MDCT run) in float, twice:
//
//   - for the 17 bands from 20 Hz to 800 Hz, the newest 171 ms with four samples summed to one (a 12 kHz stream) and a
//     2,048-point real FFT: 5.9 Hz a bin, so the 25 Hz band is a bin and a half wide;
//   - for the 14 bands from 1 kHz, the newest 21 ms at 48 kHz and a 1,024-point real FFT: 47 Hz a bin.
//
// Below 800 Hz it agrees with one 8,192-point FFT of the same samples (test_spectrum.cpp holds it to that); above, it sees the newest 21
// ms where the long one integrates 171, so on music it is the livelier of the two: that is its time resolution and not an error. The cost is
// about 100 kflops a pass (the long FFT's 290), which the caller runs every 50 ms.
//
// A real frame of 2 N samples is the complex transform of N points, unpacked: z[n] = x[2n] + i x[2n + 1]. The band's level is the
// power of the bins that fall in it, the bins at its edges shared by the overlap. Nothing here allocates after construction
// (about 63 KB of tables and scratch, which a caller allocates once, not on a stack) and nothing a second costs is double (a P4 has no double unit): the library's transform keeps two
// work buffers on the stack (8 KB at 1,024 points), which the panel's 16 KB stack, with 9 KB of it already spoken for by LVGL, cannot spare,
// so the passes are run here in buffers the analyzer owns (detail::forward_in, held bit for bit to the library's by test_spectrum.cpp).
//
// Not safe to call from two threads at once: it owns its scratch.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/dsp/detail/fft_stockham.hpp"

namespace iclforge::dsp::spectrum {

inline constexpr int kRate = 48000;
inline constexpr int kBands = 31;
// ISO 266 third-octave centres, 20 Hz to 20 kHz.
inline constexpr std::array<float, kBands> kCentreHz = {20.0F,   25.0F,   31.5F,   40.0F,   50.0F,    63.0F,    80.0F,    100.0F,
                                                        125.0F,  160.0F,  200.0F,  250.0F,  315.0F,   400.0F,   500.0F,   630.0F,
                                                        800.0F,  1000.0F, 1250.0F, 1600.0F, 2000.0F,  2500.0F,  3150.0F,  4000.0F,
                                                        5000.0F, 6300.0F, 8000.0F, 10000.0F, 12500.0F, 16000.0F, 20000.0F};
// The bands the 12 kHz stream answers; the rest are the 48 kHz frame's.
inline constexpr int kLowBands = 17;

inline constexpr std::size_t kLowFrame = 2048;   // at 12 kHz: 171 ms
inline constexpr std::size_t kHighFrame = 1024;  // at 48 kHz: 21 ms
inline constexpr std::size_t kDecimate = 4;
// The samples the analysis reads: the newest, oldest first.
inline constexpr std::size_t kHistory = kLowFrame * kDecimate;

inline constexpr float kSilentDb = -120.0F;

// A full-scale sine has a mean square of 0.5: 0 dB.
// 10^(dB / 10) and 10 log10(x) in single precision: the P4 has no double-precision unit, and a double pow or log10 there is several microseconds, thirty
// times an update (planning/p4x-meters-view.md, "Kit results"). A float keeps 1e-6 dB of the 0.01 the bars are read to.
[[nodiscard]] inline float db_to_power(float db) { return std::exp2(db * 0.33219281F); }
[[nodiscard]] inline float power_to_db(float power) { return 3.01029996F * std::log2(power); }

[[nodiscard]] inline float to_db_f(float mean_square) { return mean_square <= 1e-12F ? kSilentDb : power_to_db(mean_square / 0.5F); }

[[nodiscard]] inline float to_db(double mean_square) {
    return mean_square <= 1e-12 ? kSilentDb : static_cast<float>(10.0 * std::log10(mean_square / 0.5));
}

// A band's edges: a sixth of an octave either side of its centre.
[[nodiscard]] inline float band_low_hz(int band) { return kCentreHz[static_cast<std::size_t>(band)] * std::exp2(-1.0F / 6.0F); }
[[nodiscard]] inline float band_high_hz(int band) { return kCentreHz[static_cast<std::size_t>(band)] * std::exp2(1.0F / 6.0F); }

namespace detail {

// fft::stockham_forward with its two work buffers given, so that it keeps nothing on the stack: the same passes, the same twiddles, the same
// order, the result in `re` and `im`.
template <std::size_t P>
void forward_in(const fft::StockhamTables<P, float>& t, std::span<float, P> re, std::span<float, P> im, std::span<float, P> re2,
                std::span<float, P> im2) {
    using tiered::fft_kernels::pass_to;
    using tiered::fft_kernels::static_for;
    float* const buffers_re[2] = {re.data(), re2.data()};
    float* const buffers_im[2] = {im.data(), im2.data()};
    constexpr auto kPasses = static_cast<int>(fft::StockhamTables<P, float>::kPasses);
    static_for<kPasses>([&](auto ic) {
        constexpr auto i = static_cast<std::size_t>(decltype(ic)::value);
        using Shape = fft::detail::PassShape<P, i>;
        const float* const src_re = buffers_re[i % 2];
        const float* const src_im = buffers_im[i % 2];
        float* const dst_re = buffers_re[(i + 1) % 2];
        float* const dst_im = buffers_im[(i + 1) % 2];
        const fft::Complex<float>* const tw = t.twiddles.data() + Shape::offset;
        const auto in = [src_re, src_im](std::size_t index) noexcept { return fft::Complex<float>{src_re[index], src_im[index]}; };
        const auto store = [dst_re, dst_im](std::size_t index, fft::Complex<float> value) noexcept {
            dst_re[index] = value.re;
            dst_im[index] = value.im;
        };
        pass_to<float, static_cast<int>(Shape::r), false, true>(in, store, std::integral_constant<std::size_t, Shape::m>{},
                                                                 std::integral_constant<std::size_t, Shape::s>{}, tw, tw, tw);
    });
    if constexpr (kPasses % 2 != 0) {
        for (std::size_t k = 0; k < P; ++k) {
            re[k] = re2[k];
            im[k] = im2[k];
        }
    }
}

// The power of the bins of a real frame of `Frame` samples, one-sided: 0 to Frame / 2 inclusive. The tables are the window (a periodic Hann,
// symmetric, so half of it), the factors e^(-2 pi i k / Frame) of the unpacking, and the transform's.
template <std::size_t Frame>
class RealPower {
   public:
    static constexpr std::size_t kHalf = Frame / 2;

    RealPower() {
        double sum2 = 0.0;
        for (std::size_t n = 0; n <= kHalf; ++n) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(Frame));
            window_[n] = static_cast<float>(w);
        }
        for (std::size_t n = 0; n < Frame; ++n) {
            const double w = static_cast<double>(window_at(n));
            sum2 += w * w;
        }
        energy_ = sum2;
        for (std::size_t k = 0; k <= kHalf; ++k) {
            const double a = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(Frame);
            cos_[k] = static_cast<float>(std::cos(a));
            sin_[k] = static_cast<float>(-std::sin(a));
        }
    }

    // `frame`: Frame samples, oldest first. `re` and `im`: kHalf values of scratch each, and `re2` and `im2` the transform's work buffers, the
    // same. `out`: kHalf + 1 values.
    void power(std::span<const float, Frame> frame, std::span<float, kHalf> re, std::span<float, kHalf> im, std::span<float, kHalf> re2,
               std::span<float, kHalf> im2, std::span<float, kHalf + 1> out) const {
        for (std::size_t n = 0; n < kHalf; ++n) {
            re[n] = window_at(2 * n) * frame[2 * n];
            im[n] = window_at(2 * n + 1) * frame[2 * n + 1];
        }
        forward_in<kHalf>(tables_, re, im, re2, im2);
        for (std::size_t k = 0; k <= kHalf; ++k) {
            const std::size_t a = k % kHalf;
            const std::size_t b = (kHalf - k) % kHalf;
            const float zr = re[a];
            const float zi = im[a];
            const float cr = re[b];
            const float ci = -im[b];
            const float even_re = 0.5F * (zr + cr);
            const float even_im = 0.5F * (zi + ci);
            const float odd_re = 0.5F * (zi - ci);
            const float odd_im = -0.5F * (zr - cr);
            const float xr = even_re + cos_[k] * odd_re - sin_[k] * odd_im;
            const float xi = even_im + cos_[k] * odd_im + sin_[k] * odd_re;
            out[k] = xr * xr + xi * xi;
        }
    }

    // The mean square the bins' power adds up to, a bin at a time (Parseval, one-sided): 2 / (Frame times the window's energy).
    [[nodiscard]] double scale() const { return 2.0 / (static_cast<double>(Frame) * energy_); }

   private:
    [[nodiscard]] float window_at(std::size_t n) const { return window_[std::min(n, Frame - n)]; }

    fft::StockhamTables<kHalf, float> tables_;
    std::array<float, kHalf + 1> window_{};
    std::array<float, kHalf + 1> cos_{};
    std::array<float, kHalf + 1> sin_{};
    double energy_ = 1.0;
};

// The bins of a band at a frame's bin width: (bin, the share of the bin that lies in the band).
struct Bin {
    std::uint16_t index = 0;
    float share = 0.0F;
};

inline void bins_of(float lo_hz, float hi_hz, double bin_hz, std::size_t last_bin, std::vector<Bin>& out) {
    const double lo = static_cast<double>(lo_hz);
    const double hi = static_cast<double>(hi_hz);
    const auto first = std::max(1, static_cast<int>(std::floor(lo / bin_hz + 0.5)) - 1);
    const auto last = std::min(static_cast<int>(last_bin) - 1, static_cast<int>(std::ceil(hi / bin_hz + 0.5)) + 1);
    for (int k = first; k <= last; ++k) {
        const double b_lo = (static_cast<double>(k) - 0.5) * bin_hz;
        const double b_hi = (static_cast<double>(k) + 0.5) * bin_hz;
        const double share = (std::min(hi, b_hi) - std::max(lo, b_lo)) / bin_hz;
        if (share > 0.0) {
            out.push_back({static_cast<std::uint16_t>(k), static_cast<float>(share)});
        }
    }
}

}  // namespace detail

class Analyzer {
   public:
    Analyzer() {
        for (int b = 0; b < kBands; ++b) {
            const bool is_low = b < kLowBands;
            const double bin_hz = is_low ? static_cast<double>(kRate) / static_cast<double>(kDecimate * kLowFrame)
                                         : static_cast<double>(kRate) / static_cast<double>(kHighFrame);
            std::vector<detail::Bin>& bins = is_low ? low_bins_ : high_bins_;
            first_bin_[static_cast<std::size_t>(b)] = static_cast<std::uint16_t>(bins.size());
            detail::bins_of(band_low_hz(b), band_high_hz(b), bin_hz, is_low ? kLowFrame / 2 : kHighFrame / 2, bins);
            end_bin_[static_cast<std::size_t>(b)] = static_cast<std::uint16_t>(bins.size());
        }
    }

    // The levels of the 31 bands, in dB below a full-scale sine's power, of `history`: the newest kHistory samples, oldest first.
    void analyze(std::span<const std::int16_t, kHistory> history, std::array<float, kBands>& db) {
        analyze_low(history, db);
        analyze_high(history, db);
    }

    // The two halves of it, which a caller may run at different rates: the 17 bands up to 800 Hz look at the newest 171 ms (a window that long
    // changes little in 50 ms, so every other update loses nothing), the 14 above it at the newest 21 ms.
    void analyze_low(std::span<const std::int16_t, kHistory> history, std::array<float, kBands>& db) {
        constexpr float kInt16 = 1.0F / 32768.0F;
        for (std::size_t n = 0; n < kLowFrame; ++n) {
            std::int32_t sum = 0;
            for (std::size_t j = 0; j < kDecimate; ++j) {
                sum += history[n * kDecimate + j];
            }
            frame_[n] = static_cast<float>(sum) * (kInt16 / static_cast<float>(kDecimate));
        }
        low_.power(std::span<const float, kLowFrame>(frame_.data(), kLowFrame), std::span<float, kLowFrame / 2>(re_.data(), kLowFrame / 2),
                   std::span<float, kLowFrame / 2>(im_.data(), kLowFrame / 2), std::span<float, kLowFrame / 2>(re2_.data(), kLowFrame / 2),
                   std::span<float, kLowFrame / 2>(im2_.data(), kLowFrame / 2), std::span<float, kLowFrame / 2 + 1>(power_.data(), kLowFrame / 2 + 1));
        for (int b = 0; b < kLowBands; ++b) {
            db[static_cast<std::size_t>(b)] = band_level(b, low_bins_, static_cast<float>(low_.scale()));
        }
    }

    void analyze_high(std::span<const std::int16_t, kHistory> history, std::array<float, kBands>& db) {
        constexpr float kInt16 = 1.0F / 32768.0F;
        for (std::size_t n = 0; n < kHighFrame; ++n) {
            frame_[n] = static_cast<float>(history[kHistory - kHighFrame + n]) * kInt16;
        }
        high_.power(std::span<const float, kHighFrame>(frame_.data(), kHighFrame), std::span<float, kHighFrame / 2>(re_.data(), kHighFrame / 2),
                    std::span<float, kHighFrame / 2>(im_.data(), kHighFrame / 2), std::span<float, kHighFrame / 2>(re2_.data(), kHighFrame / 2),
                    std::span<float, kHighFrame / 2>(im2_.data(), kHighFrame / 2),
                    std::span<float, kHighFrame / 2 + 1>(power_.data(), kHighFrame / 2 + 1));
        for (int b = kLowBands; b < kBands; ++b) {
            db[static_cast<std::size_t>(b)] = band_level(b, high_bins_, static_cast<float>(high_.scale()));
        }
    }

   private:
    [[nodiscard]] float band_level(int band, const std::vector<detail::Bin>& bins, float scale) const {
        float sum = 0.0F;
        for (std::size_t i = first_bin_[static_cast<std::size_t>(band)]; i < end_bin_[static_cast<std::size_t>(band)]; ++i) {
            sum += bins[i].share * power_[bins[i].index];
        }
        return to_db_f(sum * scale);
    }

    detail::RealPower<kLowFrame> low_;
    detail::RealPower<kHighFrame> high_;
    std::vector<detail::Bin> low_bins_;
    std::vector<detail::Bin> high_bins_;
    std::array<std::uint16_t, kBands> first_bin_{};
    std::array<std::uint16_t, kBands> end_bin_{};
    // Scratch, shared by the two frames in turn.
    std::array<float, kLowFrame> frame_{};
    std::array<float, kLowFrame / 2> re_{};
    std::array<float, kLowFrame / 2> im_{};
    std::array<float, kLowFrame / 2> re2_{};
    std::array<float, kLowFrame / 2> im2_{};
    std::array<float, kLowFrame / 2 + 1> power_{};
};

// What one long FFT of the same samples says, for the tests to hold the analysis against (and for nothing else: 290 kflops and 3.4 times the work).
class Reference {
   public:
    Reference() {
        for (int b = 0; b < kBands; ++b) {
            first_bin_[static_cast<std::size_t>(b)] = static_cast<std::uint16_t>(bins_.size());
            detail::bins_of(band_low_hz(b), band_high_hz(b), static_cast<double>(kRate) / static_cast<double>(kHistory), kHistory / 2, bins_);
            end_bin_[static_cast<std::size_t>(b)] = static_cast<std::uint16_t>(bins_.size());
        }
    }

    void analyze(std::span<const std::int16_t, kHistory> history, std::array<float, kBands>& db) {
        for (std::size_t n = 0; n < kHistory; ++n) {
            frame_[n] = static_cast<float>(history[n]) / 32768.0F;
        }
        fft_.power(std::span<const float, kHistory>(frame_), std::span<float, kHistory / 2>(re_), std::span<float, kHistory / 2>(im_),
                   std::span<float, kHistory / 2>(re2_), std::span<float, kHistory / 2>(im2_), std::span<float, kHistory / 2 + 1>(power_));
        for (std::size_t b = 0; b < static_cast<std::size_t>(kBands); ++b) {
            double sum = 0.0;
            for (std::size_t i = first_bin_[b]; i < end_bin_[b]; ++i) {
                sum += static_cast<double>(bins_[i].share) * static_cast<double>(power_[bins_[i].index]);
            }
            db[b] = to_db(sum * fft_.scale());
        }
    }

   private:
    detail::RealPower<kHistory> fft_;
    std::vector<detail::Bin> bins_;
    std::array<std::uint16_t, kBands> first_bin_{};
    std::array<std::uint16_t, kBands> end_bin_{};
    std::vector<float> frame_ = std::vector<float>(kHistory);
    std::vector<float> re_ = std::vector<float>(kHistory / 2);
    std::vector<float> im_ = std::vector<float>(kHistory / 2);
    std::vector<float> re2_ = std::vector<float>(kHistory / 2);
    std::vector<float> im2_ = std::vector<float>(kHistory / 2);
    std::vector<float> power_ = std::vector<float>(kHistory / 2 + 1);
};

// The time average of a real-time analyser: each band's power, not its level, follows a one-pole of time constant `tau` (0.1 s is "fast"), so that a
// noise-like band does not flicker by the few degrees of freedom of one short frame.
struct PowerAverage {
    std::array<float, kBands> power{};
    bool primed = false;

    void feed(const std::array<float, kBands>& db, float seconds, float tau, std::array<float, kBands>& out) {
        const float a = 1.0F - std::exp(-seconds / tau);
        for (std::size_t b = 0; b < static_cast<std::size_t>(kBands); ++b) {
            const float p = db[b] <= kSilentDb ? 0.0F : 0.5F * db_to_power(db[b]);
            power[b] = primed ? power[b] + a * (p - power[b]) : p;
            out[b] = to_db_f(power[b]);
        }
        primed = true;
    }
};

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

}  // namespace iclforge::dsp::spectrum
