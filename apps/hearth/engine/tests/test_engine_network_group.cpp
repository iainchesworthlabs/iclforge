#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"

#include "burst_output.hpp"
#include "engine_thread.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/dsp/stream_resampler.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/server_host.hpp"
#include "iclforge/sendspin/server_store.hpp"
#include "item_source.hpp"
#include "network_group_sink.hpp"
#include "network_sinks.hpp"
#include "pairing_store.hpp"
#include "settings_model.hpp"
#include "sink.hpp"
#include "stream_decoder.hpp"
#include "truehd_test_support.hpp"

// Issue #874's own exit (planning/hearth-reference-player.md, A6): "from the
// app, a group of two test sinks... plays one programme." Unlike
// test_network_group_sink.cpp, which drives NetworkGroupSink directly to
// prove the wrapper's own translation, this drives it from
// iclforge::hearth::Engine - the same class HearthController wraps - exactly the
// way HearthController::start()/selectOutputGroup() do: an EngineOutputs
// with a real group resolver, OutputPreferences pinning kNetworkGroup, a
// queue item added and played. What test_group.cpp and PR #932's mixed-
// delivery case already prove at the Group level, and test_network_group_
// sink.cpp proves at the NetworkGroupSink level, this proves end to end
// through the whole application stack bar Qt itself.

namespace {

namespace fs = std::filesystem;
namespace m = iclforge::sendspin::messages;
namespace testsink = iclforge::hearth::testsink;
using namespace std::chrono_literals;
using iclforge::hearth::Engine;
using iclforge::hearth::EngineOutputs;
using iclforge::hearth::EngineStatus;
using iclforge::hearth::EngineTiming;
using iclforge::hearth::ItemLoader;
using iclforge::hearth::LoadedItem;
using iclforge::hearth::OutputMode;
using iclforge::hearth::OutputPreferences;
using iclforge::hearth::QueueItem;
using iclforge::hearth::TransportState;

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

// `frames` E-AC-3 access units of a 440 Hz tone, concatenated as one
// elementary stream - test_engine.cpp's own eac3_stream(), which this
// mirrors: each unit is a full six-block (1,536-sample, 32 ms) burst on its
// own, so `frames` * 32 ms is the programme's real-time length.
std::vector<std::byte> eac3_stream(int frames) {
    iclforge::ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = iclforge::ac3::Acmod::k2_0;
    iclforge::ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        std::vector<float> samples(iclforge::ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0));
        }
        const std::vector<std::span<const float>> views(2, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

// `count` 5.1 E-AC-3 access units of a 440 Hz tone, each channel at a level of its own so that a
// fold has something to sum: the units one at a time, and as the one elementary stream.
struct SurroundProgramme {
    std::vector<std::vector<std::byte>> units;
    std::vector<std::byte> bytes;
};

SurroundProgramme surround_programme(int count,
                                     std::optional<int> declared_objects = std::nullopt) {
    iclforge::ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    config.oba_complexity_index = declared_objects;
    iclforge::ac3::eac3::FrameEncoder encoder{config};
    SurroundProgramme out;
    for (int f = 0; f < count; ++f) {
        std::vector<float> samples(iclforge::ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.3 *
                std::sin(2.0 * std::numbers::pi * 440.0 *
                         static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0));
        }
        std::vector<std::vector<float>> by_channel;
        for (int channel = 0; channel < encoder.channel_count(); ++channel) {
            std::vector<float> scaled = samples;
            for (float& sample : scaled) {
                sample *= 0.4F + (0.1F * static_cast<float>(channel));
            }
            by_channel.push_back(std::move(scaled));
        }
        const std::vector<std::span<const float>> views(by_channel.begin(), by_channel.end());
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.units.push_back(*frame);
        out.bytes.insert(out.bytes.end(), frame->begin(), frame->end());
    }
    return out;
}

// A test sink finalizes its WAV when the stream ends, which is a message after the last chunk it
// counts in totals().frames: a read made first finds a header with no frames in it. This reads the
// file again until it holds `frames`, or gives up and returns the last read.
auto read_wav_holding(const std::string& path, std::size_t frames) {
    auto wav = iclforge::ac3::io::read_wav(path);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while ((!wav.has_value() || wav->frame_count() < frames) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        wav = iclforge::ac3::io::read_wav(path);
    }
    return wav;
}

// Polls until `done` holds, or gives up after a generous while - test_engine.cpp's own.
bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

// A test sink's dynamic pairing code, as the sink shows it in its log.
class CodeLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(text);
        }
        const std::size_t at = text.find("PAIRING CODE ");
        if (at == std::string_view::npos) {
            return;
        }
        std::string digits;
        for (const char c : text.substr(at + 13)) {
            if (c >= '0' && c <= '9') {
                digits.push_back(c);
            }
        }
        const std::lock_guard lock(mutex_);
        code_ = digits;
    }
    std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        return code_;
    }
    // Whether the sink has logged a line holding `text`.
    bool saw(std::string_view text) {
        const std::lock_guard lock(mutex_);
        return std::ranges::any_of(lines_, [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    }

   private:
    std::mutex mutex_;
    std::optional<std::string> code_;
    std::vector<std::string> lines_;
};

std::optional<iclforge::hearth::SinkFacts> find_sink(iclforge::hearth::NetworkSinks& sinks,
                                                     const std::string& id) {
    const auto status = sinks.status();
    for (const iclforge::hearth::SinkFacts& facts : status.sinks) {
        if (facts.id == id) {
            return facts;
        }
    }
    return std::nullopt;
}

// Pairs the sink `id` the way the page does: asks, reads the six digits the sink shows (from its
// log, where a board shows them on its own page), submits them, and waits for it to be paired and
// playing under the long-term key.
void pair_through(iclforge::hearth::NetworkSinks& sinks, const std::string& id, CodeLog& log) {
    REQUIRE(eventually([&] {
        const auto facts = find_sink(sinks, id);
        return facts && facts->link == iclforge::hearth::SinkLink::kConnected &&
               facts->iclforge_support;
    }));
    sinks.pair_sink(id);
    REQUIRE(eventually([&] { return log.code().has_value(); }));
    sinks.submit_pairing_code(id, *log.code());
    REQUIRE(eventually([&] {
        const auto facts = find_sink(sinks, id);
        return facts && facts->pair_state == iclforge::hearth::PairState::kPaired &&
               facts->link == iclforge::hearth::SinkLink::kConnected && facts->iclforge_state &&
               facts->clock_converged;
    }));
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// The first `count` sync frames of `path`, whose first is an I-frame, as an
// elementary stream of their own.
std::vector<std::byte> ac4_frames(const fs::path& path, std::size_t count) {
    const std::vector<std::byte> file = read_bytes(path);
    const iclforge::ac4::ScanResult scanned = iclforge::ac4::scan(file);
    REQUIRE(scanned.frames.size() > count);
    return {file.begin(), file.begin() + static_cast<std::ptrdiff_t>(scanned.frames[count].offset)};
}

// The only file in `directory` whose name starts with `prefix` and ends with `extension`.
fs::path only_file(const fs::path& directory, std::string_view prefix, std::string_view extension) {
    std::vector<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(extension)) {
            found.push_back(entry.path());
        }
    }
    REQUIRE(found.size() == 1);
    return found.front();
}

// The local time each logged burst puts the stream's first frame at
// (test_group.cpp's own).
std::vector<double> first_frame_times(const fs::path& log) {
    std::vector<double> times;
    std::ifstream in(log);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string local;
        std::string first;
        if (!std::getline(fields, local, ',') || !std::getline(fields, first, ',') ||
            local == "clear") {
            continue;
        }
        times.push_back(std::stod(local) - (std::stod(first) * 1'000'000.0 / 48000.0));
    }
    return times;
}

// A host with a PCM sink (player@v1, approved unpaired) and a burst sink
// (_iclforge_player@v1, paired) both playing, and a group of the two.
struct TwoSinkGroup {
    QuietLog log;
    std::unique_ptr<testsink::Sink> pcm_sink;
    std::unique_ptr<testsink::Sink> burst_sink;
    iclforge::sendspin::MemoryServerStore store;
    HostEvents events;
    std::unique_ptr<iclforge::sendspin::ServerHost> host;
    std::shared_ptr<iclforge::sendspin::Group> group;

    explicit TwoSinkGroup(const fs::path& scratch) {
        pcm_sink = start_sink(scratch / "pcm", "PCM sink", /*extension_role=*/false,
                              /*unpaired_access=*/true, log);
        burst_sink = start_sink(scratch / "burst", "Burst sink", /*extension_role=*/true,
                                /*unpaired_access=*/false, log);
        std::optional<iclforge::sendspin::noise::KeyPair> identity =
            iclforge::sendspin::noise::KeyPair::generate();
        REQUIRE(identity.has_value());
        auto started = iclforge::sendspin::ServerHost::start({.identity = *identity,
                                                         .name = "Test host",
                                                         .languages = {"en"},
                                                         .address = "127.0.0.1",
                                                         .port = std::nullopt,
                                                         .advertise = false,
                                                         .browse = false,
                                                         .mdns_interfaces = {}},
                                                        store, events);
        REQUIRE(started.has_value());
        host = std::move(*started);
        REQUIRE(host->enter_pairing_token(burst_sink->pairing_token()));
        host->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
        host->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
        REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
        REQUIRE(host->approve(pcm_sink->client_id(), true));
        REQUIRE(events.wait(
            [](const auto& clients) {
                return clients.size() == 2 &&
                       std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                           return entry.second.playing && entry.second.available;
                       });
            },
            20s));
        group = host->make_group("Living room");
        for (const iclforge::sendspin::ClientView& client : host->clients()) {
            group->add(client.client_id);
        }
    }
    TwoSinkGroup(const TwoSinkGroup&) = delete;
    TwoSinkGroup& operator=(const TwoSinkGroup&) = delete;

    ~TwoSinkGroup() {
        // A group must not outlive its host.
        group.reset();
        host.reset();
    }
};

// Plays `programme` from an Engine whose only output is `group`, with
// `settings`, to its end.
EngineStatus play_to_group(const std::shared_ptr<iclforge::sendspin::Group>& group,
                           std::vector<std::byte> programme,
                           const iclforge::hearth::DecoderSettings& settings) {
    const ItemLoader loader =
        [programme = std::move(programme)](
            const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    EngineOutputs outputs{
        .group = iclforge::hearth::make_group_sink([group](const std::string&) { return group; })};
    Engine engine(std::move(outputs), loader, *layout, settings,
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group->id(),
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    return engine.status();
}

}  // namespace

TEST_CASE("engine: from the app, a group of two test sinks plays one programme",
         "[hearth][group][websocket]") {
    constexpr int kFrameCount = 20;  // 640 ms
    const fs::path scratch =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_" + scratch_pid_suffix());
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
    REQUIRE((*host)->enter_pairing_token(burst_sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    REQUIRE((*host)->approve(pcm_sink->client_id(), true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    std::shared_ptr<iclforge::sendspin::Group> group = (*host)->make_group("Living room");
    for (const iclforge::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }

    // The queue item, in memory - test_engine.cpp's own Library pattern:
    // ItemLoader takes an arbitrary key, not a real path, so this needs no
    // scratch file of its own.
    const std::vector<std::byte> programme = eac3_stream(kFrameCount);
    const ItemLoader loader = [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };

    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    // No local pcm/bitstream/endpoints: this group is the only output there
    // is, so a fallback silently playing somewhere else would show up as a
    // refusal (EngineStatus::note), not a false pass.
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [&group](const std::string&) { return group; })};
    Engine engine(std::move(outputs), loader, *layout, iclforge::hearth::DecoderSettings{},
                 EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                     .follow_sink = true,
                                                     .group_name = group->id(),
                                                     .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();

    // The queue is one item, so the transport stops once it has played out
    // - by then the output has closed behind it (test_engine.cpp's own
    // completion checks all read history.size() alongside kStopped for the
    // same reason: nothing here guarantees output.mode is still set by that
    // point). history itself is the record of what was actually played.
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    const auto played_pcm = [&] { return pcm_sink->totals().frames; };
    const auto played_bursts = [&] { return burst_sink->totals().bursts; };
    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] { return played_pcm() >= expected_frames && played_bursts() >= 1; }));

    CHECK(played_pcm() == expected_frames);
    CHECK(played_bursts() == static_cast<std::uint64_t>(kFrameCount));
    CHECK(burst_sink->totals().burst_frames == expected_frames);
    // Both sinks heard the same programme, not each a different fraction of
    // it: the group plays in step, the same proof test_group.cpp's own
    // exit makes for the library alone.
    CHECK(pcm_sink->totals().connections == 1);
    CHECK(burst_sink->totals().connections == 1);
}

// A group is one programme and each member takes it in the form and layout that suits it. The
// player renders to 5.1; the group's planner (what NetworkSinks does by choose_sink_form) says one
// member is to be sent PCM at 2.0, which is the decoder's own fold and not the 5.1 mixed down.
// Three sinks play it together: one decodes the coded stream itself, one takes the player's 5.1 as
// PCM, and one a board that lists stereo, moved to PCM at 2.0.
TEST_CASE("engine: a group's members are each sent the layout their player takes",
          "[hearth][group][websocket][iclforge]") {
    namespace ss = iclforge::sendspin;
    constexpr int kFrameCount = 20;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_variants_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;

    // The programme: 5.1 E-AC-3, kept as units for the reference decodes.
    const SurroundProgramme made = surround_programme(kFrameCount);
    const std::vector<std::vector<std::byte>>& units = made.units;
    const std::vector<std::byte>& programme = made.bytes;

    const auto make_sink = [&](const std::string& name, bool extension_role, bool unpaired_access,
                               std::vector<std::int32_t> widths) {
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = scratch / name / "state";
        options.output_directory = scratch / name / "out";
        options.advertise = false;
        options.unpaired_access = unpaired_access;
        options.codecs = {m::Codec::kPcm};
        options.extension_role = extension_role;
        options.layout = "5.1";
        options.pcm_channels = std::move(widths);
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> decodes = make_sink("decodes", true, false, {2});
    const std::unique_ptr<testsink::Sink> stereo = make_sink("stereo", true, false, {2});
    const std::unique_ptr<testsink::Sink> wide = make_sink("wide", false, true, {6});

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    HostEvents events;
    auto host = ss::ServerHost::start({.identity = *identity,
                                       .name = "Test host",
                                       .languages = {"en"},
                                       .address = "127.0.0.1",
                                       .port = std::nullopt,
                                       .advertise = false,
                                       .browse = false,
                                       .mdns_interfaces = {}},
                                      store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(decodes->pairing_token()));
    REQUIRE((*host)->enter_pairing_token(stereo->pairing_token()));
    for (const testsink::Sink* sink : {decodes.get(), stereo.get(), wide.get()}) {
        (*host)->dial("ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin");
    }
    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 3; }, 15s));
    REQUIRE((*host)->approve(wide->client_id(), true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 3 &&
                   std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        30s));

    std::shared_ptr<ss::Group> group = (*host)->make_group("Variants");
    for (const ss::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }

    const ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    const auto fold = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(master.has_value());
    REQUIRE(fold.has_value());

    // What the planner was asked, and what it did: the board that lists stereo is to be sent PCM at
    // 2.0, and the one that decodes keeps the coded stream.
    std::optional<iclforge::hearth::GroupPlanRequest> asked;
    const iclforge::hearth::MemberPlanner planner =
        [&](const iclforge::hearth::GroupPlanRequest& request)
        -> std::vector<iclforge::render::OutputLayout> {
        asked = request;
        (*host)->use_pcm(stereo->client_id(), true, 0, 2);
        return {*fold};
    };
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [&group](const std::string&) { return group; }, planner)};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group->id(),
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    // The player said what the item is, so the planner could judge each member by it.
    REQUIRE(asked.has_value());
    CHECK(asked->stream == iclforge::audio::BitstreamFormat::kEac3);
    CHECK(asked->sample_rate == 48000);
    CHECK(asked->coded_channels == 6);
    CHECK(asked->layout.slots() == 6);

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] {
        return stereo->totals().frames >= expected_frames &&
               wide->totals().frames >= expected_frames &&
               decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    }));
    // The sink that decodes got the coded stream, and no PCM.
    CHECK(decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(decodes->totals().frames == 0);
    // The board moved to PCM got it, and no bursts; so did the plain player.
    CHECK(stereo->totals().frames == expected_frames);
    CHECK(stereo->totals().bursts == 0);
    CHECK(wide->totals().frames == expected_frames);
    group.reset();
    host->reset();

    // Each wrote the layout it was sent, and that layout's own render of the programme: the board's
    // stereo is what a stereo decoder of the stream makes (its fold), and the player's 5.1 what a
    // 5.1 decoder makes. 16-bit samples, so within a sample's step of the float reference.
    const auto matches = [&](const char* name, const iclforge::render::OutputLayout& layout) {
        iclforge::hearth::StreamDecoder reference{layout, 48000};
        std::vector<std::vector<float>> want(layout.slots());
        const auto deliver = [&want](std::span<const std::span<const float>> slots,
                                     std::size_t frames) {
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                want[slot].insert(want[slot].end(), slots[slot].begin(),
                                  slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
            }
        };
        for (const auto& unit : units) {
            REQUIRE(reference.decode(unit, deliver).has_value());
        }
        reference.finish(deliver);
        const auto wav = read_wav_holding(
            only_file(scratch / name / "out", "stream-", ".wav").string(), want[0].size());
        REQUIRE(wav.has_value());
        REQUIRE(wav->channels.size() == layout.slots());
        REQUIRE(wav->frame_count() == want[0].size());
        double worst = 0.0;
        double energy = 0.0;
        for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
            for (std::size_t frame = 0; frame < want[slot].size(); ++frame) {
                worst = std::max(worst, std::abs(static_cast<double>(wav->channels[slot][frame]) -
                                                 static_cast<double>(want[slot][frame])));
                energy +=
                    static_cast<double>(want[slot][frame]) * static_cast<double>(want[slot][frame]);
            }
        }
        CHECK(worst < 2.0 / 32768.0);
        // Something was played, not two silences that agree.
        CHECK(energy > 1.0);
    };
    matches("stereo", *fold);
    matches("wide", *master);
}

// The app's own planner (NetworkSinks::plan_group) against real connections: two Hearth test sinks
// paired through NetworkSinks the way the page pairs them, in one group, and a 5.1 programme from
// the Engine. One sink decodes the stream itself; one states it decodes 2.0 only and is rendered
// to 2.0, so it is moved to PCM at 2.0 and sent no burst; and one that decodes 2.0 only lists PCM
// of eight channels only, which is neither form, so it is held back and sent nothing.
TEST_CASE("engine: NetworkSinks plans a group of paired sinks and each is sent what it takes",
          "[hearth][group][websocket][iclforge]") {
    namespace ss = iclforge::sendspin;
    constexpr int kFrameCount = 20;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_planned_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const SurroundProgramme made = surround_programme(kFrameCount);

    CodeLog decodes_log;
    CodeLog limited_log;
    CodeLog held_log;
    const auto make_sink = [&](const std::string& name, const std::string& layout,
                               std::uint8_t limit, CodeLog& log,
                               std::vector<std::int32_t> widths = {2}) {
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = scratch / name / "state";
        options.output_directory = scratch / name / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = layout;
        options.max_coded_channels = limit;
        options.pcm_channels = std::move(widths);
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> decodes = make_sink("decodes", "5.1", 0, decodes_log);
    const std::unique_ptr<testsink::Sink> limited = make_sink("limited", "2.0", 2, limited_log);
    const std::unique_ptr<testsink::Sink> held = make_sink("held", "2.0", 2, held_log, {8});

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("2026-10-11"); }};
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Test Hearth", store,
                                         iclforge::hearth::NetworkSinksOptions{.browse = false}};
    REQUIRE(sinks.started());

    const std::map<const testsink::Sink*, std::string> names{
        {decodes.get(), "decodes"}, {limited.get(), "limited"}, {held.get(), "held"}};
    for (const testsink::Sink* sink : {decodes.get(), limited.get(), held.get()}) {
        sinks.on_found(ss::discovery::Service{.instance = names.at(sink),
                                              .host = "127.0.0.1",
                                              .addresses = {"127.0.0.1"},
                                              .port = sink->port(),
                                              .txt = {{.key = "path", .value = "/sendspin"}}});
    }
    pair_through(sinks, "decodes", decodes_log);
    pair_through(sinks, "limited", limited_log);
    pair_through(sinks, "held", held_log);

    const std::string group_id = sinks.create_group("Both");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, "decodes");
    sinks.add_group_member(group_id, "limited");
    sinks.add_group_member(group_id, "held");
    const std::shared_ptr<ss::Group> group = sinks.group(group_id);
    REQUIRE(group != nullptr);

    const std::vector<std::byte>& programme = made.bytes;
    const ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    const auto fold = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(master.has_value());
    REQUIRE(fold.has_value());
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group](const std::string&) { return group; },
                              [&sinks](const iclforge::hearth::GroupPlanRequest& request) {
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] {
        return limited->totals().frames >= expected_frames &&
               decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    }));
    // The sink that decodes got the coded stream, and no PCM; the one that decodes 2.0 only was
    // moved to PCM and got no burst.
    CHECK(decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(decodes->totals().frames == 0);
    CHECK(limited->totals().frames == expected_frames);
    CHECK(limited->totals().bursts == 0);
    // The one that takes neither form was sent nothing at all.
    CHECK(held->totals().frames == 0);
    CHECK(held->totals().bursts == 0);
    CHECK(held->totals().chunks == 0);

    // And what it played is a stereo decoder's render of the programme, within a 16-bit step.
    iclforge::hearth::StreamDecoder reference{*fold, 48000};
    std::vector<std::vector<float>> want(2);
    const auto deliver = [&want](std::span<const std::span<const float>> slots,
                                 std::size_t frames) {
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            want[slot].insert(want[slot].end(), slots[slot].begin(),
                              slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
        }
    };
    for (const auto& unit : made.units) {
        REQUIRE(reference.decode(unit, deliver).has_value());
    }
    reference.finish(deliver);
    const auto wav = read_wav_holding(
        only_file(scratch / "limited" / "out", "stream-", ".wav").string(), want[0].size());
    REQUIRE(wav.has_value());
    REQUIRE(wav->channels.size() == 2);
    REQUIRE(wav->frame_count() == want[0].size());
    double worst = 0.0;
    double energy = 0.0;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        for (std::size_t frame = 0; frame < want[slot].size(); ++frame) {
            worst = std::max(worst, std::abs(static_cast<double>(wav->channels[slot][frame]) -
                                             static_cast<double>(want[slot][frame])));
            energy +=
                static_cast<double>(want[slot][frame]) * static_cast<double>(want[slot][frame]);
        }
    }
    CHECK(worst < 2.0 / 32768.0);
    CHECK(energy > 1.0);
}

// A sink states how many objects it places (max_objects), and a stream that declares more goes to
// it as PCM the application renders, with the objects placed, where it would play only the bed
// itself. One sink here places none and is set to 7.1.4, which has heights; the other has no limit.
TEST_CASE(
    "engine: a sink that places fewer objects than the stream declares is sent PCM decoded here",
    "[hearth][group][websocket][iclforge]") {
    namespace ss = iclforge::sendspin;
    constexpr int kFrameCount = 20;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_objects_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    // A 5.1 programme that declares twelve objects in its addbsi, as a JOC stream does.
    const SurroundProgramme made = surround_programme(kFrameCount, 12);

    CodeLog places_log;
    CodeLog decodes_log;
    const auto make_sink = [&](const std::string& name, const std::string& layout,
                               std::optional<std::uint8_t> objects, CodeLog& log,
                               std::vector<std::int32_t> widths) {
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = scratch / name / "state";
        options.output_directory = scratch / name / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = layout;
        options.max_objects = objects;
        options.pcm_channels = std::move(widths);
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> places =
        make_sink("places", "7.1.4", 0, places_log, {12});
    const std::unique_ptr<testsink::Sink> decodes =
        make_sink("decodes", "5.1", std::nullopt, decodes_log, {2});

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("2026-10-11"); }};
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Test Hearth", store,
                                         iclforge::hearth::NetworkSinksOptions{.browse = false}};
    REQUIRE(sinks.started());
    for (const auto& [sink, name] :
         {std::pair<const testsink::Sink*, const char*>{places.get(), "places"},
          {decodes.get(), "decodes"}}) {
        sinks.on_found(ss::discovery::Service{.instance = name,
                                              .host = "127.0.0.1",
                                              .addresses = {"127.0.0.1"},
                                              .port = sink->port(),
                                              .txt = {{.key = "path", .value = "/sendspin"}}});
    }
    pair_through(sinks, "places", places_log);
    pair_through(sinks, "decodes", decodes_log);

    const std::string group_id = sinks.create_group("Objects");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, "places");
    sinks.add_group_member(group_id, "decodes");
    const std::shared_ptr<ss::Group> group = sinks.group(group_id);
    REQUIRE(group != nullptr);

    const std::vector<std::byte>& programme = made.bytes;
    const ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    const auto immersive = iclforge::render::OutputLayout::parse("7.1.4");
    REQUIRE(master.has_value());
    REQUIRE(immersive.has_value());
    std::optional<iclforge::hearth::GroupPlanRequest> asked;
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group](const std::string&) { return group; },
                              [&](const iclforge::hearth::GroupPlanRequest& request) {
                                  asked = request;
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);

    // What the stream declares reached the planner: its addbsi says twelve and no object metadata
    // shows in its first units, so the count is the index less the LFE.
    REQUIRE(asked.has_value());
    CHECK(asked->objects == 11);

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] {
        return places->totals().frames >= expected_frames &&
               decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    }));
    // The sink that places none got PCM, at its layout's twelve channels, and no burst; the one
    // with no limit got the stream as it is.
    CHECK(places->totals().frames == expected_frames);
    CHECK(places->totals().bursts == 0);
    CHECK(decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(decodes->totals().frames == 0);
    bool reasoned = false;
    for (const std::string& line : sinks.take_log()) {
        reasoned = reasoned || line.find("does not place objects") != std::string::npos;
    }
    CHECK(reasoned);

    // What it played is a 7.1.4 decoder's render of the programme, within a 16-bit step.
    iclforge::hearth::StreamDecoder reference{*immersive, 48000};
    std::vector<std::vector<float>> want(immersive->slots());
    const auto deliver = [&want](std::span<const std::span<const float>> slots,
                                 std::size_t frames) {
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            want[slot].insert(want[slot].end(), slots[slot].begin(),
                              slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
        }
    };
    for (const auto& unit : made.units) {
        REQUIRE(reference.decode(unit, deliver).has_value());
    }
    reference.finish(deliver);
    const auto wav = read_wav_holding(
        only_file(scratch / "places" / "out", "stream-", ".wav").string(), want[0].size());
    REQUIRE(wav.has_value());
    REQUIRE(wav->channels.size() == immersive->slots());
    REQUIRE(wav->frame_count() == want[0].size());
    double worst = 0.0;
    double energy = 0.0;
    for (std::size_t slot = 0; slot < immersive->slots(); ++slot) {
        for (std::size_t frame = 0; frame < want[slot].size(); ++frame) {
            worst = std::max(worst, std::abs(static_cast<double>(wav->channels[slot][frame]) -
                                             static_cast<double>(want[slot][frame])));
            energy +=
                static_cast<double>(want[slot][frame]) * static_cast<double>(want[slot][frame]);
        }
    }
    CHECK(worst < 2.0 / 32768.0);
    CHECK(energy > 1.0);
}

// A sink states the frames a second it decodes (max_frame_rate), and a stream whose frames come
// faster goes to it as PCM the application decodes, where it would fall behind and underrun. The
// programme here is 1,536 samples a frame at 48 kHz, 31.25 a second: one sink states 25 and is sent
// PCM, and one states none and is sent the stream as it is.
TEST_CASE("engine: a sink that decodes fewer frames a second than the stream has is sent PCM",
          "[hearth][group][websocket][iclforge]") {
    namespace ss = iclforge::sendspin;
    constexpr int kFrameCount = 20;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_frame_rate_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const SurroundProgramme made = surround_programme(kFrameCount);

    CodeLog slow_log;
    CodeLog decodes_log;
    const auto make_sink = [&](const std::string& name, std::uint16_t rate, CodeLog& log) {
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = scratch / name / "state";
        options.output_directory = scratch / name / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = "5.1";
        options.max_frame_rate = rate;
        options.pcm_channels = {6};
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> slow = make_sink("slow", 25, slow_log);
    const std::unique_ptr<testsink::Sink> decodes = make_sink("decodes", 0, decodes_log);

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("2026-10-12"); }};
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Test Hearth", store,
                                         iclforge::hearth::NetworkSinksOptions{.browse = false}};
    REQUIRE(sinks.started());
    for (const auto& [sink, name] :
         {std::pair<const testsink::Sink*, const char*>{slow.get(), "slow"},
          {decodes.get(), "decodes"}}) {
        sinks.on_found(ss::discovery::Service{.instance = name,
                                              .host = "127.0.0.1",
                                              .addresses = {"127.0.0.1"},
                                              .port = sink->port(),
                                              .txt = {{.key = "path", .value = "/sendspin"}}});
    }
    pair_through(sinks, "slow", slow_log);
    pair_through(sinks, "decodes", decodes_log);

    const std::string group_id = sinks.create_group("Rates");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, "slow");
    sinks.add_group_member(group_id, "decodes");
    const std::shared_ptr<ss::Group> group = sinks.group(group_id);
    REQUIRE(group != nullptr);

    const std::vector<std::byte>& programme = made.bytes;
    const ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(master.has_value());
    std::optional<iclforge::hearth::GroupPlanRequest> asked;
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group](const std::string&) { return group; },
                              [&](const iclforge::hearth::GroupPlanRequest& request) {
                                  asked = request;
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);

    // The item's frame rate reached the planner: 48,000 over 1,536.
    REQUIRE(asked.has_value());
    CHECK(asked->frame_rate == Catch::Approx(31.25).epsilon(0.001));

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] {
        return slow->totals().frames >= expected_frames &&
               decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    }));
    CHECK(slow->totals().frames == expected_frames);
    CHECK(slow->totals().bursts == 0);
    CHECK(decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(decodes->totals().frames == 0);
    bool reasoned = false;
    for (const std::string& line : sinks.take_log()) {
        reasoned = reasoned || line.find("frames a second") != std::string::npos;
    }
    CHECK(reasoned);
}

// A sink the person has set to Opus (NetworkSinks::set_sink_opus_fallback), for a link too poor for
// PCM, is sent what Hearth decodes for it as Opus: the stream it starts says so, it gets no PCM and
// no burst, and what it plays is the whole programme, lossily.
TEST_CASE("engine: a sink set to Opus is sent the decoded programme as Opus",
          "[hearth][group][websocket][iclforge]") {
    namespace ss = iclforge::sendspin;
    constexpr int kFrameCount = 20;
    const fs::path scratch =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_opus_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const SurroundProgramme made = surround_programme(kFrameCount);

    // The sink's dynamic pairing code and the streams it was started with, from its log.
    class Log final : public testsink::SinkLog {
       public:
        void line(std::string_view text) override {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(text);
            const std::size_t at = text.find("PAIRING CODE ");
            if (at == std::string_view::npos) {
                return;
            }
            std::string digits;
            for (const char c : text.substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            code_ = digits;
        }
        std::optional<std::string> code() {
            const std::lock_guard lock(mutex_);
            return code_;
        }
        bool saw(std::string_view text) {
            const std::lock_guard lock(mutex_);
            return std::ranges::any_of(lines_, [&](const std::string& line) {
                return line.find(text) != std::string::npos;
            });
        }

       private:
        std::mutex mutex_;
        std::optional<std::string> code_;
        std::vector<std::string> lines_;
    };
    Log log;
    testsink::SinkOptions options;
    options.name = "opus";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = scratch / "state";
    options.output_directory = scratch / "out";
    options.advertise = false;
    // PCM first, as a board lists it: Opus is sent only because the sink is set to it.
    options.codecs = {m::Codec::kPcm, m::Codec::kOpus};
    options.layout = "2.0";
    options.max_coded_channels = 2;
    auto started = testsink::Sink::start(options, log);
    REQUIRE(started.has_value());
    const std::unique_ptr<testsink::Sink> sink = std::move(*started);

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("2026-10-11"); }};
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Test Hearth", store,
                                         iclforge::hearth::NetworkSinksOptions{.browse = false}};
    REQUIRE(sinks.started());
    const auto find = [&](const std::string& id) -> std::optional<iclforge::hearth::SinkFacts> {
        const auto status = sinks.status();
        for (const iclforge::hearth::SinkFacts& facts : status.sinks) {
            if (facts.id == id) {
                return facts;
            }
        }
        return std::nullopt;
    };
    sinks.on_found(ss::discovery::Service{.instance = "opus",
                                          .host = "127.0.0.1",
                                          .addresses = {"127.0.0.1"},
                                          .port = sink->port(),
                                          .txt = {{.key = "path", .value = "/sendspin"}}});
    REQUIRE(eventually([&] {
        const auto facts = find("opus");
        return facts && facts->link == iclforge::hearth::SinkLink::kConnected &&
               facts->iclforge_support;
    }));
    sinks.pair_sink("opus");
    REQUIRE(eventually([&] { return log.code().has_value(); }));
    sinks.submit_pairing_code("opus", *log.code());
    REQUIRE(eventually([&] {
        const auto facts = find("opus");
        return facts && facts->pair_state == iclforge::hearth::PairState::kPaired &&
               facts->link == iclforge::hearth::SinkLink::kConnected && facts->iclforge_state &&
               facts->clock_converged;
    }));
    REQUIRE(sinks.set_sink_opus_fallback("opus", true));

    const std::string group_id = sinks.create_group("Opus");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, "opus");
    const std::shared_ptr<ss::Group> group = sinks.group(group_id);
    REQUIRE(group != nullptr);

    const std::vector<std::byte>& programme = made.bytes;
    const ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(master.has_value());
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group](const std::string&) { return group; },
                              [&sinks](const iclforge::hearth::GroupPlanRequest& request) {
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));

    // Every frame of the programme, less what Opus's own packet boundary holds at the end.
    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] { return sink->totals().frames >= expected_frames - 4800; }));
    CHECK(log.saw("stream Opus 48000 Hz"));
    CHECK_FALSE(log.saw("stream PCM"));
    CHECK(sink->totals().bursts == 0);
    // And it is the programme: Opus-coded audio of the render, not silence.
    const auto wav = read_wav_holding(only_file(scratch / "out", "stream-", ".wav").string(),
                                      expected_frames - 4800);
    REQUIRE(wav.has_value());
    REQUIRE(wav->channels.size() == 2);
    double energy = 0.0;
    for (const std::vector<float>& channel : wav->channels) {
        for (const float sample : channel) {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }
    CHECK(energy > 1.0);
}

// planning/ac4.md, I2: "AC-4 decodes to PCM for every output, and is sent as a
// bitstream only over the extension role ... to sinks that decode it." One
// AC-4 item from the Engine to a group of a player@v1 sink and an
// _iclforge_player@v1 sink that lists AC-4 (D11's test sink): the first takes
// the Engine's decode as PCM, and the second the item's own sync frames, a
// burst each, which it decodes itself - to what iclforge::ac4::Decoder makes of them,
// sample for sample, every burst's frame placed on the group's one timeline.
TEST_CASE(
    "engine: an AC-4 item reaches a group as PCM, and as a bitstream for the sink that decodes it",
    "[hearth][group][websocket][ac4]") {
    constexpr std::size_t kFrames = 24;  // DEE's 2.0 tones at frame_rate_index 13: 1.05 s
    const fs::path scratch =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_ac4_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const std::vector<std::byte> programme = ac4_frames(
        fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4", kFrames);
    TwoSinkGroup sinks(scratch);
    const EngineStatus finished =
        play_to_group(sinks.group, programme, iclforge::hearth::DecoderSettings{});
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    const std::uint64_t expected = static_cast<std::uint64_t>(kFrames) * 2048;
    CHECK(finished.history.front().frames == expected);

    const auto played_pcm = [&] { return sinks.pcm_sink->totals().frames; };
    const auto played_bursts = [&] { return sinks.burst_sink->totals().bursts; };
    REQUIRE(eventually([&] { return played_pcm() >= expected && played_bursts() >= kFrames; }));
    CHECK(played_pcm() == expected);
    CHECK(played_bursts() == kFrames);
    CHECK(sinks.burst_sink->totals().burst_frames == expected);

    // The burst sink's WAV: the frames as iclforge::ac4::Decoder decodes them, placed
    // on its layout - the test sink's own, 7.1.4 - by the bed of their
    // speakers, as BurstOutput places them.
    const std::optional<iclforge::render::OutputLayout> layout =
        iclforge::render::OutputLayout::parse(testsink::SinkOptions{}.layout);
    REQUIRE(layout.has_value());
    iclforge::ac3::io::WavStreamReader wav;
    REQUIRE(wav.open(only_file(scratch / "burst" / "out", "bursts-", ".wav").string()).has_value());
    REQUIRE(static_cast<std::size_t>(wav.channels()) == layout->slots());
    std::vector<std::vector<float>> heard(layout->slots(),
                                          std::vector<float>(iclforge::ac3::kSamplesPerBlock));
    std::vector<std::span<float>> heard_spans(heard.begin(), heard.end());
    std::vector<std::array<float, iclforge::ac3::kSamplesPerBlock>> rendered(layout->slots());
    std::vector<std::span<float>> rendered_spans;
    for (std::array<float, iclforge::ac3::kSamplesPerBlock>& slot : rendered) {
        rendered_spans.emplace_back(slot);
    }
    iclforge::ac4::Decoder decoder;
    iclforge::render::LayoutRenderer renderer(*layout);
    std::uint64_t compared = 0;
    std::uint64_t different = 0;
    const auto scanned = iclforge::ac4::scan(programme);
    for (const iclforge::ac4::SyncFrame& frame : scanned.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        renderer.set_bed(testsink::ac4_bed(pcm.speakers));
        std::vector<std::span<const float>> channels(pcm.channels.size());
        for (std::size_t at = 0; at < pcm.samples; at += iclforge::ac3::kSamplesPerBlock) {
            const std::size_t m =
                std::min<std::size_t>(iclforge::ac3::kSamplesPerBlock, pcm.samples - at);
            for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
                channels[c] = std::span<const float>(pcm.channels[c]).subspan(at, m);
            }
            renderer.render(iclforge::ac3::PcmBlock{.index = 0,
                                          .blocks = 1,
                                          .channels = channels,
                                          .objects = {},
                                          .object_indices = {},
                                          .object_metadata = nullptr},
                            false, 1.0F, rendered_spans);
            const auto got = wav.read_planar(heard_spans, m);
            REQUIRE(got.has_value());
            REQUIRE(*got == m);
            for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
                for (std::size_t t = 0; t < m; ++t) {
                    different += rendered[slot][t] == heard[slot][t] ? 0U : 1U;
                }
            }
            compared += m;
        }
    }
    CHECK(compared == expected);
    CHECK(different == 0);
    // Every burst puts the stream's first frame at the same local time, within
    // 1 ms: each is placed at its own frame's start.
    const std::vector<double> times =
        first_frame_times(only_file(scratch / "burst" / "out", "bursts-", ".times.csv"));
    REQUIRE(times.size() == kFrames);
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}

// A sink decodes the presentation it would choose with no preferences, so a
// presentation the listener has chosen that it would not reaches the group as
// PCM alone: the _iclforge_player@v1 sink is sent nothing.
TEST_CASE("engine: an AC-4 presentation a sink would not choose reaches a group as PCM alone",
          "[hearth][group][websocket][ac4]") {
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_ac4_choice_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    // E6's broadcast stream: presentation 2 is music and effects with the
    // German dialogue, not the one a decoder with no preferences selects.
    const std::vector<std::byte> programme =
        read_bytes(fs::path{AC4_GOLDEN_DIR} / "presentations" / "encoder-broadcast.ac4");
    TwoSinkGroup sinks(scratch);
    iclforge::hearth::DecoderSettings settings;
    settings.ac4.presentation_id = 2;
    const EngineStatus finished = play_to_group(sinks.group, programme, settings);
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    const std::uint64_t expected = finished.history.front().expected_frames;
    CHECK(finished.history.front().frames == expected);
    REQUIRE(eventually([&] { return sinks.pcm_sink->totals().frames >= expected; }));
    CHECK(sinks.pcm_sink->totals().frames == expected);
    CHECK(sinks.burst_sink->totals().burst_streams == 0U);
    CHECK(sinks.burst_sink->totals().bursts == 0U);
}

// ---- karaoke (planning/karaoke-phase2.md, W7: a sink that can decode, decodes) ------------------
//
// A karaoke AC-3 stream (bsmod 7, 3/2 and the LFE) played to a group of two boards: one takes the
// karaoke keys, so it is sent the stream as it is and told how to reproduce it; the other does not,
// so it is sent PCM decoded here with the reproduction applied. Both put the vocals where Annex C
// does (L, C, R and the LFE, the surrounds silent), which is what makes a group one reproduction.

namespace {

// `count` AC-3 syncframes of a karaoke 3/2+LFE stream: a tone in each channel, each its own pitch
// and level, so that a reproduction has something to move. Only the first kEncoded are encoded: a
// longer programme is those again end to end (the frames stand alone), so that a case that needs
// one that plays for minutes does not pay the encoder for them.
struct KaraokeProgramme {
    std::vector<std::vector<std::byte>> units;
    std::vector<std::byte> bytes;
};

constexpr int kEncoded = 150;

KaraokeProgramme karaoke_programme(int count) {
    iclforge::ac3::EncoderConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 448;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    config.info.bsmod = iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke;
    iclforge::ac3::FrameEncoder encoder{config};
    // Coded order: L, C, R, Ls, Rs, then the LFE.
    constexpr std::array<double, 6> kHz = {440.0, 800.0, 620.0, 1030.0, 1270.0, 90.0};
    constexpr std::array<double, 6> kLevel = {0.20, 0.10, 0.20, 0.25, 0.15, 0.10};
    KaraokeProgramme out;
    for (int f = 0; f < count; ++f) {
        if (f >= kEncoded) {
            const std::vector<std::byte> again = out.units[static_cast<std::size_t>(f % kEncoded)];
            out.units.push_back(again);
            out.bytes.insert(out.bytes.end(), again.begin(), again.end());
            continue;
        }
        std::vector<std::vector<float>> by_channel;
        for (int channel = 0; channel < encoder.channel_count(); ++channel) {
            std::vector<float> samples(iclforge::ac3::kSamplesPerFrame);
            for (std::size_t n = 0; n < samples.size(); ++n) {
                const double t = static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0;
                samples[n] = static_cast<float>(
                    kLevel[static_cast<std::size_t>(channel)] *
                    std::sin(2.0 * std::numbers::pi * kHz[static_cast<std::size_t>(channel)] * t));
            }
            by_channel.push_back(std::move(samples));
        }
        const std::vector<std::span<const float>> views(by_channel.begin(), by_channel.end());
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.units.push_back(*frame);
        out.bytes.insert(out.bytes.end(), frame->begin(), frame->end());
    }
    return out;
}

// A sink's log: its pairing code, and every line, in order.
class RecordingLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(text);
        const std::size_t at = text.find("PAIRING CODE ");
        if (at == std::string_view::npos) {
            return;
        }
        std::string digits;
        for (const char c : text.substr(at + 13)) {
            if (c >= '0' && c <= '9') {
                digits.push_back(c);
            }
        }
        code_ = digits;
    }
    std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        return code_;
    }
    [[nodiscard]] std::vector<std::string> lines() {
        const std::lock_guard lock(mutex_);
        return lines_;
    }
    [[nodiscard]] bool has(std::string_view needle) { return count(needle) > 0; }
    [[nodiscard]] std::size_t count(std::string_view needle) {
        const std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::ranges::count_if(lines_, [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        }));
    }

   private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
    std::optional<std::string> code_;
};

void pair_through(iclforge::hearth::NetworkSinks& sinks, const std::string& id, RecordingLog& log) {
    REQUIRE(eventually([&] {
        const auto facts = find_sink(sinks, id);
        return facts && facts->link == iclforge::hearth::SinkLink::kConnected &&
               facts->iclforge_support;
    }));
    sinks.pair_sink(id);
    REQUIRE(eventually([&] { return log.code().has_value(); }));
    sinks.submit_pairing_code(id, *log.code());
    REQUIRE(eventually([&] {
        const auto facts = find_sink(sinks, id);
        return facts && facts->pair_state == iclforge::hearth::PairState::kPaired &&
               facts->link == iclforge::hearth::SinkLink::kConnected && facts->iclforge_state &&
               facts->clock_converged;
    }));
}

// Mean square of the slot of `layout` named `name` in `channels`, or -1 for a name it has not.
double slot_energy(const std::vector<std::vector<float>>& channels,
                   const iclforge::render::OutputLayout& layout, std::string_view name) {
    for (std::size_t slot = 0; slot < layout.slots() && slot < channels.size(); ++slot) {
        std::array<char, 32> text{};
        layout.slot_name(slot, text);
        if (name != text.data()) {
            continue;
        }
        double sum = 0.0;
        for (const float sample : channels[slot]) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return channels[slot].empty() ? 0.0 : sum / static_cast<double>(channels[slot].size());
    }
    return -1.0;
}

}  // namespace

// The two boards, the group, the Engine and the programme of the tests below: "decodes" takes the
// karaoke keys (and lists PCM too, as a board does), "legacy" does not and takes PCM at 5.1.
struct KaraokeGroup {
    fs::path scratch;
    // The item's path: a real one under the scratch folder, so that a lyrics file can sit beside it.
    std::string item_path;
    KaraokeProgramme made;
    RecordingLog decodes_log;
    RecordingLog legacy_log;
    std::unique_ptr<testsink::Sink> decodes;
    std::unique_ptr<testsink::Sink> legacy;
    iclforge::hearth::MemorySettingsStore settings;
    std::unique_ptr<iclforge::hearth::PairingStore> store;
    std::unique_ptr<iclforge::hearth::NetworkSinks> sinks;
    std::string group_id;
    std::shared_ptr<iclforge::sendspin::Group> group;
    std::unique_ptr<Engine> engine;
    iclforge::render::OutputLayout master = iclforge::render::OutputLayout::stereo();

    KaraokeGroup(const std::string& name, int frames, const iclforge::hearth::DecoderSettings& wanted,
                 bool decodes_takes_levels = true, std::uint32_t lyrics_bytes = 0,
                 std::vector<std::string> decodes_roles = {},
                 std::vector<std::string> decodes_requests = {})
        : scratch(fs::path{ICLFORGE_TEST_SCRATCH_DIR} / (name + "_" + scratch_pid_suffix())),
          item_path((fs::path{ICLFORGE_TEST_SCRATCH_DIR} / (name + "_" + scratch_pid_suffix()) / "programme.ac3")
                         .string()),
          made(karaoke_programme(frames)) {
        namespace ss = iclforge::sendspin;
        fs::remove_all(scratch);
        const auto make_sink = [&](const std::string& id, bool karaoke, RecordingLog& log,
                                   std::vector<std::int32_t> widths, bool levels = true,
                                   std::vector<std::string> roles = {},
                                   std::vector<std::string> requests = {}) {
            testsink::SinkOptions options;
            options.name = id;
            options.address = "127.0.0.1";
            options.port = 0;
            options.state_directory = scratch / id / "state";
            options.output_directory = scratch / id / "out";
            options.advertise = false;
            options.codecs = {m::Codec::kPcm};
            options.layout = "5.1";
            options.karaoke = karaoke;
            options.karaoke_levels = levels;
            options.lyrics_max_bytes = lyrics_bytes;
            options.other_roles = std::move(roles);
            options.requests = std::move(requests);
            options.pcm_channels = std::move(widths);
            auto started = testsink::Sink::start(options, log);
            REQUIRE(started.has_value());
            return std::move(*started);
        };
        decodes = make_sink("decodes", true, decodes_log, {2, 6}, decodes_takes_levels,
                            std::move(decodes_roles), std::move(decodes_requests));
        legacy = make_sink("legacy", false, legacy_log, {6});

        store = std::make_unique<iclforge::hearth::PairingStore>(
            settings, [] { return std::string("2026-10-11"); });
        const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
        REQUIRE(identity.has_value());
        sinks = std::make_unique<iclforge::hearth::NetworkSinks>(
            *identity, "Test Hearth", *store, iclforge::hearth::NetworkSinksOptions{.browse = false});
        REQUIRE(sinks->started());
        for (const testsink::Sink* sink : {decodes.get(), legacy.get()}) {
            sinks->on_found(
                ss::discovery::Service{.instance = sink == decodes.get() ? "decodes" : "legacy",
                                       .host = "127.0.0.1",
                                       .addresses = {"127.0.0.1"},
                                       .port = sink->port(),
                                       .txt = {{.key = "path", .value = "/sendspin"}}});
        }
        pair_through(*sinks, "decodes", decodes_log);
        pair_through(*sinks, "legacy", legacy_log);

        group_id = sinks->create_group("Karaoke");
        REQUIRE_FALSE(group_id.empty());
        sinks->add_group_member(group_id, "decodes");
        sinks->add_group_member(group_id, "legacy");
        group = sinks->group(group_id);
        REQUIRE(group != nullptr);

        const std::vector<std::byte>* programme = &made.bytes;
        const ItemLoader loader =
            [programme, item = item_path](const std::string& path) -> std::expected<LoadedItem, std::string> {
            if (path != item) {
                return std::unexpected("no such file: " + path);
            }
            return LoadedItem{.bytes = *programme};
        };
        const auto layout = iclforge::render::OutputLayout::parse("5.1");
        REQUIRE(layout.has_value());
        master = *layout;
        iclforge::hearth::NetworkSinks* const planner = sinks.get();
        EngineOutputs outputs{
            .group = iclforge::hearth::make_group_sink(
                [held = group](const std::string&) { return held; },
                [planner](const iclforge::hearth::GroupPlanRequest& request) {
                    return planner->plan_group(request);
                },
                [planner](const std::string& id, const iclforge::hearth::KaraokeChoice& choice) {
                    planner->apply_karaoke(id, choice);
                },
                [planner](const std::string& id, const iclforge::hearth::LyricsUpdate& update) {
                    planner->apply_lyrics(id, update);
                })};
        engine = std::make_unique<Engine>(std::move(outputs), loader, master, wanted,
                                          EngineTiming{.period = 5ms, .budget = 4800});
        engine->set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                         .follow_sink = true,
                                                         .group_name = group_id,
                                                         .group_ready = true});
        engine->add({QueueItem{.path = item_path, .title = "Karaoke programme"}});
    }
    KaraokeGroup(const KaraokeGroup&) = delete;
    KaraokeGroup& operator=(const KaraokeGroup&) = delete;

    ~KaraokeGroup() {
        // The engine holds the group, and a group must not outlive its host.
        engine.reset();
        group.reset();
    }

    // The engine's own account: a wait that never came true while the programme had already
    // ended is not a board that was sent less than it should have been.
    [[nodiscard]] std::string engine_line() {
        const EngineStatus status = engine->status();
        std::string out = "the engine: " + std::string(iclforge::hearth::describe(status.state)) +
                          ", current item " +
                          (status.current == iclforge::hearth::Queue::kNone
                               ? std::string("none")
                               : std::to_string(status.current)) +
                          ", " + std::to_string(status.history.size()) + " in the history";
        for (const iclforge::hearth::PlayedItem& played : status.history) {
            out += " [delivered " + std::to_string(played.frames) + " of " +
                   std::to_string(played.expected_frames) + " frames]";
        }
        out += ", note \"" + status.note + "\", error \"" + status.error + "\", output_reason \"" +
               status.output_reason + "\"\n";
        return out;
    }

    // The engine's, then what each board holds and what each side logged: for a wait that never
    // came true, which says whether a board was sent less than the item or the item never reached
    // it.
    [[nodiscard]] std::string account() {
        std::string out = engine_line();
        out += "the boards' totals and the trails of both sides:\n";
        const auto totals = [&out](const char* name, const testsink::Sink& sink) {
            const testsink::Sink::Totals held = sink.totals();
            out += std::string("  ") + name + ": connections " + std::to_string(held.connections) +
                   ", streams " + std::to_string(held.streams) + ", chunks " +
                   std::to_string(held.chunks) + ", frames " + std::to_string(held.frames) +
                   ", burst streams " + std::to_string(held.burst_streams) + ", bursts " +
                   std::to_string(held.bursts) + "\n";
        };
        totals("decodes", *decodes);
        totals("legacy", *legacy);
        for (const std::string& line : sinks->take_log()) {
            out += "  host: " + line + "\n";
        }
        for (const std::string& line : decodes_log.lines()) {
            out += "  decodes: " + line + "\n";
        }
        for (const std::string& line : legacy_log.lines()) {
            out += "  legacy: " + line + "\n";
        }
        return out;
    }

    // Waits for `done` as eventually() does, and says what each board holds when it never comes.
    void await(const std::function<bool()>& done) {
        if (!eventually(done)) {
            FAIL(account());
        }
    }

    // The programme to its end, the `plays`th time.
    void play_through(std::size_t plays = 1) {
        engine->play();
        REQUIRE(eventually([&] {
            const EngineStatus status = engine->status();
            return status.state == TransportState::kStopped && status.history.size() >= plays;
        }));
        const EngineStatus finished = engine->status();
        INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                               << " / error: " << finished.error);
        REQUIRE(finished.history.size() == plays);
    }

    // The programme ended by the case rather than by its end, for a case that changes something
    // while it plays: it is still playing (the changes were made inside it) and a stop leaves it in
    // the history, as its own end does.
    void stop_playing() {
        INFO("before the stop, " << engine_line());
        REQUIRE(engine->status().state == TransportState::kPlaying);
        engine->stop();
        REQUIRE(eventually([&] { return engine->status().state == TransportState::kStopped; }));
        REQUIRE(engine->status().history.size() == 1);
    }
};

TEST_CASE("engine: a karaoke stream is one reproduction on a board that decodes it and one sent PCM",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 30;
    // The singer's default: Annex C's aware reproduction, which a karaoke player gives.
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup rig("hearth_engine_group_karaoke", kFrameCount, wanted);
    rig.play_through();

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    rig.await([&] {
        return rig.legacy->totals().frames >= expected_frames &&
               rig.decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    });
    // The board that takes the keys is sent the stream as it is, and no PCM; the other is sent PCM
    // and no burst.
    CHECK(rig.decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(rig.decodes->totals().frames == 0);
    CHECK(rig.legacy->totals().bursts == 0);
    CHECK(rig.legacy->totals().frames == expected_frames);
    // It was told, before the stream, as the singer asked.
    CHECK(rig.decodes_log.has("applied (karaoke aware, vocals both)"));
    // (Its log names the scratch folder, which has the word in it: the line has the choice.)
    CHECK_FALSE(rig.legacy_log.has("(karaoke "));

    // What each played: the host's own decode of the programme as asked, and as coded.
    const iclforge::render::OutputLayout& master = rig.master;
    const auto decode_with = [&](iclforge::ac3::KaraokeReproduction reproduction) {
        iclforge::hearth::DecoderSettings reference_settings;
        reference_settings.karaoke = reproduction;
        iclforge::hearth::StreamDecoder reference{master, 48000, reference_settings};
        std::vector<std::vector<float>> got(master.slots());
        const auto deliver = [&got](std::span<const std::span<const float>> slots, std::size_t frames) {
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                got[slot].insert(got[slot].end(), slots[slot].begin(),
                                 slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
            }
        };
        for (const auto& unit : rig.made.units) {
            REQUIRE(reference.decode(unit, deliver).has_value());
        }
        reference.finish(deliver);
        return got;
    };
    const auto want = decode_with(iclforge::ac3::KaraokeReproduction::kAware);
    const auto coded = decode_with(iclforge::ac3::KaraokeReproduction::kOff);
    REQUIRE(want[0].size() >= expected_frames - 1536);

    // The board that decodes writes its own decode as bursts-*.wav; the other's PCM is stream-*.wav.
    const auto decoded_wav = read_wav_holding(
        only_file(rig.scratch / "decodes" / "out", "bursts-", ".wav").string(), want[0].size());
    const auto pcm_wav = read_wav_holding(
        only_file(rig.scratch / "legacy" / "out", "stream-", ".wav").string(), want[0].size());
    REQUIRE(decoded_wav.has_value());
    REQUIRE(pcm_wav.has_value());
    REQUIRE(decoded_wav->channels.size() == 6);
    REQUIRE(pcm_wav->channels.size() == 6);

    for (const auto* wav : {&*decoded_wav, &*pcm_wav}) {
        double worst = 0.0;
        for (std::size_t slot = 0; slot < 6; ++slot) {
            REQUIRE(wav->channels[slot].size() >= want[slot].size());
            for (std::size_t frame = 0; frame < want[slot].size(); ++frame) {
                worst = std::max(worst, std::abs(static_cast<double>(wav->channels[slot][frame]) -
                                                 static_cast<double>(want[slot][frame])));
            }
        }
        // Each is the aware reproduction, within a 16-bit step of the host's own.
        CHECK(worst < 2.0 / 32768.0);
        // The vocals moved off the surrounds, which as coded carry them.
        CHECK(slot_energy(wav->channels, master, "Ls") < 1e-9);
        CHECK(slot_energy(wav->channels, master, "Rs") < 1e-9);
        CHECK(slot_energy(wav->channels, master, "C") > 1e-5);
    }
    // That is not what the stream plays as coded, which is what the test would see without it.
    CHECK(slot_energy(coded, master, "Ls") > 1e-4);
}

TEST_CASE("engine: a board that has applied the karaoke settings is not told them again",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 10;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup rig("hearth_engine_group_karaoke_twice", kFrameCount, wanted);
    rig.play_through(1);
    REQUIRE(eventually([&] { return rig.decodes_log.has("applied (karaoke aware, vocals both)"); }));
    CHECK(rig.decodes_log.count("applied (karaoke") == 1);
    // The board has reported the revision it applied by the time the item ends.
    REQUIRE(eventually([&] {
        const auto facts = find_sink(*rig.sinks, "decodes");
        return facts && facts->iclforge_state && facts->intended_settings &&
               facts->iclforge_state->settings_revision == facts->intended_settings->revision;
    }));

    // The same item again opens the group again and plans it again, and the board is not sent
    // another revision for what it already runs.
    rig.play_through(2);
    CHECK(rig.decodes_log.count("applied (karaoke") == 1);

    // Another choice is told.
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV2;
    rig.engine->set_decoder_settings(wanted);
    rig.play_through(3);
    REQUIRE(eventually([&] { return rig.decodes_log.has("applied (karaoke capable, vocals v2)"); }));
    CHECK(rig.decodes_log.count("applied (karaoke") == 2);
}

TEST_CASE("engine: a karaoke stream played as coded asks nothing of the board that decodes it",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 20;
    iclforge::hearth::DecoderSettings as_coded;
    as_coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    KaraokeGroup rig("hearth_engine_group_karaoke_coded", kFrameCount, as_coded);
    rig.play_through();
    rig.await([&] {
        return rig.decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount) &&
               rig.legacy->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    });
    // The stream is as every board plays it without a word, so both are sent it as it is, and
    // neither is told anything: nothing is decoded here for one of them.
    CHECK(rig.decodes->totals().frames == 0);
    CHECK(rig.legacy->totals().frames == 0);
    CHECK_FALSE(rig.decodes_log.has("(karaoke "));
    CHECK_FALSE(rig.legacy_log.has("(karaoke "));
}

// The host's own decode of the programme under `settings`, as the layout `master` plays it.
static std::vector<std::vector<float>> host_decode(const KaraokeGroup& rig,
                                                   const iclforge::hearth::DecoderSettings& settings) {
    iclforge::hearth::StreamDecoder reference{rig.master, 48000, settings};
    std::vector<std::vector<float>> got(rig.master.slots());
    const auto deliver = [&got](std::span<const std::span<const float>> slots, std::size_t frames) {
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            got[slot].insert(got[slot].end(), slots[slot].begin(),
                             slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
        }
    };
    for (const auto& unit : rig.made.units) {
        REQUIRE(reference.decode(unit, deliver).has_value());
    }
    reference.finish(deliver);
    return got;
}

// The worst sample distance between a wav's slots and `want`'s.
static double worst_distance(const std::vector<std::vector<float>>& channels,
                             const std::vector<std::vector<float>>& want) {
    double worst = 0.0;
    for (std::size_t slot = 0; slot < want.size(); ++slot) {
        REQUIRE(channels[slot].size() >= want[slot].size());
        for (std::size_t frame = 0; frame < want[slot].size(); ++frame) {
            worst = std::max(worst, std::abs(static_cast<double>(channels[slot][frame]) -
                                             static_cast<double>(want[slot][frame])));
        }
    }
    return worst;
}

TEST_CASE("engine: a part level is one level on the board that decodes it and on the one sent PCM",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kBoth;
    wanted.karaoke_melody_db = -12.0;
    wanted.karaoke_vocal1_db = 4.0;
    KaraokeGroup rig("hearth_engine_group_karaoke_levels", kFrameCount, wanted);
    rig.play_through();

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    rig.await([&] {
        return rig.legacy->totals().frames >= expected_frames &&
               rig.decodes->totals().bursts >= static_cast<std::uint64_t>(kFrameCount);
    });
    // The board that takes the keys is sent the stream as it is and told the levels with the
    // choice; the other is sent PCM that has them applied.
    CHECK(rig.decodes->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(rig.decodes->totals().frames == 0);
    CHECK(rig.legacy->totals().bursts == 0);
    CHECK(rig.decodes_log.has("applied (karaoke capable, vocals both, melody -12 dB, vocal 1 +4 dB)"));
    CHECK_FALSE(rig.legacy_log.has("(karaoke "));

    const auto want = host_decode(rig, wanted);
    iclforge::hearth::DecoderSettings table = wanted;
    table.karaoke_melody_db = 0.0;
    table.karaoke_vocal1_db = 0.0;
    const auto at_table = host_decode(rig, table);
    REQUIRE(want[0].size() >= expected_frames - 1536);

    const auto decoded_wav = read_wav_holding(
        only_file(rig.scratch / "decodes" / "out", "bursts-", ".wav").string(), want[0].size());
    const auto pcm_wav = read_wav_holding(
        only_file(rig.scratch / "legacy" / "out", "stream-", ".wav").string(), want[0].size());
    REQUIRE(decoded_wav.has_value());
    REQUIRE(pcm_wav.has_value());
    REQUIRE(decoded_wav->channels.size() == 6);
    REQUIRE(pcm_wav->channels.size() == 6);
    // Each is the host's own decode with the levels, within a 16-bit step.
    CHECK(worst_distance(decoded_wav->channels, want) < 2.0 / 32768.0);
    CHECK(worst_distance(pcm_wav->channels, want) < 2.0 / 32768.0);
    // And that is not the table, which is what the test would see if the levels went nowhere:
    // the melody's centre is 12 dB down.
    const double centre = slot_energy(decoded_wav->channels, rig.master, "C");
    const double centre_at_table = slot_energy(at_table, rig.master, "C");
    REQUIRE(centre > 0.0);
    CHECK(centre < centre_at_table * 0.5);
}

TEST_CASE("engine: a board that takes the karaoke keys and not the levels is sent PCM when one is moved",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kBoth;
    wanted.karaoke_melody_db = -6.0;
    KaraokeGroup rig("hearth_engine_group_karaoke_levels_old", kFrameCount, wanted,
                     /*decodes_takes_levels=*/false);
    rig.play_through();
    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    rig.await([&] {
        return rig.legacy->totals().frames >= expected_frames &&
               rig.decodes->totals().frames >= expected_frames;
    });
    // Both are sent PCM: the board that would have played the table is not sent the stream.
    CHECK(rig.decodes->totals().bursts == 0);
    CHECK(rig.legacy->totals().bursts == 0);
    CHECK_FALSE(rig.decodes_log.has("(karaoke "));

    const auto want = host_decode(rig, wanted);
    const auto first = read_wav_holding(
        only_file(rig.scratch / "decodes" / "out", "stream-", ".wav").string(), want[0].size());
    const auto second = read_wav_holding(
        only_file(rig.scratch / "legacy" / "out", "stream-", ".wav").string(), want[0].size());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(worst_distance(first->channels, want) < 2.0 / 32768.0);
    CHECK(worst_distance(second->channels, want) < 2.0 / 32768.0);
}

TEST_CASE("engine: a part level changed while a karaoke item plays reaches the board that decodes it",
          "[hearth][group][websocket][iclforge][karaoke]") {
    // Four minutes, and ended by the case: see the case below on why not 4.8 s.
    constexpr int kFrameCount = 7500;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kBoth;
    KaraokeGroup rig("hearth_engine_group_karaoke_levels_change", kFrameCount, wanted);
    rig.engine->play();
    rig.await([&] { return rig.decodes->totals().bursts >= 20; });
    CHECK(rig.decodes_log.has("applied (karaoke capable, vocals both)"));
    CHECK_FALSE(rig.decodes_log.has("melody"));

    // The singer pulls the melody down: told to the board, not by starting the item again.
    wanted.karaoke_melody_db = -9.0;
    rig.engine->set_decoder_settings(wanted);
    rig.await([&] {
        return rig.decodes_log.has("applied (karaoke capable, vocals both, melody -9 dB)");
    });
    // Taking it back to the table clears it: settings replace the board's whole.
    wanted.karaoke_melody_db = 0.0;
    rig.engine->set_decoder_settings(wanted);
    rig.await([&] { return rig.decodes_log.count("applied (karaoke capable, vocals both)") >= 2; });
    // A level of a vocal the choice leaves out is not sent.
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV1;
    wanted.karaoke_vocal2_db = -6.0;
    rig.engine->set_decoder_settings(wanted);
    rig.await([&] { return rig.decodes_log.has("applied (karaoke capable, vocals v1)"); });
    CHECK_FALSE(rig.decodes_log.has("vocal 2"));
    CHECK_FALSE(rig.legacy_log.has("(karaoke "));
    rig.stop_playing();
}

TEST_CASE("engine: a change of vocals while a karaoke item plays reaches the board that decodes it",
          "[hearth][group][websocket][iclforge][karaoke]") {
    // The choice is changed in the middle of a programme, and a programme plays in real time. At
    // 150 frames (4.8 s) a loaded machine got to the second change after the item had ended (seen:
    // the engine stopped with every frame delivered, the first change applied and the second
    // never), and a choice made while nothing plays is told to no board. So the programme is four
    // minutes, which the waits below (a minute each at the most) cannot outlast, and the case ends
    // it itself.
    constexpr int kFrameCount = 7500;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kBoth;
    KaraokeGroup rig("hearth_engine_group_karaoke_change", kFrameCount, wanted);
    rig.engine->play();
    rig.await([&] { return rig.decodes->totals().bursts >= 20; });
    CHECK(rig.decodes_log.has("applied (karaoke capable, vocals both)"));
    CHECK_FALSE(rig.decodes_log.has("vocals v1"));

    // The singer picks the first vocal: told to the board, not by starting the item again.
    wanted.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV1;
    rig.engine->set_decoder_settings(wanted);
    rig.await([&] { return rig.decodes_log.has("applied (karaoke capable, vocals v1)"); });
    // And going back to "as coded" tells it to play the coded channels.
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    rig.engine->set_decoder_settings(wanted);
    rig.await([&] { return rig.decodes_log.has("applied (karaoke off, vocals v1)"); });
    // It went to the board that decodes and not to the one sent PCM.
    CHECK_FALSE(rig.legacy_log.has("(karaoke "));
    rig.stop_playing();
}

// --- the lyrics beside an item (W7d, planning/karaoke-phase2.md) ----------------------------------

namespace {

using iclforge::sendspin::player::Command;

// The first lyric command of kind `command`, and every one of it, from what a sink was sent.
std::vector<iclforge::sendspin::player::CommandMessage> of_kind(
    const std::vector<iclforge::sendspin::player::CommandMessage>& all, Command command) {
    std::vector<iclforge::sendspin::player::CommandMessage> out;
    for (const auto& message : all) {
        if (message.command == command) {
            out.push_back(message);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("engine: the lyrics beside an item reach the boards that hold them, and where the sound is in them",
          "[hearth][group][websocket][iclforge][lyrics]") {
    constexpr int kFrameCount = 60;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup group("lyrics_group", kFrameCount, wanted, true, 16384);
    fs::create_directories(fs::path(group.item_path).parent_path());
    {
        std::ofstream lrc(fs::path(group.item_path).replace_extension(".lrc"), std::ios::binary);
        lrc << "[ti:Auld Lang Syne]\n[00:00.50]Should auld acquaintance\n[00:01.00]be forgot\n";
    }
    group.play_through();

    // The board that decodes for itself is sent the stream as it is, on the role that carries
    // lyrics; the one sent PCM is on player@v1, which has none, and is sent nothing: nor in the
    // moment before the host has moved it there (the case after this one).
    for (const testsink::Sink* sink : {group.decodes.get()}) {
        const bool got = eventually([&] {
            const auto seen = sink->roles().lyric_commands;
            return !of_kind(seen, Command::kLyrics).empty() && !of_kind(seen, Command::kLyricsPosition).empty();
        });
        const auto all = sink->roles().lyric_commands;
        INFO((sink == group.decodes.get() ? "decodes" : "legacy")
             << ": " << all.size() << " lyric commands, "
             << of_kind(all, Command::kLyrics).size() << " documents, "
             << of_kind(all, Command::kLyricsPosition).size() << " anchors");
        REQUIRE(got);
        // The document first, then anchors for the same item.
        REQUIRE_FALSE(all.empty());
        CHECK(all.front().command == Command::kLyrics);
        const auto documents = of_kind(all, Command::kLyrics);
        CHECK(documents.front().lyrics.text.find("[00:00.50]Should auld acquaintance") != std::string::npos);
        CHECK(documents.front().lyrics.text.find("be forgot") != std::string::npos);
        CHECK(documents.front().lyrics.item >= 1);
        const auto anchors = of_kind(all, Command::kLyricsPosition);
        for (const auto& anchor : anchors) {
            CHECK(anchor.lyrics_position.item == documents.front().lyrics.item);
            CHECK(anchor.lyrics_position.at_us > 0);
            CHECK(anchor.lyrics_position.position_ms >= 0);
            // The item is about two seconds long.
            CHECK(anchor.lyrics_position.position_ms < 3000);
        }
        // The first anchor says the sound is playing, from the start.
        CHECK(anchors.front().lyrics_position.speed == 1.0);
        CHECK(anchors.front().lyrics_position.position_ms < 500);
        // The anchors' times and positions only go forward.
        for (std::size_t i = 1; i < anchors.size(); ++i) {
            CHECK(anchors[i].lyrics_position.at_us >= anchors[i - 1].lyrics_position.at_us);
        }
    }
    {
        const auto sent = group.legacy->roles().lyric_commands;
        INFO("the board sent PCM was sent "
             << sent.size() << " lyric commands (" << of_kind(sent, Command::kLyrics).size()
             << " documents, " << of_kind(sent, Command::kLyricsPosition).size() << " anchors)\n"
             << group.account());
        CHECK(sent.empty());
    }

    // An anchor of an item the group was never given a document for is sent to no one.
    iclforge::hearth::LyricsUpdate stray;
    stray.item = 987654;
    stray.anchor = iclforge::hearth::LyricsUpdate::Anchor{.position_ms = 1000, .at_us = 1, .speed = 1.0};
    group.sinks->apply_lyrics(group.group_id, stray);
    std::this_thread::sleep_for(300ms);
    const auto roles = group.decodes->roles();
    for (const auto& message : roles.lyric_commands) {
        CHECK(message.lyrics.item != 987654);
        CHECK(message.lyrics_position.item != 987654);
    }
}

namespace {

// Holds the host's own thread: the handler of a board's request
// (NetworkSinks::set_request_handler()) runs on it, and what the host is asked to do waits behind
// a handler that does not return.
class HostThreadHold {
   public:
    // The handler to give the request handler: it says it is in, and stays until released.
    void block() {
        std::unique_lock lock(mutex_);
        inside_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }
    [[nodiscard]] bool wait_inside() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 60s, [&] { return inside_; });
    }
    void release() {
        {
            const std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool inside_ = false;
    bool released_ = false;
};

}  // namespace

TEST_CASE("engine: a member the plan sends PCM is sent no lyrics before the host has moved it onto player@v1",
          "[hearth][group][websocket][iclforge][lyrics]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    // The hold is made first and the release after the rig, so that a failed REQUIRE lets the
    // host's thread go before the rig takes the host down, and the hold outlives both.
    HostThreadHold hold;
    // Both boards state lyrics (the fixture gives each the size), and both are on the extension
    // role with their state in: the group is made and nothing has been played. The board that
    // decodes may ask for the vocals, which is how the host's thread is held below.
    KaraokeGroup group("lyrics_moving_group", kFrameCount, wanted, true, 16384, {},
                       {"karaoke_vocals"});
    struct ReleaseOnExit {
        HostThreadHold& held;
        ~ReleaseOnExit() { held.release(); }
    } const release_on_exit{hold};

    group.sinks->set_request_handler(
        [&](const iclforge::hearth::NetworkSinks::SinkRequest& /*request*/) { hold.block(); });
    iclforge::sendspin::player::Request ask;
    ask.decoder.karaoke_vocals = iclforge::sendspin::player::KaraokeVocals::kV1;
    group.decodes->send_request(ask);
    REQUIRE(hold.wait_inside());

    // The plan the engine makes when it opens the item: the board that takes the karaoke keys is
    // sent the stream, the other is decoded for and sent PCM. For that one plan_group() only asks
    // the host to move it onto player@v1 (use_pcm()), and the host's thread does it, which is held;
    // the engine's first pump sends the lyrics straight behind the plan, as here, whichever thread
    // is quicker. (Unheld, the move wins most of the time, and a loaded machine decides the rest.)
    const std::size_t activations = group.legacy_log.count("activated for playback");
    const iclforge::hearth::KaraokeChoice choice{
        .reproduction = iclforge::sendspin::player::Karaoke::kAware};
    const auto variants = group.sinks->plan_group({.group_name = group.group_id,
                                                   .stream = iclforge::audio::BitstreamFormat::kAc3,
                                                   .sample_rate = 48000,
                                                   .coded_channels = 6,
                                                   .karaoke = choice,
                                                   .layout = group.master});
    static_cast<void>(variants);
    iclforge::hearth::LyricsUpdate document;
    document.item = 5;
    document.document = "[00:00.50]Should auld acquaintance\n[00:01.00]be forgot\n";
    group.sinks->apply_lyrics(group.group_id, document);
    iclforge::hearth::LyricsUpdate anchor;
    anchor.item = 5;
    anchor.anchor =
        iclforge::hearth::LyricsUpdate::Anchor{.position_ms = 0, .at_us = 1, .speed = 1.0};
    group.sinks->apply_lyrics(group.group_id, anchor);

    // Now the host moves the member. Whatever was sent it on the role it was leaving came before
    // the activation on that connection, so once the board is activated again nothing more is to
    // come.
    hold.release();
    group.await([&] { return group.legacy_log.count("activated for playback") > activations; });
    // The board that decodes holds them, so that the member sent PCM going without is the plan's
    // doing and not that nothing was sent.
    group.await([&] {
        const auto seen = group.decodes->roles().lyric_commands;
        return !of_kind(seen, Command::kLyrics).empty() &&
               !of_kind(seen, Command::kLyricsPosition).empty();
    });
    const auto sent = group.legacy->roles().lyric_commands;
    INFO("the member sent PCM was sent "
         << sent.size() << " lyric commands (" << of_kind(sent, Command::kLyrics).size()
         << " documents, " << of_kind(sent, Command::kLyricsPosition).size() << " anchors)\n"
         << group.account());
    CHECK(sent.empty());
}

TEST_CASE("engine: an item with no lyrics tells the boards it has none, and a board that takes none is not sent any",
          "[hearth][group][websocket][iclforge][lyrics]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    // No board states a lyrics size, so neither lists the lyric commands; no lyrics file is beside
    // the item.
    KaraokeGroup group("lyrics_none_group", kFrameCount, wanted, true, 0);
    group.play_through();
    // Neither takes them, so neither is sent any.
    CHECK(group.decodes->roles().lyric_commands.empty());
    CHECK(group.legacy->roles().lyric_commands.empty());
}

TEST_CASE("engine: a member's request for the karaoke choice reaches the application with its group and name (D8)",
          "[hearth][group][websocket][iclforge][karaoke]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    // The decoding board lists the vocals and one level it may ask for; the other lists none.
    KaraokeGroup group("request_group", kFrameCount, wanted, true, 0, {}, {"karaoke_vocals", "karaoke_vocal1_db"});

    std::mutex mutex;
    std::vector<iclforge::hearth::NetworkSinks::SinkRequest> heard;
    group.sinks->set_request_handler([&](const iclforge::hearth::NetworkSinks::SinkRequest& request) {
        const std::lock_guard lock(mutex);
        heard.push_back(request);
    });
    const auto seen = [&] {
        const std::lock_guard lock(mutex);
        return heard;
    };

    iclforge::sendspin::player::Request ask;
    ask.decoder.karaoke_vocals = iclforge::sendspin::player::KaraokeVocals::kV1;
    ask.decoder.karaoke = iclforge::sendspin::player::Karaoke::kOff;
    group.decodes->send_request(ask);
    group.await([&] { return !seen().empty(); });
    const auto first = seen();
    REQUIRE(first.size() == 1);
    CHECK(first.front().group_id == group.group_id);
    CHECK(first.front().sink_name == "decodes");
    CHECK_FALSE(first.front().sink_id.empty());
    CHECK(first.front().request.decoder.karaoke_vocals == iclforge::sendspin::player::KaraokeVocals::kV1);
    // The reproduction was not on the sink's list, so it was cut before it left.
    CHECK_FALSE(first.front().request.decoder.karaoke.has_value());

    // The board that lists none sends none.
    iclforge::sendspin::player::Request other;
    other.decoder.karaoke_vocals = iclforge::sendspin::player::KaraokeVocals::kV2;
    group.legacy->send_request(other);
    std::this_thread::sleep_for(300ms);
    CHECK(seen().size() == 1);

    // Cleared, a request reaches no one.
    group.sinks->set_request_handler({});
    group.decodes->send_request(ask);
    std::this_thread::sleep_for(300ms);
    CHECK(seen().size() == 1);
}

TEST_CASE("engine: a member that shows what is playing is told the item and where the sound is (D7)",
          "[hearth][group][websocket][iclforge][metadata]") {
    constexpr int kFrameCount = 60;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    // The decoding board lists metadata@v1 as well; no lyrics file is beside the item, so the title is
    // the one the queue shows.
    KaraokeGroup group("now_playing_group", kFrameCount, wanted, true, 0, {"metadata@v1"});
    group.engine->play();
    const auto shown = [&] { return group.decodes->roles().metadata; };
    // The state while the sound moves, kept from the one read that found it. Before the output has
    // played anything the position is held (speed 0), and it is held again at the item's end; on a
    // slow output (the macOS runners) the held state is the first one a reader sees.
    auto playing = shown();
    group.await([&] {
        playing = shown();
        return playing && playing->title == "Karaoke programme" && playing->progress &&
               playing->progress->playback_speed == 1000;
    });
    REQUIRE(playing.has_value());
    CHECK_FALSE(playing->artist.has_value());
    CHECK_FALSE(playing->album.has_value());
    REQUIRE(playing->progress.has_value());
    CHECK(playing->progress->playback_speed == 1000);
    // The item is about two seconds long, and the position is somewhere in it.
    CHECK(playing->progress->track_duration_ms > 1000);
    CHECK(playing->progress->track_duration_ms < 4000);
    CHECK(playing->progress->track_progress_ms >= 0);
    CHECK(playing->progress->track_progress_ms <= playing->progress->track_duration_ms);
    // The board that lists no metadata role is told nothing.
    CHECK_FALSE(group.legacy->roles().metadata.has_value());

    // The item ends and the output closes: nothing is playing, and the board stops showing it.
    REQUIRE(eventually([&] {
        const EngineStatus status = group.engine->status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    group.await([&] { return !shown().has_value(); });
}

TEST_CASE("engine: a board that lists lyrics is told an item has none, so it drops the last item's",
          "[hearth][group][websocket][iclforge][lyrics]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup group("lyrics_empty_group", kFrameCount, wanted, true, 16384);
    group.play_through();
    REQUIRE(eventually([&] { return !group.decodes->roles().lyric_commands.empty(); }));
    const auto all = group.decodes->roles().lyric_commands;
    const auto documents = of_kind(all, Command::kLyrics);
    REQUIRE_FALSE(documents.empty());
    CHECK(documents.front().lyrics.text.empty());
}

TEST_CASE("engine: a document too big for a board is sent as none, so that it drops the last item's",
          "[hearth][group][websocket][iclforge][lyrics]") {
    constexpr int kFrameCount = 30;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    // The board holds 64 bytes of document, and the item's lyrics are more.
    KaraokeGroup group("lyrics_big_group", kFrameCount, wanted, true, 64);
    fs::create_directories(fs::path(group.item_path).parent_path());
    {
        std::ofstream lrc(fs::path(group.item_path).replace_extension(".lrc"), std::ios::binary);
        lrc << "[00:00.50]Should auld acquaintance be forgot and never brought to mind"
            << '\n'
            << "[00:01.00]Should auld acquaintance be forgot and days of auld lang syne"
            << '\n';
    }
    group.play_through();
    REQUIRE(eventually([&] { return !group.decodes->roles().lyric_commands.empty(); }));
    const auto documents = of_kind(group.decodes->roles().lyric_commands, Command::kLyrics);
    REQUIRE_FALSE(documents.empty());
    CHECK(documents.front().lyrics.text.empty());
}

TEST_CASE("engine: the Network page's facts read a board's reported line against the document it was sent",
          "[hearth][group][websocket][iclforge][lyrics]") {
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup group("lyrics_page_group", 30, wanted, true, 16384);

    // Before anything is sent the board holds nothing, and the page has no count to give.
    const auto before = find_sink(*group.sinks, "decodes");
    REQUIRE(before.has_value());
    CHECK_FALSE(before->lyrics_lines.has_value());
    CHECK(before->lyrics_line_text.empty());

    iclforge::hearth::LyricsUpdate document;
    document.item = 41;
    document.document = "[00:01.00]one\n[00:02.00]two\n[00:03.00]three\n";
    group.sinks->apply_lyrics(group.group_id, document);
    const auto reports_line = [&](std::int32_t line) {
        const auto facts = find_sink(*group.sinks, "decodes");
        return facts && facts->iclforge_state && facts->iclforge_state->lyrics &&
               facts->iclforge_state->lyrics->item == 41 && facts->iclforge_state->lyrics->line == line;
    };
    REQUIRE(eventually([&] { return reports_line(-1); }));
    auto facts = find_sink(*group.sinks, "decodes");
    REQUIRE(facts.has_value());
    CHECK(facts->lyrics_lines == std::size_t{3});
    CHECK(facts->lyrics_line_text.empty());

    // The sound is 2.5 s in: the board is on the second line, and the page can say which.
    iclforge::hearth::LyricsUpdate anchor;
    anchor.item = 41;
    anchor.anchor = iclforge::hearth::LyricsUpdate::Anchor{.position_ms = 2500, .at_us = 1, .speed = 1.0};
    group.sinks->apply_lyrics(group.group_id, anchor);
    REQUIRE(eventually([&] { return reports_line(1); }));
    facts = find_sink(*group.sinks, "decodes");
    REQUIRE(facts.has_value());
    CHECK(facts->lyrics_lines == std::size_t{3});
    CHECK(facts->lyrics_line_text == "two");

    // A new item's document replaces both: the count and the text are the new file's.
    iclforge::hearth::LyricsUpdate next;
    next.item = 42;
    next.document = "[00:00.50]alpha\n[00:01.00]beta\n";
    group.sinks->apply_lyrics(group.group_id, next);
    REQUIRE(eventually([&] {
        const auto now = find_sink(*group.sinks, "decodes");
        return now && now->iclforge_state && now->iclforge_state->lyrics && now->iclforge_state->lyrics->item == 42;
    }));
    facts = find_sink(*group.sinks, "decodes");
    REQUIRE(facts.has_value());
    CHECK(facts->lyrics_lines == std::size_t{2});
    CHECK(facts->lyrics_line_text.empty());
}

TEST_CASE("engine: a board that lost the lyrics is sent the document again before the next anchor",
          "[hearth][group][websocket][iclforge][lyrics]") {
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup group("lyrics_recover_group", 30, wanted, true, 16384);

    const auto item_line = [&](std::int64_t item) -> std::optional<std::int32_t> {
        const auto facts = find_sink(*group.sinks, "decodes");
        if (facts && facts->iclforge_state && facts->iclforge_state->lyrics &&
            facts->iclforge_state->lyrics->item == item) {
            return facts->iclforge_state->lyrics->line;
        }
        return std::nullopt;
    };
    const auto position = [&](std::int64_t item, std::int64_t position_ms) {
        iclforge::hearth::LyricsUpdate update;
        update.item = item;
        update.anchor = iclforge::hearth::LyricsUpdate::Anchor{.position_ms = position_ms, .at_us = 1, .speed = 1.0};
        group.sinks->apply_lyrics(group.group_id, update);
    };

    iclforge::hearth::LyricsUpdate document;
    document.item = 7;
    document.document = "[00:01.00]one\n[00:02.00]two\n[00:03.00]three\n";
    group.sinks->apply_lyrics(group.group_id, document);
    // The anchor comes straight after the document, as the engine's does: before the board has
    // reported the item, which is no reason to send the document again (it was, three times).
    position(7, 1200);
    REQUIRE(eventually([&] { return item_line(7) == std::optional<std::int32_t>{0}; }));
    CHECK(of_kind(group.decodes->roles().lyric_commands, Command::kLyrics).size() == 1);

    // A member that reports the item it was sent is not sent the document again.
    position(7, 1500);
    REQUIRE(eventually([&] { return of_kind(group.decodes->roles().lyric_commands, Command::kLyricsPosition).size() == 2; }));
    CHECK(of_kind(group.decodes->roles().lyric_commands, Command::kLyrics).size() == 1);

    // The board loses its document (a restart of its player, or a reconnection) and says so, once
    // the document has had its time to be reported: a document just sent is not sent again.
    std::this_thread::sleep_for(2200ms);
    group.decodes->forget_lyrics();
    REQUIRE(eventually([&] {
        const auto facts = find_sink(*group.sinks, "decodes");
        return facts && facts->iclforge_state && !facts->iclforge_state->lyrics.has_value();
    }));
    const std::size_t sent = group.decodes->roles().lyric_commands.size();

    // The next anchor, with no document of its own, is sent behind the group's last one.
    position(7, 2500);
    REQUIRE(eventually([&] { return item_line(7) == std::optional<std::int32_t>{1}; }));
    const auto all = group.decodes->roles().lyric_commands;
    REQUIRE(all.size() == sent + 2);
    CHECK(all[sent].command == Command::kLyrics);
    CHECK(all[sent].lyrics.item == 7);
    CHECK(all[sent].lyrics.text == document.document.value());
    CHECK(all[sent + 1].command == Command::kLyricsPosition);
    CHECK(all[sent + 1].lyrics_position.item == 7);
    CHECK(all[sent + 1].lyrics_position.position_ms == 2500);

    // And holding it again, the board is not sent it a second time.
    position(7, 3000);
    REQUIRE(eventually([&] { return item_line(7) == std::optional<std::int32_t>{2}; }));
    CHECK(of_kind(group.decodes->roles().lyric_commands, Command::kLyrics).size() == 2);
}

TEST_CASE("engine: members that leave and rejoin a group while it plays do not deadlock the page's poll",
          "[hearth][group][websocket][iclforge]") {
    // The group logs from inside its own lock when a member starts (Group::State::try_start), and
    // NetworkSinks::status() reads the group's facts from inside NetworkSinks' own: the trail was
    // guarded by that mutex, so a member rejoining a playing group while the page polled took the
    // two in opposite orders and stopped the engine for good (found by a real board restarting
    // mid-item, planning/karaoke-phase2.md, W7d).
    constexpr int kFrameCount = 200;
    iclforge::hearth::DecoderSettings wanted;
    wanted.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    KaraokeGroup rig("hearth_engine_group_rejoin_poll", kFrameCount, wanted);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> polls{0};
    std::thread page([&] {
        while (!stop.load()) {
            (void)rig.sinks->status();
            (void)rig.sinks->take_log();
            ++polls;
        }
    });
    rig.engine->play();
    // Back and forth while it plays: each rejoin starts the member afresh, logging as it does.
    std::atomic<bool> finished{false};
    std::thread churn([&] {
        for (int round = 0; round < 60 && !finished.load(); ++round) {
            rig.sinks->remove_group_member(rig.group_id, "legacy");
            std::this_thread::sleep_for(15ms);
            rig.sinks->add_group_member(rig.group_id, "legacy");
            std::this_thread::sleep_for(15ms);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 40s;
    while (std::chrono::steady_clock::now() < deadline) {
        const EngineStatus status = rig.engine->status();
        if (status.state == TransportState::kStopped && !status.history.empty()) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    const std::uint64_t before = polls.load();
    std::this_thread::sleep_for(200ms);
    const bool page_alive = polls.load() > before || finished.load();
    if (!finished.load() || !page_alive) {
        // A deadlock leaves threads that cannot be joined: say what happened and stop the process.
        std::fprintf(stderr, "a rejoining member deadlocked the poll: finished=%d page_alive=%d\n",
                     finished.load() ? 1 : 0, page_alive ? 1 : 0);
        std::fflush(stderr);
        std::_Exit(3);
    }
    churn.join();
    stop = true;
    page.join();
    CHECK(polls.load() > 0);
}

// planning/truehd-parity-hearth-player.md, H4: a 96 kHz item to a group whose sinks list 48 kHz and
// 44.1 kHz. Two take PCM and one is set to Opus; the sink that lists 8 channels only has no form at
// all and is sent nothing, with the reason. The audio is converted here, once, for the group: each
// PCM sink plays the item at 48 kHz, half as many frames as the item has, and what it plays is the
// converter's output for the item's own samples.
TEST_CASE("engine: a 96 kHz item reaches a group of 48 kHz sinks converted, as PCM and as Opus",
          "[hearth][group][websocket][iclforge][truehd][rate]") {
    namespace ss = iclforge::sendspin;
    namespace th = iclforge::hearth::testing;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_rate_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    constexpr std::uint64_t kItemFrames = 96000;
    const th::MadeStream made = th::make_stream(2, kItemFrames, 96000);

    CodeLog first_log;
    CodeLog second_log;
    CodeLog opus_log;
    CodeLog held_log;
    const auto make_sink = [&](const std::string& name, CodeLog& log,
                               std::vector<m::Codec> codecs, std::vector<std::int32_t> widths) {
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = scratch / name / "state";
        options.output_directory = scratch / name / "out";
        options.advertise = false;
        options.codecs = std::move(codecs);
        options.layout = "2.0";
        options.max_coded_channels = 2;
        options.pcm_channels = std::move(widths);
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> first =
        make_sink("first", first_log, {m::Codec::kPcm}, {2});
    const std::unique_ptr<testsink::Sink> second =
        make_sink("second", second_log, {m::Codec::kPcm}, {2});
    const std::unique_ptr<testsink::Sink> opus =
        make_sink("opus", opus_log, {m::Codec::kPcm, m::Codec::kOpus}, {2});
    const std::unique_ptr<testsink::Sink> held = make_sink("held", held_log, {m::Codec::kPcm}, {8});

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("2026-10-31"); }};
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Test Hearth", store,
                                         iclforge::hearth::NetworkSinksOptions{.browse = false}};
    REQUIRE(sinks.started());
    const std::map<const testsink::Sink*, std::string> names{
        {first.get(), "first"}, {second.get(), "second"}, {opus.get(), "opus"}, {held.get(), "held"}};
    for (const testsink::Sink* sink : {first.get(), second.get(), opus.get(), held.get()}) {
        sinks.on_found(ss::discovery::Service{.instance = names.at(sink),
                                              .host = "127.0.0.1",
                                              .addresses = {"127.0.0.1"},
                                              .port = sink->port(),
                                              .txt = {{.key = "path", .value = "/sendspin"}}});
    }
    pair_through(sinks, "first", first_log);
    pair_through(sinks, "second", second_log);
    pair_through(sinks, "opus", opus_log);
    pair_through(sinks, "held", held_log);
    REQUIRE(sinks.set_sink_opus_fallback("opus", true));

    const std::string group_id = sinks.create_group("Rates");
    REQUIRE_FALSE(group_id.empty());
    for (const char* id : {"first", "second", "opus", "held"}) {
        sinks.add_group_member(group_id, id);
    }
    const std::shared_ptr<ss::Group> group = sinks.group(group_id);
    REQUIRE(group != nullptr);

    const ItemLoader loader =
        [&made](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "item") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.source = std::make_shared<iclforge::hearth::MemoryByteSource>(made.bytes),
                          .truehd = true};
    };
    const auto master = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(master.has_value());
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group](const std::string&) { return group; },
                              [&sinks](const iclforge::hearth::GroupPlanRequest& request) {
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "item", .title = "96 kHz"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    // The item played to its end, counted in its own frames.
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    // The group is sent 48 kHz: half the item's frames, less the converter's delay, which the
    // item's end does not flush (it is under a millisecond).
    const std::uint64_t group_frames = kItemFrames / 2;
    REQUIRE(eventually([&] {
        return first->totals().frames >= group_frames - 200 &&
               second->totals().frames >= group_frames - 200 &&
               opus->totals().frames >= group_frames - 4800;
    }));
    CHECK(first->totals().frames <= group_frames);
    CHECK(second->totals().frames == first->totals().frames);
    CHECK(opus_log.saw("stream Opus 48000 Hz"));
    CHECK(first_log.saw("stream PCM"));
    // The sink with no 2-channel PCM was sent nothing, and the page is told why.
    CHECK(held->totals().frames == 0);
    CHECK(held->totals().chunks == 0);
    bool said_so = false;
    for (const std::string& line : sinks.take_log()) {
        said_so = said_so ||
                  (line.find("converted here from 96000 Hz to 48000 Hz") != std::string::npos);
    }
    CHECK(said_so);

    // What a PCM sink played is the converter's output for the item's samples, within a step of
    // the 16-bit WAV it wrote.
    std::vector<std::vector<float>> source(2);
    for (std::size_t frame = 0; frame < made.frames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            source[channel].push_back(
                static_cast<float>(made.pcm[(frame * 2) + channel]) / 8388608.0F);
        }
    }
    iclforge::dsp::StreamResampler reference{96000, 48000, 2};
    std::vector<std::vector<float>> want;
    const std::array<std::span<const float>, 2> in{source[0], source[1]};
    reference.process(in, source[0].size(), want);
    const auto wav = read_wav_holding(
        only_file(scratch / "first" / "out", "stream-", ".wav").string(), first->totals().frames);
    REQUIRE(wav.has_value());
    REQUIRE(wav->channels.size() == 2);
    const std::size_t compared = std::min<std::size_t>(wav->frame_count(), want[0].size());
    REQUIRE(compared > group_frames - 400);
    double worst = 0.0;
    double energy = 0.0;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < compared; ++frame) {
            worst = std::max(worst, std::abs(static_cast<double>(wav->channels[channel][frame]) -
                                             static_cast<double>(want[channel][frame])));
            energy += static_cast<double>(want[channel][frame]) *
                      static_cast<double>(want[channel][frame]);
        }
    }
    CHECK(worst < 2.0 / 32768.0);
    CHECK(energy > 1.0);
}

namespace {

// A group of the Hearth test sinks in one place, paired through NetworkSinks the way the page pairs
// them, for the TrueHD cases below. `configure` sets the options each sink starts with.
struct TruehdGroup {
    fs::path scratch;
    std::map<std::string, std::unique_ptr<testsink::Sink>> sinks_by_name;
    std::map<std::string, std::unique_ptr<CodeLog>> logs;
    std::unique_ptr<iclforge::hearth::MemorySettingsStore> settings;
    std::unique_ptr<iclforge::hearth::PairingStore> store;
    std::unique_ptr<iclforge::hearth::NetworkSinks> sinks;
    std::shared_ptr<iclforge::sendspin::Group> group;
    std::string group_id;

    testsink::Sink& sink(const std::string& name) { return *sinks_by_name.at(name); }
};

std::unique_ptr<TruehdGroup> make_truehd_group(
    const std::string& tag,
    const std::vector<std::pair<std::string, std::function<void(testsink::SinkOptions&)>>>& members) {
    namespace ss = iclforge::sendspin;
    auto out = std::make_unique<TruehdGroup>();
    out->scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_" + tag + "_" + scratch_pid_suffix());
    fs::remove_all(out->scratch);
    for (const auto& [name, configure] : members) {
        out->logs[name] = std::make_unique<CodeLog>();
        testsink::SinkOptions options;
        options.name = name;
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = out->scratch / name / "state";
        options.output_directory = out->scratch / name / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = "2.0";
        configure(options);
        auto started = testsink::Sink::start(options, *out->logs[name]);
        REQUIRE(started.has_value());
        out->sinks_by_name[name] = std::move(*started);
    }
    out->settings = std::make_unique<iclforge::hearth::MemorySettingsStore>();
    out->store = std::make_unique<iclforge::hearth::PairingStore>(
        *out->settings, [] { return std::string("2026-10-31"); });
    const std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    out->sinks = std::make_unique<iclforge::hearth::NetworkSinks>(
        *identity, "Test Hearth", *out->store, iclforge::hearth::NetworkSinksOptions{.browse = false});
    REQUIRE(out->sinks->started());
    for (const auto& [name, configure] : members) {
        static_cast<void>(configure);
        out->sinks->on_found(ss::discovery::Service{.instance = name,
                                                    .host = "127.0.0.1",
                                                    .addresses = {"127.0.0.1"},
                                                    .port = out->sink(name).port(),
                                                    .txt = {{.key = "path", .value = "/sendspin"}}});
    }
    for (const auto& [name, configure] : members) {
        static_cast<void>(configure);
        pair_through(*out->sinks, name, *out->logs[name]);
    }
    out->group_id = out->sinks->create_group("TrueHD");
    REQUIRE_FALSE(out->group_id.empty());
    for (const auto& [name, configure] : members) {
        static_cast<void>(configure);
        out->sinks->add_group_member(out->group_id, name);
    }
    out->group = out->sinks->group(out->group_id);
    REQUIRE(out->group != nullptr);
    return out;
}

// Plays the TrueHD item to the end through the application's planner and the engine.
EngineStatus play_truehd_item(TruehdGroup& rig, const iclforge::hearth::testing::MadeStream& made) {
    const ItemLoader loader =
        [&made](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "item") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.source = std::make_shared<iclforge::hearth::MemoryByteSource>(made.bytes),
                          .truehd = true};
    };
    const auto master = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(master.has_value());
    EngineOutputs outputs{.group = iclforge::hearth::make_group_sink(
                              [group = rig.group](const std::string&) { return group; },
                              [&sinks = *rig.sinks](const iclforge::hearth::GroupPlanRequest& request) {
                                  return sinks.plan_group(request);
                              })};
    Engine engine(std::move(outputs), loader, *master, iclforge::hearth::DecoderSettings{},
                  EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                    .follow_sink = true,
                                                    .group_name = rig.group_id,
                                                    .group_ready = true});
    engine.add({QueueItem{.path = "item", .title = "TrueHD"}});
    engine.play();
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    return engine.status();
}

}  // namespace

// planning/truehd-parity-hearth-player.md, H5: a TrueHD item is sent to a sink that lists "truehd" as
// its access units, 24 to a chunk, which it decodes itself, and to a sink that does not as the PCM
// the application decodes. The two play the same samples, because the codec is lossless and the
// layout is the stream's own.
TEST_CASE("engine: a TrueHD item reaches a group as access units for the sink that decodes them and as PCM "
          "for the one that does not",
          "[hearth][group][websocket][iclforge][truehd][coded]") {
    namespace th = iclforge::hearth::testing;
    constexpr std::uint64_t kItemFrames = 96000;  // 2 s: 2,400 units, 100 chunks of 24
    const th::MadeStream made = th::make_stream(2, kItemFrames, 48000);
    auto rig = make_truehd_group("truehd_coded", {
        {"decodes", [](testsink::SinkOptions&) {}},
        {"plays_pcm", [](testsink::SinkOptions& options) {
             options.data_types = {iclforge::sendspin::player::DataType::kAc3,
                                   iclforge::sendspin::player::DataType::kEac3};
         }}});
    QuietLog quiet;
    static_cast<void>(quiet);

    const EngineStatus finished = play_truehd_item(*rig, made);
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);
    CHECK(finished.history.front().frames == kItemFrames);

    testsink::Sink& decodes = rig->sink("decodes");
    testsink::Sink& plays_pcm = rig->sink("plays_pcm");
    // (A sink's frames are the PCM chunks it was sent; what a sink decoded from chunks is in its WAV.)
    const bool arrived = eventually([&] {
        return decodes.totals().bursts >= 100 && plays_pcm.totals().frames >= kItemFrames;
    });
    INFO("decodes: " << decodes.totals().bursts << " bursts, " << decodes.totals().frames
                     << " frames; plays_pcm: " << plays_pcm.totals().bursts << " bursts, "
                     << plays_pcm.totals().frames << " frames");
    REQUIRE(arrived);
    // 2,400 units in chunks of 24: 100 chunks, and no PCM to the sink that decodes them.
    CHECK(decodes.totals().bursts == 100);
    CHECK(decodes.totals().frames == 0);
    CHECK(plays_pcm.totals().bursts == 0);
    CHECK(plays_pcm.totals().frames == kItemFrames);

    // The page is told what each was sent, and why.
    bool coded_line = false;
    bool pcm_line = false;
    for (const std::string& line : rig->sinks->take_log()) {
        coded_line = coded_line || (line.find("decodes: Sending TrueHD as it is") != std::string::npos);
        pcm_line = pcm_line || (line.find("plays_pcm: This sink does not decode TrueHD") != std::string::npos);
    }
    CHECK(coded_line);
    CHECK(pcm_line);

    // And the samples: what the sink decoded from the chunks is what the application decoded for the
    // other, and both are the stream's own PCM.
    const auto coded_wav = read_wav_holding(
        only_file(rig->scratch / "decodes" / "out", "bursts-", ".wav").string(), kItemFrames);
    const auto pcm_wav = read_wav_holding(
        only_file(rig->scratch / "plays_pcm" / "out", "stream-", ".wav").string(), kItemFrames);
    REQUIRE(coded_wav.has_value());
    REQUIRE(pcm_wav.has_value());
    REQUIRE(coded_wav->channels.size() == 2);
    REQUIRE(pcm_wav->channels.size() == 2);
    REQUIRE(coded_wav->frame_count() == kItemFrames);
    REQUIRE(pcm_wav->frame_count() == kItemFrames);
    double worst_between = 0.0;
    double worst_from_source = 0.0;
    double energy = 0.0;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < kItemFrames; ++frame) {
            const double coded = static_cast<double>(coded_wav->channels[channel][frame]);
            const double pcm = static_cast<double>(pcm_wav->channels[channel][frame]);
            const double source = static_cast<double>(made.pcm[(frame * 2) + channel]) / 8388608.0;
            worst_between = std::max(worst_between, std::abs(coded - pcm));
            worst_from_source = std::max(worst_from_source, std::abs(coded - source));
            energy += source * source;
        }
    }
    CHECK(worst_between < 2.0 / 32768.0);
    CHECK(worst_from_source < 2.0 / 32768.0);
    CHECK(energy > 1.0);
}

// A sink states how much coded data it takes (the support object's max_data_rate), and a stream whose
// declared peak is more goes to it as PCM, with the reason on the page: refused by name before any
// chunk is sent.
TEST_CASE("engine: a TrueHD item over a sink's stated data rate is sent that sink as PCM, by name",
          "[hearth][group][websocket][iclforge][truehd][coded]") {
    namespace th = iclforge::hearth::testing;
    constexpr std::uint64_t kItemFrames = 24000;
    const th::MadeStream made = th::make_stream(2, kItemFrames, 48000);
    auto rig = make_truehd_group("truehd_limit", {
        {"limited", [](testsink::SinkOptions& options) { options.max_data_rate = 1000; }},
        {"open", [](testsink::SinkOptions&) {}}});

    const EngineStatus finished = play_truehd_item(*rig, made);
    INFO("output_reason: " << finished.output_reason << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    testsink::Sink& limited = rig->sink("limited");
    testsink::Sink& open = rig->sink("open");
    REQUIRE(eventually([&] { return limited.totals().frames >= kItemFrames && open.totals().bursts >= 25; }));
    // The stream declares 18 Mbit/s as its peak (the encoder's default), more than 1,000 kbit/s.
    CHECK(limited.totals().bursts == 0);
    CHECK(limited.totals().frames == kItemFrames);
    CHECK(open.totals().bursts == 25);
    CHECK(open.totals().frames == 0);
    bool said = false;
    for (const std::string& line : rig->sinks->take_log()) {
        said = said || (line.find("limited: This sink takes TrueHD up to 1000 kbit/s") != std::string::npos);
    }
    CHECK(said);
}
