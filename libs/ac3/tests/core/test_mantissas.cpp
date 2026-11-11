#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "iclforge/ac3/core/mantissas.hpp"

namespace {

double to_unit(std::int32_t mantissa25) { return mantissa25 / 16777216.0; }

}  // namespace

TEST_CASE("quantize/dequantize round-trip error is within half a step", "[mantissas]") {
    std::mt19937 rng(0x073A);
    std::uniform_real_distribution<double> dist(-0.999, 0.999);
    for (int bap = 1; bap <= 15; ++bap) {
        const double step =
            bap <= 5 ? 2.0 / iclforge::ac3::kSymmetricLevels[static_cast<std::size_t>(bap)]
                     : 1.0 / (1 << (iclforge::ac3::kBapBits[static_cast<std::size_t>(bap)] - 1));
        // Asymmetric two's complement tops out at 1 - step (A/52 7.3.2:
        // the mantissa word spans (1.0 - 2^-(qntztab-1)) to -1.0); values
        // beyond that clamp by design and carry a larger error.
        const double max_representable = bap <= 5 ? 1.0 : 1.0 - step;
        for (int trial = 0; trial < 200; ++trial) {
            const double value = dist(rng);
            const auto mantissa = static_cast<std::int32_t>(std::lround(value * 16777216.0));
            const auto code = iclforge::ac3::quantize_mantissa(mantissa, bap);
            const double reconstructed = iclforge::ac3::dequantize_mantissa(code, bap);
            CAPTURE(bap, value, code);
            if (value <= max_representable + step / 2) {
                CHECK(std::abs(reconstructed - to_unit(mantissa)) <= step / 2 + 1e-9);
            } else {
                CHECK(reconstructed == max_representable);  // clamped to the top code
            }
        }
    }
}

TEST_CASE("symmetric quantizer maps zero to the middle code", "[mantissas]") {
    CHECK(iclforge::ac3::quantize_mantissa(0, 1) == 1);   // 3-level: code 1 = 0
    CHECK(iclforge::ac3::quantize_mantissa(0, 2) == 2);   // 5-level
    CHECK(iclforge::ac3::quantize_mantissa(0, 3) == 3);   // 7-level
    CHECK(iclforge::ac3::quantize_mantissa(0, 4) == 5);   // 11-level
    CHECK(iclforge::ac3::quantize_mantissa(0, 5) == 7);   // 15-level
    for (int bap = 1; bap <= 5; ++bap) {
        CHECK(iclforge::ac3::dequantize_mantissa(iclforge::ac3::quantize_mantissa(0, bap), bap) ==
              0.0);
    }
}

TEST_CASE("grouping: codeword sits at first member, counts match", "[mantissas]") {
    // Hand case: five bap-1 mantissas (one full group + a padded partial),
    // one bap-3, two bap-4 (one pair). Codes: bap1 of 0 -> 1 (middle).
    iclforge::ac3::MantissaBlockWriter writer;
    for (int i = 0; i < 5; ++i) {
        writer.add(0, 1);
    }
    writer.add(0, 3);
    writer.add(0, 4);
    writer.add(0, 4);
    writer.finish_block();

    const auto& tokens = writer.tokens();
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].bits == 5);   // bap-1 group of codes 1,1,1 -> 9+3+1 = 13
    CHECK(tokens[0].value == 13);
    CHECK(tokens[1].bits == 5);   // partial group: codes 1,1 + dummy 0 -> 12
    CHECK(tokens[1].value == 12);
    CHECK(tokens[2].bits == 3);   // direct bap-3 code 3
    CHECK(tokens[2].value == 3);
    CHECK(tokens[3].bits == 7);   // bap-4 pair codes 5,5 -> 55*... 11*5+5 = 60
    CHECK(tokens[3].value == 60);
    CHECK(writer.bit_count() == 5 + 5 + 3 + 7);
}

TEST_CASE("bit counter agrees with the writer for random allocations", "[mantissas]") {
    std::mt19937 rng(0x0B77);
    std::uniform_int_distribution<int> bap_dist(0, 15);
    std::uniform_int_distribution<std::int32_t> mant_dist(-16777216, 16777215);

    for (int trial = 0; trial < 30; ++trial) {
        std::array<std::vector<std::uint8_t>, 2> baps;
        iclforge::ac3::MantissaBlockWriter writer;
        for (auto& channel : baps) {
            channel.resize(253);
            for (auto& bap : channel) {
                bap = static_cast<std::uint8_t>(bap_dist(rng));
            }
        }
        for (const auto& channel : baps) {
            for (const auto bap : channel) {
                writer.add(mant_dist(rng), bap);
            }
        }
        writer.finish_block();

        const std::array<std::span<const std::uint8_t>, 2> views = {baps[0], baps[1]};
        std::size_t token_bits = 0;
        for (const auto& token : writer.tokens()) {
            token_bits += token.bits;
        }
        CAPTURE(trial);
        CHECK(writer.bit_count() == iclforge::ac3::mantissa_bits_per_block(views));
        CHECK(token_bits == writer.bit_count());
    }
}

namespace {

// Today's mantissa_bits_per_block, transcribed from before the tally form
// replaced it: a switch per bin, then the group arithmetic.
std::size_t reference_mantissa_bits(std::span<const std::span<const std::uint8_t>> channel_baps) {
    std::size_t direct = 0;
    std::size_t count1 = 0;
    std::size_t count2 = 0;
    std::size_t count4 = 0;
    for (const auto& baps : channel_baps) {
        for (const auto bap : baps) {
            switch (bap) {
                case 0: break;
                case 1: ++count1; break;
                case 2: ++count2; break;
                case 4: ++count4; break;
                default: direct += static_cast<std::size_t>(iclforge::ac3::kBapBits[bap]); break;
            }
        }
    }
    return direct + 5 * ((count1 + 2) / 3) + 7 * ((count2 + 2) / 3) + 7 * ((count4 + 1) / 2);
}

std::vector<std::span<const std::uint8_t>> views_of(
    const std::vector<std::vector<std::uint8_t>>& channels) {
    std::vector<std::span<const std::uint8_t>> views;
    views.reserve(channels.size());
    for (const auto& channel : channels) {
        views.emplace_back(channel);
    }
    return views;
}

}  // namespace

TEST_CASE("bit count equals the per-bin switch for every bap and every short length",
          "[mantissas]") {
    // Every bap alone, at every length through two full cycles of the
    // 3-member groups and the 2-member pairs and a bit more: the partial
    // groups at the end of the count are where an error would sit.
    for (int bap = 0; bap <= 15; ++bap) {
        for (std::size_t length = 0; length <= 70; ++length) {
            const std::vector<std::vector<std::uint8_t>> one{
                std::vector<std::uint8_t>(length, static_cast<std::uint8_t>(bap))};
            const auto views = views_of(one);
            CAPTURE(bap, length);
            CHECK(iclforge::ac3::mantissa_bits_per_block(views) == reference_mantissa_bits(views));
        }
    }
}

TEST_CASE("bit count equals the per-bin switch on random allocations", "[mantissas]") {
    std::mt19937 rng(0x14C0);
    // Weighted towards 1, 2 and 4 so the grouped counts take every residue.
    std::discrete_distribution<int> pick({3, 6, 6, 3, 6, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2});
    std::uniform_int_distribution<std::size_t> length_dist(0, 300);
    std::uniform_int_distribution<int> channel_dist(0, 7);
    for (int trial = 0; trial < 4000; ++trial) {
        std::vector<std::vector<std::uint8_t>> channels(
            static_cast<std::size_t>(channel_dist(rng)));
        for (auto& channel : channels) {
            channel.resize(length_dist(rng));
            for (auto& bap : channel) {
                bap = static_cast<std::uint8_t>(pick(rng));
            }
        }
        const auto views = views_of(channels);
        CAPTURE(trial);
        CHECK(iclforge::ac3::mantissa_bits_per_block(views) == reference_mantissa_bits(views));
    }
}

TEST_CASE("bit count equals the per-bin switch across the 16-bit tally limit", "[mantissas]") {
    // A channel longer than the run the packed tally takes at once, so the
    // fields are unpacked and restarted part-way: all of one bap (the widest
    // direct code and each grouped bap fill a field fastest) and a random mix.
    std::mt19937 rng(0x14C1);
    std::uniform_int_distribution<int> any(0, 15);
    for (const std::size_t length : {std::size_t{4094}, std::size_t{4095}, std::size_t{4096},
                                     std::size_t{4097}, std::size_t{8189}, std::size_t{8190},
                                     std::size_t{8191}, std::size_t{12290}, std::size_t{40000}}) {
        for (int bap = 0; bap <= 15; ++bap) {
            const std::vector<std::vector<std::uint8_t>> one{
                std::vector<std::uint8_t>(length, static_cast<std::uint8_t>(bap))};
            const auto views = views_of(one);
            CAPTURE(bap, length);
            CHECK(iclforge::ac3::mantissa_bits_per_block(views) == reference_mantissa_bits(views));
        }
        std::vector<std::vector<std::uint8_t>> mixed(3, std::vector<std::uint8_t>(length));
        for (auto& channel : mixed) {
            for (auto& bap : channel) {
                bap = static_cast<std::uint8_t>(any(rng));
            }
        }
        const auto views = views_of(mixed);
        CAPTURE(length);
        CHECK(iclforge::ac3::mantissa_bits_per_block(views) == reference_mantissa_bits(views));
    }
}
