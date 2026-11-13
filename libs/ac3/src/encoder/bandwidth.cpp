#include "iclforge/ac3/encoder/bandwidth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/ac3/core/bitalloc.hpp"
#include "iclforge/ac3/core/bitalloc_tables.hpp"
#include "iclforge/ac3/core/exponents.hpp"
#include "iclforge/ac3/core/types.hpp"

namespace iclforge::ac3::encoder {

namespace {

// §7.1.3 in reverse. endmant = (chbwcod + 12) * 3 + 37, so the grid is every
// third bin from 73; anything between two grid points rounds up.
constexpr int kMinEndmant = endmant_for_chbwcod(0);
constexpr int kMaxEndmant = endmant_for_chbwcod(60);

template <typename Scalar>
void accumulate_peak_exponents_over(std::span<const Scalar> coefficients,
                                    std::span<std::uint8_t> peak_exponents) {
    const std::size_t bins = std::min(coefficients.size(), peak_exponents.size());
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const auto exponent =
            static_cast<std::uint8_t>(exponent_from_fixed(to_fixed25(coefficients[bin])));
        peak_exponents[bin] = std::min(peak_exponents[bin], exponent);
    }
}

template <typename Scalar>
void accumulate_peak_scan_over(PeakScan<Scalar>& scan, std::span<const Scalar> coefficients) {
    const std::size_t bins = std::min(coefficients.size(), kPeakScanBins);
    const Scalar* const in = coefficients.data();
    Scalar* const peak = scan.magnitude.data();
    // The NaN test comes first, as a loop of its own, OR-ed rather than
    // branched on: folded into the max loop it made MSVC leave both scalar.
    unsigned unordered = 0;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        unordered |= static_cast<unsigned>(std::isunordered(in[bin], in[bin]));
    }
    if (unordered != 0) {
        // A NaN in this block. It is excluded before any float-to-int
        // conversion (that cast is undefined for a NaN) and before any
        // max, whose NaN behaviour differs between targets (fmax on arm64
        // propagates it). A NaN counts as a zero coefficient: exponent
        // kMaxExponent, neutral to the minimum, and no raise of the magnitude.
        // Rare path, so a plain scalar loop with explicit branches.
        for (std::size_t bin = 0; bin < bins; ++bin) {
            if (!std::isnan(in[bin])) {
                const Scalar a = std::abs(in[bin]);
                if (a > peak[bin]) {
                    peak[bin] = a;
                }
            }
        }
        return;
    }
    // `a > peak ? a : peak`, not std::max: the select is the form a compiler
    // turns into a vector max (maxps/maxpd on x86-64, checked in the MSVC
    // disassembly). No NaN can be here, so the target's max semantics for
    // one do not matter.
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const Scalar a = std::abs(in[bin]);
        peak[bin] = a > peak[bin] ? a : peak[bin];
    }
}

template <typename Scalar>
void peak_scan_exponents_over(const PeakScan<Scalar>& scan, std::span<std::uint8_t> exponents) {
    const std::size_t bins = std::min(exponents.size(), kPeakScanBins);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const auto exponent = static_cast<std::uint8_t>(
            exponent_from_fixed(to_fixed25(scan.magnitude[bin])));
        exponents[bin] = std::min(scan.floor_exponent[bin], exponent);
    }
}

}  // namespace

void accumulate_peak_scan(PeakScan<double>& scan, std::span<const double> coefficients) {
    accumulate_peak_scan_over<double>(scan, coefficients);
}

void accumulate_peak_scan(PeakScan<float>& scan, std::span<const float> coefficients) {
    accumulate_peak_scan_over<float>(scan, coefficients);
}

void peak_scan_exponents(const PeakScan<double>& scan, std::span<std::uint8_t> exponents) {
    peak_scan_exponents_over<double>(scan, exponents);
}

void peak_scan_exponents(const PeakScan<float>& scan, std::span<std::uint8_t> exponents) {
    peak_scan_exponents_over<float>(scan, exponents);
}

int chbwcod_for_endmant(int endmant) {
    if (endmant <= kMinEndmant) {
        return 0;
    }
    if (endmant >= kMaxEndmant) {
        return 60;
    }
    // Round up: (endmant - 37 + 2) / 3 is the smallest count of 3-bin groups
    // that reaches endmant.
    return std::clamp(((endmant - 37 + 2) / 3) - 12, 0, 60);
}

void accumulate_peak_exponents(std::span<const double> coefficients,
                               std::span<std::uint8_t> peak_exponents) {
    accumulate_peak_exponents_over<double>(coefficients, peak_exponents);
}

void accumulate_peak_exponents(std::span<const float> coefficients,
                               std::span<std::uint8_t> peak_exponents) {
    accumulate_peak_exponents_over<float>(coefficients, peak_exponents);
}

int audible_endmant(std::span<const std::uint8_t> peak_exponents, SampleRate sample_rate) {
    const auto bins = static_cast<int>(
        std::min<std::size_t>(peak_exponents.size(), static_cast<std::size_t>(kMaxEndmant)));
    if (bins <= kMinEndmant) {
        return kMinEndmant;
    }

    // §7.2.2.2: exponents -> 13-bit signed log PSD, 128 units per exponent
    // step (one 6 dB Table 5.17 step).
    std::array<int, 253> psd{};
    for (int bin = 0; bin < bins; ++bin) {
        psd[static_cast<std::size_t>(bin)] =
            3072 - (static_cast<int>(peak_exponents[static_cast<std::size_t>(bin)]) << 7);
    }
    const std::array<int, 50> bndpsd = band_psd(psd, 0, bins);
    const auto& hth =
        *tables::kHearingThreshold[static_cast<std::size_t>(fscod_family(sample_rate))];

    // Walk down from the top: the answer is the end of the highest band that
    // still stands above the threshold. Walking down rather than up is what
    // makes a quiet gap between two audible bands irrelevant - the band edge
    // is a property of where content STOPS, not of the first hole in it.
    const int top_band = bin_to_band(bins - 1);
    for (int band = top_band; band >= 0; --band) {
        if (bndpsd[static_cast<std::size_t>(band)] <= hth[static_cast<std::size_t>(band)]) {
            continue;
        }
        // std::min<int>, not bare std::min: kBandStart/kBandSize are
        // std::int32_t, which is 'long int' on arm-none-eabi and 'int' on the
        // hosted targets this had only ever been compiled for. Bare std::min
        // cannot deduce one type from the two and fails to compile there -
        // which is how a bare-metal build of the ENCODER found it, the decoder
        // having never reached this file.
        const int end = std::min<int>(tables::kBandStart[static_cast<std::size_t>(band)] +
                                          tables::kBandSize[static_cast<std::size_t>(band)],
                                      bins);
        return std::clamp(end, kMinEndmant, kMaxEndmant);
    }
    return kMinEndmant;
}

int rate_ceiling_chbwcod(std::uint32_t bitrate_kbps, int nfchans) {
    const int per_channel_kbps = static_cast<int>(bitrate_kbps) / std::max(nfchans, 1);
    return std::clamp(per_channel_kbps * 2 / 3, 24, 60);
}

int choose_chbwcod(std::uint32_t bitrate_kbps, int nfchans,
                   std::span<const std::uint8_t> peak_exponents, SampleRate sample_rate,
                   int previous_chbwcod) {
    const int ceiling = rate_ceiling_chbwcod(bitrate_kbps, nfchans);
    const int per_channel_kbps = static_cast<int>(bitrate_kbps) / std::max(nfchans, 1);
    if (per_channel_kbps >= kContentNarrowingCeiling) {
        return ceiling;
    }
    const int content = chbwcod_for_endmant(audible_endmant(peak_exponents, sample_rate));
    int target = std::min(ceiling, content);
    if (previous_chbwcod >= 0) {
        target = std::max(target, previous_chbwcod - kMaxNarrowStep);
    }
    return std::clamp(target, 0, ceiling);
}

}  // namespace iclforge::ac3::encoder
