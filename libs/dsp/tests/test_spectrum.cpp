#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>

#include "iclforge/dsp/spectrum.hpp"

namespace sp = iclforge::dsp::spectrum;

namespace {

using History = std::array<std::int16_t, sp::kHistory>;
using Levels = std::array<float, sp::kBands>;

// A deterministic generator, so that a test does not depend on a standard library's distribution.
struct Noise {
    std::uint32_t state = 0x9E3779B9U;
    // Uniform in [-1, 1).
    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<double>(state) / 2147483648.0 - 1.0);
    }
};

std::int16_t sample(double v) { return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0)); }

History sine(double hz, double amplitude) {
    History h{};
    for (std::size_t n = 0; n < h.size(); ++n) {
        h[n] = sample(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / sp::kRate));
    }
    return h;
}

int loudest_band(const Levels& db) { return static_cast<int>(std::max_element(db.begin(), db.end()) - db.begin()); }

// The analysers are 50 KB and more of tables: never on a stack.
struct Rig {
    std::unique_ptr<sp::Analyzer> analyzer = std::make_unique<sp::Analyzer>();
    std::unique_ptr<sp::Reference> reference = std::make_unique<sp::Reference>();
};

}  // namespace

TEST_CASE("the bands are a third of an octave apart from 20 Hz to 20 kHz", "[spectrum]") {
    REQUIRE(sp::kCentreHz.size() == 31);
    REQUIRE(sp::kCentreHz.front() == 20.0F);
    REQUIRE(sp::kCentreHz.back() == 20000.0F);
    for (std::size_t b = 1; b < sp::kCentreHz.size(); ++b) {
        const float step = sp::kCentreHz[b] / sp::kCentreHz[b - 1];
        INFO(sp::kCentreHz[b]);
        REQUIRE(step > 1.2F);  // the ISO series is 10^(1/10), 1.259, to the nearest of the nominal values
        REQUIRE(step < 1.3F);
        REQUIRE(sp::band_low_hz(static_cast<int>(b)) < sp::kCentreHz[b]);
        REQUIRE(sp::band_high_hz(static_cast<int>(b)) > sp::kCentreHz[b]);
    }
    // The 12 kHz stream answers the bands up to 800 Hz, and its Nyquist frequency is above that band's top.
    REQUIRE(sp::kCentreHz[sp::kLowBands - 1] == 800.0F);
    REQUIRE(sp::band_high_hz(sp::kLowBands - 1) < static_cast<float>(sp::kRate) / static_cast<float>(sp::kDecimate) / 2.0F);
}

TEST_CASE("a sine reads its own level in its own band", "[spectrum]") {
    Rig rig;
    // From 63 Hz up a tone sitting on a band's centre is that band's level to within 0.4 dB; the 25 Hz band is a bin and a half
    // wide, and reads about 3 dB low.
    for (const float hz : {63.0F, 100.0F, 250.0F, 500.0F, 800.0F, 1000.0F, 2000.0F, 4000.0F, 10000.0F, 16000.0F}) {
        for (const double amplitude : {1.0, 0.1, 0.01}) {
            Levels db{};
            rig.analyzer->analyze(sine(static_cast<double>(hz), amplitude), db);
            const auto band = static_cast<std::size_t>(loudest_band(db));
            INFO("hz " << hz << " amplitude " << amplitude);
            REQUIRE(sp::kCentreHz[band] == hz);
            REQUIRE(std::fabs(db[band] - 20.0F * std::log10(static_cast<float>(amplitude))) < 0.4F);
        }
    }
    Levels db{};
    rig.analyzer->analyze(sine(25.0, 1.0), db);
    REQUIRE(sp::kCentreHz[static_cast<std::size_t>(loudest_band(db))] == 25.0F);
    REQUIRE(db[1] > -3.6F);
    REQUIRE(db[1] < -2.2F);
}

TEST_CASE("silence reads as silence in every band, and a full-scale sine is no louder than 0 dB in any", "[spectrum]") {
    Rig rig;
    const History quiet{};
    Levels db{};
    rig.analyzer->analyze(quiet, db);
    for (const float level : db) {
        REQUIRE(level == sp::kSilentDb);
    }
    // A full-scale tone cannot be more than 0 dB in any band (to the tenth of a decibel the transform rounds to).
    rig.analyzer->analyze(sine(1000.0, 1.0), db);
    for (const float level : db) {
        REQUIRE(level < 0.1F);
    }
}

TEST_CASE("below 800 Hz it is what one 8192-point FFT says, and above 1 kHz it sees the newest 21 ms", "[spectrum]") {
    Rig rig;
    // A steady signal: a few tones. The two agree to 0.15 dB below the crossover, down to the 105 dB under full scale that 16 bits round to (noise
    // is a statistical matter and is held in the next test, over many frames: one frame of it differs by a few tenths between two estimators).
    History steady{};
    for (std::size_t n = 0; n < steady.size(); ++n) {
        const double t = static_cast<double>(n) / sp::kRate;
        steady[n] = sample(0.2 * std::sin(2.0 * std::numbers::pi * 63.0 * t) + 0.1 * std::sin(2.0 * std::numbers::pi * 200.0 * t) +
                           0.05 * std::sin(2.0 * std::numbers::pi * 640.0 * t));
    }
    Levels two_rate{};
    Levels long_fft{};
    rig.analyzer->analyze(steady, two_rate);
    rig.reference->analyze(steady, long_fft);
    for (int b = 0; b < sp::kLowBands; ++b) {
        INFO("band " << sp::kCentreHz[static_cast<std::size_t>(b)]);
        REQUIRE(std::fabs(two_rate[static_cast<std::size_t>(b)] - long_fft[static_cast<std::size_t>(b)]) < 0.15F);
    }

    // A burst of a 10 kHz tone in the newest 21 ms and silence before it: the long FFT has it at the tail of its 171 ms window, where a Hann
    // window is nearly nothing; the high frame sees all of it.
    History burst{};
    for (std::size_t n = sp::kHistory - sp::kHighFrame; n < sp::kHistory; ++n) {
        burst[n] = sample(0.5 * std::sin(2.0 * std::numbers::pi * 10000.0 * static_cast<double>(n) / sp::kRate));
    }
    rig.analyzer->analyze(burst, two_rate);
    rig.reference->analyze(burst, long_fft);
    const auto band = static_cast<std::size_t>(loudest_band(two_rate));
    REQUIRE(sp::kCentreHz[band] == 10000.0F);
    REQUIRE(two_rate[band] - long_fft[band] > 6.0F);
}

TEST_CASE("white noise rises a decibel a band, averaged over many frames, in both analyses", "[spectrum]") {
    Rig rig;
    Noise noise;
    std::array<double, sp::kBands> mean_two{};
    std::array<double, sp::kBands> mean_long{};
    constexpr int kFrames = 300;
    for (int f = 0; f < kFrames; ++f) {
        History h{};
        for (std::int16_t& v : h) {
            v = sample(0.5 * static_cast<double>(noise.next()));
        }
        Levels two_rate{};
        Levels long_fft{};
        rig.analyzer->analyze(h, two_rate);
        rig.reference->analyze(h, long_fft);
        for (std::size_t b = 0; b < static_cast<std::size_t>(sp::kBands); ++b) {
            mean_two[b] += 0.5 * std::pow(10.0, static_cast<double>(two_rate[b]) / 10.0) / kFrames;
            mean_long[b] += 0.5 * std::pow(10.0, static_cast<double>(long_fft[b]) / 10.0) / kFrames;
        }
    }
    const auto level = [](double p) { return 10.0 * std::log10(p / 0.5); };
    // Bands 4 to 29: the ends are one or two bins wide, or at the edge of the passband.
    for (std::size_t b = 4; b < 30; ++b) {
        INFO("band " << sp::kCentreHz[b]);
        const double step = level(mean_two[b]) - level(mean_two[b - 1]);
        REQUIRE(step > 0.6);
        REQUIRE(step < 1.4);
        // And the two analyses say the same of noise at every band, across the crossover, to a quarter of a decibel.
        REQUIRE(std::fabs(level(mean_two[b]) - level(mean_long[b])) < 0.25);
    }
}

TEST_CASE("the transform in the analyzer's own buffers is the library's, bit for bit, at both lengths", "[spectrum]") {
    // detail::forward_in repeats stockham_forward's passes with its work buffers given (the panel's stack cannot hold the library's): a
    // change to the library's glue that the copy does not follow shows here, as a different bit.
    const auto check = [](auto length) {
        constexpr std::size_t kP = decltype(length)::value;
        iclforge::dsp::fft::StockhamTables<kP, float> tables;
        Noise noise;
        std::array<float, kP> re{};
        std::array<float, kP> im{};
        for (std::size_t n = 0; n < kP; ++n) {
            re[n] = noise.next();
            im[n] = noise.next();
        }
        auto lib_re = re;
        auto lib_im = im;
        iclforge::dsp::fft::stockham_forward<kP, float, float>(tables, std::span<float, kP>(lib_re), std::span<float, kP>(lib_im));
        auto own_re = re;
        auto own_im = im;
        std::array<float, kP> work_re{};
        std::array<float, kP> work_im{};
        sp::detail::forward_in<kP>(tables, std::span<float, kP>(own_re), std::span<float, kP>(own_im), std::span<float, kP>(work_re),
                                   std::span<float, kP>(work_im));
        REQUIRE(own_re == lib_re);
        REQUIRE(own_im == lib_im);
    };
    check(std::integral_constant<std::size_t, sp::kLowFrame / 2>{});
    check(std::integral_constant<std::size_t, sp::kHighFrame / 2>{});
    // The odd number of passes (the copy back) and the even one are both among them: 1,024 and 512 are five passes each, 256 is four.
    check(std::integral_constant<std::size_t, 256>{});
}

TEST_CASE("the two halves of the analysis are the whole, bit for bit, and each leaves the other's bands alone", "[spectrum]") {
    Noise noise;
    History h{};
    for (std::size_t n = 0; n < h.size(); ++n) {
        h[n] = static_cast<std::int16_t>(noise.next() * 9000.0F + 6000.0F * std::sin(0.3F * static_cast<float>(n)));
    }
    auto analyzer = std::make_unique<sp::Analyzer>();
    Levels whole{};
    analyzer->analyze(h, whole);
    Levels low{};
    Levels high{};
    low.fill(-77.0F);
    high.fill(-77.0F);
    analyzer->analyze_low(h, low);
    analyzer->analyze_high(h, high);
    for (int b = 0; b < sp::kBands; ++b) {
        const auto i = static_cast<std::size_t>(b);
        if (b < sp::kLowBands) {
            CHECK(low[i] == whole[i]);
            CHECK(high[i] == -77.0F);
        } else {
            CHECK(high[i] == whole[i]);
            CHECK(low[i] == -77.0F);
        }
    }
}

TEST_CASE("the power average follows a step by its time constant, and starts at the first level", "[spectrum]") {
    sp::PowerAverage avg;
    Levels loud{};
    loud.fill(-20.0F);
    Levels quiet{};
    quiet.fill(sp::kSilentDb);
    Levels out{};
    avg.feed(loud, 0.05F, 0.1F, out);
    REQUIRE(std::fabs(out[10] + 20.0F) < 1e-3F);
    // Silence after it: the power falls by e each time constant.
    for (int i = 0; i < 2; ++i) {
        avg.feed(quiet, 0.05F, 0.1F, out);
    }
    const double expected = 10.0 * std::log10(std::exp(-1.0));  // two steps of 0.05 s at 0.1 s: e^-1 of the power
    REQUIRE(std::fabs(static_cast<double>(out[10]) + 20.0 - expected) < 0.3);
}

TEST_CASE("a ballistic bar rises at once, falls at its rate, and its peak is held then falls", "[spectrum]") {
    sp::Ballistic bar;
    bar.update(-10.0F, 0.05F, 30.0F, 1.0F, 15.0F);
    REQUIRE(bar.level == -10.0F);
    REQUIRE(bar.peak == -10.0F);
    // Falling: 30 dB a second is 1.5 dB a step.
    bar.update(-60.0F, 0.05F, 30.0F, 1.0F, 15.0F);
    REQUIRE(std::fabs(bar.level + 11.5F) < 1e-4F);
    REQUIRE(bar.peak == -10.0F);
    // Held for a second (20 steps), then it falls at 15 dB a second.
    for (int i = 0; i < 19; ++i) {
        bar.update(-60.0F, 0.05F, 30.0F, 1.0F, 15.0F);
    }
    REQUIRE(bar.peak == -10.0F);
    for (int i = 0; i < 4; ++i) {
        bar.update(-60.0F, 0.05F, 30.0F, 1.0F, 15.0F);
    }
    REQUIRE(bar.peak < -10.0F);
    // A louder level takes both up at once and starts the hold again.
    bar.update(-5.0F, 0.05F, 30.0F, 1.0F, 15.0F);
    REQUIRE(bar.level == -5.0F);
    REQUIRE(bar.peak == -5.0F);
    REQUIRE(bar.hold_left == 1.0F);
}
