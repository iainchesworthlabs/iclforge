#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/transient.hpp"

namespace {

// Consecutive 256-sample segments of a continuous signal, in stream order -
// the detector's own calling convention (one call per channel per block
// period, each carrying that period's NEW samples; the previous segment is
// the comparison baseline via the persistent filter/tree state).
std::vector<std::array<float, 256>> segments_of(const std::vector<float>& signal) {
    std::vector<std::array<float, 256>> out;
    for (std::size_t i = 0; i + 256 <= signal.size(); i += 256) {
        std::array<float, 256> s{};
        for (std::size_t n = 0; n < 256; ++n) {
            s[n] = signal[i + n];
        }
        out.push_back(s);
    }
    return out;
}

std::vector<float> tone(std::size_t from, std::size_t count, double amplitude,
                         double fs = 48000.0) {
    std::vector<float> signal(from + count, 0.0F);
    for (std::size_t n = from; n < signal.size(); ++n) {
        signal[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 / fs * static_cast<double>(n)));
    }
    return signal;
}

}  // namespace

TEST_CASE("a steady tone never trips the transient detector", "[transient]") {
    iclforge::ac3::TransientDetector detector(iclforge::ac3::SampleRate::k48000);
    for (const auto& s : segments_of(tone(0, 256 * 16, 0.5))) {
        CHECK_FALSE(detector.detect(s));
    }
}

TEST_CASE("digital silence never trips the transient detector", "[transient]") {
    std::vector<float> signal(256 * 8, 0.0F);
    iclforge::ac3::TransientDetector detector(iclforge::ac3::SampleRate::k48000);
    for (const auto& s : segments_of(signal)) {
        CHECK_FALSE(detector.detect(s));
    }
}

TEST_CASE("a sudden loud onset trips the detector in the onset's own segment",
         "[transient]") {
    // Seven segments of silence to settle history and clear the first-call
    // guard, then a loud 1 kHz tone from segment 7's own first sample -
    // exactly the case §8.2.2 defines blksw for: a transient in the 256 new
    // samples of one block period.
    const auto segments = segments_of(tone(256 * 7, 256 * 2, 0.9));
    iclforge::ac3::TransientDetector detector(iclforge::ac3::SampleRate::k48000);
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK_FALSE(detector.detect(segments[i]));
    }
    CHECK(detector.detect(segments[7]));
    // The segment after the onset holds the tone steady at its new level -
    // no ratio jump against its own (now-loud) predecessor, so no switch.
    CHECK_FALSE(detector.detect(segments[8]));
}

TEST_CASE("the very first segment a detector sees never trips it", "[transient]") {
    // A fresh detector's only baseline is synthetic silence; §8.2.2's
    // comparisons against it would flag any non-silent opening - so the
    // first call's result is suppressed even for a full-scale onset. The
    // SECOND segment, steady at the same level, must not trip either (the
    // suppressed pass still primed real history).
    const auto segments = segments_of(tone(0, 256 * 2, 0.9));
    iclforge::ac3::TransientDetector detector(iclforge::ac3::SampleRate::k48000);
    CHECK_FALSE(detector.detect(segments[0]));
    CHECK_FALSE(detector.detect(segments[1]));
}

// A loud onset trips the detector at every one of A/52's six sample rates,
// not just k48000 - the earlier tests above never varied it, so a rate that
// somehow broke the RBJ coefficient formula (a divide-by-zero, a NaN, a
// sign flip) had no test that could see it. See cross-toolchain bitstream audit: the biquad's
// coefficients are runtime std::cos/std::sin calls, and this pins the
// DISCRETE blksw outcome they feed (not the raw coefficients themselves,
// which is what a cross-toolchain bit-exactness check operates on instead -
// see that entry for the empirical result) at every rate this project ships.
TEST_CASE("a loud onset trips the detector at every A/52 sample rate", "[transient]") {
    struct Case {
        iclforge::ac3::SampleRate rate;
        bool onset_trips;
    };
    // k16000 is deliberately excluded from "trips": A/52's 8 kHz cutoff
    // (transient.cpp's kCutoffHz, fixed regardless of rate) IS that rate's
    // Nyquist frequency exactly, so the RBJ highpass's own passband
    // collapses to zero width there - std::cos(w0) lands on exactly -1.0,
    // making b0 and b2 exactly 0.0 and the filtered signal identically zero
    // for any input. Every ratio test then compares 0 against 0 (and never
    // even reaches them, since p1 stays below kSilenceThreshold), so blksw
    // is always false at this one rate by construction. This is a
    // pre-existing property of the fixed cutoff constant versus a variable
    // rate, not something this test's own scope changes or fixes - see
    // cross-toolchain bitstream audit's note on it.
    const std::array<Case, 6> cases{{
        {iclforge::ac3::SampleRate::k48000, true},
        {iclforge::ac3::SampleRate::k44100, true},
        {iclforge::ac3::SampleRate::k32000, true},
        {iclforge::ac3::SampleRate::k24000, true},
        {iclforge::ac3::SampleRate::k22050, true},
        {iclforge::ac3::SampleRate::k16000, false},
    }};
    for (const Case& c : cases) {
        const double fs = static_cast<double>(iclforge::ac3::sample_rate_hz(c.rate));
        const auto segments = segments_of(tone(256 * 7, 256 * 2, 0.9, fs));
        iclforge::ac3::TransientDetector detector(c.rate);
        for (std::size_t i = 0; i < 7; ++i) {
            CHECK_FALSE(detector.detect(segments[i]));
        }
        CHECK(detector.detect(segments[7]) == c.onset_trips);
        CHECK_FALSE(detector.detect(segments[8]));
    }
}

TEST_CASE("the float detector decides as the double one does on a loud onset", "[transient]") {
    // iclforge::ac3::BasicTransientDetector<float> is the minimum-footprint profile's
    // form (ac3/internal/encode_scalar.hpp). The recipe is identical; only
    // the rounding differs, so on a clear onset the two must agree segment
    // by segment - and the tone's steady segments must not trip either.
    const auto segments = segments_of(tone(256 * 7, 256 * 2, 0.9));
    iclforge::ac3::TransientDetector wide(iclforge::ac3::SampleRate::k48000);
    iclforge::ac3::BasicTransientDetector<float> narrow(iclforge::ac3::SampleRate::k48000);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        CAPTURE(i);
        const bool wide_hit = wide.detect(segments[i]);
        const bool narrow_hit = narrow.detect(segments[i]);
        CHECK(narrow_hit == wide_hit);
    }
}

namespace {

// The detector as it was before run_pass kept the filter's memory in locals
// and reduced each block of 64 to its peak as it went: the cascade through
// members, a stored array of filtered samples, peak_abs over each quarter,
// the same tree and the same comparisons. A transcription, so a change to
// the arithmetic of the real one shows as a different decision here.
template <typename Scalar>
class ReferenceDetector {
   public:
    explicit ReferenceDetector(iclforge::ac3::SampleRate sample_rate) {
        const double fs = static_cast<double>(iclforge::ac3::sample_rate_hz(sample_rate));
        const double w0 = 2.0 * std::numbers::pi * 8000.0 / fs;
        const double cos_w0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * (std::numbers::sqrt2 / 2.0));
        const double a0 = 1.0 + alpha;
        const Biquad stage{.b0 = static_cast<Scalar>((1.0 + cos_w0) / 2.0 / a0),
                           .b1 = static_cast<Scalar>(-(1.0 + cos_w0) / a0),
                           .b2 = static_cast<Scalar>((1.0 + cos_w0) / 2.0 / a0),
                           .a1 = static_cast<Scalar>((-2.0 * cos_w0) / a0),
                           .a2 = static_cast<Scalar>((1.0 - alpha) / a0)};
        stages_.fill(stage);
    }

    bool detect(const std::array<float, 256>& pcm) {
        const bool suppress = first_block_;
        first_block_ = false;
        const bool hit = run_pass(pcm);
        return !suppress && hit;
    }

   private:
    struct Biquad {
        Scalar b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        Scalar x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        Scalar process(Scalar x) {
            const Scalar y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    static Scalar peak_abs(std::span<const Scalar> x) {
        Scalar m = 0;
        for (const Scalar v : x) {
            m = std::max(m, std::abs(v));
        }
        return m;
    }

    bool run_pass(const std::array<float, 256>& half) {
        std::array<Scalar, 256> filtered{};
        for (std::size_t n = 0; n < half.size(); ++n) {
            Scalar x = static_cast<Scalar>(half[n]);
            for (auto& stage : stages_) {
                x = stage.process(x);
            }
            filtered[n] = x;
        }
        const std::span<const Scalar> all(filtered);
        const std::array<Scalar, 4> p3{
            peak_abs(all.subspan(0, 64)),
            peak_abs(all.subspan(64, 64)),
            peak_abs(all.subspan(128, 64)),
            peak_abs(all.subspan(192, 64)),
        };
        const std::array<Scalar, 2> p2{std::max(p3[0], p3[1]), std::max(p3[2], p3[3])};
        const Scalar p1 = std::max(p2[0], p2[1]);
        bool transient = false;
        if (p1 >= static_cast<Scalar>(100.0 / 32768.0)) {
            if (p1 * static_cast<Scalar>(0.1) > prev_level1_) {
                transient = true;
            }
            if (p2[0] * static_cast<Scalar>(0.075) > prev_level2_) {
                transient = true;
            }
            if (p2[1] * static_cast<Scalar>(0.075) > p2[0]) {
                transient = true;
            }
            if (p3[0] * static_cast<Scalar>(0.05) > prev_level3_) {
                transient = true;
            }
            for (std::size_t k = 1; k < p3.size(); ++k) {
                if (p3[k] * static_cast<Scalar>(0.05) > p3[k - 1]) {
                    transient = true;
                }
            }
        }
        prev_level1_ = p1;
        prev_level2_ = p2[1];
        prev_level3_ = p3.back();
        return transient;
    }

    std::array<Biquad, 2> stages_;
    Scalar prev_level1_ = 0;
    Scalar prev_level2_ = 0;
    Scalar prev_level3_ = 0;
    bool first_block_ = true;
};

struct Tally {
    int hits = 0;
    int misses = 0;
};

// Feeds one stream to the real detector and the transcription and holds every
// segment's decision equal. Returns how many decisions there were of each
// kind, so a caller can see the signal exercised both.
template <typename Scalar>
Tally agree_on(const std::vector<std::array<float, 256>>& segments,
               iclforge::ac3::SampleRate rate) {
    iclforge::ac3::BasicTransientDetector<Scalar> real(rate);
    ReferenceDetector<Scalar> reference(rate);
    Tally tally;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const bool want = reference.detect(segments[i]);
        const bool got = real.detect(segments[i]);
        CAPTURE(i);
        CHECK(got == want);
        (want ? tally.hits : tally.misses) += 1;
    }
    return tally;
}

}  // namespace

TEST_CASE("the detector decides as the transcription of its old form on random streams",
          "[transient]") {
    std::mt19937 rng(0x7A41);
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
    std::uniform_int_distribution<int> level_pick(0, 5);
    std::uniform_int_distribution<int> position(0, 255);
    std::uniform_int_distribution<int> burst_length(1, 96);
    std::uniform_int_distribution<int> burst_gain(3, 60);
    std::uniform_int_distribution<int> coin(0, 3);
    // Quiet enough to sit under the silence gate, near it, and well over it.
    constexpr std::array<float, 6> kLevels = {0.0F, 0.0004F, 0.002F, 0.01F, 0.06F, 0.4F};
    Tally total_double;
    Tally total_float;
    for (const auto rate : {iclforge::ac3::SampleRate::k48000, iclforge::ac3::SampleRate::k44100,
                            iclforge::ac3::SampleRate::k32000}) {
        for (int stream = 0; stream < 6; ++stream) {
            std::vector<std::array<float, 256>> segments(300);
            float level = kLevels[static_cast<std::size_t>(level_pick(rng))];
            for (auto& segment : segments) {
                if (coin(rng) == 0) {
                    level = kLevels[static_cast<std::size_t>(level_pick(rng))];
                }
                for (auto& v : segment) {
                    v = noise(rng) * level;
                }
                if (coin(rng) == 0) {
                    const int from = position(rng);
                    const int to = std::min(256, from + burst_length(rng));
                    const float gain = static_cast<float>(burst_gain(rng));
                    for (int n = from; n < to; ++n) {
                        auto& v = segment[static_cast<std::size_t>(n)];
                        v = std::clamp(v * gain + noise(rng) * 0.05F, -1.0F, 1.0F);
                    }
                }
            }
            const Tally d = agree_on<double>(segments, rate);
            total_double.hits += d.hits;
            total_double.misses += d.misses;
            const Tally f = agree_on<float>(segments, rate);
            total_float.hits += f.hits;
            total_float.misses += f.misses;
        }
    }
    // The streams are not all one answer: both kinds of decision were held.
    CHECK(total_double.hits > 100);
    CHECK(total_double.misses > 100);
    CHECK(total_float.hits > 100);
    CHECK(total_float.misses > 100);
}

TEST_CASE("the detector decides as the transcription at every onset position", "[transient]") {
    // Six quiet segments, then a loud burst starting at each sample of the
    // seventh in turn (so each of the four blocks of 64 and every offset in
    // it is the first to hold the onset), then a decaying tail.
    std::mt19937 rng(0x7A42);
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
    int hits = 0;
    int misses = 0;
    for (int onset = 0; onset < 256; ++onset) {
        std::vector<std::array<float, 256>> segments(9);
        for (std::size_t s = 0; s < segments.size(); ++s) {
            for (auto& v : segments[s]) {
                v = noise(rng) * 0.01F;
            }
        }
        for (std::size_t s = 6; s < 8; ++s) {
            for (int n = (s == 6 ? onset : 0); n < 256; ++n) {
                segments[s][static_cast<std::size_t>(n)] += noise(rng) * (s == 6 ? 0.6F : 0.3F);
            }
        }
        const Tally d = agree_on<double>(segments, iclforge::ac3::SampleRate::k48000);
        const Tally f = agree_on<float>(segments, iclforge::ac3::SampleRate::k48000);
        hits += d.hits + f.hits;
        misses += d.misses + f.misses;
    }
    CHECK(hits > 100);
    CHECK(misses > 100);
}
