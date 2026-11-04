#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// DecoderConfig::readings: the other reading of a provisional point (ERRATA.md questions 5, 6, 18
// and 101). A stream scores a reading when it decodes to its source with the library's reading and
// to something else, or fails its own lossless check, with the alternative; a stream that does not
// depend on the reading decodes the same either way. Each reading is tried on a stream that depends
// on it and on one that does not.

namespace {

using namespace iclforge::truehd;

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

struct Decoded {
    std::vector<std::int32_t> pcm;
    DecoderCounters counters;
};

Decoded decode_with(const std::string& path, const AlternativeReadings& readings) {
    const std::vector<std::byte> stream = read_file(path);
    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    config.readings = readings;
    Decoder decoder{config};
    ElementaryReader reader(stream);
    Decoded out;
    while (const auto unit = reader.next()) {
        auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            out.pcm.insert(out.pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
        }
    }
    out.counters = decoder.counters();
    return out;
}

std::string handbuilt(const std::string& name) {
    return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/handbuilt/" + name;
}

std::string ffmpeg_made(const std::string& name) {
    return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name;
}

// The first sample (per channel) at which two decodes differ, or -1 when they are the same.
long first_difference(const Decoded& a, const Decoded& b, std::size_t channels) {
    if (a.pcm.size() != b.pcm.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < a.pcm.size(); ++i) {
        if (a.pcm[i] != b.pcm[i]) {
            return static_cast<long>(i / channels);
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("the default is the library's reading in all four", "[truehd][readings]") {
    const AlternativeReadings none;
    CHECK_FALSE(none.seed_shr7_32_bit);
    CHECK_FALSE(none.zero_histories_at_restart);
    CHECK_FALSE(none.output_shift_by_position);
    CHECK_FALSE(none.fold_by_position);
    const DecoderConfig config;
    CHECK_FALSE(config.readings.seed_shr7_32_bit);
}

TEST_CASE("a 32 bit shr7 changes a stream that draws on noise generator 0 and no other",
          "[truehd][readings]") {
    AlternativeReadings wide;
    wide.seed_shr7_32_bit = true;
    const Decoded base = decode_with(handbuilt("hb_noise_generator_0.thd"), {});
    const Decoded alt = decode_with(handbuilt("hb_noise_generator_0.thd"), wide);
    // ERRATA entry 23: the two readings agree on the first two noise samples and differ from the
    // third on.
    CHECK(first_difference(base, alt, 2) >= 0);
    CHECK(first_difference(base, alt, 2) <= 2);
    const Decoded plain_base = decode_with(ffmpeg_made("truehd_st48_s24.thd"), {});
    const Decoded plain_alt = decode_with(ffmpeg_made("truehd_st48_s24.thd"), wide);
    CHECK(first_difference(plain_base, plain_alt, 2) == -1);
}

TEST_CASE("zeroing the histories at a restart changes a stream that filters from its first sample",
          "[truehd][readings]") {
    AlternativeReadings zero;
    zero.zero_histories_at_restart = true;
    const Decoded base = decode_with(handbuilt("hb_filters.thd"), {});
    const Decoded alt = decode_with(handbuilt("hb_filters.thd"), zero);
    // ERRATA entry 13: hb_filters restarts every 640 samples and filters at once after it, so the
    // first restart (sample 640) is the first difference.
    CHECK(first_difference(base, alt, 2) == 640);
    const Decoded plain_base = decode_with(ffmpeg_made("truehd_st48_s24.thd"), {});
    const Decoded plain_alt = decode_with(ffmpeg_made("truehd_st48_s24.thd"), zero);
    CHECK(first_difference(plain_base, plain_alt, 2) == -1);
}

TEST_CASE("the output shift by position changes a stream whose shifts differ by channel",
          "[truehd][readings]") {
    AlternativeReadings by_position;
    by_position.output_shift_by_position = true;
    const std::string path = handbuilt("hb_output_shift_permuted.thd");
    const Decoded base = decode_with(path, {});
    const Decoded alt = decode_with(path, by_position);
    CHECK(base.counters.lossless_check_failures == 0);
    CHECK(base.counters.lossless_checks_ok > 0);
    CHECK(first_difference(base, alt, 3) >= 0);
    // The fold still shifts by the matrix channel in the alternative, so the check it makes of the
    // shifted-by-position output fails.
    CHECK(alt.counters.lossless_check_failures > 0);
    const Decoded plain_base = decode_with(ffmpeg_made("truehd_st48_s24.thd"), {});
    const Decoded plain_alt = decode_with(ffmpeg_made("truehd_st48_s24.thd"), by_position);
    CHECK(first_difference(plain_base, plain_alt, 2) == -1);
}

TEST_CASE(
    "the fold by position fails the lossless check of a stream with a permuted ch_assign "
    "and leaves the samples alone",
    "[truehd][readings]") {
    AlternativeReadings by_position;
    by_position.fold_by_position = true;
    const std::string path = handbuilt("hb_output_shift_permuted.thd");
    const Decoded base = decode_with(path, {});
    const Decoded alt = decode_with(path, by_position);
    CHECK(base.counters.lossless_check_failures == 0);
    CHECK(first_difference(base, alt, 3) == -1);
    CHECK(alt.counters.lossless_check_failures > 0);
    const Decoded plain = decode_with(ffmpeg_made("truehd_st48_s24.thd"), by_position);
    CHECK(plain.counters.lossless_check_failures == 0);
    CHECK(plain.counters.lossless_checks_ok > 0);
}
