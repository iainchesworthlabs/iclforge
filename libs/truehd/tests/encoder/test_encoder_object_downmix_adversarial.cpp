#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/object_bed.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// Adversarial checks of EncoderConfig::object_downmix (review of 15771fda0). The expected values
// here are worked out from the downmix gains and the object samples alone: literals computed by
// hand, or gains times samples in doubles; the realised downmix of a chain is recovered from the
// matrices the chain hands the stream writer, never from the chain builder's own bookkeeping.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBed = 8;
constexpr int kUnits = 8;

using Gains = std::vector<std::vector<double>>;  // [8][n]

Gains empty_gains(int n) { return Gains(kBed, std::vector<double>(static_cast<std::size_t>(n), 0.0)); }

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

// The downmix a chain realises, from its matrices alone. The decoder's primitive is
// row[out] = row[out] + sum c_t row[t]; undone last to first from the outputs (matrix channel m
// is object assign[m]) it gives the rows the decoder starts from, of which 0..7 are the bed.
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
    return forms;  // rows 0..7 the bed, the rest what substream 3 codes
}

double worst_gain_error(const Gains& got, const Gains& want, const std::vector<int>& skip) {
    double worst = 0.0;
    for (int r = 0; r < kBed; ++r) {
        if (std::ranges::find(skip, r) != skip.end()) {
            continue;
        }
        for (std::size_t e = 0; e < want.front().size(); ++e) {
            worst = std::max(worst, std::abs(got[static_cast<std::size_t>(r)][e] -
                                             want[static_cast<std::size_t>(r)][e]));
        }
    }
    return worst;
}

// The shape of the existing tests' downmix: LFE = object 0, rows 4..7 one object at 0.84195,
// L R C an object at 0.75 with two more at 0.5 and 0.3.
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
    // the ring by azimuth: Lrs 150, Ls 90, L 30, C 0, R -30, Rs -90, Rrs -150 (and round)
    constexpr std::array<int, 7> kRow{6, 4, 0, 2, 1, 5, 7};
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
        } else {  // behind: between Rrs (-150) and Lrs (150) through 180
            const double from = az < 0 ? -150.0 - az : 210.0 - az;  // degrees past Rrs
            f = from / 60.0;
        }
        rows[static_cast<std::size_t>(kRow[a])][static_cast<std::size_t>(e)] =
            std::cos(f * std::numbers::pi / 2);
        rows[static_cast<std::size_t>(kRow[b])][static_cast<std::size_t>(e)] =
            std::sin(f * std::numbers::pi / 2);
    }
    return rows;
}

}  // namespace

TEST_CASE("adversarial: a DC downmix decodes to the bed worked out by hand",
          "[truehd][encoder][.][object-downmix-adversarial][adv-dc]") {
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
            std::printf("[adv-dc] offset %d row %d want %.0f got %d worst |err| %.1f LSB24 (%.3f LSB16)\n",
                        offset, r, kWant[static_cast<std::size_t>(r)],
                        bed[static_cast<std::size_t>(r)], worst, worst / 256.0);
            CAPTURE(offset, r);
            CHECK(worst <= 256.0);  // one LSB of 16 bits
        }
    }
}

TEST_CASE("adversarial: the bed of noisy objects in LSBs, 24 and 16 bit sources",
          "[truehd][encoder][.][object-downmix-adversarial][adv-lsb]") {
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
        std::printf("[adv-lsb] %d bit source: %s\n", source_bits,
                    out.error.empty() ? "encoded" : out.error.c_str());
        REQUIRE(out.error.empty());
        CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
        const auto bed = decode(out.units, Presentation::kBest, kBed);
        for (int r = 0; r < kBed; ++r) {
            const double worst = worst_lsb(bed, pcm, rows, r);
            std::printf("[adv-lsb] %d bit row %d worst |err| %.1f LSB24 (%.3f LSB16)\n",
                        source_bits, r, worst, worst / 256.0);
            CAPTURE(source_bits, r);
            CHECK(worst <= 256.0);
        }
    }
}

TEST_CASE("adversarial: silent objects and the matrices' noise",
          "[truehd][encoder][.][object-downmix-adversarial][adv-silence]") {
    const Gains rows = fixture_rows();
    const int frames = 40 * kUnits * 2;
    const std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 12, 0);
    for (const int noise_shift : {0, 1, 3, 8}) {
        const Outcome out = try_encode(config_of(rows, noise_shift), pcm);
        std::printf("[adv-silence] noise_shift %d: %s\n", noise_shift,
                    out.error.empty() ? "encoded" : out.error.c_str());
        if (!out.error.empty()) {
            CHECK(out.error.empty());
            continue;
        }
        CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
        const auto bed = decode(out.units, Presentation::kBest, kBed);
        std::array<std::int32_t, kBed> peak{};
        for (std::size_t i = 0; i < bed.size(); ++i) {
            peak[i % kBed] = std::max(peak[i % kBed], std::abs(bed[i]));
        }
        std::printf("[adv-silence] noise_shift %d bed peaks (LSB24):", noise_shift);
        for (const std::int32_t p : peak) {
            std::printf(" %d", p);
        }
        std::printf("\n");
        CAPTURE(noise_shift);
        // silence in, silence out of every speaker: the downmix of nothing is nothing
        CHECK(*std::ranges::max_element(peak) == 0);
    }
}

TEST_CASE("adversarial: a downmix that cannot clip, at full scale",
          "[truehd][encoder][.][object-downmix-adversarial][adv-headroom]") {
    // every row's gains add to at most 1 in magnitude, so no bed row can pass full scale
    Gains rows = fixture_rows();
    for (int r = 0; r < 3; ++r) {
        for (double& g : rows[static_cast<std::size_t>(r)]) {
            g *= 1.0 / 1.55;  // 0.75 + 0.5 + 0.3
        }
    }
    const int frames = 40 * kUnits * 2;
    for (const int down : {0, 1, 2, 3, 4}) {  // objects at full scale >> down
        Lcg rng{77};
        std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 12);
        for (std::int32_t& v : pcm) {
            v = (rng.unit() < 0.5 ? -8388608 : 8388607) >> down;  // worst case: square noise
        }
        const Outcome out = try_encode(config_of(rows), pcm);
        std::printf("[adv-headroom] objects at %d dBFS: %s\n", -6 * down,
                    out.error.empty() ? "encoded" : out.error.c_str());
        CAPTURE(down);
        CHECK(out.error.empty());
    }
}

TEST_CASE("adversarial: what each chain realises, from its matrices",
          "[truehd][encoder][.][object-downmix-adversarial][adv-fuzz]") {
    SECTION("the fixture") {
        const Gains rows = fixture_rows();
        const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0);
        REQUIRE(chain.has_value());
        const double worst = worst_gain_error(realised(*chain, 12), rows, chain->zero_rows);
        double biggest = 0.0;
        for (const MatrixSpec& m : chain->matrices) {
            for (std::size_t t = 0; t < 16; ++t) {
                if (m.present[t]) {
                    biggest = std::max(biggest, std::abs(m.coeff[t] * std::ldexp(1.0, m.f_shift - m.frac_bits)));
                }
            }
        }
        std::printf("[adv-fuzz] fixture: %zu matrices, worst gain error %.2e (%.1f dB), largest coefficient %.2f\n",
                    chain->matrices.size(), worst, 20 * std::log10(worst + 1e-30), biggest);
        CHECK(worst < 1.0 / 32768.0);  // half an LSB of 16 bits at full scale
    }
    SECTION("pan-law downmixes of 9 to 16 objects") {
        Lcg rng{2026};
        for (int n = 9; n <= 16; ++n) {
            std::map<std::string, int> refusals;
            int built = 0;
            double worst = 0.0;
            std::size_t longest = 0;
            constexpr int kTrials = 200;
            for (int trial = 0; trial < kTrials; ++trial) {
                const Gains rows = ring_pan_rows(n, rng);
                const auto chain = detail::build_downmix_chain(flatten(rows), n, 0);
                if (!chain) {
                    ++refusals[std::string(chain.error()).substr(0, 60)];
                    continue;
                }
                ++built;
                longest = std::max(longest, chain->matrices.size());
                worst = std::max(worst, worst_gain_error(realised(*chain, n), rows, chain->zero_rows));
            }
            std::printf("[adv-fuzz] ring pan n=%d: built %d/%d, longest %zu, worst gain error %.2e (%.1f dB)\n",
                        n, built, kTrials, longest, worst, 20 * std::log10(worst + 1e-30));
            for (const auto& [why, count] : refusals) {
                std::printf("[adv-fuzz]    refused %d: %s\n", count, why.c_str());
            }
            CAPTURE(n);
            CHECK(built == kTrials);
            CHECK(worst < 1.0 / 32768.0);
        }
    }
    SECTION("dense random downmixes") {
        Lcg rng{11};
        for (const int n : {9, 12, 16}) {
            std::map<std::string, int> refusals;
            int built = 0;
            double worst = 0.0;
            constexpr int kTrials = 200;
            for (int trial = 0; trial < kTrials; ++trial) {
                Gains rows = empty_gains(n);
                for (auto& row : rows) {
                    for (double& g : row) {
                        g = 2.0 * rng.unit() - 1.0;
                    }
                }
                const auto chain = detail::build_downmix_chain(flatten(rows), n, 0);
                if (!chain) {
                    ++refusals[std::string(chain.error()).substr(0, 60)];
                    continue;
                }
                ++built;
                worst = std::max(worst, worst_gain_error(realised(*chain, n), rows, chain->zero_rows));
            }
            std::printf("[adv-fuzz] dense n=%d: built %d/%d, worst gain error %.2e (%.1f dB)\n", n,
                        built, kTrials, worst, 20 * std::log10(worst + 1e-30));
            for (const auto& [why, count] : refusals) {
                std::printf("[adv-fuzz]    refused %d: %s\n", count, why.c_str());
            }
            CAPTURE(n);
            CHECK(built == kTrials);
            CHECK(worst < 1.0 / 32768.0);
        }
    }
    SECTION("a quiet row") {
        // an object that only just reaches a speaker: a legal, independent downmix
        for (const double gain : {0.1, 0.01, 0.004, 0.001, 0.0005}) {
            Gains rows = fixture_rows();
            rows[7].assign(12, 0.0);
            rows[7][7] = gain;
            const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0);
            std::printf("[adv-fuzz] Rrs = %.4f * object 7 (%.1f dB): %s\n", gain,
                        20 * std::log10(gain),
                        chain ? "built" : std::string(chain.error()).c_str());
            CAPTURE(gain);
            CHECK(chain.has_value());
        }
    }
}

TEST_CASE("adversarial: object counts and the lower presentations",
          "[truehd][encoder][.][object-downmix-adversarial][adv-edges]") {
    SECTION("9 and 16 objects through a stream") {
        Lcg rng{5};
        for (const int n : {9, 16}) {
            Gains rows = ring_pan_rows(n, rng);
            // keep clear of the quiet-row refusals: one object squarely on each speaker
            constexpr std::array<int, 7> kRows{0, 1, 2, 4, 5, 6, 7};
            for (int k = 0; k < 7; ++k) {
                for (auto& row : rows) {
                    row[static_cast<std::size_t>(k + 1)] = 0.0;
                }
                rows[static_cast<std::size_t>(kRows[static_cast<std::size_t>(k)])]
                    [static_cast<std::size_t>(k + 1)] = 1.0;
            }
            const int frames = 40 * kUnits * 2 + 3;
            std::vector<std::int32_t> pcm = make_pcm(n, frames, 24, 3);
            for (std::int32_t& v : pcm) {
                v >>= 3;
            }
            const Outcome out = try_encode(config_of(rows), pcm);
            std::printf("[adv-edges] n=%d: %s\n", n, out.error.empty() ? "encoded" : out.error.c_str());
            CAPTURE(n);
            REQUIRE(out.error.empty());
            CHECK(decode(out.units, Presentation::kAll, n) == pcm);
            const auto bed = decode(out.units, Presentation::kBest, kBed);
            for (int r = 0; r < kBed; ++r) {
                const double worst = worst_lsb(bed, pcm, rows, r);
                std::printf("[adv-edges] n=%d row %d worst |err| %.1f LSB24\n", n, r, worst);
                CHECK(worst <= 256.0);
            }
        }
    }
    SECTION("a zero row is zero in the stereo and 5.1 presentations too") {
        Gains rows = fixture_rows();
        rows[0].assign(12, 0.0);  // L
        rows[2].assign(12, 0.0);  // C
        rows[6].assign(12, 0.0);  // Lrs
        std::vector<std::int32_t> pcm = make_pcm(12, 40 * kUnits * 2, 24, 4);
        for (std::int32_t& v : pcm) {
            v >>= 2;
        }
        const Outcome out = try_encode(config_of(rows), pcm);
        REQUIRE(out.error.empty());
        const auto count_nonzero = [](const std::vector<std::int32_t>& bed, int channels, int row) {
            std::size_t count = 0;
            for (std::size_t t = 0; t < bed.size() / static_cast<std::size_t>(channels); ++t) {
                count += bed[t * static_cast<std::size_t>(channels) + static_cast<std::size_t>(row)] != 0;
            }
            return count;
        };
        const auto two = decode(out.units, Presentation::k2ch, 2);
        const auto six = decode(out.units, Presentation::k6ch, 6);
        const auto eight = decode(out.units, Presentation::k8ch, 8);
        std::printf("[adv-edges] nonzero samples: 2ch L %zu; 6ch L %zu C %zu; 8ch L %zu C %zu Lrs %zu\n",
                    count_nonzero(two, 2, 0), count_nonzero(six, 6, 0), count_nonzero(six, 6, 2),
                    count_nonzero(eight, 8, 0), count_nonzero(eight, 8, 2), count_nonzero(eight, 8, 6));
        CHECK(count_nonzero(two, 2, 0) == 0);
        CHECK(count_nonzero(six, 6, 0) == 0);
        CHECK(count_nonzero(six, 6, 2) == 0);
        CHECK(count_nonzero(eight, 8, 6) == 0);
    }
    SECTION("the LFE row is not held to the encoder's LFE") {
        // lfe_present says object 0 is the LFE. These contradict it and are taken as they are.
        Gains to_speaker = fixture_rows();
        to_speaker[3][0] = 0.0;
        to_speaker[0][0] = 1.0;   // the LFE object into L, the LFE row silent
        Gains full_range = fixture_rows();
        full_range[3][5] = 0.7;   // a full-range object into the LFE row
        std::printf("[adv-edges] LFE object into L, LFE row zero: refusal '%s'\n",
                    std::string(Encoder::refusal_reason(config_of(to_speaker))).c_str());
        std::printf("[adv-edges] full-range object into the LFE row: refusal '%s'\n",
                    std::string(Encoder::refusal_reason(config_of(full_range))).c_str());
        CHECK_FALSE(Encoder::refusal_reason(config_of(to_speaker)).empty());
        CHECK_FALSE(Encoder::refusal_reason(config_of(full_range)).empty());
    }
}

TEST_CASE("adversarial: single rows of unequal gains, coded-row headroom, worst accepted chain",
          "[truehd][encoder][.][object-downmix-adversarial][adv-more]") {
    SECTION("four to seven single rows whose gains differ") {
        Lcg rng{404};
        for (const int singles : {4, 5, 6, 7}) {
            std::map<std::string, int> refusals;
            int built = 0;
            int within15 = 0;
            constexpr int kTrials = 200;
            for (int trial = 0; trial < kTrials; ++trial) {
                Gains rows = fixture_rows();
                // rows 7 down to 8 - singles (skipping the LFE) become one object each, -12..0 dB
                constexpr std::array<int, 7> kOrder{7, 6, 5, 4, 2, 1, 0};
                constexpr std::array<int, 8> kPrimary{1, 2, 3, 0, 4, 5, 6, 7};
                for (int k = 0; k < singles; ++k) {
                    const auto r = static_cast<std::size_t>(kOrder[static_cast<std::size_t>(k)]);
                    rows[r].assign(12, 0.0);
                    rows[r][static_cast<std::size_t>(kPrimary[r])] = std::pow(10.0, -12.0 * rng.unit() / 20.0);
                }
                const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0);
                if (!chain) {
                    ++refusals[std::string(chain.error()).substr(0, 60)];
                    continue;
                }
                ++built;
                within15 += chain->matrices.size() <= 15;
            }
            std::printf("[adv-more] %d single rows, gains -12..0 dB: built %d/%d, of them <=15 primitives %d\n",
                        singles, built, kTrials, within15);
            for (const auto& [why, count] : refusals) {
                std::printf("[adv-more]    refused %d: %s\n", count, why.c_str());
            }
            CAPTURE(singles);
            CHECK(built == kTrials);
        }
    }
    SECTION("what the coded rows of the fixture hold") {
        Gains rows = fixture_rows();
        for (int r = 0; r < 3; ++r) {
            for (double& g : rows[static_cast<std::size_t>(r)]) {
                g *= 1.0 / 1.55;
            }
        }
        const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0);
        REQUIRE(chain.has_value());
        const Gains all = realised(*chain, 12);
        for (std::size_t r = 0; r < all.size(); ++r) {
            double sum = 0.0;
            for (const double g : all[r]) {
                sum += std::abs(g);
            }
            std::printf("[adv-more] row %zu (%s): sum of |gains| %.3f (%+.1f dB over one object)\n", r,
                        r < 8 ? "bed" : "coded", sum, 20 * std::log10(sum + 1e-30));
            if (r < 8) {
                CHECK(sum <= 1.0 + 1e-3);
            }
        }
    }
    SECTION("the worst chain the builder accepts, through a stream") {
        Lcg rng{2026};
        Gains worst_rows;
        double worst = 0.0;
        for (int trial = 0; trial < 200; ++trial) {
            const Gains rows = ring_pan_rows(12, rng);
            const auto chain = detail::build_downmix_chain(flatten(rows), 12, 0);
            if (!chain) {
                continue;
            }
            const double error = worst_gain_error(realised(*chain, 12), rows, chain->zero_rows);
            if (error > worst) {
                worst = error;
                worst_rows = rows;
            }
        }
        REQUIRE_FALSE(worst_rows.empty());
        const int frames = 40 * kUnits * 2;
        for (int down = 0; down <= 8; ++down) {  // the loudest level the chain's rows have room for
            std::vector<std::int32_t> pcm = make_pcm(12, frames, 24, 8);
            double peak = 0.0;
            for (std::int32_t& v : pcm) {
                v >>= down;
                peak = std::max(peak, std::abs(static_cast<double>(v)));
            }
            const Outcome out = try_encode(config_of(worst_rows), pcm);
            std::printf("[adv-more] worst accepted ring pan of 12 (gain error %.2e), objects peaking %.1f dBFS: %s\n",
                        worst, 20 * std::log10(peak / 8388608.0),
                        out.error.empty() ? "encoded" : out.error.c_str());
            if (!out.error.empty()) {
                continue;
            }
            CHECK(decode(out.units, Presentation::kAll, 12) == pcm);
            const auto bed = decode(out.units, Presentation::kBest, kBed);
            for (int r = 0; r < kBed; ++r) {
                const double lsb = worst_lsb(bed, pcm, worst_rows, r);
                std::printf("[adv-more] row %d worst |err| %.1f LSB24 = %.2f LSB16; scaled to full-scale objects %.1f LSB16\n",
                            r, lsb, lsb / 256.0, lsb / 256.0 * 8388608.0 / peak);
                CHECK(lsb <= 256.0);
            }
            CHECK(down == 0);  // a pan-law downmix cannot clip the bed: it should take full scale
            break;
        }
    }
    SECTION("counts and values at the edges") {
        for (const int n : {0, 1, 8, 9, 16, 17}) {
            const std::vector<double> gains(static_cast<std::size_t>(8 * std::max(n, 1)), 0.0);
            const auto chain = detail::build_downmix_chain(gains, n, 0);
            std::printf("[adv-more] all-zero downmix of %d objects: %s\n", n,
                        chain ? "built" : std::string(chain.error()).c_str());
        }
        Gains rows = fixture_rows();
        rows[0][1] = std::numeric_limits<double>::infinity();
        CHECK_FALSE(detail::build_downmix_chain(flatten(rows), 12, 0).has_value());
        rows = fixture_rows();
        rows[0][1] = -0.75;  // a polarity flip is a legal gain
        CHECK(detail::build_downmix_chain(flatten(rows), 12, 0).has_value());
    }
}
