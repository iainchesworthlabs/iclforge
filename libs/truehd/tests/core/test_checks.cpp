#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
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

}  // namespace

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

TEST_CASE("the extra data check nibble is the XOR of the length's nibbles", "[truehd][checks]") {
    CHECK(extra_data_check_nibble(0x000) == 0x0);
    CHECK(extra_data_check_nibble(0x123) == (0x1 ^ 0x2 ^ 0x3));
    CHECK(extra_data_check_nibble(0xFFF) == 0xF);
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
    // Only the low 24 bits of a sample count, and a shift of 32 or more contributes nothing.
    CHECK(lossless_fold_step(0, 0xFF000001U, 0) == 0x000001U);
    CHECK(lossless_fold_step(7, 0x000001, 32) == 7U);
    // The four bytes of the fold are XORed into one.
    CHECK(lossless_check_of(0x01020408U) == (0x01 ^ 0x02 ^ 0x04 ^ 0x08));
}
