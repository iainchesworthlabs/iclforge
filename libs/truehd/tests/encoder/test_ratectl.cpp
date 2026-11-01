#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/ratectl_shape.hpp"
#include "encoder/ratectl_support.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"

// Rate control (libs/truehd/ratectl.hpp, ERRATA.md entry 71): the arrival model, the figures kept
// for each restart interval and read back from a stream, and RateMode::kShape, which lets the object
// channels give up low bits so that the stream fits a chosen bitrate. The model is the encoder's
// reading of TH spec §3.4 and §18: [no-external-oracle].

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

// The bytes of a unit that take `samples` samples to arrive at 18 Mbit/s and 48 kHz: 46.875 bytes a
// sample. Written out here so that the expected numbers below do not come from the code under test.
constexpr std::size_t kBytesFor40Samples = 1875;

TEST_CASE("the arrival model gives the figures worked out by hand", "[truehd][rate][ratectl]") {
    RateModel model(48000, 40, 18'000'000);
    REQUIRE(model.latency_limit() == 3600);  // 75 ms at 48 kHz

    // Unit 0 (slot -40): 1875 bytes arrive in 40 samples, right at the slot.
    const RateStep a = model.admit(0, kBytesFor40Samples);
    CHECK(a.start == -40);
    CHECK(a.arrival == 40);
    CHECK(a.headroom == 3600 - 40);

    // Unit 1 (slot 0): twice as long; it starts when its slot opens, which is when unit 0 ended.
    const RateStep b = model.admit(40, 2 * kBytesFor40Samples);
    CHECK(b.start == 0);
    CHECK(b.arrival == 80);
    CHECK(b.headroom == 3600 - 80);

    // Unit 2 (slot 40): it has to wait for unit 1, which ends at 80, so the queue is 40 samples long.
    const RateStep c = model.admit(80, kBytesFor40Samples);
    CHECK(c.start == 80);
    CHECK(c.arrival == 40);
    CHECK(c.headroom == 3600 - (80 + 40 - 40));

    // 4688 bytes is a little over 100 samples: rounded up, 101.
    const RateStep d = model.admit(120, 4688);
    CHECK(d.start == 120);
    CHECK(d.arrival == 101);
    CHECK(d.headroom == 3600 - (120 + 101 - 80));
    CHECK_FALSE(d.late());

    // A unit of 200,000 bytes arrives in 4267 samples, which is longer than the buffer: late.
    const RateStep e = model.admit(160, 200'000);
    CHECK(e.start == 221);
    CHECK(e.arrival == 4267);
    CHECK(e.headroom == 3600 - (221 + 4267 - 120));
    CHECK(e.headroom == -768);
    CHECK(e.late());
    CHECK(samples_to_us(e.headroom, 48000) == -16000);
    CHECK(samples_to_us(d.headroom, 48000) == 72062);  // 3459 / 48000 s, rounded down
    CHECK(samples_to_us(-1, 48000) == -21);             // toward minus infinity, not toward 0

    // reset() starts a stream again: the first unit starts at its slot.
    model.reset();
    const RateStep again = model.admit(0, kBytesFor40Samples);
    CHECK(again.start == -40);
    CHECK(again.headroom == a.headroom);
}

TEST_CASE("a backlog drains when units arrive faster than their own duration",
          "[truehd][rate][ratectl]") {
    RateModel model(48000, 40, 18'000'000);
    // Ten units' worth of bytes in one unit: 18,750 bytes, 400 samples, so the next unit waits.
    const RateStep big = model.admit(0, 10 * kBytesFor40Samples);
    CHECK(big.arrival == 400);
    const RateStep behind = model.admit(40, 100);  // 100 bytes: 3 samples
    CHECK(behind.start == 360);                    // the big unit ends at -40 + 400
    CHECK(behind.headroom == 3600 - (360 + 3 - 0));
    // Each small unit gains 37 samples on its slot; after 10 the queue is gone.
    RateStep last = behind;
    std::int64_t first_sample = 80;
    for (int i = 0; i < 12; ++i, first_sample += 40) {
        last = model.admit(first_sample, 100);
    }
    CHECK(last.start == (first_sample - 40) - 40);  // its own slot: the queue is gone
    CHECK(last.headroom == 3600 - 3);
}

TEST_CASE("a unit that is late at a lower rate is on time at a higher one",
          "[truehd][rate][ratectl]") {
    RateModel slow(48000, 40, 9'000'000);
    RateModel fast(48000, 40, 18'000'000);
    // 1875 bytes: 40 samples at 18 Mbit/s, 80 at 9.
    CHECK(fast.admit(0, kBytesFor40Samples).arrival == 40);
    CHECK(slow.admit(0, kBytesFor40Samples).arrival == 80);
}

TEST_CASE("the declared peak rate is what a reader gets back from the major sync field",
          "[truehd][rate][ratectl]") {
    CHECK(declared_peak_rate_bps(18'000'000, 48000) == 18'000'000);
    // raw = 12345678 * 16 / 96000 = 2057.6, so 2057, which reads back as 12,342,000.
    CHECK(declared_peak_rate_bps(12'345'678, 96000) == 12'342'000);
    // 15 bits is as far as the field goes.
    CHECK(declared_peak_rate_bps(1'000'000'000, 48000) == 98'301'000);
}

TEST_CASE("an interval meter keeps the figures of its units", "[truehd][rate][ratectl]") {
    IntervalMeter meter(48000, 40);
    meter.begin(10);
    meter.add(1875, 40, RateStep{-40, 40, 3560});
    meter.add(3750, 40, RateStep{0, 80, 3520});
    meter.add(200'000, 40, RateStep{221, 4267, -768});
    const IntervalRate r = meter.finish();
    CHECK(r.first_unit == 10);
    CHECK(r.units == 3);
    CHECK(r.samples == 120);
    CHECK(r.bytes == 205'625);
    CHECK(r.max_unit_bytes == 200'000);
    CHECK(r.bitrate_bps == 658'000'000U);  // 205,625 bytes in 120 samples
    CHECK(r.peak_unit_bps == 1'920'000'000U);               // 200,000 bytes in 40 samples
    CHECK(r.min_headroom_us == -16000);
    CHECK(r.late_units == 1);

    meter.begin(0);
    const IntervalRate empty = meter.finish();
    CHECK(empty.units == 0);
    CHECK(empty.bitrate_bps == 0);
    CHECK(empty.min_headroom_us == 0);
}

TEST_CASE("the figures of a stream read back are the figures the encoder kept",
          "[truehd][rate][ratectl]") {
    // Two shapes: stereo with a last interval that is not whole, and an Atmos stream with substream 3.
    for (const bool atmos : {false, true}) {
        EncoderConfig config = atmos ? atmos_config(12) : EncoderConfig{};
        config.restart_interval_units = atmos ? 128 : 16;
        const MixedSignal signal{.bed = atmos ? 8 : 2, .objects = atmos ? 12 : 0};
        // 3.1 s: not a whole number of 128 unit intervals.
        const std::uint64_t frames = 148'800 + 25;
        const StreamOut out = encode_signal(config, signal, frames, 7000);
        const auto report = analyse_rate(out.bytes);
        REQUIRE(report.has_value());
        CAPTURE(atmos);
        CHECK(report->sample_rate == 48000);
        CHECK(report->samples_per_unit == 40);
        CHECK(report->peak_data_rate_bps == 18'000'000);
        CHECK(report->units == out.stats.access_units);
        CHECK(report->bytes == out.stats.bytes);
        CHECK(report->min_headroom_us == out.stats.min_headroom_us);
        CHECK(report->late_units == 0);
        REQUIRE(report->intervals.size() == out.intervals.size());
        CHECK(report->intervals == out.intervals);
        CHECK(report->intervals.size() == out.stats.restart_intervals);
        std::uint64_t units = 0;
        for (const IntervalRate& interval : report->intervals) {
            CHECK(interval.first_unit == units);
            units += interval.units;
        }
        CHECK(units == report->units);
    }
}

TEST_CASE("the units that are late in a stream are the encoder's violations",
          "[truehd][rate][ratectl]") {
    EncoderConfig config = atmos_config(12);
    config.peak_data_rate_bps = 6'000'000;
    config.allow_rate_violations = true;
    // Objects of 22 bit noise: about 12 Mbit/s more than 6.
    const MixedSignal signal{.object_noise_bits = 22};
    const StreamOut out = encode_signal(config, signal, 48000 * 2);
    REQUIRE(out.stats.rate_violations > 0);
    const auto report = analyse_rate(out.bytes);
    REQUIRE(report.has_value());
    CHECK(report->peak_data_rate_bps == declared_peak_rate_bps(6'000'000, 48000));
    CHECK(report->late_units == out.stats.rate_violations);
    CHECK(report->min_headroom_us < 0);
    CHECK(report->min_headroom_us == out.stats.min_headroom_us);
}

TEST_CASE("analysing a stream at another rate re-runs the model at that rate",
          "[truehd][rate][ratectl]") {
    EncoderConfig config = atmos_config(12);
    const MixedSignal signal{.object_noise_bits = 16};
    const StreamOut out = encode_signal(config, signal, 48000 * 3);
    const auto at_declared = analyse_rate(out.bytes);
    const auto at_8m = analyse_rate(out.bytes, 8'000'000);
    REQUIRE(at_declared.has_value());
    REQUIRE(at_8m.has_value());
    CHECK(at_declared->late_units == 0);
    CHECK(at_8m->peak_data_rate_bps == 8'000'000);
    // The same bytes, a slower rate: the burst of object noise cannot be sent in time.
    CHECK(at_8m->late_units > 0);
    CHECK(at_8m->min_headroom_us < at_declared->min_headroom_us);
    CHECK(at_8m->bytes == at_declared->bytes);
}

TEST_CASE("a stream with no major sync has no rate figures", "[truehd][rate][ratectl]") {
    const std::vector<std::byte> junk(4096, std::byte{0x55});
    const auto report = analyse_rate(junk);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().kind == ErrorKind::kBadSync);
}

// --- RateMode::kShape -----------------------------------------------------------------------

// A bitrate a fifth below the most any restart interval of the signal's lossless stream needs, so
// that the loud intervals do not fit it and the quiet ones do. The signal is the first 3 seconds
// (its six phases); the bed alone is far below it.
std::uint32_t tight_target(const MixedSignal& signal, const EncoderConfig& lossless_config = atmos_config(12)) {
    const StreamOut lossless = encode_signal(lossless_config, signal, 48000 * 3);
    std::uint32_t most = 0;
    for (const IntervalRate& interval : lossless.intervals) {
        most = std::max(most, interval.bitrate_bps);
    }
    return most / 5 * 4;
}

EncoderConfig shaped_config(std::uint32_t target, int objects = 12) {
    EncoderConfig config = atmos_config(objects);
    config.rate_control.mode = RateMode::kShape;
    config.rate_control.target_bitrate_bps = target;
    return config;
}

TEST_CASE("a stream that does not fit the chosen bitrate is kRateExceeded in strict mode",
          "[truehd][rate][ratectl]") {
    EncoderConfig config = atmos_config(12);
    config.rate_control.target_bitrate_bps = tight_target(MixedSignal{.object_noise_bits = 15});  // RateMode::kStrict
    const MixedSignal signal{.object_noise_bits = 15};
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::optional<EncodeErrorKind> failure;
    for (std::uint64_t at = 0; at < 48000 * 3 && !failure; at += 48000) {
        if (auto units = encoder->encode(signal.frames(at, 48000)); !units) {
            failure = units.error().kind;
        }
    }
    if (!failure) {
        if (auto tail = encoder->flush(); !tail) {
            failure = tail.error().kind;
        }
    }
    REQUIRE(failure.has_value());
    CHECK(*failure == EncodeErrorKind::kRateExceeded);
}

TEST_CASE("RateMode::kShape fits the chosen bitrate and leaves the bed alone",
          "[truehd][rate][ratectl][object-bed]") {
    const std::uint32_t target = tight_target(MixedSignal{.object_noise_bits = 15});
    const EncoderConfig config = shaped_config(target);
    const MixedSignal signal{.object_noise_bits = 15};
    const std::uint64_t frames = 48000 * 7 / 2;  // seven phases: quiet, mid, noise, silence, loud, bed noise, quiet
    const StreamOut out = encode_signal(config, signal, frames, 48000, true);

    // It was necessary and it did something, in some intervals and not all.
    CHECK(out.stats.shaped_intervals > 0);
    CHECK(out.stats.shaped_intervals < out.stats.restart_intervals);
    CHECK(out.stats.shape_retries >= out.stats.shaped_intervals);
    CHECK(out.stats.max_shape_bits >= 1);
    CHECK(out.stats.max_shape_bits <= config.rate_control.max_shape_bits);
    // The tries that were thrown away left nothing in the counters.
    CHECK(out.stats.rate_violations == 0);
    CHECK(out.stats.access_units == out.units.size());
    CHECK(out.stats.access_units == (frames + 39) / 40);
    CHECK(out.stats.restart_intervals == out.intervals.size());

    // The stream, read back, fits the chosen bitrate and the declared peak rate.
    const auto at_target = analyse_rate(out.bytes, target);
    const auto at_peak = analyse_rate(out.bytes);
    REQUIRE(at_target.has_value());
    REQUIRE(at_peak.has_value());
    CHECK(at_target->late_units == 0);
    CHECK(at_target->min_headroom_us >= 0);
    CHECK(at_peak->late_units == 0);
    CHECK(at_peak->min_headroom_us >= at_target->min_headroom_us);

    // The bed is the input, sample for sample. The objects are the input's, rounded: within half of
    // the largest step the stream gave up, exactly so in the quiet start, and different somewhere.
    const std::vector<std::int32_t> input = signal.frames(0, static_cast<int>(frames));
    const std::vector<std::int32_t> bed = decode_all(out.units, 24, 8, Presentation::kBest);
    const std::vector<std::int32_t> objects = decode_all(out.units, 24, 12, Presentation::kAll);
    REQUIRE(bed.size() >= frames * 8);
    REQUIRE(objects.size() >= frames * 12);
    const std::int64_t bound = std::int64_t{1} << (out.stats.max_shape_bits - 1);
    std::uint64_t changed = 0;
    for (std::uint64_t t = 0; t < frames; ++t) {
        for (std::size_t c = 0; c < 8; ++c) {
            REQUIRE(bed[t * 8 + c] == input[t * 20 + c]);
        }
        for (std::size_t c = 0; c < 12; ++c) {
            const std::int64_t want = input[t * 20 + 8 + c];
            const std::int64_t got = objects[t * 12 + c];
            const std::int64_t error = std::abs(want - got);
            REQUIRE(error <= bound);
            if (error != 0) {
                ++changed;
                CHECK(t >= 5120);  // not in the first restart interval, which was quiet
            }
        }
    }
    CHECK(changed > 0);
}

TEST_CASE("a stream that is shaped is the same bytes whatever the size of the calls",
          "[truehd][rate][ratectl]") {
    const EncoderConfig config = shaped_config(tight_target(MixedSignal{.object_noise_bits = 15}));
    const MixedSignal signal{.object_noise_bits = 15};
    const std::uint64_t frames = 48000 * 3 / 2 + 777;
    const StreamOut whole = encode_signal(config, signal, frames, 100'000);
    const StreamOut steps = encode_signal(config, signal, frames, 40);
    CHECK(whole.stats.shaped_intervals > 0);
    CHECK(whole.bytes == steps.bytes);
    CHECK(whole.intervals == steps.intervals);
}

TEST_CASE("the encoder's own decoder agrees with a shaped stream", "[truehd][rate][ratectl]") {
    EncoderConfig config = shaped_config(tight_target(MixedSignal{.object_noise_bits = 15}));
    config.verify = true;
    const MixedSignal signal{.object_noise_bits = 15};
    const StreamOut out = encode_signal(config, signal, 48000 * 2);
    CHECK(out.stats.shaped_intervals > 0);
    CHECK(out.stats.verified_units == out.stats.access_units);
}

TEST_CASE("shaping with the object matrices of the bed", "[truehd][rate][ratectl][object-bed]") {
    for (const bool bypass : {false, true}) {
        EncoderConfig config;
        config.channels = 8;
        config.object_channels = 12;
        config.object_layout = ObjectLayout::kBedMatrices;
        config.object_lsb_bypass = bypass;
        config.object_noise_shift = 1;
        config.rate_control.mode = RateMode::kShape;
        const MixedSignal signal{.bed = 0, .objects = 12, .object_noise_bits = 15};
        EncoderConfig lossless = config;
        const std::uint32_t target = tight_target(signal, lossless);
        config.rate_control.target_bitrate_bps = target;
        config.verify = true;
        const StreamOut out = encode_signal(config, signal, 48000 * 2, 48000, true);
        CAPTURE(bypass);
        CHECK(out.stats.shaped_intervals > 0);
        CHECK(out.stats.rate_violations == 0);
        // The bypass cannot keep a quantiser step, so it gives up at most what the output shift
        // holds: 7 bits in all.
        CHECK(out.stats.max_shape_bits <= (bypass ? 7 : 8));
        const auto at_target = analyse_rate(out.bytes, target);
        REQUIRE(at_target.has_value());
        CHECK(at_target->late_units == 0);
        const std::vector<std::int32_t> input = signal.frames(0, 48000 * 2);
        const std::vector<std::int32_t> objects = decode_all(out.units, 24, 12, Presentation::kAll);
        REQUIRE(objects.size() >= input.size());
        const std::int64_t bound = std::int64_t{1} << (out.stats.max_shape_bits - 1);
        for (std::size_t i = 0; i < input.size(); ++i) {
            REQUIRE(std::abs(std::int64_t{objects[i]} - input[i]) <= bound);
        }
    }
}

TEST_CASE("the shaping stops where nothing more can be given up", "[truehd][rate][ratectl]") {
    // Objects the bed's size dominates: shaping cannot help, so the stream is an error after the
    // objects have given up everything they were allowed (and not after a try for each bit of 24).
    EncoderConfig config = shaped_config(0);
    config.peak_data_rate_bps = 3'000'000;
    config.rate_control.target_bitrate_bps = 0;
    config.rate_control.max_shape_bits = 3;
    const MixedSignal signal{.object_noise_bits = 22};
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::optional<EncodeErrorKind> failure;
    if (auto units = encoder->encode(signal.frames(0, 48000 * 2)); !units) {
        failure = units.error().kind;
    }
    REQUIRE(failure.has_value());
    CHECK(*failure == EncodeErrorKind::kRateExceeded);
}

TEST_CASE("rate control refuses configurations it cannot honour", "[truehd][rate][ratectl][config]") {
    EncoderConfig stereo;
    stereo.rate_control.mode = RateMode::kShape;
    CHECK_FALSE(Encoder::refusal_reason(stereo).empty());  // no object channels to shape

    EncoderConfig config = atmos_config(12);
    CHECK(Encoder::refusal_reason(config).empty());
    config.rate_control.max_shape_bits = 17;
    CHECK_FALSE(Encoder::refusal_reason(config).empty());
    config.rate_control.max_shape_bits = -1;
    CHECK_FALSE(Encoder::refusal_reason(config).empty());
    config.rate_control.max_shape_bits = 16;
    CHECK(Encoder::refusal_reason(config).empty());
    config.rate_control.target_bitrate_bps = 999'999;
    CHECK_FALSE(Encoder::refusal_reason(config).empty());
    config.rate_control.target_bitrate_bps = 1'000'000;
    CHECK(Encoder::refusal_reason(config).empty());
}

TEST_CASE("shape_planes rounds to a step and keeps the sample within 24 bits",
          "[truehd][rate][ratectl]") {
    using Plane = std::vector<std::int32_t>;
    // Multiples of 256 (8 bits clear): giving up 2 more makes a step of 1024, ties going up.
    std::vector<Plane> planes{{256 * 5, 256 * 3, 256 * 2, -256 * 2, -256 * 3, 0},
                              {256, -256, 256 * 7, -256 * 5, 0, 256 * 4}};
    CHECK(detail::common_zero_bits(planes) == 8);
    CHECK(detail::shape_planes(planes, 2, 22) == 10);
    CHECK(planes[0] == Plane{1024, 1024, 1024, 0, -1024, 0});
    CHECK(planes[1] == Plane{0, 0, 2048, -1024, 0, 1024});
    CHECK(detail::common_zero_bits(planes) == 10);

    // The step never goes above the limit.
    std::vector<Plane> limited{{256 * 3, 256}};
    CHECK(detail::shape_planes(limited, 8, 9) == 9);
    CHECK(limited[0] == Plane{1024, 512});

    // Nothing to give up in silence.
    std::vector<Plane> silent{{0, 0, 0}};
    CHECK(detail::common_zero_bits(silent) == 24);
    CHECK(detail::shape_planes(silent, 4, 22) == 24);
    CHECK(silent[0] == Plane{0, 0, 0});

    // The largest sample rounds down rather than out of 24 bits.
    std::vector<Plane> top{{0x7FFFFF, -0x800000, 1}};
    CHECK(detail::shape_planes(top, 4, 22) == 4);
    CHECK(top[0] == Plane{0x7FFFF0, -0x800000, 0});
}

// --- the acceptance streams: 10 s, 2 min, 20 min of a mixed signal --------------------------

struct Acceptance {
    std::uint64_t units = 0;
    std::uint64_t late = 0;
    std::int64_t min_headroom_us = 0;
    std::uint32_t bitrate_bps = 0;
    std::uint64_t shaped = 0;
};

// Encodes `seconds` of the signal with the encoder's own decoder checking every unit against the
// input (EncoderConfig::verify) and the units cut up for the arrival model as they come, so that a
// 20 minute stream is never held in memory.
Acceptance run_acceptance(EncoderConfig config, const MixedSignal& signal, std::uint64_t seconds,
                          std::uint32_t model_rate_bps) {
    config.verify = true;
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    RateModel model(48000, 40, model_rate_bps);
    Acceptance result;
    std::uint64_t bytes = 0;
    std::int64_t min_headroom = 0;
    const auto take = [&](const std::vector<EncodedAccessUnit>& units) {
        for (const EncodedAccessUnit& unit : units) {
            const RateStep step = model.admit(static_cast<std::int64_t>(result.units) * 40,
                                              unit.bytes.size());
            min_headroom = result.units == 0 ? step.headroom : std::min(min_headroom, step.headroom);
            result.late += step.late() ? 1U : 0U;
            bytes += unit.bytes.size();
            ++result.units;
        }
    };
    for (std::uint64_t second = 0; second < seconds; ++second) {
        auto units = encoder->encode(signal.frames(second * 48000, 48000));
        if (!units) {
            UNSCOPED_INFO("encode failed at second " << second << ": " << to_string(units.error().kind)
                                                     << ": " << units.error().detail
                                                     << " (shaped intervals so far "
                                                     << encoder->stats().shaped_intervals << ", retries "
                                                     << encoder->stats().shape_retries << ", max bits "
                                                     << encoder->stats().max_shape_bits << ")");
        }
        REQUIRE(units.has_value());
        take(*units);
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    take(*tail);
    result.min_headroom_us = samples_to_us(min_headroom, 48000);
    result.bitrate_bps = static_cast<std::uint32_t>(bytes * 8U * 48000U / (result.units * 40U));
    result.shaped = encoder->stats().shaped_intervals;
    CHECK(encoder->stats().verified_units == encoder->stats().access_units);
    CHECK(encoder->stats().rate_violations == 0);
    return result;
}

void accept_duration(std::uint64_t seconds) {
    // Lossless within Blu-ray's 18 Mbit/s: the objects' noise is 14 bits.
    {
        const Acceptance a = run_acceptance(atmos_config(12), MixedSignal{.object_noise_bits = 14},
                                            seconds, 18'000'000);
        CAPTURE(seconds);
        CHECK(a.units == seconds * 48000 / 40);
        CHECK(a.late == 0);
        CHECK(a.min_headroom_us >= 0);
        CHECK(a.shaped == 0);
        CHECK(a.bitrate_bps < 18'000'000);
    }
    // Shaped to a bitrate a fifth below what the loudest interval needs lossless.
    {
        const MixedSignal signal{.object_noise_bits = 15};
        const std::uint32_t target = tight_target(signal);
        const EncoderConfig config = shaped_config(target);
        const Acceptance a = run_acceptance(config, signal, seconds, target);
        CHECK(a.units == seconds * 48000 / 40);
        CHECK(a.late == 0);
        CHECK(a.min_headroom_us >= 0);
        CHECK(a.shaped > 0);
    }
}

TEST_CASE("10 seconds of a mixed signal stay inside the budget", "[truehd][rate][ratectl]") {
    accept_duration(10);
}

TEST_CASE("2 minutes of a mixed signal stay inside the budget", "[.long][truehd][rate][ratectl]") {
    accept_duration(120);
}

TEST_CASE("20 minutes of a mixed signal stay inside the budget",
          "[.long][truehd][rate][ratectl]") {
    accept_duration(1200);
}

}  // namespace
