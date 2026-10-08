#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/dlc.hpp"

// AudioDataDLC (SMPTE ST 2098-2:2022 Annex B) decoder tests. Every fixture is built with a
// from-scratch bit writer and an encoder written here against §9.6 Table 10, and the expected
// output is computed by plain recurrences written out in the test, not by the decoder's own
// 64-entry circular history, so a mistake in the decoder's index handling or coefficient
// conversion shows up as a mismatch.

namespace {

using iclforge::iab::AudioDataDlc;
using iclforge::iab::DlcDecodeOptions;
using iclforge::iab::IabError;

class BitWriter {
public:
    void push_bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            push_bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1U));
        }
    }

    void align() {
        while (bit_count_ % 8 != 0) {
            push_bit(0);
        }
    }

    [[nodiscard]] std::vector<std::byte> bytes() const { return bytes_; }

private:
    void push_bit(unsigned bit) {
        if (bit_count_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit != 0) {
            bytes_.back() |= static_cast<std::byte>(1U << (7 - (bit_count_ % 8)));
        }
        ++bit_count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

constexpr std::uint8_t kFrameRate48Fps = 0x3;  // Table 30: 5 sub blocks of 200 (48 kHz)
constexpr unsigned kSubBlocks = 5;
constexpr unsigned kSize48 = 200;
constexpr unsigned kSize96 = 400;

enum class Coding { kPcm, kRice };

struct Region {
    unsigned length = 0;
    std::vector<unsigned> k_codes;  // KCoeff[1..order]
};

// A small deterministic generator so fixtures do not depend on <random> implementation details.
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_(seed) {}
    std::int32_t next(int bits) {
        state_ = state_ * 1664525U + 1013904223U;
        const auto raw = static_cast<std::int32_t>(state_ >> 8) - (1 << 23);
        return raw % (1 << (bits - 1));
    }

private:
    std::uint32_t state_;
};

void push_residual_sub_block(BitWriter& bw, std::span<const std::int32_t> residual, Coding coding,
                             unsigned rice_bits) {
    if (coding == Coding::kPcm) {
        bw.push_bits(0, 1);  // CodeType
        std::int32_t max_abs = 0;
        for (const auto r : residual) {
            max_abs = std::max(max_abs, std::abs(r));
        }
        unsigned bit_depth = 0;
        while ((1 << bit_depth) <= max_abs) {
            ++bit_depth;
        }
        bw.push_bits(bit_depth, 5);
        for (const auto r : residual) {
            if (bit_depth != 0) {
                bw.push_bits(static_cast<std::uint32_t>(std::abs(r)), bit_depth);
            }
            if (r != 0) {
                bw.push_bits(r < 0 ? 1U : 0U, 1);
            }
        }
        return;
    }
    bw.push_bits(1, 1);  // CodeType
    bw.push_bits(rice_bits, 5);
    for (const auto r : residual) {
        const auto magnitude = static_cast<std::uint32_t>(std::abs(r));
        const std::uint32_t quotient = magnitude >> rice_bits;
        for (std::uint32_t q = 0; q < quotient; ++q) {
            bw.push_bits(1, 1);
        }
        bw.push_bits(0, 1);
        if (rice_bits != 0) {
            bw.push_bits(magnitude & ((1U << rice_bits) - 1U), rice_bits);
        }
        if (r != 0) {
            bw.push_bits(r < 0 ? 1U : 0U, 1);
        }
    }
}

void push_layer(BitWriter& bw, const std::vector<Region>& regions, std::span<const std::int32_t> residual,
                unsigned sub_block_size, const std::vector<Coding>& coding) {
    bw.push_bits(regions.size(), 2);
    for (const auto& region : regions) {
        bw.push_bits(region.length, 4);
        bw.push_bits(region.k_codes.size(), 5);
        for (const auto k : region.k_codes) {
            bw.push_bits(k, 10);
        }
    }
    for (unsigned n = 0; n < kSubBlocks; ++n) {
        push_residual_sub_block(bw, residual.subspan(n * sub_block_size, sub_block_size),
                                coding[n % coding.size()], 3);
    }
}

// Builds a complete element: DLCSampleRate, ShiftBits, then the supplied layers.
struct ElementBuilder {
    BitWriter bw;
    ElementBuilder(unsigned sample_rate_code, unsigned shift_bits) {
        bw.push_bits(sample_rate_code, 2);
        bw.push_bits(shift_bits, 5);
    }
    AudioDataDlc finish() {
        bw.align();
        AudioDataDlc element;
        element.audio_data_id = 1;
        element.coded = bw.bytes();
        return element;
    }
};

std::vector<std::int32_t> random_samples(std::size_t count, int bits, std::uint32_t seed) {
    Lcg lcg(seed);
    std::vector<std::int32_t> out(count);
    for (auto& s : out) {
        s = lcg.next(bits);
    }
    return out;
}

}  // namespace

TEST_CASE("DLC: 48 kHz, no predictor, direct PCM residual", "[dlc]") {
    const auto samples = random_samples(kSubBlocks * kSize48, 24, 7);
    ElementBuilder builder(0, 8);
    push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});

    auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
    REQUIRE(audio.has_value());
    CHECK(audio->sample_rate == 48000);
    REQUIRE(audio->samples.size() == samples.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        REQUIRE(audio->samples[n] == static_cast<std::int32_t>(static_cast<std::uint32_t>(samples[n]) << 8));
    }

    const auto normalized = audio->normalized();
    CHECK(normalized.size() == samples.size());
    CHECK(normalized[0] == static_cast<float>(static_cast<double>(audio->samples[0]) / 2147483648.0));
}

TEST_CASE("DLC: Rice/Golomb residual with RiceRemBits 3 and 0", "[dlc]") {
    const auto samples = random_samples(kSubBlocks * kSize48, 12, 11);
    ElementBuilder builder(0, 16);
    BitWriter& bw = builder.bw;
    bw.push_bits(0, 2);  // NumPredRegions48
    for (unsigned n = 0; n < kSubBlocks; ++n) {
        const unsigned rice_bits = (n % 2 == 0) ? 3 : 0;
        push_residual_sub_block(bw, std::span(samples).subspan(n * kSize48, kSize48), Coding::kRice, rice_bits);
    }

    auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == samples.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        REQUIRE(audio->samples[n] == static_cast<std::int32_t>(static_cast<std::uint32_t>(samples[n]) << 16));
    }
}

TEST_CASE("DLC: first order predictor matches the Annex B recurrence", "[dlc]") {
    // KCoeff code 20 -> (20 - 512) << 11 = -1007616, a Q20 coefficient of -0.9609. B.7 for order 1
    // gives ACoeff[1] = KCoeff, and B.8 computes pcm[n] = residual[n] + ((-pcm[n-1] * a1) >> 20).
    constexpr unsigned kCode = 20;
    const std::int64_t a1 = static_cast<std::int64_t>(static_cast<int>(kCode) - 512) * 2048;

    const auto target = random_samples(kSubBlocks * kSize48, 20, 3);
    std::vector<std::int32_t> residual(target.size());
    std::int64_t previous = 0;
    for (std::size_t n = 0; n < target.size(); ++n) {
        const auto predicted = static_cast<std::int32_t>((-previous * a1) >> 20);
        residual[n] = target[n] - predicted;
        previous = target[n];
    }

    ElementBuilder builder(0, 0);
    push_layer(builder.bw, {Region{kSubBlocks, {kCode}}}, residual, kSize48, {Coding::kPcm});

    auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == target.size());
    for (std::size_t n = 0; n < target.size(); ++n) {
        REQUIRE(audio->samples[n] == target[n]);
    }
}

TEST_CASE("DLC: second order lattice to direct form conversion", "[dlc]") {
    // B.7 for order 2: ACoeff[1] = K1 + K2*K1/2^20, ACoeff[2] = K2, evaluated with the pseudocode's
    // shifts.
    constexpr unsigned kCode1 = 300;
    constexpr unsigned kCode2 = 700;
    const std::int64_t k1 = static_cast<std::int64_t>(static_cast<int>(kCode1) - 512) * 2048;
    const std::int64_t k2 = static_cast<std::int64_t>(static_cast<int>(kCode2) - 512) * 2048;
    const std::int64_t a1 = k1 + ((k2 * k1) >> 20);
    const std::int64_t a2 = k2;

    const auto target = random_samples(kSubBlocks * kSize48, 18, 5);
    std::vector<std::int32_t> residual(target.size());
    for (std::size_t n = 0; n < target.size(); ++n) {
        const std::int64_t p1 = n >= 1 ? target[n - 1] : 0;
        const std::int64_t p2 = n >= 2 ? target[n - 2] : 0;
        const auto predicted = static_cast<std::int32_t>((-(p1 * a1) - (p2 * a2)) >> 20);
        residual[n] = target[n] - predicted;
    }

    ElementBuilder builder(0, 0);
    push_layer(builder.bw, {Region{kSubBlocks, {kCode1, kCode2}}}, residual, kSize48, {Coding::kPcm});

    auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == target.size());
    for (std::size_t n = 0; n < target.size(); ++n) {
        REQUIRE(audio->samples[n] == target[n]);
    }
}

TEST_CASE("DLC: predictor history carries across regions and order 0 disables a region", "[dlc]") {
    constexpr unsigned kCode = 100;
    const std::int64_t a1 = static_cast<std::int64_t>(static_cast<int>(kCode) - 512) * 2048;

    // Region 0 (sub blocks 0-1): order 1. Region 1 (sub blocks 2-4): order 0, so the output is the
    // residual itself while the history keeps the last values.
    const auto target = random_samples(kSubBlocks * kSize48, 16, 9);
    std::vector<std::int32_t> residual(target.size());
    std::int64_t previous = 0;
    for (std::size_t n = 0; n < target.size(); ++n) {
        if (n < 2 * kSize48) {
            const auto predicted = static_cast<std::int32_t>((-previous * a1) >> 20);
            residual[n] = target[n] - predicted;
        } else {
            residual[n] = target[n];
        }
        previous = target[n];
    }

    ElementBuilder builder(0, 0);
    push_layer(builder.bw, {Region{2, {kCode}}, Region{3, {}}}, residual, kSize48, {Coding::kPcm});

    auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
    REQUIRE(audio.has_value());
    for (std::size_t n = 0; n < target.size(); ++n) {
        REQUIRE(audio->samples[n] == target[n]);
    }
}

namespace {

// B.9 written out directly: even outputs are the base layer delayed by 8, odd outputs a 16 tap
// half band interpolation of the base layer's most recent samples.
std::vector<std::int32_t> reference_upsample(const std::vector<std::int32_t>& base) {
    constexpr std::array<std::int64_t, 16> kOddTaps{-138,  305,  -618,  1128, -1952, 3377,  -6450, 20688,
                                                    20688, -6450, 3377, -1952, 1128,  -618,  305,   -138};
    std::vector<std::int32_t> out(base.size() * 2, 0);
    for (std::size_t m = 0; m < base.size(); ++m) {
        out[2 * m] = m >= 8 ? base[m - 8] : 0;
        std::int64_t accum = 0;
        for (std::size_t t = 0; t < 16; ++t) {
            // Interp[1] multiplies the newest sample base[m], Interp[3] base[m - 1], and so on.
            if (m >= t) {
                accum += static_cast<std::int64_t>(base[m - t]) * kOddTaps[t];
            }
        }
        out[2 * m + 1] = static_cast<std::int32_t>(accum >> 15);
    }
    return out;
}

}  // namespace

TEST_CASE("DLC: 96 kHz extension adds the upsampled base layer", "[dlc]") {
    const auto base = random_samples(kSubBlocks * kSize48, 16, 21);
    const auto extension_residual = random_samples(kSubBlocks * kSize96, 10, 22);
    constexpr unsigned kShift = 8;

    ElementBuilder builder(1, kShift);
    push_layer(builder.bw, {}, base, kSize48, {Coding::kPcm});
    push_layer(builder.bw, {}, extension_residual, kSize96, {Coding::kRice, Coding::kPcm});
    const AudioDataDlc element = builder.finish();

    const auto upsampled = reference_upsample(base);

    auto full = iclforge::iab::decode_dlc(element, kFrameRate48Fps);
    REQUIRE(full.has_value());
    CHECK(full->sample_rate == 96000);
    REQUIRE(full->samples.size() == kSubBlocks * kSize96);
    for (std::size_t n = 0; n < full->samples.size(); ++n) {
        const auto expected = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(extension_residual[n] + upsampled[n]) << kShift);
        REQUIRE(full->samples[n] == expected);
    }

    auto base_only = iclforge::iab::decode_dlc(element, kFrameRate48Fps, DlcDecodeOptions{.base_layer_only = true});
    REQUIRE(base_only.has_value());
    CHECK(base_only->sample_rate == 48000);
    REQUIRE(base_only->samples.size() == base.size());
    for (std::size_t n = 0; n < base.size(); ++n) {
        REQUIRE(base_only->samples[n] == static_cast<std::int32_t>(static_cast<std::uint32_t>(base[n]) << kShift));
    }
}

TEST_CASE("DLC: every integer frame rate uses its Table 30 sub block layout", "[dlc]") {
    struct Row {
        std::uint8_t code;
        unsigned blocks;
        unsigned size48;
    };
    const std::array<Row, 9> rows{{{0, 10, 200}, {1, 10, 192}, {2, 8, 200}, {3, 5, 200}, {4, 5, 192},
                                   {5, 4, 200},  {6, 5, 100},  {7, 4, 120}, {8, 4, 100}}};
    for (const auto& row : rows) {
        BitWriter bw;
        bw.push_bits(0, 2);  // DLCSampleRate
        bw.push_bits(0, 5);  // ShiftBits
        bw.push_bits(0, 2);  // NumPredRegions48
        for (unsigned n = 0; n < row.blocks; ++n) {
            bw.push_bits(0, 1);  // CodeType: PCM
            bw.push_bits(0, 5);  // BitDepth 0: every residual is 0
        }
        bw.align();
        AudioDataDlc element;
        element.coded = bw.bytes();
        auto audio = iclforge::iab::decode_dlc(element, row.code);
        REQUIRE(audio.has_value());
        CHECK(audio->samples.size() == static_cast<std::size_t>(row.blocks) * row.size48);
    }
}

TEST_CASE("DLC: malformed elements are refused", "[dlc]") {
    const auto samples = random_samples(kSubBlocks * kSize48, 12, 1);

    SECTION("reserved DLCSampleRate") {
        ElementBuilder builder(2, 0);
        push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});
        auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kBadDlc);
    }
    SECTION("region lengths that do not add up to the sub block count") {
        ElementBuilder builder(0, 0);
        push_layer(builder.bw, {Region{2, {}}}, samples, kSize48, {Coding::kPcm});
        auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kBadDlc);
    }
    SECTION("a region of length zero") {
        ElementBuilder builder(0, 0);
        push_layer(builder.bw, {Region{0, {}}, Region{5, {}}}, samples, kSize48, {Coding::kPcm});
        auto audio = iclforge::iab::decode_dlc(builder.finish(), kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kBadDlc);
    }
    SECTION("the non-integer frame rate") {
        ElementBuilder builder(0, 0);
        push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});
        auto audio = iclforge::iab::decode_dlc(builder.finish(), 0x9);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kBadDlc);
    }
    SECTION("a reserved frame rate") {
        ElementBuilder builder(0, 0);
        push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});
        auto audio = iclforge::iab::decode_dlc(builder.finish(), 0xA);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kReservedFrameRate);
    }
    SECTION("truncated residual") {
        ElementBuilder builder(0, 0);
        push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});
        AudioDataDlc element = builder.finish();
        element.coded.resize(element.coded.size() / 2);
        auto audio = iclforge::iab::decode_dlc(element, kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kTruncated);
    }
    SECTION("an unterminated unary code") {
        BitWriter bw;
        bw.push_bits(0, 2);
        bw.push_bits(0, 5);
        bw.push_bits(0, 2);
        bw.push_bits(1, 1);  // CodeType: Rice
        bw.push_bits(0, 5);
        for (int i = 0; i < 64; ++i) {
            bw.push_bits(1, 1);
        }
        AudioDataDlc element;
        element.coded = bw.bytes();
        auto audio = iclforge::iab::decode_dlc(element, kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kTruncated);
    }
    SECTION("an empty element") {
        auto audio = iclforge::iab::decode_dlc(AudioDataDlc{}, kFrameRate48Fps);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error() == IabError::kTruncated);
    }
}

TEST_CASE("DLC: decode_audio returns PCM and DLC essence together", "[dlc]") {
    const auto samples = random_samples(kSubBlocks * kSize48, 24, 4);
    ElementBuilder builder(0, 8);
    push_layer(builder.bw, {}, samples, kSize48, {Coding::kPcm});

    iclforge::iab::IaFrame frame;
    frame.sample_rate = 48000;
    frame.frame_rate_code = kFrameRate48Fps;
    frame.audio_pcm.push_back({.audio_data_id = 5, .samples = std::vector<float>(1000, 0.25F)});
    AudioDataDlc element = builder.finish();
    element.audio_data_id = 9;
    frame.audio_dlc.push_back(element);

    auto audio = iclforge::iab::decode_audio(frame);
    REQUIRE(audio.has_value());
    REQUIRE(audio->size() == 2);
    CHECK((*audio)[0].audio_data_id == 5);
    CHECK((*audio)[1].audio_data_id == 9);
    REQUIRE((*audio)[1].samples.size() == samples.size());
    CHECK((*audio)[1].samples[3] == static_cast<float>(static_cast<double>(samples[3]) / 8388608.0));

    SECTION("a DLC sample rate that disagrees with the IAFrame is refused") {
        frame.sample_rate = 96000;
        auto mismatched = iclforge::iab::decode_audio(frame);
        REQUIRE_FALSE(mismatched.has_value());
        CHECK(mismatched.error() == IabError::kBadDlc);
    }
}
