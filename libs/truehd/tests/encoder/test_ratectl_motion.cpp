#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/ratectl_support.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/encoder/object_paths.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"

// Rate control with the moving objects of the object substream (EncoderConfig::object_paths,
// ERRATA.md entries 69 and 70, ERRATA.md entry 71): the objects give up low bits before the
// matrices that mix them into the bed are built, and the matrix sections that change from block to
// block still decode, with the decoder's own check of the ramp (lab settings) where the stream has
// one. [no-external-oracle].

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kObjects = 14;
constexpr int kBed = 8;
constexpr int kRowOfObject[8] = {0, 1, 2, 4, 5, 6, 7, 3};
constexpr std::int32_t kGain = 46341;  // 1/sqrt(2) in units of 2^-16

EncoderConfig moving_config(ObjectRamp ramp) {
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = kObjects;
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = 1;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    config.restart_interval_units = 16;
    // Every object above the bed sits at its own row at -3 dB, and the first one moves from its row
    // to the next over two seconds.
    for (int k = 0; k + kBed < kObjects; ++k) {
        ObjectPath path;
        path.object = kBed + k;
        ObjectGainKnot start;
        start.sample = 0;
        start.gain[static_cast<std::size_t>(kRowOfObject[k])] = kGain;
        path.knots.push_back(start);
        if (k == 0) {
            ObjectGainKnot end;
            end.sample = 96000;
            end.gain[static_cast<std::size_t>(kRowOfObject[1])] = kGain;
            path.knots.push_back(end);
        }
        config.object_paths.push_back(path);
    }
    config.object_ramp = ramp;
    return config;
}

TEST_CASE("objects that move still decode when they give up bits to fit a bitrate",
          "[truehd][rate][ratectl][object-motion]") {
    for (const ObjectRamp ramp : {ObjectRamp::kNone, ObjectRamp::kEnd}) {
        const MixedSignal signal{.bed = 0, .objects = kObjects, .object_noise_bits = 15};
        EncoderConfig lossless = moving_config(ramp);
        const StreamOut base = encode_signal(lossless, signal, 48000 * 3);
        std::uint32_t most = 0;
        for (const IntervalRate& interval : base.intervals) {
            most = std::max(most, interval.bitrate_bps);
        }
        const std::uint32_t target = most / 20 * 17;

        EncoderConfig config = moving_config(ramp);
        config.rate_control.mode = RateMode::kShape;
        config.rate_control.target_bitrate_bps = target;
        config.verify =
            true;  // the encoder's own strict decode of every unit, ramp settings included
        const StreamOut out = encode_signal(config, signal, 48000 * 3);
        CAPTURE(ramp == ObjectRamp::kEnd);
        CHECK(out.stats.shaped_intervals > 0);
        CHECK(out.stats.rate_violations == 0);
        CHECK(out.stats.verified_units == out.stats.access_units);
        const auto at_target = analyse_rate(out.bytes, target);
        REQUIRE(at_target.has_value());
        CHECK(at_target->late_units == 0);
        const LayoutAudit audit = audit_layout(out.bytes);
        CHECK(audit.ok);
        CHECK(audit.segments == 4 * audit.units);
    }
}

}  // namespace
