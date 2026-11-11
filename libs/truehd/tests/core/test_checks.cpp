#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <span>
#include <vector>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/format.hpp"

namespace {

using namespace iclforge::truehd;

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// The checks as they were before they took several bytes a step: the byte loop for the parity and the
// CRC-8 (polynomial 0x63, initial value 0x3C) one bit at a time, which needs no table to be right.
std::uint8_t reference_parity(std::span<const std::byte> payload) {
    std::uint8_t xor_all = 0;
    for (const std::byte b : payload) {
        xor_all ^= static_cast<std::uint8_t>(b);
    }
    return static_cast<std::uint8_t>(xor_all ^ 0xA9U);
}

std::uint8_t reference_crc(std::span<const std::byte> payload) {
    if (payload.empty()) {
        return 0;
    }
    std::uint8_t reg = 0x3C;
    for (std::size_t i = 0; i + 1 < payload.size(); ++i) {
        reg = static_cast<std::uint8_t>(reg ^ static_cast<std::uint8_t>(payload[i]));
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x80U) != 0
                      ? static_cast<std::uint8_t>((static_cast<unsigned>(reg) << 1U) ^ 0x63U)
                      : static_cast<std::uint8_t>(reg << 1U);
        }
    }
    return static_cast<std::uint8_t>(reg ^ static_cast<std::uint8_t>(payload.back()));
}

// Pseudo-random buffers (a fixed seed) of every length 0 to 64 and some long ones, each taken at
// every offset 0 to 15 of a buffer, so that the words and the steps of the check meet every
// alignment and every remainder. `check` is given the span.
template <typename Check>
void for_each_random_span(Check&& check) {
    std::mt19937 rng(0x7D1C0DE5U);
    constexpr std::size_t kPad = 16;
    const std::size_t lengths[] = {65, 100, 127, 128, 129, 255, 256, 257, 500, 1000, 1023, 1024, 4093};
    std::vector<std::size_t> all;
    for (std::size_t n = 0; n <= 64; ++n) {
        all.push_back(n);
    }
    all.insert(all.end(), std::begin(lengths), std::end(lengths));
    std::vector<std::byte> buffer;
    for (const std::size_t n : all) {
        for (int round = 0; round < 6; ++round) {
            buffer.resize(n + kPad);
            for (std::byte& b : buffer) {
                // Round 0 is all ones and round 1 all zeros: the extremes of a register.
                b = static_cast<std::byte>(round == 0 ? 0xFFU : (round == 1 ? 0U : rng() & 0xFFU));
            }
            for (std::size_t offset = 0; offset < kPad; ++offset) {
                check(std::span<const std::byte>(buffer).subspan(offset, n));
            }
        }
    }
}

}  // namespace

TEST_CASE("the word-wise substream parity matches the byte loop", "[truehd][checks]") {
    std::size_t spans = 0;
    for_each_random_span([&](std::span<const std::byte> payload) {
        ++spans;
        REQUIRE(substream_parity(payload) == reference_parity(payload));
    });
    CHECK(spans > 5000);
}

TEST_CASE("the byte-wise block header CRC matches the bit-by-bit CRC", "[truehd][checks]") {
    // The register as it was: one bit at a time, a bit past the data reading as zero.
    const auto reference = [](std::span<const std::byte> data, std::size_t first_bit,
                              std::size_t end_bit) {
        std::uint8_t reg = 0xA2;
        for (std::size_t bit = first_bit; bit < end_bit; ++bit) {
            const std::size_t byte = bit >> 3U;
            const unsigned in =
                byte < data.size()
                    ? (std::to_integer<unsigned>(data[byte]) >> (7U - (bit & 7U))) & 1U
                    : 0U;
            const bool top = (reg & 0x80U) != 0;
            reg = static_cast<std::uint8_t>((static_cast<unsigned>(reg) << 1U) | in);
            if (top) {
                reg ^= 0x1DU;
            }
        }
        return reg;
    };
    std::mt19937 rng(0x5EED0001U);
    std::size_t cases = 0;
    for (std::size_t size = 0; size <= 40; ++size) {
        std::vector<std::byte> data(size);
        for (std::byte& b : data) {
            b = static_cast<std::byte>(rng() & 0xFFU);
        }
        // Every start in the first two bytes, and ends from before the start to past the data.
        for (std::size_t first = 0; first < 17; ++first) {
            for (std::size_t end = 0; end <= size * 8 + 17; ++end) {
                ++cases;
                REQUIRE(block_header_crc(data, first, end) == reference(data, first, end));
            }
        }
    }
    CHECK(cases > 10000);
    // Long ones, from an odd bit.
    std::vector<std::byte> longer(3000);
    for (std::byte& b : longer) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    for (std::size_t first : {0U, 3U, 7U, 8U, 13U}) {
        REQUIRE(block_header_crc(longer, first, longer.size() * 8 - 5) ==
                reference(longer, first, longer.size() * 8 - 5));
    }
}

TEST_CASE("the multi-byte substream CRC matches the bit-by-bit CRC", "[truehd][checks]") {
    std::size_t spans = 0;
    for_each_random_span([&](std::span<const std::byte> payload) {
        ++spans;
        REQUIRE(substream_crc(payload) == reference_crc(payload));
    });
    CHECK(spans > 5000);
}

TEST_CASE("the substream parity is the XOR of the payload with 0xA9", "[truehd][checks]") {
    CHECK(substream_parity(bytes_of({})) == 0xA9);
    CHECK(substream_parity(bytes_of({0xA9})) == 0x00);
    CHECK(substream_parity(bytes_of({0x01, 0x02, 0x04})) == (0x07 ^ 0xA9));
}

TEST_CASE("the substream CRC folds in the last payload byte", "[truehd][checks]") {
    // With one byte the register never runs: the result is the initial value 0x3C with the byte
    // XORed in.
    CHECK(substream_crc(bytes_of({0x5A})) == (0x3C ^ 0x5A));
    CHECK(substream_crc(bytes_of({})) == 0);
    // Changing any byte changes the CRC.
    const auto a = substream_crc(bytes_of({1, 2, 3, 4}));
    const auto b = substream_crc(bytes_of({1, 2, 3, 5}));
    const auto c = substream_crc(bytes_of({1, 9, 3, 4}));
    CHECK(a != b);
    CHECK(a != c);
}

TEST_CASE("the four nibbles of the extra data's header word XOR to 0xF", "[truehd][checks]") {
    // TH spec §16.2: check_nibble = nibble_2 ^ nibble_1 ^ nibble_0 ^ 0xF.
    CHECK(extra_data_check_nibble(0x000) == 0xF);
    CHECK(extra_data_check_nibble(0x123) == 0xF);  // 1 ^ 2 ^ 3 is 0
    CHECK(extra_data_check_nibble(0x002) == 0xD);
    CHECK(extra_data_check_nibble(0xFFF) == 0x0);
    for (std::uint32_t length = 0; length < 0x1000; ++length) {
        const std::uint32_t word = (std::uint32_t{extra_data_check_nibble(length)} << 12U) | length;
        CHECK(((word >> 12U) ^ (word >> 8U) ^ (word >> 4U) ^ word) % 16U == 0xFU);
    }
}

TEST_CASE("the major sync CRC XORs its last two bytes into the register", "[truehd][checks]") {
    // Two bytes: the register is untouched, the result is the big-endian word.
    CHECK(major_sync_crc(bytes_of({0x12, 0x34})) == 0x1234);
    CHECK(major_sync_crc(bytes_of({0x01})) == 0);
    // A byte before them runs through the table: the entry for 0x01 is the polynomial, 0x002D.
    CHECK(major_sync_crc(bytes_of({0x01, 0x00, 0x00})) == 0x002D);
}

TEST_CASE("sample rates and word sizes", "[truehd][format]") {
    CHECK(sample_rate_hz(0) == 48000);
    CHECK(sample_rate_hz(1) == 96000);
    CHECK(sample_rate_hz(2) == 192000);
    CHECK(sample_rate_hz(8) == 44100);
    CHECK(sample_rate_hz(9) == 88200);
    CHECK(sample_rate_hz(10) == 176400);
    CHECK(sample_rate_hz(3) == 0);
    CHECK(sample_rate_hz(11) == 0);
    CHECK(nominal_samples_per_access_unit(0) == 40);
    CHECK(nominal_samples_per_access_unit(1) == 80);
    CHECK(nominal_samples_per_access_unit(10) == 160);
    CHECK(mlp_word_bits(0) == 16);
    CHECK(mlp_word_bits(2) == 24);
    CHECK(mlp_word_bits(3) == 0);
}

TEST_CASE("channel assignments list their speakers in stream order", "[truehd][format]") {
    CHECK(channel_count_of_assignment(0x1) == 2);
    CHECK(channel_count_of_assignment(0xF) == 6);
    CHECK(channel_count_of_assignment(0x4F) == 8);  // L R, C, LFE, Ls Rs, Lrs Rrs
    const auto channels = channels_of_assignment(0xF);
    REQUIRE(channels.size() == 6);
    CHECK(channels[0] == Channel::kL);
    CHECK(channels[1] == Channel::kR);
    CHECK(channels[2] == Channel::kC);
    CHECK(channels[3] == Channel::kLfe);
    CHECK(channels[4] == Channel::kLs);
    CHECK(channels[5] == Channel::kRs);
    CHECK(channel_count_of_mlp_arrangement(0) == 1);
    CHECK(channel_count_of_mlp_arrangement(1) == 2);
    CHECK(channel_count_of_mlp_arrangement(12) == 6);
    CHECK(channel_count_of_mlp_arrangement(31) == -1);
}

TEST_CASE("the common channel assignments are bitmasks, not an enumeration (TH spec 13.2)",
          "[truehd][format]") {
    // TH spec v3.9 §13.2: 0x01 stereo, 0x0B 5.0, 0x0F 5.1, 0x4F 7.1. The earlier 0x0B = 5.1,
    // 0x0F = 7.1, 0x13 and 0x17 do not exist: 0x13 is bits 0, 1 and 4 (L R, C, Lvh Rvh) and 0x17
    // adds the LFE to it, and there are no Atmos codes.
    struct Case {
        std::uint32_t assignment;
        std::vector<Channel> channels;
    };
    const std::vector<Case> cases{
        {0x01, {Channel::kL, Channel::kR}},
        {0x0B, {Channel::kL, Channel::kR, Channel::kC, Channel::kLs, Channel::kRs}},
        {0x0F, {Channel::kL, Channel::kR, Channel::kC, Channel::kLfe, Channel::kLs, Channel::kRs}},
        {0x4F,
         {Channel::kL, Channel::kR, Channel::kC, Channel::kLfe, Channel::kLs, Channel::kRs,
          Channel::kLrs, Channel::kRrs}},
        {0x13, {Channel::kL, Channel::kR, Channel::kC, Channel::kLvh, Channel::kRvh}},
        {0x17,
         {Channel::kL, Channel::kR, Channel::kC, Channel::kLfe, Channel::kLvh, Channel::kRvh}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.assignment);
        CHECK(channels_of_assignment(c.assignment) == c.channels);
        CHECK(channel_count_of_assignment(c.assignment) == static_cast<int>(c.channels.size()));
    }
}

TEST_CASE("the lossless fold shifts a sample by the matrix channel its output channel takes",
          "[truehd][checks]") {
    // ERRATA entry 12 (provisional): not by the output channel's position. With the identity
    // ch_assign of every stream FFmpeg's encoder writes the two are the same; with ch_assign
    // {1, 0} they are not.
    std::uint32_t by_assign = 0;
    by_assign = lossless_fold_step(by_assign, 0x000001, 1);
    by_assign = lossless_fold_step(by_assign, 0x000200, 0);
    CHECK(by_assign == 0x000202U);
    std::uint32_t by_position = 0;
    by_position = lossless_fold_step(by_position, 0x000001, 0);
    by_position = lossless_fold_step(by_position, 0x000200, 1);
    CHECK(by_position == 0x000401U);
    CHECK(lossless_check_of(by_assign) != lossless_check_of(by_position));
    // Only the low 24 bits of a sample count.
    CHECK(lossless_fold_step(0, 0xFF000001U, 0) == 0x000001U);
    // The four bytes of the fold are XORed into one.
    CHECK(lossless_check_of(0x01020408U) == (0x01 ^ 0x02 ^ 0x04 ^ 0x08));
}

TEST_CASE("the lossless fold of a matrix channel of 8 or more keeps the sample's top bits",
          "[truehd][checks]") {
    // ERRATA entry 69: an object substream has 12 to 16 matrix channels. A shift by the channel
    // would push the top bits of a 24 bit sample out of the word; the check counts the channel
    // modulo 8, so that the byte it folds to is the one a rotation of the word by the channel gives.
    const std::uint32_t sample = 0xF0F0F0U;  // a negative sample: its top bits are set
    for (unsigned channel = 0; channel < 16; ++channel) {
        const std::uint32_t rotated =
            channel % 32 == 0 ? sample : (sample << channel) | (sample >> (32 - channel));
        CHECK(lossless_check_of(lossless_fold_step(0, sample, channel)) ==
              lossless_check_of(rotated));
    }
    CHECK(lossless_fold_step(0, 0x000001, 9) == lossless_fold_step(0, 0x000001, 1));
    CHECK(lossless_fold_step(7, 0x000001, 32) == (7U ^ 1U));
}
