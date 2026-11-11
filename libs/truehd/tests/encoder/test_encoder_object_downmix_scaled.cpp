#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/object_bed.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// ObjectDownmixScaling::kBedSubstreams: the gains of EncoderConfig::object_downmix applied by the
// lower substreams, so that the rows substream 3 codes stay the objects themselves. Expected values
// are worked out from the downmix gains and the object samples alone (literals by hand, or gains
// times samples in doubles); what a chain realises is recovered from the matrices it hands the
// stream writer. No decoder outside this repository has read these streams ([no-external-oracle]).

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBed = 8;
constexpr int kUnits = 8;
constexpr auto kScaled = ObjectDownmixScaling::kBedSubstreams;

using Gains = std::vector<std::vector<double>>;  // [8][n]

Gains empty_gains(int n) {
    return Gains(kBed, std::vector<double>(static_cast<std::size_t>(n), 0.0));
}

std::vector<double> flatten(const Gains& rows) {
    std::vector<double> out;
    for (const auto& row : rows) {
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

EncoderConfig config_of(const Gains& rows, int noise_shift = 0) {
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = static_cast<int>(rows.front().size());
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = noise_shift;
    config.object_downmix = flatten(rows);
    config.object_downmix_scaling = kScaled;
    config.restart_interval_units = kUnits;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    config.verify = true;
    return config;
}

struct Outcome {
    std::vector<EncodedAccessUnit> units;
    std::string error;  // empty: encoded
};

Outcome try_encode(const EncoderConfig& config, const std::vector<std::int32_t>& pcm) {
    Outcome out;
    auto encoder = Encoder::create(config);
    if (!encoder) {
        out.error = "create: " + std::string(encoder.error().detail);
        return out;
    }
    auto units = encoder->encode(pcm);
    if (!units) {
        out.error = "encode: " + std::string(units.error().detail);
        return out;
    }
    out.units = std::move(*units);
    auto tail = encoder->flush();
    if (!tail) {
        out.error = "flush: " + std::string(tail.error().detail);
        return out;
    }
    out.units.insert(out.units.end(), tail->begin(), tail->end());
    return out;
}

std::vector<std::int32_t> decode(const std::vector<EncodedAccessUnit>& units, Presentation p,
                                 int channels) {
    DecoderConfig config;
    config.presentation = p;
    config.strictness = Strictness::kStrict;
    Decoder decoder(config);
    std::vector<std::int32_t> pcm;
    for (const EncodedAccessUnit& unit : units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        REQUIRE((*decoded)->channels == channels);
        pcm.insert(pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    CHECK(decoder.counters().lossless_check_failures == 0);
    CHECK(decoder.counters().lossless_checks_ok > 0);
    return pcm;
}

// The largest distance, in LSBs of the 24 bit samples, of bed row `row` from gains times objects.
double worst_lsb(const std::vector<std::int32_t>& bed, const std::vector<std::int32_t>& pcm,
                 const Gains& rows, int row) {
    const std::size_t n = rows.front().size();
    const std::size_t frames = pcm.size() / n;
    double worst = 0.0;
    for (std::size_t t = 0; t < frames; ++t) {
        double want = 0.0;
        for (std::size_t e = 0; e < n; ++e) {
            want += rows[static_cast<std::size_t>(row)][e] * pcm[t * n + e];
        }
        worst = std::max(worst, std::abs(bed[t * kBed + static_cast<std::size_t>(row)] - want));
    }
    return worst;
}

// The downmix a chain realises, from its matrices alone: substream 3's primitives undone last to
// first from the outputs (matrix channel m is object assign[m]) give the rows as coded, and the
// lower substreams' matrix of a row multiplies it by the row's own coefficient.
Gains realised(const detail::DownmixChain& chain, int n) {
    std::vector<std::vector<double>> forms(static_cast<std::size_t>(n),
                                           std::vector<double>(static_cast<std::size_t>(n), 0.0));
    for (int m = 0; m < n; ++m) {
        forms[static_cast<std::size_t>(m)][chain.assign[static_cast<std::size_t>(m)]] = 1.0;
    }
    for (std::size_t i = chain.matrices.size(); i-- > 0;) {
        const MatrixSpec& matrix = chain.matrices[i];
        const double unit = std::ldexp(1.0, matrix.f_shift - matrix.frac_bits);
        REQUIRE(matrix.present[matrix.out_ch]);
        REQUIRE(matrix.coeff[matrix.out_ch] * unit == 1.0);
        for (int t = 0; t < n; ++t) {
            if (t == matrix.out_ch || !matrix.present[static_cast<std::size_t>(t)]) {
                continue;
            }
            const double c = matrix.coeff[static_cast<std::size_t>(t)] * unit;
            for (int e = 0; e < n; ++e) {
                forms[matrix.out_ch][static_cast<std::size_t>(e)] -=
                    c * forms[static_cast<std::size_t>(t)][static_cast<std::size_t>(e)];
            }
        }
    }
    forms.resize(kBed);
    // the lower substreams' matrices, one after another as a decoder applies them
    for (const MatrixSpec& matrix : chain.bed_matrices) {
        REQUIRE(matrix.out_ch < kBed);
        std::vector<double> made(static_cast<std::size_t>(n), 0.0);
        for (std::size_t c = 0; c < matrix.present.size(); ++c) {
            if (!matrix.present[c]) {
                continue;
            }
            // a row of its own substream or an earlier one: every presentation can make it
            const auto substream = [](std::size_t row) { return row < 2 ? 0 : row < 6 ? 1 : 2; };
            REQUIRE(substream(c) <= substream(matrix.out_ch));
            const double gain = std::ldexp(static_cast<double>(matrix.coeff[c]), -matrix.frac_bits);
            for (std::size_t e = 0; e < made.size(); ++e) {
                made[e] += gain * forms[c][e];
            }
        }
        forms[matrix.out_ch] = made;
    }
    return forms;
}

double worst_gain_error(const Gains& got, const Gains& want) {
    double worst = 0.0;
    for (std::size_t r = 0; r < kBed; ++r) {
        for (std::size_t e = 0; e < want.front().size(); ++e) {
            worst = std::max(worst, std::abs(got[r][e] - want[r][e]));
        }
    }
    return worst;
}

// The shape of the chain tests' downmix: LFE = object 0, rows 4..7 one object at 0.84195, L R C an
// object at 0.75 with two more at 0.5 and 0.3.
Gains fixture_rows() {
    Gains rows = empty_gains(12);
    rows[3][0] = 1.0;
    rows[0][1] = 0.75; rows[0][8] = 0.5;  rows[0][9] = 0.3;
    rows[1][2] = 0.75; rows[1][9] = 0.5;  rows[1][10] = 0.3;
    rows[2][3] = 0.75; rows[2][10] = 0.5; rows[2][11] = 0.3;
    rows[4][4] = 0.84195;
    rows[5][5] = 0.84195;
    rows[6][6] = 0.84195;
    rows[7][7] = 0.84195;
    return rows;
}

struct Lcg {
    std::uint32_t state;
    double unit() {  // 0..1
        state = state * 1664525U + 1013904223U;
        return static_cast<double>(state >> 8U) / 16777216.0;
    }
};

// Each object but 0 (the LFE, at unity into row 3) panned by sine and cosine between the two
// speakers of the 7.1 ring it lies between.
Gains ring_pan_rows(int n, Lcg& rng) {
    constexpr std::array<int, 7> kRow{6, 4, 0, 2, 1, 5, 7};  // Lrs Ls L C R Rs Rrs
    constexpr std::array<double, 7> kAz{150, 90, 30, 0, -30, -90, -150};
    Gains rows = empty_gains(n);
    rows[3][0] = 1.0;
    for (int e = 1; e < n; ++e) {
        const double az = 180.0 - 360.0 * rng.unit();
        std::size_t a = 6;
        std::size_t b = 0;
        double f = 0.0;
        if (az <= 150.0 && az >= -150.0) {
            for (std::size_t k = 0; k + 1 < kAz.size(); ++k) {
                if (az <= kAz[k] && az >= kAz[k + 1]) {
                    a = k;
                    b = k + 1;
                    f = (kAz[k] - az) / (kAz[k] - kAz[k + 1]);
                    break;
                }
            }
        } else {  // behind: from Rrs (-150) to Lrs (150) through 180
            f = (az < 0 ? -150.0 - az : 210.0 - az) / 60.0;
        }
        rows[static_cast<std::size_t>(kRow[a])][static_cast<std::size_t>(e)] =
            std::cos(f * std::numbers::pi / 2);
        rows[static_cast<std::size_t>(kRow[b])][static_cast<std::size_t>(e)] =
            std::sin(f * std::numbers::pi / 2);
    }
    return rows;
}

}  // namespace

TEST_CASE("a scaled downmix leaves the rows substream 3 codes as the objects",
          "[truehd][encoder][atmos][object-bed][object-downmix][object-downmix-scaled]") {
    const Gains rows = fixture_rows();
    const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
    REQUIRE(chain.has_value());
    // L R C need one main primitive each; the LFE and the four single rows need none
    CHECK(chain->matrices.size() == 3);
    CHECK(chain->single_rows == 4);
    CHECK(chain->zero_rows.empty());
    CHECK(chain->bed_matrices.size() == 7);  // every row but the unit LFE row is scaled
    REQUIRE(chain->row_gain.size() == 12);
    for (std::size_t r = 8; r < 12; ++r) {
        CHECK(chain->row_gain[r] == 1.0);  // one object, as it is
    }
    // by hand: L as coded is (0.75 a + 0.5 b + 0.3 c) / 0.75, gains adding to 1 + 2/3 + 0.4
    CHECK(std::abs(chain->row_gain[0] - (1.0 + 0.5 / 0.75 + 0.3 / 0.75)) < 1e-3);
    CHECK(chain->row_gain[4] == 1.0);  // 0.84195 of an object is coded as the object
    CHECK(worst_gain_error(realised(*chain, 12), rows) < 1.0 / 16384.0);

    // the chain in substream 3 pays for the same bed with its coded rows
    const auto in_chain = detail::build_downmix_chain(flatten(rows), 12, 0);
    REQUIRE(in_chain.has_value());
    CHECK(*std::ranges::max_element(in_chain->row_gain) > 2.0);
}

TEST_CASE("a scaled DC downmix decodes to the bed worked out by hand",
          "[truehd][encoder][atmos][object-bed][object-downmix][object-downmix-scaled]") {
    // objects: e -> 100000 * (e + 1), constant. By hand:
    //   L   = 0.75*200000 + 0.5*900000  + 0.3*1000000 =  900000
    //   R   = 0.75*300000 + 0.5*1000000 + 0.3*1100000 = 1055000
    //   C   = 0.75*400000 + 0.5*1100000 + 0.3*1200000 = 1210000
    //   LFE = 100000
    //   Ls Rs Lrs Rrs = 0.84195 * 500000, 600000, 700000, 800000 = 420975, 505170, 589365, 673560
    constexpr std::array<double, kBed> kWant{900000, 1055000, 1210000, 100000,
                                             420975, 505170,  589365,  673560};
    const Gains rows = fixture_rows();
    const int frames = 40 * kUnits * 2;
    for (const int offset : {0, 1}) {  // 0: every sample a multiple of 32 (an output shift of 5)
        std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 12);
        for (std::size_t t = 0; t < static_cast<std::size_t>(frames); ++t) {
            for (std::size_t e = 0; e < 12; ++e) {
                pcm[t * 12 + e] = 100000 * static_cast<std::int32_t>(e + 1) + (e == 11 ? offset : 0);
            }
        }
        const Outcome out = try_encode(config_of(rows), pcm);
        REQUIRE(out.error.empty());
        CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
        const auto bed = decode(out.units, Presentation::kBest, kBed);
        for (int r = 0; r < kBed; ++r) {
            double worst = 0.0;
            for (std::size_t t = 0; t < static_cast<std::size_t>(frames); ++t) {
                worst = std::max(worst, std::abs(bed[t * kBed + static_cast<std::size_t>(r)] -
                                                 (kWant[static_cast<std::size_t>(r)] +
                                                  (r == 2 ? 0.3 * offset : 0.0))));
            }
            std::printf("[scaled-dc] offset %d row %d want %.0f got %d worst %.1f LSB24\n", offset, r,
                        kWant[static_cast<std::size_t>(r)], bed[static_cast<std::size_t>(r)], worst);
            CAPTURE(offset, r);
            CHECK(worst <= 128.0);  // half an LSB of 16 bits
        }
    }
}

TEST_CASE("a scaled downmix that cannot clip is coded at full scale",
          "[truehd][encoder][atmos][object-bed][object-downmix][object-downmix-scaled]") {
    // each row is one object, or gains adding to 1 in magnitude: no bed row can pass full scale,
    // and the pivot of L R C is 0.75 / 1.55, so they are coded 1.55 / 0.75 hot for coherent
    // objects. Square noise that is not coherent leaves that room unused.
    Gains rows = fixture_rows();
    for (int r = 0; r < 3; ++r) {
        for (double& g : rows[static_cast<std::size_t>(r)]) {
            g /= 1.55;
        }
    }
    const int frames = 40 * kUnits * 2;
    SECTION("single rows of full-scale objects, the others silent") {
        Lcg rng{77};
        std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 12, 0);
        for (std::size_t t = 0; t < static_cast<std::size_t>(frames); ++t) {
            for (const std::size_t e : {0U, 4U, 5U, 6U, 7U}) {
                pcm[t * 12 + e] = rng.unit() < 0.5 ? -8388608 : 8388607;
            }
        }
        const Outcome out = try_encode(config_of(rows), pcm);
        CAPTURE(out.error);
        REQUIRE(out.error.empty());
        CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
        const auto bed = decode(out.units, Presentation::kBest, kBed);
        for (int r = 3; r < kBed; ++r) {
            CAPTURE(r);
            CHECK(worst_lsb(bed, pcm, rows, r) <= 257.0);  // 0.84195 on its 2^-14 grid, and the floor
        }
    }
    SECTION("every object at full scale needs the room the chain's row_gain names") {
        const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
        REQUIRE(chain.has_value());
        const double most = *std::ranges::max_element(chain->row_gain);
        CHECK(std::abs(most - 1.55 / 0.75) < 1e-3);  // 6.3 dB, against 9.5 dB inside the chain
        for (const int down : {0, 1, 2}) {
            Lcg rng{77};
            std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 12);
            for (std::int32_t& v : pcm) {
                v = (rng.unit() < 0.5 ? -8388608 : 8388607) >> down;
            }
            const Outcome out = try_encode(config_of(rows), pcm);
            std::printf("[scaled-headroom] objects at %d dBFS: %s\n", -6 * down,
                        out.error.empty() ? "encoded" : out.error.c_str());
            CAPTURE(down);
            if (8388608.0 / (1 << down) * most < 8388608.0) {
                CHECK(out.error.empty());  // row_gain's promise
            }
            if (out.error.empty()) {
                CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
            }
        }
    }
}

TEST_CASE("scaled downmixes of pan laws, unequal single rows and quiet rows all build",
          "[truehd][encoder][atmos][object-bed][object-downmix][object-downmix-scaled]") {
    SECTION("pan-law downmixes of 9 to 16 objects") {
        Lcg rng{2026};
        for (int n = 9; n <= 16; ++n) {
            double worst = 0.0;
            double most = 0.0;
            int built = 0;
            for (int trial = 0; trial < 200; ++trial) {
                const Gains rows = ring_pan_rows(n, rng);
                const auto chain = detail::build_downmix_chain(flatten(rows), n, 0, kScaled);
                if (!chain) {
                    std::printf("[scaled-fuzz] n=%d refused: %s\n", n, std::string(chain.error()).c_str());
                    continue;
                }
                ++built;
                CHECK(chain->matrices.size() <= 8);
                worst = std::max(worst, worst_gain_error(realised(*chain, n), rows));
                most = std::max(most, *std::ranges::max_element(chain->row_gain));
                for (std::size_t r = 8; r < static_cast<std::size_t>(n); ++r) {
                    REQUIRE(chain->row_gain[r] == 1.0);
                }
            }
            std::printf("[scaled-fuzz] ring pan n=%d: built %d/200, worst gain error %.2e, largest row gain %.2f\n",
                        n, built, worst, most);
            CAPTURE(n);
            CHECK(built == 200);
            CHECK(worst < 1.0 / 8192.0);
        }
    }
    SECTION("dense random downmixes") {
        Lcg rng{11};
        for (const int n : {9, 12, 16}) {
            double worst = 0.0;
            int built = 0;
            for (int trial = 0; trial < 200; ++trial) {
                Gains rows = empty_gains(n);
                for (auto& row : rows) {
                    for (double& g : row) {
                        g = 2.0 * rng.unit() - 1.0;
                    }
                }
                const auto chain = detail::build_downmix_chain(flatten(rows), n, 0, kScaled);
                if (!chain) {
                    // gains of both signs up to 1 in every place: elimination can need a multiple
                    // of 2 or more, which a lower substream's coefficient does not hold
                    REQUIRE(std::string(chain.error()).find("coefficient of 2 or more") !=
                            std::string::npos);
                    continue;
                }
                ++built;
                worst = std::max(worst, worst_gain_error(realised(*chain, n), rows));
            }
            std::printf("[scaled-fuzz] dense n=%d: built %d/200, worst gain error %.2e\n", n, built, worst);
            CAPTURE(n);
            CHECK(built >= 100);
            CHECK(worst < 1.0 / 8192.0);
        }
    }
    SECTION("eight single rows of unequal gains need no primitive at all") {
        Lcg rng{404};
        for (int trial = 0; trial < 50; ++trial) {
            Gains rows = empty_gains(12);
            for (std::size_t r = 0; r < kBed; ++r) {
                rows[r][r + 2] = std::pow(10.0, -40.0 * rng.unit() / 20.0);  // 0 to -40 dB
            }
            const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
            REQUIRE(chain.has_value());
            CHECK(chain->matrices.empty());
            CHECK(worst_gain_error(realised(*chain, 12), rows) <= 1.0 / 32768.0);
        }
    }
    SECTION("a quiet row") {
        for (const double gain : {0.1, 0.01, 0.004, 0.001, 0.0005}) {
            Gains rows = fixture_rows();
            rows[7].assign(12, 0.0);
            rows[7][7] = gain;
            const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
            CAPTURE(gain);
            REQUIRE(chain.has_value());
            CHECK(worst_gain_error(realised(*chain, 12), rows) < 1.0 / 16384.0);
        }
    }
    SECTION("rows that are combinations of other rows") {
        // one object alone between L and C: C is L times tan, which no chain of determinant 1 makes
        Gains rows = fixture_rows();
        rows[0].assign(12, 0.0);
        rows[2].assign(12, 0.0);
        rows[0][1] = std::cos(0.3);
        rows[2][1] = std::sin(0.3);
        auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
        REQUIRE(chain.has_value());
        CHECK(worst_gain_error(realised(*chain, 12), rows) < 1.0 / 16384.0);
        CHECK_FALSE(detail::build_downmix_chain(flatten(rows), 12, 0).has_value());
        rows = fixture_rows();
        rows[1] = rows[0];  // R is L
        chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
        REQUIRE(chain.has_value());
        CHECK(worst_gain_error(realised(*chain, 12), rows) < 1.0 / 16384.0);
    }
    SECTION("what is still refused") {
        Gains rows = fixture_rows();
        rows[7][7] = 2.5;  // a row louder than its object by more than a coefficient holds
        const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0, kScaled);
        REQUIRE_FALSE(chain.has_value());
        CHECK(std::string(chain.error()).find("coefficient of 2 or more") != std::string::npos);
    }
}

TEST_CASE("scaled downmix streams: the bed in LSBs, zero rows and the lower presentations",
          "[truehd][encoder][atmos][object-bed][object-downmix][object-downmix-scaled]") {
    SECTION("noisy objects from 24 and 16 bit sources") {
        const Gains rows = fixture_rows();
        const int frames = 40 * kUnits * 3 + 11;
        for (const int source_bits : {24, 16}) {
            std::vector<std::int32_t> pcm = make_pcm(12, frames, 24, 31);
            for (std::int32_t& v : pcm) {
                v >>= 2;
                if (source_bits == 16) {
                    v = (v >> 8) * 256;
                }
            }
            const Outcome out = try_encode(config_of(rows), pcm);
            REQUIRE(out.error.empty());
            CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
            const auto bed = decode(out.units, Presentation::kBest, kBed);
            for (int r = 0; r < kBed; ++r) {
                const double worst = worst_lsb(bed, pcm, rows, r);
                std::printf("[scaled-lsb] %d bit row %d worst %.1f LSB24 (%.3f LSB16)\n", source_bits,
                            r, worst, worst / 256.0);
                CAPTURE(source_bits, r);
                // a 24 bit source: the 2^-14 grids. A 16 bit source: the bed is on the source's
                // 16 bit grid, floored once by the main primitive and once by the scale.
                CHECK(worst <= (source_bits == 24 ? 128.0 : 2 * 256.0));
            }
        }
    }
    SECTION("pan law of 12 at the level of the test signal, with zero rows and noise") {
        Lcg rng{9};
        Gains rows = ring_pan_rows(12, rng);
        rows[3].assign(12, 0.0);  // no LFE
        const int frames = 40 * kUnits * 2 + 5;
        std::vector<std::int32_t> pcm = make_pcm(12, frames, 24, 8);
        for (std::int32_t& v : pcm) {
            v >>= 2;
        }
        for (const int noise_shift : {0, 3}) {
            const Outcome out = try_encode(config_of(rows, noise_shift), pcm);
            CAPTURE(noise_shift, out.error);
            REQUIRE(out.error.empty());
            CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
            const auto eight = decode(out.units, Presentation::k8ch, 8);
            const auto six = decode(out.units, Presentation::k6ch, 6);
            const auto two = decode(out.units, Presentation::k2ch, 2);
            const std::size_t n = eight.size() / 8;
            for (std::size_t t = 0; t < n; ++t) {
                REQUIRE(eight[t * 8 + 3] == 0);  // the zero row
                for (std::size_t c = 0; c < 6; ++c) {
                    REQUIRE(six[t * 6 + c] == eight[t * 8 + c]);  // the same scaled rows
                }
                REQUIRE(two[t * 2] == eight[t * 8]);
                REQUIRE(two[t * 2 + 1] == eight[t * 8 + 1]);
            }
            for (int r = 0; r < kBed; ++r) {
                const double worst = worst_lsb(eight, pcm, rows, r);
                std::printf("[scaled-lsb] pan 12 noise_shift %d row %d worst %.1f LSB24\n", noise_shift, r, worst);
                CAPTURE(r);
                CHECK(worst <= (noise_shift == 0 ? 256.0 : 512.0));
            }
        }
    }
    SECTION("the loudest level a pan-law downmix is coded at, against the chain in substream 3") {
        // of 200 pan-law downmixes of 12, the one whose chain in substream 3 has the hottest row
        Lcg rng{2026};
        Gains rows;
        double hottest = 0.0;
        std::vector<double> chain_gains;
        std::vector<double> scaled_gains;
        std::vector<double> bed_gains;
        for (int trial = 0; trial < 200; ++trial) {
            const Gains candidate = ring_pan_rows(12, rng);
            const auto in_chain = detail::build_downmix_chain(flatten(candidate), 12, 0);
            const auto scaled = detail::build_downmix_chain(flatten(candidate), 12, 0, kScaled);
            REQUIRE(scaled.has_value());
            scaled_gains.push_back(*std::ranges::max_element(scaled->row_gain));
            double bed_most = 0.0;  // what the bed itself can reach: no coding avoids it
            for (const auto& row : candidate) {
                double sum = 0.0;
                for (const double g : row) {
                    sum += std::abs(g);
                }
                bed_most = std::max(bed_most, sum);
            }
            bed_gains.push_back(bed_most);
            if (!in_chain) {
                continue;
            }
            chain_gains.push_back(*std::ranges::max_element(in_chain->row_gain));
            if (chain_gains.back() > hottest) {
                hottest = chain_gains.back();
                rows = candidate;
            }
        }
        REQUIRE_FALSE(rows.empty());
        const auto median = [](std::vector<double> v) {
            std::ranges::sort(v);
            return v[v.size() / 2];
        };
        std::printf("[scaled-headroom] hottest coded row over 200 pan laws of 12 (gain on full-scale "
                    "objects): bed itself median %.2f max %.2f; scaled median %.2f max %.2f; chain "
                    "(%zu built) median %.2f max %.2f\n",
                    median(bed_gains), *std::ranges::max_element(bed_gains), median(scaled_gains),
                    *std::ranges::max_element(scaled_gains), chain_gains.size(), median(chain_gains),
                    *std::ranges::max_element(chain_gains));
        CHECK(median(scaled_gains) < median(chain_gains));
        const auto loudest = [&](ObjectDownmixScaling scaling) {
            for (int down = 0; down <= 10; ++down) {
                std::vector<std::int32_t> pcm = make_pcm(12, 40 * kUnits * 2, 24, 8);
                for (std::int32_t& v : pcm) {
                    v >>= down;
                }
                EncoderConfig config = config_of(rows);
                config.object_downmix_scaling = scaling;
                if (try_encode(config, pcm).error.empty()) {
                    return down;
                }
            }
            return 99;
        };
        const int in_chain = loudest(ObjectDownmixScaling::kChain);
        const int scaled = loudest(kScaled);
        std::printf("[scaled-headroom] pan law of 12, test signal peaking -7.8 dBFS: the chain codes it %d dB down, scaled %d dB down\n",
                    6 * in_chain, 6 * scaled);
        CHECK(scaled <= 1);
        CHECK(scaled < in_chain);
    }
    SECTION("chunking does not change the stream") {
        const Gains rows = fixture_rows();
        const std::vector<std::int32_t> pcm = make_pcm(12, 40 * kUnits * 2 + 5, 24, 5);
        std::vector<std::int32_t> quiet = pcm;
        for (std::int32_t& v : quiet) {
            v >>= 2;
        }
        const Encoded whole = encode_all(config_of(rows, 1), quiet);
        const Encoded pieces = encode_all(config_of(rows, 1), quiet, 37);
        REQUIRE(whole.units.size() == pieces.units.size());
        for (std::size_t u = 0; u < whole.units.size(); ++u) {
            CHECK(whole.units[u].bytes == pieces.units[u].bytes);
        }
    }
}
