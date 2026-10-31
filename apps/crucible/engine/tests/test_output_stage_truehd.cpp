#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fake_devices.hpp"
#include "iclforge/base/wav.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "output_stage.hpp"

// The TrueHD 5.1 leg over fake devices (planning/truehd-parity-crucible.md,
// rows C1, C9, C15, C16 and C21). The leg takes the encoder's bed, not its
// unit, rounds it to 24-bit integers, encodes it as TrueHD and hands the
// sink MAT bursts. The oracle is the codec's own lossless property: unwrap
// what reached the sink, decode it with iclforge::truehd, and the PCM must
// equal the rounded input sample for sample. The signal is the repository's
// own 5.1 programme material (testdata/audio/reference_51.wav), not a tone.

using namespace iclforge::crucible;
using namespace iclforge::crucible::testing;

namespace {

namespace mat = iclforge::containers::iec61937;

constexpr std::size_t kChannels = 6;
// WAVE order (L R C LFE Ls Rs) to the bed's coded order (L C R Ls Rs LFE):
// the position in the bed of WAVE channel w.
constexpr std::size_t kCodedForWave[kChannels] = {0, 2, 1, 5, 3, 4};
constexpr std::size_t kAccessUnitSamples = 40;  // at 48 kHz

// A clip in the engine's bed order.
struct Clip {
    std::vector<std::vector<float>> bed;
    [[nodiscard]] std::size_t frames() const { return bed.empty() ? 0 : bed[0].size(); }
};

Clip programme_clip() {
    const std::string path = std::string(ICLFORGE_GOLDEN_AUDIO_DIR) + "/reference_51.wav";
    auto wav = iclforge::base::read_wav(path);
    REQUIRE(wav.has_value());
    REQUIRE(wav->channels.size() == kChannels);
    REQUIRE(wav->sample_rate == 48000);
    Clip clip;
    clip.bed.resize(kChannels);
    for (std::size_t w = 0; w < kChannels; ++w) {
        clip.bed[kCodedForWave[w]] = std::move(wav->channels[w]);
    }
    return clip;
}

// The rounding the documents promise: to the nearest 24-bit integer, ties to
// even, clamped to the range, silence for a value that is not a number.
// Written from the definition, not from the stage's code.
std::int32_t round_to_24(float x) {
    if (std::isnan(x)) {
        return 0;
    }
    const double scaled = std::nearbyint(static_cast<double>(x) * 8388608.0);
    return static_cast<std::int32_t>(std::clamp(scaled, -8388608.0, 8388607.0));
}

// What the sink's decode has to equal for frames [first, first + count) of
// the clip: interleaved in WAVE order, which is the encoder's channel order.
std::vector<std::int32_t> expected_pcm(const Clip& clip, std::size_t first, std::size_t count) {
    std::vector<std::int32_t> pcm;
    pcm.reserve(count * kChannels);
    for (std::size_t i = first; i < first + count; ++i) {
        for (std::size_t w = 0; w < kChannels; ++w) {
            pcm.push_back(round_to_24(clip.bed[kCodedForWave[w]][i]));
        }
    }
    return pcm;
}

// Frames [first, last) of the clip, `frame` at a time, as the engine hands
// them over: only the bed matters to this leg.
void feed(OutputStage& stage, const Clip& clip, std::size_t first, std::size_t last, std::size_t frame) {
    std::array<std::span<const float>, kChannels> views;
    for (std::size_t at = first; at < last; at += frame) {
        const std::size_t n = std::min(frame, last - at);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            views[ch] = std::span<const float>(clip.bed[ch]).subspan(at, n);
        }
        stage.submit({}, RawFrame{.objects = {}, .placements = {}, .bed = views});
    }
}

struct Decoded {
    std::vector<std::int32_t> pcm;  // interleaved, 6 channels
    std::size_t units = 0;
    bool first_unit_started = false;  // the first unit alone gave samples, behind a restart header
    std::uint64_t waiting = 0;        // units that gave none for want of a restart header
};

Decoded decode_carrier(std::span<const std::byte> carrier) {
    auto units = mat::unwrap_mat_stream(carrier);
    REQUIRE(units.has_value());
    Decoded out;
    out.units = units->size();
    iclforge::truehd::Decoder decoder;
    for (std::size_t i = 0; i < units->size(); ++i) {
        auto decoded = decoder.decode((*units)[i]);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (i == 0) {
            out.first_unit_started = (*decoded)->restarted;
        }
        REQUIRE((*decoded)->channels == static_cast<int>(kChannels));
        REQUIRE((*decoded)->padding_samples == 0);
        out.pcm.insert(out.pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    out.waiting = decoder.counters().units_waiting;
    return out;
}

OutputStageConfig leg_config(const std::shared_ptr<FakeDevices>& devices,
                             std::optional<OutputMode> pinned = OutputMode::kTrueHd51) {
    return {.devices = devices,
            .null_sink_substring = "Crucible Silent Output",
            .pinned = pinned,
            .sample_rate = 48000};
}

std::shared_ptr<FakeDevices> receiver_that_takes_truehd() {
    auto devices = std::make_shared<FakeDevices>();
    devices->record_bursts = true;
    devices->devices = {realtek_default(), null_sink(), hdmi_avr_truehd()};
    return devices;
}

}  // namespace

TEST_CASE("truehd leg: the bursts unwrap and decode to the rounded 24-bit input, sample for sample",
          "[crucible][output_stage][truehd]") {
    // 1536 is the engine's frame, 256 its low-latency one; the leg gathers
    // either into whatever the encoder takes.
    const std::size_t frame = GENERATE(std::size_t{1536}, std::size_t{256});
    CAPTURE(frame);
    const Clip clip = programme_clip();

    auto devices = receiver_that_takes_truehd();
    OutputStage stage(leg_config(devices));
    const auto& status = stage.reprobe(false);
    REQUIRE(status.mode == OutputMode::kTrueHd51);
    CHECK(status.endpoint_id == "avr");
    REQUIRE(devices->burst_sinks.size() == 1);
    const auto sink = devices->burst_sinks[0];
    CHECK(sink->format == BurstFormat::kTrueHd);
    CHECK(sink->sample_rate == 48000);

    feed(stage, clip, 0, clip.frames(), frame);

    CHECK(stage.status().underruns == 0);
    CHECK(sink->submits > 100);  // 120000 samples are 125 bursts; the encoder holds one interval back
    CHECK(sink->bytes == sink->submits * mat::kMatBurstBytes);
    REQUIRE(sink->carrier.size() == sink->bytes);

    const Decoded decoded = decode_carrier(sink->carrier);
    CHECK(decoded.units == sink->submits * mat::kMatAccessUnitsPerFrame);
    CHECK(decoded.waiting == 0);
    CHECK(decoded.first_unit_started);
    const std::size_t decoded_frames = decoded.pcm.size() / kChannels;
    CHECK(decoded_frames == decoded.units * kAccessUnitSamples);
    // Nothing but the encoder's last restart interval and the last partial burst is held back.
    CHECK(decoded_frames + (128 + mat::kMatAccessUnitsPerFrame) * kAccessUnitSamples >= clip.frames());
    CHECK(decoded.pcm == expected_pcm(clip, 0, decoded_frames));
}

TEST_CASE("truehd leg: values off the 24-bit grid round to nearest even and clamp at the ends",
          "[crucible][output_stage][truehd]") {
    Clip clip = programme_clip();
    // Real programme material, every sample moved off the 16-bit grid a little
    // so that most of the rounding is not exact, and a run of values that are
    // each a rounding or a clamping boundary spread through every channel.
    constexpr float kLsb = 1.0F / 8388608.0F;
    const std::vector<float> boundaries = {
        0.5F * kLsb, 1.5F * kLsb, 2.5F * kLsb, -0.5F * kLsb, -1.5F * kLsb, -2.5F * kLsb,
        1.0F,        -1.0F,       1.5F,        -1.5F,        std::nextafter(1.0F, 0.0F),
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), 0.0F};
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        auto& samples = clip.bed[ch];
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const int k = static_cast<int>((i * 37 + ch) % 5) - 2;
            samples[i] *= 1.0F + 1e-7F * static_cast<float>(k);
            if (i % 97 == ch * 11) {
                samples[i] = boundaries[(i / 97 + ch) % boundaries.size()];
            }
        }
    }

    auto devices = receiver_that_takes_truehd();
    auto config = leg_config(devices);
    config.truehd_restart_interval_units = 8;  // the shortest the encoder accepts besides 1
    OutputStage stage(config);
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);
    feed(stage, clip, 0, clip.frames(), 1536);

    const auto sink = devices->burst_sinks.at(0);
    const Decoded decoded = decode_carrier(sink->carrier);
    const std::size_t decoded_frames = decoded.pcm.size() / kChannels;
    REQUIRE(decoded_frames > clip.frames() / 2);
    const auto expected = expected_pcm(clip, 0, decoded_frames);
    CHECK(decoded.pcm == expected);

    // The cases above must be there to be tested: at least one tie that went
    // to an even integer and one value clamped at each end.
    CHECK(std::count(expected.begin(), expected.end(), 8388607) > 0);
    CHECK(std::count(expected.begin(), expected.end(), -8388608) > 0);
}

TEST_CASE("truehd leg: switching into the mode mid-stream starts with a major sync",
          "[crucible][output_stage][truehd]") {
    const Clip clip = programme_clip();
    auto devices = receiver_that_takes_truehd();

    // First the AC-3 leg, which also works from the bed alone, on the same endpoint.
    OutputStage stage(leg_config(devices, OutputMode::kDd51));
    REQUIRE(stage.reprobe(false).mode == OutputMode::kDd51);
    constexpr std::size_t kSwitchAt = 1536 * 20 + 700;  // not on a frame, access unit or burst boundary
    feed(stage, clip, 0, kSwitchAt, 1536);
    REQUIRE(devices->burst_sinks.size() == 1);
    CHECK(devices->burst_sinks[0]->format == BurstFormat::kAc3);

    stage.set_pinned(OutputMode::kTrueHd51);
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);
    REQUIRE(devices->burst_sinks.size() == 2);
    CHECK(devices->burst_sinks[0]->stopped);
    const auto sink = devices->burst_sinks[1];
    CHECK(sink->format == BurstFormat::kTrueHd);
    CHECK(sink->submits == 0);  // nothing leaves before the new encoder has a whole interval

    constexpr std::size_t kAwayAt = 80'000;
    feed(stage, clip, kSwitchAt, kAwayAt, 1536);
    REQUIRE(sink->submits > 0);

    // A receiver joining here has nothing before this burst: its first access
    // unit has to carry the major sync and the restart header, and decode.
    const Decoded first = decode_carrier(sink->carrier);
    CHECK(first.first_unit_started);
    CHECK(first.waiting == 0);
    const std::size_t first_frames = first.pcm.size() / kChannels;
    REQUIRE(first_frames > 0);
    CHECK(first.pcm == expected_pcm(clip, kSwitchAt, first_frames));

    // Away to the AC-3 leg and back: the first stretch's encoder is gone, so the
    // second stretch starts with a major sync of its own, not in the middle of
    // a restart interval the first one left running.
    constexpr std::size_t kBackAt = 90'000;
    stage.set_pinned(OutputMode::kDd51);
    REQUIRE(stage.reprobe(false).mode == OutputMode::kDd51);
    CHECK(sink->stopped);
    feed(stage, clip, kAwayAt, kBackAt, 1536);
    stage.set_pinned(OutputMode::kTrueHd51);
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);
    REQUIRE(devices->burst_sinks.size() == 4);
    const auto again = devices->burst_sinks[3];
    CHECK(again->format == BurstFormat::kTrueHd);
    feed(stage, clip, kBackAt, clip.frames(), 1536);
    REQUIRE(again->submits > 0);
    const Decoded second = decode_carrier(again->carrier);
    CHECK(second.first_unit_started);
    CHECK(second.waiting == 0);
    const std::size_t second_frames = second.pcm.size() / kChannels;
    REQUIRE(second_frames > 0);
    CHECK(second.pcm == expected_pcm(clip, kBackAt, second_frames));
}

TEST_CASE("truehd leg: a sink that refuses to start leaves the stage in none, with the reason",
          "[crucible][output_stage][truehd]") {
    auto devices = receiver_that_takes_truehd();
    devices->refuse_sink_starts = true;
    OutputStage stage(leg_config(devices));
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kNone);
    CHECK_FALSE(status.running);
    CHECK(status.reason.find("could not start: refused by the test") != std::string::npos);
}

TEST_CASE("truehd leg: a sample rate the encoder does not write is refused and the sink is let go",
          "[crucible][output_stage][truehd]") {
    auto devices = receiver_that_takes_truehd();
    auto config = leg_config(devices);
    config.sample_rate = 8000;
    OutputStage stage(config);
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kNone);
    CHECK(status.reason.find("could not start") != std::string::npos);
    REQUIRE(devices->burst_sinks.size() == 1);
    CHECK(devices->burst_sinks[0]->stopped);
}

TEST_CASE("truehd leg: a stream the declared peak rate cannot carry stops the output and says why",
          "[crucible][output_stage][truehd]") {
    const Clip clip = programme_clip();
    auto devices = receiver_that_takes_truehd();
    auto config = leg_config(devices);
    config.truehd_peak_data_rate_bps = 1'500'000;  // the least the encoder accepts is 1 Mbit/s; 5.1 music needs about 4
    config.truehd_restart_interval_units = 8;
    OutputStage stage(config);
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);
    feed(stage, clip, 0, clip.frames(), 1536);
    CHECK(stage.status().mode == OutputMode::kNone);
    CHECK_FALSE(stage.status().running);
    CHECK(stage.status().reason.find("TrueHD encoder") != std::string::npos);
    CHECK(devices->burst_sinks.at(0)->stopped);
}

TEST_CASE("truehd leg: a frame whose channels disagree in length is skipped, not encoded",
          "[crucible][output_stage][truehd]") {
    const Clip clip = programme_clip();
    auto devices = receiver_that_takes_truehd();
    OutputStage stage(leg_config(devices));
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);

    std::array<std::span<const float>, kChannels> views;
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        views[ch] = std::span<const float>(clip.bed[ch]).subspan(0, ch == 3 ? 1000 : 1536);
    }
    stage.submit({}, RawFrame{.objects = {}, .placements = {}, .bed = views});
    // A bed of five channels is not a 5.1 bed either.
    stage.submit({}, RawFrame{.objects = {},
                              .placements = {},
                              .bed = std::span<const std::span<const float>>(views.data(), 5)});

    // The stream that follows is whole and decodes from its first unit.
    feed(stage, clip, 0, clip.frames(), 1536);
    const Decoded decoded = decode_carrier(devices->burst_sinks.at(0)->carrier);
    CHECK(decoded.first_unit_started);
    CHECK(decoded.pcm == expected_pcm(clip, 0, decoded.pcm.size() / kChannels));
}

// Not a test of anything on its own: writes what the leg sent a sink to
// ICLFORGE_CRUCIBLE_TRUEHD_DUMP_DIR so that tools/checks/verify_crucible_truehd_leg.py can hold
// the same bursts to FFmpeg's strict decode. Hidden; run it by tag.
TEST_CASE("truehd leg: write the bursts and the input for the FFmpeg check",
          "[.truehd_dump][crucible][output_stage][truehd]") {
    const char* dir = std::getenv("ICLFORGE_CRUCIBLE_TRUEHD_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("ICLFORGE_CRUCIBLE_TRUEHD_DUMP_DIR is not set");
    }
    const Clip clip = programme_clip();
    auto devices = receiver_that_takes_truehd();
    OutputStage stage(leg_config(devices));
    REQUIRE(stage.reprobe(false).mode == OutputMode::kTrueHd51);
    feed(stage, clip, 0, clip.frames(), 1536);
    const auto sink = devices->burst_sinks.at(0);
    REQUIRE(sink->submits > 0);

    const std::filesystem::path out(dir);
    {
        std::ofstream file(out / "leg.mat", std::ios::binary);
        file.write(reinterpret_cast<const char*>(sink->carrier.data()),
                   static_cast<std::streamsize>(sink->carrier.size()));
        REQUIRE(file.good());
    }
    // The rounded input, 24 bits in an int32 each, interleaved in WAVE order: the whole clip, of
    // which the bursts carry a prefix.
    const auto pcm = expected_pcm(clip, 0, clip.frames());
    std::ofstream file(out / "leg.s32", std::ios::binary);
    file.write(reinterpret_cast<const char*>(pcm.data()),
               static_cast<std::streamsize>(pcm.size() * sizeof(std::int32_t)));
    REQUIRE(file.good());
}
