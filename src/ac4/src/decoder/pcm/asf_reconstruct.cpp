#include "decoder/pcm/asf_reconstruct.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "decoder/pcm/pow43.hpp"
#include "tiered/scalar_traits.hpp"
#include "core/tables/sfb_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

using dsp::MantExp;

// sign(q) |q|^(4/3), clause 5.1.3.2, from pcm/pow43.hpp's table in read-only
// data. The note under clause 5.1.3.1's quant_spec makes 8 191 the largest
// magnitude, and asf.cpp refuses a longer ext_code; the clamp is for a caller that
// builds an SfData by hand.
template <typename R>
[[nodiscard]] R reconstruct_line(std::int32_t q) noexcept {
    const std::int64_t wide = q;
    const auto magnitude = static_cast<std::uint64_t>(wide < 0 ? -wide : wide);
    const R value =
        kPow43<R>[static_cast<std::size_t>(std::min<std::uint64_t>(magnitude, kMaxQuant))];
    return q < 0 ? -value : value;
}

// The sum of squares Pseudocodes 22 and 23 call band_rms before dividing it
// by the band's line count over every window of its group.
template <typename R>
[[nodiscard]] R band_energy(std::span<const R> scaled, std::size_t begin, std::size_t end) noexcept {
    R sum{};
    for (std::size_t k = begin; k < end; ++k) {
        sum += scaled[k] * scaled[k];
    }
    return sum;
}

// A track's bands as Pseudocodes 21 to 23 walk them: the audio spectral frontend's own, group by
// group (CoreBands), and, where the track's HSF extension is active, the extension's, group by
// group again (HsfBands). asf_scalefac_data() and asf_snf_data() read the core's scale factors
// and noise levels, and asf_hsf_scalefac_data() and asf_hsf_snf_data() (Tables 42b and 42c) the
// extension's after them, each a difference from the one transmitted before it, so the walk
// takes the core's bands first and the extension's next, carrying its state across (ERRATA.md,
// "Scale factors and noise levels across an HSF extension"). Line indices are into one vector:
// the core's lines, then the extension's, which follow them at `base`.
struct CoreBands {
    const SfData& data;

    [[nodiscard]] std::size_t count(std::size_t g) const noexcept {
        return static_cast<std::size_t>(std::max(data.max_sfb[g], 0));
    }
    [[nodiscard]] bool has_scale_factor(std::size_t g, std::size_t si) const noexcept {
        return data.scale_factor_present[g][si];
    }
    [[nodiscard]] int dpcm_sf(std::size_t g, std::size_t si) const noexcept {
        return data.dpcm_sf[g][si];
    }
    [[nodiscard]] std::size_t begin(std::size_t g, std::size_t si) const noexcept {
        return data.sect_sfb_offset[g][si];
    }
    [[nodiscard]] std::size_t end(std::size_t g, std::size_t si) const noexcept {
        return data.sect_sfb_offset[g][si + 1];
    }
    [[nodiscard]] bool has_noise_code(std::size_t g, std::size_t si) const noexcept {
        return data.snf_present[g][si];
    }
    [[nodiscard]] int dpcm_snf(std::size_t g, std::size_t si) const noexcept {
        return data.dpcm_snf[g][si];
    }
};

struct HsfBands {
    const HsfSfData& data;
    std::size_t base = 0;  // where the extension's lines start in the track's vector

    // Bands past the end of what asf_hsf_scalefac_data() sized are not walked: a hand-built
    // HsfSfData that was never read has none.
    [[nodiscard]] std::size_t count(std::size_t g) const noexcept {
        const int bands = data.max_sfb_hsf[g] - data.start_sfb[g];
        return bands > 0
                   ? std::min({static_cast<std::size_t>(bands), data.scale_factor_present[g].size(),
                               data.sect_sfb_offset[g].size() - 1,
                               static_cast<std::size_t>(kMaxSfb)})
                   : 0;
    }
    [[nodiscard]] bool has_scale_factor(std::size_t g, std::size_t si) const noexcept {
        return data.scale_factor_present[g][si];
    }
    [[nodiscard]] int dpcm_sf(std::size_t g, std::size_t si) const noexcept {
        return data.dpcm_sf[g][si];
    }
    [[nodiscard]] std::size_t begin(std::size_t g, std::size_t si) const noexcept {
        return base + data.sect_sfb_offset[g][si];
    }
    [[nodiscard]] std::size_t end(std::size_t g, std::size_t si) const noexcept {
        return base + data.sect_sfb_offset[g][si + 1];
    }
    [[nodiscard]] bool has_noise_code(std::size_t g, std::size_t si) const noexcept {
        return si < data.snf_present[g].size() && data.snf_present[g][si];
    }
    [[nodiscard]] int dpcm_snf(std::size_t g, std::size_t si) const noexcept {
        return data.dpcm_snf[g][si];
    }
};

// Pseudocode 21's walk over one part's bands, for either arithmetic: the first band with a scale
// factor takes `scale_factor` as it stands (reference_scale_factor, at the start), each later
// one adds its codeword's index less 60, and a scale factor outside 0 to 255 is an invalid
// stream. `scale_factor` and `first_scf_found` are the walk's state, handed on to the next part.
// on_band(group, band, begin, end, scale_factor) is called for each band that has a scale
// factor, in order.
template <typename Bands, typename OnBand>
[[nodiscard]] ParseResult walk_scale_factor_bands(const AsfPsyInfo& psy, const Bands& bands,
                                                  int& scale_factor, bool& first_scf_found,
                                                  OnBand&& on_band) {
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (std::size_t si = 0; si < bands.count(gi); ++si) {
            if (!bands.has_scale_factor(gi, si)) {
                continue;
            }
            if (first_scf_found) {
                scale_factor += bands.dpcm_sf(gi, si) - 60;
            } else {
                first_scf_found = true;
            }
            if (scale_factor < 0 || scale_factor > 255) {
                return fail(DecodeError::kInvalidStream, "a scale factor outside 0 to 255");
            }
            on_band(gi, si, bands.begin(gi, si), bands.end(gi, si), scale_factor);
        }
    }
    return {};
}

// Pseudocodes 22 and 23's control flow over the core's bands and, where it is given, the
// extension's, for either arithmetic. `Level` is what the noise floor's tracking is held in.
// band_level(part, group, band, begin, end, level) sets `level` to the band's level and returns
// true when the band has any energy, and leaves it alone otherwise (`part` is 0 for the core's
// bands and 1 for the extension's); offset(level, delta) is the level plus a codeword's delta;
// fill(begin, end, level) writes a band's noise.
// Pseudocode 22: the reference level is that of the first band with any energy, `initial` when
// none has. Pseudocode 23: a band with energy sets the level; a band without it that
// asf_snf_data() read a codeword for (sfb_cb 0 or max_quant_idx 0; the two conditions coincide)
// moves the level by the codeword's delta and is filled, except for the escape, which fills
// nothing and does not update the level.
template <typename Level, typename BandLevel, typename Offset, typename Fill>
void for_each_noise_fill_band(const AsfPsyInfo& psy, const CoreBands& core, const HsfBands* hsf,
                              Level initial, BandLevel band_level, Offset offset, Fill fill) {
    Level previous = initial;
    bool have_previous = false;
    const auto find_reference = [&](int part, const auto& bands) {
        for (int g = 0; g < psy.num_window_groups && !have_previous; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            for (std::size_t si = 0; si < bands.count(gi); ++si) {
                if (band_level(part, gi, si, bands.begin(gi, si), bands.end(gi, si), previous)) {
                    have_previous = true;
                    break;
                }
            }
        }
    };
    find_reference(0, core);
    if (hsf != nullptr && !have_previous) {
        find_reference(1, *hsf);
    }
    const auto run = [&](int part, const auto& bands) {
        for (int g = 0; g < psy.num_window_groups; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            for (std::size_t si = 0; si < bands.count(gi); ++si) {
                const std::size_t begin = bands.begin(gi, si);
                const std::size_t end = bands.end(gi, si);
                if (band_level(part, gi, si, begin, end, previous)) {
                    continue;
                }
                if (!bands.has_noise_code(gi, si)) {
                    continue;
                }
                const int delta = bands.dpcm_snf(gi, si) - 17;
                if (delta == -17) {
                    continue;  // the escape: no noise, and the level is not updated
                }
                previous = offset(previous, delta);
                fill(begin, end, previous);
            }
        }
    };
    run(0, core);
    if (hsf != nullptr) {
        run(1, *hsf);
    }
}

// Pseudocodes 21 to 23 at double and float. With `hsf`, the track's lines are the core's and then
// the extension's, in one vector.
template <typename R>
ParseResult reconstruct_track_floating(const SfInfo& info, const SfData& data, const HsfSfData* hsf,
                                       const std::array<R, 256>& sf_gain_table, RandGenState& noise,
                                       std::vector<R>& scaled) {
    const AsfPsyInfo& psy = info.psy;
    const std::size_t core_lines = data.quant_spec.size();
    scaled.assign(core_lines + (hsf != nullptr ? hsf->quant_spec.size() : 0), R{});
    static const HsfSfData kNoExtension{};
    const CoreBands core{data};
    const HsfBands extension{hsf != nullptr ? *hsf : kNoExtension, core_lines};

    // Pseudocode 21: the extension's scale factors carry on from the core's.
    int scale_factor = data.reference_scale_factor;
    bool first_scf_found = false;
    if (auto ok = walk_scale_factor_bands(
            psy, core, scale_factor, first_scf_found,
            [&](std::size_t, std::size_t, std::size_t begin, std::size_t end, int sf) {
                const R sf_gain = sf_gain_table[static_cast<std::size_t>(sf)];
                for (std::size_t k = begin; k < end; ++k) {
                    scaled[k] = sf_gain * reconstruct_line<R>(data.quant_spec[k]);
                }
            });
        !ok) {
        return ok;
    }
    if (hsf != nullptr) {
        if (auto ok = walk_scale_factor_bands(
                psy, extension, scale_factor, first_scf_found,
                [&](std::size_t, std::size_t, std::size_t begin, std::size_t end, int sf) {
                    const R sf_gain = sf_gain_table[static_cast<std::size_t>(sf)];
                    for (std::size_t k = begin; k < end; ++k) {
                        scaled[k] = sf_gain * reconstruct_line<R>(hsf->quant_spec[k - core_lines]);
                    }
                });
            !ok) {
            return ok;
        }
    }

    if (!data.b_snf_data_exists) {
        return {};
    }
    // Pseudocodes 22 and 23. 1.44269504 is the text's own rounding of 1/ln 2.
    // The noise floor's tracking (previous_rms, band_rms and amplitude) stays
    // double regardless of R: it is a once-per-band control value, not a
    // per-sample one, in the same "computed in double, narrowed once" shape
    // as a downmix or DRC gain (band_energy's own sum of squares is R, the
    // actual per-sample precision, then widened once for std::log).
    constexpr double kLog2E = 1.44269504;
    for_each_noise_fill_band<double>(
        psy, core, hsf != nullptr ? &extension : nullptr, -1000.0,
        [&](int, std::size_t, std::size_t, std::size_t begin, std::size_t end, double& level) {
            const auto band_rms = static_cast<double>(band_energy<R>(scaled, begin, end));
            if (band_rms > 0.0) {
                level = kLog2E * std::log(band_rms / static_cast<double>(end - begin));
                return true;
            }
            return false;
        },
        [](double previous, int delta) { return previous + static_cast<double>(delta); },
        [&](std::size_t begin, std::size_t end, double noise_rms) {
            const double amplitude = std::pow(2.0, 0.5 * noise_rms);
            for (std::size_t k = begin; k < end; ++k) {
                scaled[k] = static_cast<R>(static_cast<double>(get_random_noise_value(noise)) * amplitude);
            }
        });
    return {};
}

// The fixed-point tier's reconstruction (planning/ac4.md, D14d): the same Pseudocodes 21 to 23,
// each line's value sign(q) |q|^(4/3) 2^((sf - 100) / 4) formed as a mantissa and a power of two
// (dsp::MantExp) and the noise fill's levels from the bands' exact energies, by MantExp's own
// log2 and exp2. The track is walked twice with the same arithmetic: once for the largest
// magnitude any line can take, which fixes the track's exponent, and once to write the lines,
// each of which is then below 1, with the format's seven bits above it for the stereo and
// multichannel matrices. Each line is rounded once, to 2^-24 of that bound: with the bound at
// 1/8 the rounding of a frame's many small lines held a tone's spectrum to 107 dB from the
// double decoder's.
class FixedTrack {
   public:
    FixedTrack(const SfInfo& info, const SfData& data, const HsfSfData* hsf)
        : psy_(info.psy), data_(data), hsf_(hsf) {}

    [[nodiscard]] ParseResult run(RandGenState& noise, std::vector<dsp::Fixed32>& scaled, int& exponent) {
        scaled.assign(data_.quant_spec.size() + (hsf_ != nullptr ? hsf_->quant_spec.size() : 0),
                      dsp::Fixed32{});
        exponent = 0;
        RandGenState unused = noise;
        if (auto ok = walk(false, unused, scaled); !ok) {
            return ok;
        }
        if (largest_.is_zero()) {
            return {};
        }
        // Every line below 2^largest_.e, and written at 2^-largest_.e.
        exponent = largest_.e;
        return walk(true, noise, scaled);
    }

   private:
    // 2^(n / 4) for n in 0 to 3.
    static constexpr std::array<MantExp, 4> kQuarterPowers = {
        MantExp{1.0}, MantExp{1.1892071150027210667}, MantExp{1.4142135623730950488},
        MantExp{1.6817928305074290861}};
    // 1.44269504 ln 2: the text's rounding of 1/ln 2, applied to a natural log, as a factor on log2.
    static constexpr MantExp kLog2EOnLog2 = MantExp{1.44269504 * 0.69314718055994530942};
    // The noise fill's values are a sum of two table entries, each at most 1.87 in magnitude.
    static constexpr int kNoiseBits = 2;

    [[nodiscard]] static MantExp gain(int scale_factor) noexcept {
        const int n = scale_factor - 100;
        const int whole = n >= 0 ? n / 4 : -((-n + 3) / 4);
        return kQuarterPowers[static_cast<std::size_t>(n - 4 * whole)].scaled_by_pow2(whole);
    }

    void note(MantExp bound) noexcept {
        if (!bound.is_zero() && (largest_.is_zero() || bound.e > largest_.e)) {
            largest_ = bound;
        }
    }

    [[nodiscard]] dsp::Fixed32 stored(MantExp value) const noexcept {
        return value.scaled_by_pow2(-largest_.e).to_fixed();
    }

    [[nodiscard]] ParseResult walk(bool write, RandGenState& noise, std::vector<dsp::Fixed32>& scaled) {
        const auto& pow43 = kPow43<MantExp>;
        const auto magnitude = [](std::int32_t q) {
            const std::int64_t wide = q;
            return static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(wide < 0 ? -wide : wide), kMaxQuant));
        };
        static const HsfSfData kNoExtension{};
        const std::size_t core_lines = data_.quant_spec.size();
        const CoreBands core{data_};
        const HsfBands extension{hsf_ != nullptr ? *hsf_ : kNoExtension, core_lines};
        // Pseudocode 21, with each coded band's energy kept for Pseudocodes 22 and 23.
        std::array<std::array<MantExp, kMaxSfb>, kMaxWindows> energy{};
        std::vector<MantExp> energy_extension;
        if (hsf_ != nullptr) {
            energy_extension.assign(static_cast<std::size_t>(kMaxWindows) * kMaxSfb, MantExp{});
        }
        // One band's lines: written where `write`, its energy kept and the largest value any line
        // can take noted. `quant(k)` is line k's quantised value.
        const auto accumulate = [&](std::size_t begin, std::size_t end, int scale_factor,
                                    auto&& quant, MantExp& band_energy) {
            const MantExp sf_gain = gain(scale_factor);
            MantExp sum{};
            std::size_t largest_q = 0;
            for (std::size_t k = begin; k < end; ++k) {
                const std::int32_t q = quant(k);
                const std::size_t m = magnitude(q);
                largest_q = std::max(largest_q, m);
                const MantExp value = sf_gain * pow43[m];
                sum += value * value;
                if (write) {
                    scaled[k] = stored(q < 0 ? -value : value);
                }
            }
            band_energy = sum;
            note(sf_gain * pow43[largest_q]);
        };
        // The extension's scale factors carry on from the core's.
        int scale_factor = data_.reference_scale_factor;
        bool first_scf_found = false;
        if (auto ok = walk_scale_factor_bands(
                psy_, core, scale_factor, first_scf_found,
                [&](std::size_t gi, std::size_t si, std::size_t begin, std::size_t end, int sf) {
                    accumulate(
                        begin, end, sf,
                        [&](std::size_t k) { return std::int32_t{data_.quant_spec[k]}; },
                        energy[gi][si]);
                });
            !ok) {
            return ok;
        }
        if (hsf_ != nullptr) {
            if (auto ok = walk_scale_factor_bands(psy_, extension, scale_factor, first_scf_found,
                                                  [&](std::size_t gi, std::size_t si,
                                                      std::size_t begin, std::size_t end, int sf) {
                                                      accumulate(
                                                          begin, end, sf,
                                                          [&](std::size_t k) {
                                                              return std::int32_t{
                                                                  hsf_->quant_spec[k - core_lines]};
                                                          },
                                                          energy_extension[gi * kMaxSfb + si]);
                                                  });
                !ok) {
                return ok;
            }
        }
        if (!data_.b_snf_data_exists) {
            return {};
        }
        const auto level = [&](MantExp band_rms, std::size_t lines) {
            return scalar_log2(band_rms / MantExp{static_cast<int>(lines)}) * kLog2EOnLog2;
        };
        // Pseudocodes 22 and 23.
        for_each_noise_fill_band<MantExp>(
            psy_, core, hsf_ != nullptr ? &extension : nullptr, MantExp{-1000},
            [&](int part, std::size_t gi, std::size_t si, std::size_t begin, std::size_t end,
                MantExp& out) {
                // Only a band with a scale factor has lines Pseudocode 21 set.
                MantExp band_rms{};
                if (part == 0) {
                    band_rms = data_.scale_factor_present[gi][si] ? energy[gi][si] : MantExp{};
                } else {
                    band_rms = hsf_->scale_factor_present[gi][si]
                                   ? energy_extension[gi * kMaxSfb + si]
                                   : MantExp{};
                }
                if (band_rms > MantExp{}) {
                    out = level(band_rms, end - begin);
                    return true;
                }
                return false;
            },
            [](MantExp previous, int delta) { return previous + MantExp{delta}; },
            [&](std::size_t begin, std::size_t end, MantExp noise_rms) {
                const MantExp amplitude = scalar_exp2(noise_rms.scaled_by_pow2(-1));
                note(amplitude.scaled_by_pow2(kNoiseBits));
                if (write) {
                    for (std::size_t k = begin; k < end; ++k) {
                        scaled[k] = stored(MantExp{static_cast<double>(get_random_noise_value(noise))} * amplitude);
                    }
                }
            });
        return {};
    }

    const AsfPsyInfo& psy_;
    const SfData& data_;
    const HsfSfData* hsf_;
    MantExp largest_{};
};

template <typename R>
ParseResult reconstruct_track_at(const SfInfo& info, const SfData& data, const HsfSfData* hsf,
                                 const std::array<R, 256>& sf_gain, RandGenState& noise,
                                 std::vector<R>& scaled, int& exponent) {
    if constexpr (dsp::kFixed<R>) {
        return FixedTrack(info, data, hsf).run(noise, scaled, exponent);
    } else {
        exponent = 0;
        return reconstruct_track_floating<R>(info, data, hsf, sf_gain, noise, scaled);
    }
}

}  // namespace

ParseResult reconstruct_track(const SfInfo& info, const SfData& data,
                              const ScaleFactorGains& sf_gain, RandGenState& noise,
                              std::vector<Real>& scaled, int& exponent, const HsfSfData* hsf) {
    return reconstruct_track_at<Real>(info, data, hsf, sf_gain, noise, scaled, exponent);
}

ScaleFactorGains scale_factor_gains() {
    ScaleFactorGains gains{};
    if (dsp::kFixed<Real>) {
        return gains;  // the fixed tier forms each gain as a MantExp (FixedTrack)
    }
    for (std::size_t sf = 0; sf < gains.size(); ++sf) {
        gains[sf] = static_cast<Real>(
            std::pow(2.0, 0.25 * static_cast<double>(static_cast<int>(sf) - 100)));
    }
    return gains;
}

ParseResult window_lengths(const SubstreamContext& ctx, const AsfPsyInfo& psy,
                           std::vector<int>& lengths, int multiplier) {
    lengths.clear();
    if (psy.b_long_frame && ctx.frame_len_base >= 1536) {
        lengths.push_back(ctx.frame_len_base * multiplier);
        return {};
    }
    int total = 0;
    for (int w = 0; w < psy.num_windows; ++w) {
        const int g = psy.window_to_group[static_cast<std::size_t>(w)];
        const int length =
            transform_length_samples(ctx, get_transf_length(ctx, psy, g)) * multiplier;
        if (length <= 0) {
            return fail(DecodeError::kInvalidStream, "a transform length the frame length does not have");
        }
        lengths.push_back(length);
        total += length;
    }
    if (total != ctx.frame_len_base * multiplier) {
        return fail(DecodeError::kInvalidStream, "the frame's blocks do not add up to its length");
    }
    return {};
}

void ungroup(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data, std::span<const int> lengths,
             std::span<const Real> scaled, std::vector<Real>& spec_reord) {
    std::size_t total = 0;
    std::vector<std::size_t> win_offset;
    win_offset.reserve(lengths.size());
    for (const int length : lengths) {
        win_offset.push_back(total);
        total += static_cast<std::size_t>(length);
    }
    spec_reord.assign(total, Real{});
    std::size_t k = 0;
    std::size_t win = 0;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const std::span<const std::uint16_t> offsets =
            tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, psy, g)));
        const std::size_t windows = psy.num_win_in_group[gi];
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            for (std::size_t w = 0; w < windows; ++w) {
                for (std::size_t l = offsets[si]; l < offsets[si + 1]; ++l) {
                    spec_reord[win_offset[win + w] + l] = scaled[k++];
                }
            }
        }
        win += windows;
    }
}

void ungroup_hsf(const SubstreamContext& ctx, const AsfPsyInfo& psy, const HsfSfData& hsf,
                 int multiplier, std::span<const int> lengths, std::span<const Real> scaled,
                 std::size_t core_lines, std::vector<Real>& spec_reord) {
    std::vector<std::size_t> win_offset;
    win_offset.reserve(lengths.size());
    std::size_t total = 0;
    for (const int length : lengths) {
        win_offset.push_back(total);
        total += static_cast<std::size_t>(length);
    }
    std::size_t win = 0;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const int core_length = transform_length_samples(ctx, get_transf_length(ctx, psy, g));
        const int length = core_length * multiplier;
        const std::span<const std::uint16_t> offsets =
            multiplier == 4 ? tables::sfb_offsets_192(length) : tables::sfb_offsets_96(length);
        const std::size_t windows = psy.num_win_in_group[gi];
        const int bands = hsf.max_sfb_hsf[gi] - hsf.start_sfb[gi];
        for (int i = 0; i < bands && i < static_cast<int>(hsf.sect_sfb_offset[gi].size()) - 1;
             ++i) {
            const auto sfb = static_cast<std::size_t>(hsf.start_sfb[gi] + i);
            if (sfb + 1 >= offsets.size()) {
                break;
            }
            std::size_t k = core_lines + hsf.sect_sfb_offset[gi][static_cast<std::size_t>(i)];
            for (std::size_t w = 0; w < windows; ++w) {
                for (std::size_t l = offsets[sfb]; l < offsets[sfb + 1]; ++l) {
                    spec_reord[win_offset[win + w] + l] = scaled[k++];
                }
            }
        }
        win += windows;
    }
}

bool ungroup_in_place(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data,
                      std::span<const int> lengths, std::vector<Real>& scaled,
                      std::vector<Real>& spec_reord) {
    if (lengths.size() != 1 || psy.num_window_groups != 1 || psy.num_win_in_group[0] != 1) {
        return false;
    }
    const std::span<const std::uint16_t> offsets =
        tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, psy, 0)));
    const auto bands = static_cast<std::size_t>(std::max(data.max_sfb[0], 0));
    const auto total = static_cast<std::size_t>(std::max(lengths[0], 0));
    // The lines the bands cover are the first ones, one for one: offsets[0] is 0 and the bands
    // follow each other.
    if (offsets.empty() || offsets[0] != 0 || bands >= offsets.size() ||
        scaled.size() != offsets[bands] || scaled.size() > total) {
        return false;
    }
    std::swap(spec_reord, scaled);
    spec_reord.resize(total, Real{});
    return true;
}

void reconstruct_ssf_track(const SsfData& data, std::vector<Real>& scaled, int& exponent) {
    scaled.resize(data.lines.size());
    exponent = 0;
    double factor = 1.0;
    if constexpr (dsp::kFixed<Real>) {
        double largest = 0.0;
        for (const double value : data.lines) {
            largest = std::max(largest, std::abs(value));
        }
        if (largest > 0.0) {
            // largest is in [2^(e - 1), 2^e): every line below 1 at 2^-e.
            (void)std::frexp(largest, &exponent);
            factor = std::ldexp(1.0, -exponent);
        }
    }
    for (std::size_t k = 0; k < data.lines.size(); ++k) {
        scaled[k] = static_cast<Real>(data.lines[k] * factor);
    }
}

}  // namespace iclforge::ac4::detail
