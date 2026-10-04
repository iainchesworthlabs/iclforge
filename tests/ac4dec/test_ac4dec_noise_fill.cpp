// The audio spectral frontend's noise fill (ETSI TS 103 190-1 V1.4.1 clause 5.1.4, Pseudocodes 22
// and 23) in reconstruct_track(), held to the clause's own steps on a hand-built SfData. No stream
// available here sets b_snf_data_exists (src/ac4dec/ERRATA.md, "x = x++ in Pseudocode 57"), so
// the committed streams never reach this code; the generator under it is held by
// test_ac4dec_pcm.cpp, and these hold what draws from it: the level each filled band takes, which
// bands are filled, and the order and count of the draws.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "pcm/asf_reconstruct.hpp"
#include "pcm/snf_random.hpp"
#include "syntax/asf.hpp"

namespace {

namespace detail = iclforge::ac4::detail;
using detail::Real;

constexpr std::size_t kBandWidth = 4;
constexpr std::size_t kBands = 8;
constexpr int kNoDelta = 17;  // Pseudocode 23 reads a codeword's delta as its index less 17

// One long block of eight bands, each four lines wide, in one window group. `quant[b]` is a band's
// four quantised lines (all zero for a silent band). Each band's scale factor is 100, whose gain
// 2^((100 - 100) / 4) is 1, so a line's value is sign(q) |q|^(4/3).
struct Track {
    detail::SfInfo info;
    std::unique_ptr<detail::SfData> data = std::make_unique<detail::SfData>();
    std::array<std::array<int, kBandWidth>, kBands> quant{};
    std::array<int, kBands> dpcm{};  // dpcm_snf, 0 where no codeword was read

    Track() {
        info.psy.num_win_in_group[0] = 1;
        data->max_sfb[0] = static_cast<int>(kBands);
        data->reference_scale_factor = 100;
        for (std::size_t b = 0; b <= kBands; ++b) {
            data->sect_sfb_offset[0][b] = static_cast<std::uint16_t>(b * kBandWidth);
        }
        data->b_snf_data_exists = true;
        dpcm.fill(-1);
    }

    [[nodiscard]] bool silent(std::size_t band) const {
        return std::all_of(quant[band].begin(), quant[band].end(), [](int q) { return q == 0; });
    }

    // Marks band `b` as carrying a noise fill codeword whose delta is `delta` dB-steps (Pseudocode
    // 23's dpcm_snf - 17); delta -17 is the escape.
    void fill(std::size_t band, int delta) { dpcm[band] = delta + kNoDelta; }

    void finish() {
        data->quant_spec.clear();
        for (std::size_t b = 0; b < kBands; ++b) {
            for (const int q : quant[b]) {
                data->quant_spec.push_back(static_cast<std::int16_t>(q));
            }
            // A band with a nonzero line has a scale factor; every one here repeats the previous
            // (index 60: a delta of 0), the first taking reference_scale_factor.
            data->scale_factor_present[0][b] = !silent(b);
            data->dpcm_sf[0][b] = 60;
            data->snf_present[0][b] = dpcm[b] >= 0;
            data->dpcm_snf[0][b] = static_cast<std::int16_t>(std::max(dpcm[b], 0));
        }
    }
};

struct Reconstruction {
    std::vector<double> lines;  // in the double decoder's units
    detail::RandGenState noise;
};

Reconstruction reconstruct(Track& track, int sequence_counter) {
    track.finish();
    Reconstruction out;
    out.noise = detail::reset_rand_gen_state_snf(sequence_counter);
    std::vector<Real> scaled;
    int exponent = 0;
    const detail::ScaleFactorGains gains = detail::scale_factor_gains();
    REQUIRE(detail::reconstruct_track(track.info, *track.data, gains, out.noise, scaled, exponent));
    for (const Real v : scaled) {
        out.lines.push_back(std::ldexp(static_cast<double>(v), exponent));
    }
    return out;
}

double coded_value(int q) {
    const double magnitude = std::pow(static_cast<double>(std::abs(q)), 4.0 / 3.0);
    return q < 0 ? -magnitude : magnitude;
}

// Pseudocode 22's and 23's level of a coded band: the base 2 logarithm of its mean square, which
// a filled band's amplitude 2^(level / 2) then restores as a root mean square.
double mean_square_level(const Track& track, std::size_t band) {
    double sum = 0.0;
    for (const int q : track.quant[band]) {
        sum += coded_value(q) * coded_value(q);
    }
    return std::log2(sum / static_cast<double>(kBandWidth));
}

// What the clause's steps give a band the noise fills, drawing from `noise` in band order.
void expect_noise(const Reconstruction& got, std::size_t band, double level,
                  detail::RandGenState& noise) {
    const double amplitude = std::pow(2.0, 0.5 * level);
    for (std::size_t k = 0; k < kBandWidth; ++k) {
        CAPTURE(band, k, level);
        const double expected =
            static_cast<double>(detail::get_random_noise_value(noise)) * amplitude;
        // Fixed32 holds a track's lines to its largest line's 2^-31, which the small bands here
        // sit well above; float keeps a part in 10^6.
        const double tolerance = detail::dsp::kFixed<Real> ? 1e-3 : 1e-5;
        CHECK(got.lines[band * kBandWidth + k] ==
              Catch::Approx(expected).epsilon(tolerance).margin(tolerance * amplitude));
    }
}

}  // namespace

TEST_CASE("noise fill levels follow the coded band's and each codeword's delta",
          "[ac4dec][asf][noise]") {
    Track t;
    t.quant[1] = {4, -3, 2, 1};  // the first band with energy: the reference level
    t.quant[5] = {1, 0, 0, 0};   // a coded band later on sets the level again
    t.fill(0, 0);                // silent and ahead of the first coded band: that band's level
    t.fill(2, +3);               // 3 above the level of band 1
    t.fill(3, -kNoDelta);        // the escape: nothing is written and the level stays
    t.fill(4, -5);               // 5 below band 2's level, not 5 below band 1's
    t.fill(6, +2);               // 2 above band 5's level
    // Band 7 has no codeword: it stays silent.

    constexpr int kCounter = 77;
    const Reconstruction got = reconstruct(t, kCounter);
    REQUIRE(got.lines.size() == kBands * kBandWidth);

    detail::RandGenState noise = detail::reset_rand_gen_state_snf(kCounter);
    const double level1 = mean_square_level(t, 1);
    const double level5 = mean_square_level(t, 5);
    expect_noise(got, 0, level1, noise);
    expect_noise(got, 2, level1 + 3.0, noise);
    expect_noise(got, 4, level1 + 3.0 - 5.0, noise);
    expect_noise(got, 6, level5 + 2.0, noise);

    // The coded bands are the scaled values, untouched by the fill.
    for (const std::size_t band : {std::size_t{1}, std::size_t{5}}) {
        for (std::size_t k = 0; k < kBandWidth; ++k) {
            CHECK(got.lines[band * kBandWidth + k] ==
                  Catch::Approx(coded_value(t.quant[band][k])).epsilon(1e-5).margin(1e-6));
        }
    }
    // The escaped band and the band with no codeword are silent.
    for (const std::size_t band : {std::size_t{3}, std::size_t{7}}) {
        for (std::size_t k = 0; k < kBandWidth; ++k) {
            CHECK(got.lines[band * kBandWidth + k] == 0.0);
        }
    }

    // Sixteen lines were drawn, and none for the escape: the generator is where the clause's
    // steps leave it.
    CHECK(got.noise.offset_a == noise.offset_a);
    CHECK(got.noise.offset_b == noise.offset_b);
    CHECK(got.noise.state_idx == noise.state_idx);
    CHECK(got.noise.current_idx == noise.current_idx);
}

TEST_CASE("noise fill is off unless b_snf_data_exists, whatever the bands carry",
          "[ac4dec][asf][noise]") {
    Track t;
    t.quant[0] = {2, 2, 2, 2};
    t.fill(1, 0);
    t.fill(2, +4);
    t.finish();
    t.data->b_snf_data_exists = false;

    detail::RandGenState noise = detail::reset_rand_gen_state_snf(5);
    const detail::RandGenState before = noise;
    std::vector<Real> scaled;
    int exponent = 0;
    REQUIRE(detail::reconstruct_track(t.info, *t.data, detail::scale_factor_gains(), noise, scaled,
                                      exponent));
    for (std::size_t k = kBandWidth; k < scaled.size(); ++k) {
        CHECK(static_cast<double>(scaled[k]) == 0.0);
    }
    CHECK(noise.state_idx == before.state_idx);
    CHECK(noise.current_idx == before.current_idx);
    CHECK(noise.offset_a == before.offset_a);
    CHECK(noise.offset_b == before.offset_b);
}

TEST_CASE("noise fill draws continue across tracks of a frame rather than restarting",
          "[ac4dec][asf][noise]") {
    // ERRATA: the generator starts once per substream per frame and each track draws on from where
    // the one before stopped, so two channels of a pair do not get the same noise.
    Track a;
    a.quant[0] = {3, 3, 3, 3};
    a.fill(1, 0);
    Track b;
    b.quant[0] = {3, 3, 3, 3};
    b.fill(1, 0);

    a.finish();
    b.finish();
    detail::RandGenState noise = detail::reset_rand_gen_state_snf(9);
    std::vector<Real> first;
    std::vector<Real> second;
    int exponent = 0;
    const detail::ScaleFactorGains gains = detail::scale_factor_gains();
    REQUIRE(detail::reconstruct_track(a.info, *a.data, gains, noise, first, exponent));
    REQUIRE(detail::reconstruct_track(b.info, *b.data, gains, noise, second, exponent));

    bool differ = false;
    for (std::size_t k = kBandWidth; k < 2 * kBandWidth; ++k) {
        differ = differ || static_cast<double>(first[k]) != static_cast<double>(second[k]);
    }
    CHECK(differ);
}
