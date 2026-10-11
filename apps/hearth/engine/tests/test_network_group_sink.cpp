#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/server_host.hpp"
#include "iclforge/sendspin/server_store.hpp"
#include "network_group_sink.hpp"
#include "sink.hpp"

// iclforge::hearth::NetworkGroupSink (apps/hearth/engine/src/network_group_sink.cpp)
// against a real iclforge::sendspin::Group and two real in-process test sinks
// (apps/hearth/testsink), proving what Player itself does not re-prove: that
// this sink's own translation is correct - planar float to Group::push()'s
// interleaved int32 (checked sample for sample against the sink's WAV, the
// same way apps/hearth/engine/tests/test_group.cpp checks Group itself), and a burst's
// pc/pd/payload/frame forwarded to Group::push_burst() unchanged (checked by
// the sink's own burst count and frame total, since decoding a real AC-3
// frame correctly is what test_group.cpp and the wider codec suite already
// cover - this is about the wrapper, not the codec).
//
// It dials, so under ThreadSanitizer it needs what libs/sendspin/tests/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = iclforge::sendspin::messages;
namespace testsink = iclforge::hearth::testsink;
using namespace std::chrono_literals;

std::string scratch_pid_suffix() { return iclforge::test::platform::process_id(); }

class QuietLog final : public testsink::SinkLog {
   public:
    void line(std::string_view /*text*/) override {}
};

class HostEvents final : public iclforge::sendspin::ServerHostEvents {
   public:
    void on_client(const iclforge::sendspin::ClientView& client) override {
        {
            const std::lock_guard lock(mutex_);
            clients_[client.client_id] = client;
        }
        changed_.notify_all();
    }
    void on_client_gone(const std::string& /*client_id*/) override {}
    void on_pairing_code_wanted(const std::string& /*client_id*/) override {}
    void on_paired(const std::string& /*client_id*/) override {}
    void on_pairing_ended(const std::string& /*client_id*/,
                          std::optional<iclforge::sendspin::pairing_messages::AbortReason> /*reason*/) override {}
    void on_log(std::string_view /*line*/) override {}

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(clients_); });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, iclforge::sendspin::ClientView> clients_;
};

// `unpaired_access` matches test_group.cpp's own two flows: a plain
// player@v1 sink is approved for unpaired access (needs no pairing to play
// PCM), while an _iclforge_player@v1 sink is paired by its token instead
// (below) - SinkOptions::unpaired_access defaults to false, which
// test_group.cpp's own paired sinks rely on rather than setting explicitly,
// so this does too rather than guessing the extension role also works
// unpaired.
std::unique_ptr<testsink::Sink> start_sink(const fs::path& directory, std::string name, bool extension_role,
                                           bool unpaired_access, QuietLog& log) {
    testsink::SinkOptions options;
    options.name = std::move(name);
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = directory / "state";
    options.output_directory = directory / "out";
    options.advertise = false;
    options.unpaired_access = unpaired_access;
    options.codecs = {m::Codec::kPcm};
    options.extension_role = extension_role;
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    return std::move(*sink);
}

// One real, valid AC-3 access unit: a quiet stereo tone, 48 kHz.
std::vector<std::byte> ac3_unit() {
    iclforge::ac3::EncoderConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 192;
    config.acmod = iclforge::ac3::Acmod::k2_0;
    iclforge::ac3::FrameEncoder encoder{config};
    std::vector<float> samples(iclforge::ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        samples[n] = 0.1F * std::sin(static_cast<float>(n) * 0.1F);
    }
    const std::vector<std::span<const float>> views(2, samples);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

}  // namespace

TEST_CASE("network group sink: PCM and a burst reach real sinks through the wrapper",
         "[hearth][group][websocket]") {
    const fs::path scratch =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_group_sink_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> pcm_sink =
        start_sink(scratch / "pcm", "PCM sink", /*extension_role=*/false, /*unpaired_access=*/true, log);
    const std::unique_ptr<testsink::Sink> burst_sink =
        start_sink(scratch / "burst", "Burst sink", /*extension_role=*/true, /*unpaired_access=*/false, log);

    std::optional<iclforge::sendspin::noise::KeyPair> identity = iclforge::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = iclforge::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1",
         .port = std::nullopt, .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    // burst_sink pairs by its own token, entered before it even connects
    // (test_group.cpp's own recipe for an _iclforge_player@v1 sink) - it
    // needs no separate approve() below, unlike pcm_sink's unpaired access.
    REQUIRE((*host)->enter_pairing_token(burst_sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    const std::string pcm_client_id = pcm_sink->client_id();
    REQUIRE((*host)->approve(pcm_client_id, true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    // Diagnostic: which negotiated the extension role. bursts=false is
    // fine for pcm_sink (extension_role=false forces player@v1 there);
    // burst_sink (extension_role=true, default) is expected to have
    // negotiated _iclforge_player@v1 (bursts=true) - if it did not, no
    // burst this test sends can ever reach it, whatever Group does.
    std::size_t clients_with_bursts = 0;
    for (const iclforge::sendspin::ClientView& client : (*host)->clients()) {
        INFO("client " << client.name << " playing=" << client.playing << " bursts=" << client.bursts
                       << " available=" << client.available);
        clients_with_bursts += client.bursts ? 1U : 0U;
    }
    CHECK(clients_with_bursts == 1);

    // The resolver a real HearthController<->NetworkController coupling
    // would supply - here, just the one group this test made.
    std::shared_ptr<iclforge::sendspin::Group> group = (*host)->make_group("Test group");
    for (const iclforge::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }
    const std::unique_ptr<iclforge::hearth::NetworkGroupSink> sink =
        iclforge::hearth::make_group_sink([&](const std::string&) { return group; });

    const iclforge::render::OutputLayout layout =
        iclforge::render::OutputLayout::named("2.0").value();
    const auto opened =
        sink->open("Test group", iclforge::hearth::NetworkGroupSink::Format{
                                     .sample_rate = 48000,
                                     .layout = layout,
                                     .stream = iclforge::audio::BitstreamFormat::kAc3});
    REQUIRE(opened.has_value());
    CHECK(opened->mode == iclforge::hearth::OutputMode::kNetworkGroup);

    // PCM: a known tone, pushed until every frame is taken, planar - the
    // shape Player::drain_group() itself offers.
    constexpr std::size_t kFrames = 4800;  // 0.1 s at 48 kHz
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);
    for (std::size_t n = 0; n < kFrames; ++n) {
        left[n] = 0.5F * std::sin(static_cast<float>(n) * 0.05F);
        right[n] = -0.5F * std::sin(static_cast<float>(n) * 0.05F);
    }
    std::size_t offset = 0;
    const auto pcm_deadline = std::chrono::steady_clock::now() + 30s;
    while (offset < kFrames && std::chrono::steady_clock::now() < pcm_deadline) {
        const std::array<std::span<const float>, 2> remaining{
            std::span<const float>(left).subspan(offset), std::span<const float>(right).subspan(offset)};
        const std::size_t taken = sink->submit_pcm(remaining, kFrames - offset);
        if (taken == 0) {
            std::this_thread::sleep_for(10ms);
        }
        offset += taken;
    }
    REQUIRE(offset == kFrames);
    // Taken is not played: the group reads ahead of its own timeline, which
    // starts a lead after the first push, so the tone is all still queued -
    // Player waits on this before it closes the output, or the programme's
    // last second or so would never be heard.
    {
        const auto position = sink->position();
        REQUIRE(position.has_value());
        CHECK(position->frames_played + position->frames_queued == kFrames);
        CHECK(position->frames_played < kFrames);
    }

    // A burst: one real AC-3 frame, wrapped exactly as Player's own
    // send_unit() would, pc/pd read back from the wrap the same way
    // (player.cpp's own comment on iec61937's stable, documented preamble
    // layout explains why that read-back is safe rather than a guess).
    const std::vector<std::byte> unit = ac3_unit();
    const auto wrapped = iclforge::containers::iec61937::wrap_frame(unit);
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->size() >= 8);
    const auto byte_at = [&](std::size_t i) { return std::to_integer<unsigned>((*wrapped)[i]); };
    const auto pc = static_cast<std::uint16_t>(byte_at(4) | (byte_at(5) << 8));
    const auto pd = static_cast<std::uint16_t>(byte_at(6) | (byte_at(7) << 8));
    // Group::push_burst() returns true once the group's own pacing accepts
    // a burst, not once a member has actually received it - a member only
    // takes bursts once its own `started` flag catches up, asynchronously,
    // shortly after add()/start() (server_host.cpp's own Group::push_burst()
    // skips a member silently until then, and still returns true). A single
    // burst can race that, so this resends at the real burst period
    // (kSamplesPerFrame is 1,536, matching push_burst()'s own "one burst,
    // of 1,536 samples") until the sink actually counts one - the same
    // retry-until-taken shape Player's own drain_group() uses, just kept
    // running past the first "taken" to outlast the member's own start-up.
    const auto played_pcm = [&] { return pcm_sink->totals().frames; };
    const auto played_burst = [&] { return burst_sink->totals().bursts; };
    std::int64_t next_frame = 0;
    const auto burst_deadline = std::chrono::steady_clock::now() + 30s;
    while (played_burst() < 1 && std::chrono::steady_clock::now() < burst_deadline) {
        if (sink->submit_burst(pc, pd, unit, next_frame, iclforge::ac3::kSamplesPerFrame)) {
            next_frame += iclforge::ac3::kSamplesPerFrame;
        }
        std::this_thread::sleep_for(32ms);
    }

    const auto until = std::chrono::steady_clock::now() + 10s;
    while ((played_pcm() < kFrames || played_burst() < 1) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(played_pcm() == kFrames);
    CHECK(played_burst() >= 1);
    CHECK(burst_sink->totals().burst_frames >=
          static_cast<std::uint64_t>(iclforge::ac3::kSamplesPerFrame));
    // The group's timeline runs through it within its lead (a sink may count
    // a chunk as it arrives, before its time).
    {
        const auto timeline_deadline = std::chrono::steady_clock::now() + 10s;
        while (sink->position()->frames_played < kFrames && std::chrono::steady_clock::now() < timeline_deadline) {
            std::this_thread::sleep_for(20ms);
        }
        const auto position = sink->position();
        REQUIRE(position.has_value());
        CHECK(position->frames_played == kFrames);
        CHECK(position->frames_queued == 0);
    }

    sink->close();
    group.reset();
    host->reset();

    // The PCM sink's WAV is the tone, sample for sample - the float to
    // int32 conversion round-tripped through a real 16-bit player@v1
    // stream and back without a scale or a sign error. The sink writes the
    // WAV's length when its stream ends, on its own thread, which close()'s
    // stream/end and the host's going only set off: a read straight after can
    // find the header still saying no frames (Linux GCC in CI, 2026-09-25).
    const std::string wav_path = (scratch / "pcm" / "out" / "stream-1-1.wav").string();
    auto wav = iclforge::ac3::io::read_wav(wav_path);
    const auto wav_deadline = std::chrono::steady_clock::now() + 10s;
    while ((!wav || wav->frame_count() != kFrames) && std::chrono::steady_clock::now() < wav_deadline) {
        std::this_thread::sleep_for(20ms);
        wav = iclforge::ac3::io::read_wav(wav_path);
    }
    REQUIRE(wav.has_value());
    REQUIRE(wav->frame_count() == kFrames);
    std::size_t different = 0;
    for (std::size_t n = 0; n < kFrames; ++n) {
        different += std::abs(wav->channels[0][n] - left[n]) > 0.001F ? 1U : 0U;
        different += std::abs(wav->channels[1][n] - right[n]) > 0.001F ? 1U : 0U;
    }
    CHECK(different == 0);
}

// --- the lyrics a group's members hold (W7d, planning/karaoke-phase2.md) ------------------------

TEST_CASE("LyricsAnchors: a first sample is an anchor, and playing on is not one until it is stale",
          "[hearth][group][lyrics]") {
    using iclforge::hearth::LyricsAnchors;
    constexpr std::int64_t kMs = 1000;
    LyricsAnchors anchors;

    // The first look at an item: the members hold nothing, and the sound is taken to be playing.
    const auto first = anchors.sample(0, 100 * kMs);
    REQUIRE(first.has_value());
    CHECK(first->position_ms == 0);
    CHECK(first->at_us == 100 * kMs);
    CHECK(first->speed == 1.0);

    // Playing on at the pace of the clock, sampled every 20 ms: nothing to say for the next 5 s.
    std::int64_t now = 100 * kMs;
    std::int64_t position = 0;
    int sent = 0;
    while (now < 100 * kMs + 4'900 * kMs) {
        now += 20 * kMs;
        position += 20;
        if (anchors.sample(position, now)) {
            ++sent;
        }
    }
    CHECK(sent == 0);

    // And a refresh once the anchor is five seconds old, with the position as it is.
    now += 200 * kMs;
    position += 200;
    const auto refresh = anchors.sample(position, now);
    REQUIRE(refresh.has_value());
    CHECK(refresh->position_ms == position);
    CHECK(refresh->speed == 1.0);
}

TEST_CASE("LyricsAnchors: a stop is a hold once the position has stood still, and a restart moves it again",
          "[hearth][group][lyrics]") {
    using iclforge::hearth::LyricsAnchors;
    constexpr std::int64_t kMs = 1000;
    LyricsAnchors anchors;
    REQUIRE(anchors.sample(1000, 0).has_value());

    // The sound goes on, then the buffer runs out and the position stands at 1,500.
    std::int64_t now = 0;
    std::int64_t position = 1000;
    for (int turn = 0; turn < 25; ++turn) {
        now += 20 * kMs;
        position = std::min<std::int64_t>(position + 20, 1500);
        (void)anchors.sample(position, now);
    }
    // 25 turns of 20 ms: the position stopped at 1,500 after 25 turns of 20 ms = 500 ms, which is the
    // end of the buffer. The last turn that saw it move was the last that agreed with the anchor.
    std::optional<iclforge::hearth::LyricsUpdate::Anchor> hold;
    for (int turn = 0; turn < 10 && !hold; ++turn) {
        now += 20 * kMs;
        hold = anchors.sample(1500, now);
    }
    REQUIRE(hold.has_value());
    CHECK(hold->speed == 0.0);
    CHECK(hold->position_ms == 1500);

    // Held, and sampled on: nothing more until it moves.
    int sent = 0;
    for (int turn = 0; turn < 100; ++turn) {
        now += 20 * kMs;
        if (anchors.sample(1500, now)) {
            ++sent;
        }
    }
    CHECK(sent == 0);

    // It plays again: the position moves 20 ms a turn, and the first anchor that says so is a speed of 1
    // at a position that is where the sound is.
    std::optional<iclforge::hearth::LyricsUpdate::Anchor> restart;
    std::int64_t at = 1500;
    for (int turn = 0; turn < 10 && !restart; ++turn) {
        now += 20 * kMs;
        at += 20;
        restart = anchors.sample(at, now);
    }
    REQUIRE(restart.has_value());
    CHECK(restart->speed == 1.0);
    CHECK(restart->position_ms == at);
}

TEST_CASE("LyricsAnchors: a seek is an anchor at once, and a new item forgets the last",
          "[hearth][group][lyrics]") {
    using iclforge::hearth::LyricsAnchors;
    constexpr std::int64_t kMs = 1000;
    LyricsAnchors anchors;
    REQUIRE(anchors.sample(0, 0).has_value());
    CHECK_FALSE(anchors.sample(20, 20 * kMs).has_value());

    // 60 seconds on in 40 ms: far off what the anchor predicts, and the speed is still 1.
    const auto jumped = anchors.sample(60'000, 60 * kMs);
    REQUIRE(jumped.has_value());
    CHECK(jumped->position_ms == 60'000);
    CHECK(jumped->speed == 1.0);

    // Back 10 seconds is as far off as forward is.
    const auto back = anchors.sample(50'000, 100 * kMs);
    REQUIRE(back.has_value());
    CHECK(back->position_ms == 50'000);
    // A jump is not a stop: the sound goes on at its speed from the new place.
    CHECK(back->speed == 1.0);

    // A drift under the threshold is left to the refresh.
    CHECK_FALSE(anchors.sample(50'040 + 100, 140 * kMs).has_value());

    // A new item starts again: the first sample is an anchor.
    anchors.reset();
    const auto fresh = anchors.sample(0, 200 * kMs);
    REQUIRE(fresh.has_value());
    CHECK(fresh->position_ms == 0);
}
