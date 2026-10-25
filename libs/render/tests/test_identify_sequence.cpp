// The identify tone a person starts on a board (render/identify_sequence.hpp): a burst of pink noise out of one
// speaker, or out of each in turn.
//
// It comes out of real speakers, so the limits are the tests' main business: the level can not be asked past its
// cap, a burst is as long as it is said to be, a sequence ends where its total limit says whatever is asked, and
// only the speaker whose turn it is makes a sound.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "iclforge/render/identify_sequence.hpp"

namespace {

using iclforge::render::IdentifySequence;
using iclforge::render::IdentifyTone;
using Mode = IdentifySequence::Mode;
using Target = IdentifySequence::Target;
using Catch::Approx;

constexpr std::uint32_t kRate = 48000;

// Everything a sequence writes, per output, run in blocks of `block` samples until it ends or `limit` samples.
struct Recording {
    std::vector<std::vector<float>> output;
    std::size_t samples = 0;
    bool ended = false;
};

Recording record(IdentifySequence& sequence, std::size_t outputs, std::size_t block, std::size_t limit) {
    Recording result;
    result.output.resize(outputs);
    std::vector<std::vector<float>> planes(outputs, std::vector<float>(block));
    std::vector<std::span<float>> spans(outputs);
    for (std::size_t done = 0; done < limit; done += block) {
        const std::size_t n = std::min(block, limit - done);
        for (std::size_t o = 0; o < outputs; ++o) {
            spans[o] = std::span<float>(planes[o].data(), n);
            std::fill(planes[o].begin(), planes[o].end(), 7.0F);  // a sequence writes every sample, or fails
        }
        const bool going = sequence.fill(spans);
        for (std::size_t o = 0; o < outputs; ++o) {
            result.output[o].insert(result.output[o].end(), planes[o].begin(), planes[o].begin() + static_cast<std::ptrdiff_t>(n));
        }
        result.samples += n;
        if (!going) {
            result.ended = true;
            break;
        }
    }
    return result;
}

double rms_db(std::span<const float> samples) {
    double sum = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return 10.0 * std::log10(sum / static_cast<double>(samples.size()));
}

std::size_t non_zero(std::span<const float> samples) {
    return static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(), [](float s) { return s != 0.0F; }));
}

float peak(std::span<const float> samples) {
    float p = 0.0F;
    for (const float s : samples) {
        p = std::max(p, std::fabs(s));
    }
    return p;
}

}  // namespace

TEST_CASE("the level can not be asked past its cap", "[render][identify-sequence]") {
    REQUIRE(IdentifySequence::clamp_level_db(-30.0) == -30.0);
    REQUIRE(IdentifySequence::clamp_level_db(-24.0) == -24.0);
    REQUIRE(IdentifySequence::clamp_level_db(-23.9) == -24.0);
    REQUIRE(IdentifySequence::clamp_level_db(0.0) == -24.0);
    REQUIRE(IdentifySequence::clamp_level_db(12.0) == -24.0);
    REQUIRE(IdentifySequence::clamp_level_db(-200.0) == -60.0);
    REQUIRE(IdentifySequence::clamp_level_db(std::numeric_limits<double>::quiet_NaN()) == -30.0);
    REQUIRE(IdentifySequence::clamp_level_db(std::numeric_limits<double>::infinity()) == -24.0);
    REQUIRE(IdentifySequence::kMaxLevelDb < IdentifyTone::kMaxLevelDb);

    // A sequence asked for full scale plays at the cap: measured, with the fades out of the way.
    for (const double asked : {0.0, -6.0, 100.0, std::numeric_limits<double>::infinity()}) {
        INFO(asked);
        IdentifySequence sequence(kRate);
        const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
        REQUIRE(sequence.start(Mode::kOnce, targets, asked));
        const Recording r = record(sequence, 1, 256, 200'000);
        const std::span<const float> middle(r.output[0].data() + 4800, 48000);
        REQUIRE(rms_db(middle) == Approx(-24.0).margin(1.5));
        // Pink noise peaks about 12 dB over its RMS: nothing near full scale.
        REQUIRE(peak(r.output[0]) < 0.35F);
    }
}

TEST_CASE("the default level is -30 dB and is what was asked for below the cap", "[render][identify-sequence]") {
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
    for (const double asked : {-30.0, -40.0, -55.0}) {
        INFO(asked);
        IdentifySequence sequence(kRate);
        REQUIRE(sequence.start(Mode::kOnce, targets, asked));
        const Recording r = record(sequence, 1, 256, 200'000);
        const std::span<const float> middle(r.output[0].data() + 4800, 48000);
        REQUIRE(rms_db(middle) == Approx(asked).margin(1.5));
    }
    IdentifySequence sequence(kRate);
    REQUIRE(sequence.start(Mode::kOnce, targets));
    const Recording r = record(sequence, 1, 256, 200'000);
    REQUIRE(rms_db(std::span<const float>(r.output[0].data() + 4800, 48000)) == Approx(-30.0).margin(1.5));
}

TEST_CASE("a single burst is 1.5 seconds on the one output and silence everywhere else", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 1> targets = {Target{2, IdentifyTone::Band::kFull}};
    REQUIRE(sequence.start(Mode::kOnce, targets, -30.0));
    REQUIRE(sequence.active());
    REQUIRE(sequence.output() == 2);
    const Recording r = record(sequence, 4, 256, 200'000);
    REQUIRE(r.ended);
    REQUIRE_FALSE(sequence.active());
    REQUIRE(sequence.output() == -1);
    // It ended in the block that holds sample 72,000: 1.5 s at 48 kHz, to the block.
    REQUIRE(r.samples >= 72'000);
    REQUIRE(r.samples < 72'000 + 256);
    for (const std::size_t silent : {0U, 1U, 3U}) {
        REQUIRE(non_zero(r.output[silent]) == 0);
    }
    // The tone: 72,000 samples, none of them the 7.0 the buffer was filled with, and nothing after.
    REQUIRE(non_zero(r.output[2]) >= 71'900);
    REQUIRE(non_zero(r.output[2]) <= 72'000);
    REQUIRE(non_zero(std::span<const float>(r.output[2].data() + 72'000, r.samples - 72'000)) == 0);
    REQUIRE(peak(r.output[2]) < 1.0F);
}

TEST_CASE("a burst fades in and out, so no speaker is clicked", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
    REQUIRE(sequence.start(Mode::kOnce, targets, -24.0));
    const Recording r = record(sequence, 1, 256, 100'000);
    const std::vector<float>& out = r.output[0];

    // The same noise, undressed: the burst is that noise times a smoothstep over the first and last 20 ms
    // (960 samples) and 1 between.
    IdentifyTone raw(kRate);
    REQUIRE(raw.set_level_db(-24.0));
    std::vector<float> plain(72'000);
    raw.generate(plain, IdentifyTone::Band::kFull);
    const auto fade = [&](std::size_t from_edge) {
        const double x = (static_cast<double>(from_edge) + 0.5) / 960.0;
        return x * x * (3.0 - (2.0 * x));
    };
    for (std::size_t k = 0; k < 72'000; ++k) {
        double gain = 1.0;
        if (k < 960) {
            gain = fade(k);
        }
        if (72'000 - 1 - k < 960) {
            gain = std::min(gain, fade(72'000 - 1 - k));
        }
        REQUIRE(out[k] == Approx(static_cast<double>(plain[k]) * gain).margin(1e-6).epsilon(1e-5));
    }
    // So the first and last samples are almost nothing, and the middle is the noise as it is.
    REQUIRE(std::fabs(out[0]) < 0.001F);
    REQUIRE(std::fabs(out[71'999]) < 0.001F);
    REQUIRE(std::equal(out.begin() + 960, out.begin() + 71'040, plain.begin() + 960));
}

TEST_CASE("the sequence is the same sound for every block size", "[render][identify-sequence]") {
    const std::array<Target, 2> targets = {Target{1, IdentifyTone::Band::kFull}, Target{0, IdentifyTone::Band::kLow}};
    IdentifySequence reference(kRate);
    REQUIRE(reference.start(Mode::kEach, targets, -30.0));
    const Recording whole = record(reference, 2, 256, 400'000);
    REQUIRE(whole.ended);
    for (const std::size_t block : {1U, 7U, 100U, 255U, 1000U}) {
        INFO(block);
        IdentifySequence sequence(kRate);
        REQUIRE(sequence.start(Mode::kEach, targets, -30.0));
        const Recording r = record(sequence, 2, block, 400'000);
        REQUIRE(r.ended);
        const std::size_t common = std::min(r.output[0].size(), whole.output[0].size());
        // Both bursts and the gap between them, 168,000 samples (they end in different blocks): identical samples.
        REQUIRE(common >= 168'000);
        for (std::size_t o = 0; o < 2; ++o) {
            REQUIRE(std::equal(r.output[o].begin(), r.output[o].begin() + static_cast<std::ptrdiff_t>(168'000), whole.output[o].begin()));
        }
    }
}

TEST_CASE("each speaker in turn: one at a time, in order, 0.5 seconds apart, and then it ends", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 3> targets = {Target{0, IdentifyTone::Band::kFull}, Target{2, IdentifyTone::Band::kFull},
                                           Target{1, IdentifyTone::Band::kLow}};
    REQUIRE(sequence.start(Mode::kEach, targets, -30.0));
    const Recording r = record(sequence, 3, 256, 400'000);
    REQUIRE(r.ended);
    // 3 bursts of 1.5 s and 2 gaps of 0.5 s: 5.5 s, which is 264,000 samples, to the block.
    REQUIRE(r.samples >= 264'000);
    REQUIRE(r.samples < 264'000 + 256);
    const auto sounding = [&](std::size_t output, std::size_t from, std::size_t length) {
        return non_zero(std::span<const float>(r.output[output].data() + from, length));
    };
    // Burst 1 on output 0 over [0, 72000), burst 2 on output 2 over [96000, 168000), burst 3 on output 1 over
    // [192000, 264000). Each output is silent the rest of the time.
    REQUIRE(sounding(0, 100, 71'800) > 71'000);
    REQUIRE(sounding(0, 72'000, r.samples - 72'000) == 0);
    REQUIRE(sounding(2, 0, 96'000) == 0);
    REQUIRE(sounding(2, 96'100, 71'800) > 71'000);
    REQUIRE(sounding(2, 168'000, r.samples - 168'000) == 0);
    REQUIRE(sounding(1, 0, 192'000) == 0);
    REQUIRE(sounding(1, 192'100, 71'800) > 70'000);
    // The gaps are silent on every output.
    for (std::size_t o = 0; o < 3; ++o) {
        REQUIRE(sounding(o, 72'000, 24'000) == 0);
        REQUIRE(sounding(o, 168'000, 24'000) == 0);
    }
    // The third is an LFE-style feed: the low band, which is quieter in the room for the same level but made
    // 11.5 dB hotter by the generator to be the stated level.
    REQUIRE(rms_db(std::span<const float>(r.output[1].data() + 192'000 + 4800, 48000)) == Approx(-30.0).margin(2.0));
}

TEST_CASE("which output sounds, and where the sequence is, can be asked", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 2> targets = {Target{3, IdentifyTone::Band::kFull}, Target{5, IdentifyTone::Band::kFull}};
    REQUIRE_FALSE(sequence.active());
    REQUIRE(sequence.output() == -1);
    REQUIRE(sequence.start(Mode::kEach, targets));
    REQUIRE(sequence.step() == 0);
    REQUIRE(sequence.output() == 3);
    std::vector<std::vector<float>> planes(6, std::vector<float>(256));
    std::vector<std::span<float>> spans;
    for (auto& p : planes) {
        spans.emplace_back(p);
    }
    // 1.5 s of tone is 281.25 blocks; the 282nd block ends it, and the gap begins inside that block.
    for (int block = 0; block < 282; ++block) {
        REQUIRE(sequence.fill(spans));
    }
    REQUIRE(sequence.output() == -1);
    // The gap lasts 0.5 s, 93.75 blocks, after which the second burst begins.
    for (int block = 0; block < 94; ++block) {
        REQUIRE(sequence.fill(spans));
    }
    REQUIRE(sequence.step() == 1);
    REQUIRE(sequence.output() == 5);
}

TEST_CASE("a loop repeats one speaker and ends at the total limit, whatever is asked", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
    REQUIRE(sequence.start(Mode::kLoop, targets, -30.0));
    REQUIRE(sequence.remaining_ms() == 60'000);
    // Ask for far more than the limit: it ends at 60 s of samples, in the block that holds them.
    const Recording r = record(sequence, 1, 256, 10'000'000);
    REQUIRE(r.ended);
    REQUIRE(r.samples >= 2'880'000);
    REQUIRE(r.samples < 2'880'000 + 256);
    REQUIRE(sequence.remaining_ms() == 0);
    // Nothing is written past the limit.
    REQUIRE(non_zero(std::span<const float>(r.output[0].data() + 2'880'000, r.samples - 2'880'000)) == 0);
    // 30 bursts of 1.5 s in 60 s of 2 s periods, each with the same noise.
    std::size_t sounding_samples = non_zero(r.output[0]);
    REQUIRE(sounding_samples >= 30 * 71'900);
    REQUIRE(sounding_samples <= 30 * 72'000);
    // The same noise for every burst: burst 1 and burst 7 are sample for sample the same.
    REQUIRE(std::equal(r.output[0].begin(), r.output[0].begin() + 72'000, r.output[0].begin() + (6 * 96'000)));
}

TEST_CASE("starting a sequence again begins its count again, and never longer than the limit", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
    REQUIRE(sequence.start(Mode::kLoop, targets));
    const Recording first = record(sequence, 1, 256, 1'000'000);
    REQUIRE_FALSE(first.ended);
    REQUIRE(sequence.remaining_ms() < 60'000 - 20'000);
    REQUIRE(sequence.start(Mode::kLoop, targets));
    REQUIRE(sequence.remaining_ms() == 60'000);
    const Recording second = record(sequence, 1, 256, 10'000'000);
    REQUIRE(second.ended);
    REQUIRE(second.samples < 2'880'000 + 256);
}

TEST_CASE("a sequence with no speaker does not start, and more than sixteen are cut to sixteen", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    REQUIRE_FALSE(sequence.start(Mode::kEach, std::span<const Target>{}));
    REQUIRE_FALSE(sequence.active());

    std::array<Target, 20> many{};
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = Target{static_cast<std::uint8_t>(i), IdentifyTone::Band::kFull};
    }
    REQUIRE(sequence.start(Mode::kEach, many));
    const Recording r = record(sequence, 20, 256, 10'000'000);
    REQUIRE(r.ended);
    // 16 bursts and 15 gaps: 16 * 1.5 + 15 * 0.5 = 31.5 s, which is under the 60 s limit.
    REQUIRE(r.samples >= 1'512'000);
    REQUIRE(r.samples < 1'512'000 + 256);
    for (std::size_t o = 16; o < 20; ++o) {
        REQUIRE(non_zero(r.output[o]) == 0);
    }
    REQUIRE(non_zero(r.output[15]) > 0);
}

TEST_CASE("a target past the block's outputs is silent, and the rest of the sequence is not shifted", "[render][identify-sequence]") {
    const std::array<Target, 2> targets = {Target{5, IdentifyTone::Band::kFull}, Target{0, IdentifyTone::Band::kFull}};
    IdentifySequence sequence(kRate);
    REQUIRE(sequence.start(Mode::kEach, targets));
    const Recording r = record(sequence, 2, 256, 400'000);
    REQUIRE(r.ended);
    REQUIRE(non_zero(std::span<const float>(r.output[0].data(), 96'000)) == 0);
    REQUIRE(non_zero(std::span<const float>(r.output[0].data() + 96'100, 71'800)) > 71'000);
}

TEST_CASE("stop fades a sounding tone out, and cuts it at once when asked", "[render][identify-sequence]") {
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kFull}};
    std::vector<float> plane(256);
    const std::array<std::span<float>, 1> spans = {std::span<float>(plane)};

    IdentifySequence fading(kRate);
    REQUIRE(fading.start(Mode::kLoop, targets, -24.0));
    for (int i = 0; i < 40; ++i) {
        REQUIRE(fading.fill(spans));
    }
    fading.stop();
    // 20 ms is 960 samples: it is gone in the fourth block. The tone is still there in the first.
    REQUIRE(fading.fill(spans));
    REQUIRE(rms_db(plane) > -40.0);
    int blocks = 1;
    while (fading.fill(spans)) {
        ++blocks;
        REQUIRE(blocks < 10);
    }
    // Three blocks that went on, and the fourth that ended it.
    REQUIRE(blocks == 3);
    REQUIRE_FALSE(fading.active());
    REQUIRE(fading.output() == -1);
    // The end of the last block is silence already, and after the end it writes silence.
    REQUIRE(non_zero(plane) < 256);
    REQUIRE(plane[255] == 0.0F);
    REQUIRE_FALSE(fading.fill(spans));
    REQUIRE(non_zero(plane) == 0);

    IdentifySequence cut(kRate);
    REQUIRE(cut.start(Mode::kLoop, targets, -24.0));
    for (int i = 0; i < 40; ++i) {
        REQUIRE(cut.fill(spans));
    }
    cut.stop(true);
    REQUIRE_FALSE(cut.active());
    REQUIRE_FALSE(cut.fill(spans));
    REQUIRE(non_zero(plane) == 0);

    // In a gap, stop ends it at once, tone or no.
    IdentifySequence gap(kRate);
    REQUIRE(gap.start(Mode::kLoop, targets, -24.0));
    std::vector<float> big(80'000);
    const std::array<std::span<float>, 1> long_block = {std::span<float>(big)};
    REQUIRE(gap.fill(long_block));
    REQUIRE(gap.output() == -1);
    gap.stop();
    REQUIRE_FALSE(gap.active());

    // And stopping what is not running is nothing.
    IdentifySequence idle(kRate);
    idle.stop();
    idle.stop(true);
    REQUIRE_FALSE(idle.active());
}

TEST_CASE("the low band is made for an LFE feed", "[render][identify-sequence]") {
    IdentifySequence sequence(kRate);
    const std::array<Target, 1> targets = {Target{0, IdentifyTone::Band::kLow}};
    REQUIRE(sequence.start(Mode::kOnce, targets, -30.0));
    const Recording r = record(sequence, 1, 256, 100'000);
    // Energy above 200 Hz is far below the whole: count zero crossings of the body as a proxy for its pitch.
    const std::span<const float> body(r.output[0].data() + 4800, 48000);
    std::size_t crossings = 0;
    for (std::size_t k = 1; k < body.size(); ++k) {
        crossings += ((body[k - 1] < 0.0F) != (body[k] < 0.0F)) ? 1U : 0U;
    }
    // Under 80 Hz-ish: at most a few hundred crossings a second, where full-band noise has tens of thousands.
    REQUIRE(crossings < 400);
}
