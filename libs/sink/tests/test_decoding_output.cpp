#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/decoding_output.hpp"

// The decoding output against a PcmSink that writes down what it was given (phase 4a): what the sink
// is told and in which order, which is what a program that plays the samples builds on. What the samples
// are is held where they were held before the decode moved here: the test sink's suites, and the
// byte-equality of its WAV files on the programmes of testdata.

namespace {

namespace ac = iclforge::sendspin::player;
namespace sink = iclforge::sink;

// One event the sink saw, in order.
struct Event {
    enum class Kind { kOpen, kBurst, kWrite, kClear, kClose };
    Kind kind = Kind::kOpen;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::int64_t time = 0;
};

struct Journal {
    std::vector<Event> events;
    std::uint64_t frames = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    bool refuse = false;
    bool counting_only = false;
    double peak = 0.0;
};

class RecordingSink final : public sink::PcmSink {
   public:
    explicit RecordingSink(Journal& journal) : journal_(&journal) {}

    bool open(std::uint32_t sample_rate, std::uint16_t channels) override {
        if (journal_->refuse) {
            return false;
        }
        journal_->rate = sample_rate;
        journal_->channels = channels;
        journal_->events.push_back({.kind = Event::Kind::kOpen, .a = sample_rate, .b = channels});
        return true;
    }
    [[nodiscard]] bool taking() const override { return !journal_->counting_only; }
    void burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t frames) override {
        journal_->events.push_back(
            {.kind = Event::Kind::kBurst, .a = first_frame, .b = frames, .time = local_time_us});
    }
    void write(std::span<const float> interleaved) override {
        journal_->frames += interleaved.size() / journal_->channels;
        for (const float sample : interleaved) {
            journal_->peak = std::max(journal_->peak, static_cast<double>(std::fabs(sample)));
        }
        journal_->events.push_back({.kind = Event::Kind::kWrite, .a = interleaved.size()});
    }
    void clear(std::uint64_t frames) override {
        journal_->events.push_back({.kind = Event::Kind::kClear, .a = frames});
    }
    void close() override { journal_->events.push_back({.kind = Event::Kind::kClose}); }
    [[nodiscard]] const std::filesystem::path& file() const override { return file_; }

   private:
    Journal* journal_;
    std::filesystem::path file_;
};

// Eight frames of a stereo tone as AC-3, one frame to a burst as a server sends them.
std::vector<std::vector<std::uint8_t>> tone_bursts(int frames) {
    iclforge::ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .dialnorm = 27, .acmod = iclforge::ac3::Acmod::k2_0}};
    std::vector<std::vector<float>> pcm(2, std::vector<float>(iclforge::ac3::kSamplesPerFrame));
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (int i = 0; i < iclforge::ac3::kSamplesPerFrame; ++i) {
            const double f = 440.0 + 220.0 * static_cast<double>(channel);
            pcm[channel][static_cast<std::size_t>(i)] =
                static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * f * i / 48000.0));
        }
    }
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
    std::vector<std::vector<std::uint8_t>> bursts;
    for (int i = 0; i < frames; ++i) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        std::vector<std::uint8_t> bytes;
        bytes.reserve(frame->size());
        for (const std::byte b : *frame) {
            bytes.push_back(static_cast<std::uint8_t>(b));
        }
        bursts.push_back(std::move(bytes));
    }
    return bursts;
}

void send(sink::DecodingBurstOutput& output, const std::vector<std::uint8_t>& payload, std::int64_t due_us) {
    iclforge::sendspin::BurstChunk chunk;
    chunk.chunk.timestamp_us = due_us;
    chunk.chunk.data = payload;
    chunk.pc = static_cast<std::uint16_t>(iclforge::sendspin::BurstDataType::kAc3);
    chunk.pd = static_cast<std::uint16_t>(payload.size() * 8U);
    output.write(chunk, due_us);
}

ac::StreamStart ac3_stream() {
    ac::StreamStart stream;
    stream.data_type = ac::DataType::kAc3;
    stream.sample_rate = 48000;
    return stream;
}

}  // namespace

TEST_CASE("decoding output: the sink is opened for the layout's slots, told when each burst is due and handed "
          "the decoded frames in order",
          "[sink][decode]") {
    Journal journal;
    sink::DecodingBurstOutput output(iclforge::render::OutputLayout::stereo(), std::make_unique<RecordingSink>(journal));
    const auto bursts = tone_bursts(8);

    REQUIRE(output.start(ac3_stream()));
    // A stream's start ends what was open (nothing yet) and opens the sink for the new one.
    REQUIRE(journal.events.size() == 2);
    CHECK(journal.events[0].kind == Event::Kind::kClose);
    CHECK(journal.events[1].kind == Event::Kind::kOpen);
    CHECK(journal.rate == 48000);
    CHECK(journal.channels == 2);
    CHECK(output.output() == ac::Output::kPcm);
    CHECK_FALSE(output.decoder().has_value());

    for (std::size_t i = 0; i < bursts.size(); ++i) {
        send(output, bursts[i], 1'000'000 + static_cast<std::int64_t>(i) * 32'000);
    }

    // Each burst is announced before the frames it stands for: its time, its first frame and its length.
    std::uint64_t announced = 0;
    std::size_t burst_events = 0;
    for (const Event& event : journal.events) {
        if (event.kind == Event::Kind::kBurst) {
            CHECK(event.a == burst_events * 1536U);
            CHECK(event.b == 1536U);
            CHECK(event.time == 1'000'000 + static_cast<std::int64_t>(burst_events) * 32'000);
            ++burst_events;
            announced += event.b;
        }
    }
    CHECK(burst_events == 8);
    CHECK(announced == 8U * 1536U);
    // A decoder delivers what it has decoded as it goes, and a tone is not silence.
    CHECK(journal.frames == 8U * 1536U);
    CHECK(journal.peak > 0.1);
    CHECK(output.bursts() == 8);
    CHECK(output.frames() == journal.frames);
    CHECK(output.undecodable() == 0);
    CHECK(output.counts().units == 8);
    // What the decoder found in the first frame: 2/0, no LFE.
    REQUIRE(output.decoder().has_value());
    CHECK(output.decoder()->data_type == ac::DataType::kAc3);
    CHECK(output.decoder()->acmod == 2);
    CHECK_FALSE(output.decoder()->lfe);
}

TEST_CASE("decoding output: a clear and an end reach the sink with the frames made so far, and a new stream opens it again",
          "[sink][decode]") {
    Journal journal;
    sink::DecodingBurstOutput output(iclforge::render::OutputLayout::stereo(), std::make_unique<RecordingSink>(journal));
    const auto bursts = tone_bursts(4);

    REQUIRE(output.start(ac3_stream()));
    send(output, bursts[0], 0);
    send(output, bursts[1], 32'000);
    output.clear();
    // The clear says how many frames the stream had made, which a log or a player counts from.
    const auto cleared = std::ranges::find_if(journal.events, [](const Event& e) { return e.kind == Event::Kind::kClear; });
    REQUIRE(cleared != journal.events.end());
    CHECK(cleared->a == 2U * 1536U);

    // The stream goes on from the next burst, decoding afresh.
    send(output, bursts[2], 64'000);
    output.end();
    CHECK(journal.events.back().kind == Event::Kind::kClose);
    CHECK_FALSE(output.decoder().has_value());

    // Bursts after the end are counted and not decoded.
    const std::uint64_t made = output.frames();
    send(output, bursts[3], 96'000);
    CHECK(output.frames() == made);
    CHECK(output.bursts() == 4);

    // A second stream: the sink is closed and opened, and the count of streams is two.
    REQUIRE(output.start(ac3_stream()));
    CHECK(output.streams() == 2);
    std::size_t opens = 0;
    for (const Event& event : journal.events) {
        opens += event.kind == Event::Kind::kOpen ? 1 : 0;
    }
    CHECK(opens == 2);
}

TEST_CASE("decoding output: a sink that cannot take the stream refuses it, and one that only counts is spared the samples",
          "[sink][decode]") {
    const auto bursts = tone_bursts(2);

    Journal refused;
    refused.refuse = true;
    sink::DecodingBurstOutput refusing(iclforge::render::OutputLayout::stereo(), std::make_unique<RecordingSink>(refused));
    CHECK_FALSE(refusing.start(ac3_stream()));
    send(refusing, bursts[0], 0);
    // Not taken: nothing is decoded and nothing reaches the sink (the burst is counted).
    CHECK(refusing.frames() == 0);
    CHECK(refused.frames == 0);

    Journal counting;
    counting.counting_only = true;
    sink::DecodingBurstOutput counted(iclforge::render::OutputLayout::stereo(), std::make_unique<RecordingSink>(counting));
    REQUIRE(counted.start(ac3_stream()));
    send(counted, bursts[0], 0);
    send(counted, bursts[1], 32'000);
    // The decoder ran and counted the frames; the sink was told of the bursts and handed no samples.
    CHECK(counted.frames() == 2U * 1536U);
    CHECK(counting.frames == 0);
    CHECK(counted.decoder().has_value());
}
