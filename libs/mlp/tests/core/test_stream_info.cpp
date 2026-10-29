#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/stream_info.hpp"

// The public stream description: sample rate codes (1.4), samples per access unit (P1), channel
// assignments (13.1, P25) and format_info packing (5.1), the last against the words FFmpeg writes
// for stereo (0x0050A001) and 5.1 (0x0007800F).

TEST_CASE("mlp sample rate codes and access unit sizes", "[mlp][stream_info]") {
    using namespace iclforge::mlp;
    CHECK(sample_rate_from_code(0x0) == 48000U);
    CHECK(sample_rate_from_code(0x2) == 192000U);
    CHECK(sample_rate_from_code(0x8) == 44100U);
    CHECK(sample_rate_from_code(0xA) == 176400U);
    CHECK_FALSE(sample_rate_from_code(0x3).has_value());
    CHECK(sample_rate_code(96000U) == 0x1);
    CHECK_FALSE(sample_rate_code(32000U).has_value());
    CHECK(samples_per_access_unit(44100U) == 40);
    CHECK(samples_per_access_unit(48000U) == 40);
    CHECK(samples_per_access_unit(96000U) == 80);
    CHECK(samples_per_access_unit(176400U) == 160);
}

TEST_CASE("mlp channel assignments count and name their speakers", "[mlp][stream_info]") {
    using namespace iclforge::mlp;
    CHECK(channel_count(kAssignStereo) == 2);
    CHECK(channel_count(kAssign51) == 6);
    CHECK(channel_count(kAssign71) == 8);
    CHECK(channel_count(0x0B) == 5);
    const auto s = speakers(kAssign71);
    REQUIRE(s.count == 8);
    CHECK(s.labels[0] == "L");
    CHECK(s.labels[3] == "LFE");
    CHECK(s.labels[6] == "Lrs");
    CHECK(s.labels[7] == "Rrs");
}

TEST_CASE("mlp format_info packs as FFmpeg's streams hold it", "[mlp][stream_info]") {
    using namespace iclforge::mlp;
    const FormatInfo stereo = FormatInfo::unpack(0x0050A001U);
    CHECK(stereo.sample_rate_code == 0);
    CHECK(stereo.twoch_modifier == 1);
    CHECK(stereo.sixch_assignment == 1);
    CHECK(stereo.eightch_assignment == 1);
    CHECK(stereo.pack() == 0x0050A001U);
    const FormatInfo five = FormatInfo::unpack(0x0007800FU);
    CHECK(five.sixch_assignment == kAssign51);
    CHECK(five.eightch_assignment == kAssign51);
    CHECK(five.pack() == 0x0007800FU);
    MajorSync m;
    m.format.sample_rate_code = 0;
    m.peak_data_rate = 6000;
    CHECK(m.peak_bits_per_second() == 18000000U);
    CHECK_FALSE(describe(Error::kLosslessCheck).empty());
}
