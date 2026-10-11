#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "engine_thread.hpp"
#include "network_group_sink.hpp"
#include "network_sinks.hpp"
#include "settings_model.hpp"
#include "stream_decoder.hpp"

// Last: on Windows it brings windows.h, whose macros would break the headers above.
#include <httplib.h>

// The whole Network page against real Hearth sinks on this network, hidden (it needs the boards):
// the app's own NetworkSinks finds them over mDNS, pairs each by the code it shows on its own page
// (read from its GET /status, as a person reads it off the page), puts them in one group, and the
// app's own Engine plays an E-AC-3 programme to the group; every sink reports the bursts it played,
// and the pairings are withdrawn at the end (server/unpair), so each board is left as it was.
// Another server holding a sink (Music Assistant) is displaced by the pairing, and takes it back
// afterwards if it wants it.
//
//   ICLFORGE_HEARTH_LIVE_SINKS    the sinks' mDNS instance names, separated by commas
//   (hearth-47b39c,...);
//                           the case is skipped without it
//   ICLFORGE_HEARTH_LIVE_SECONDS  how long the programme plays (default 10)
//
// iclforge-hearth-tests has no firewall exception of its own: it asks mDNS for nothing a Windows
// firewall stops (NetworkSinks' browser's replies come back as replies to its own queries).

namespace {

namespace ss = iclforge::sendspin;
using namespace std::chrono_literals;

[[nodiscard]] std::optional<std::string> environment(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) {
            out.push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

// A board's GET /status, or nothing.
[[nodiscard]] std::optional<std::string> board_status(const std::string& address) {
    httplib::Client client("http://" + address);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(3, 0);
    const httplib::Result result = client.Get("/status");
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return result->body;
}

// The value of "key" in the status JSON's "sendspin" object, as its raw text: a string's
// characters, or a number's digits. Flat enough a search is all it needs.
[[nodiscard]] std::optional<std::string> sendspin_field(const std::string& status, const std::string& key) {
    const std::size_t section = status.find("\"sendspin\":{");
    if (section == std::string::npos) {
        return std::nullopt;
    }
    const std::string quoted = "\"" + key + "\":";
    const std::size_t at = status.find(quoted, section);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    std::size_t value = at + quoted.size();
    std::string out;
    if (value < status.size() && status[value] == '"') {
        for (++value; value < status.size() && status[value] != '"'; ++value) {
            out.push_back(status[value]);
        }
        return out;
    }
    for (; value < status.size() && (std::isdigit(static_cast<unsigned char>(status[value])) != 0); ++value) {
        out.push_back(status[value]);
    }
    return out;
}

// `frames` E-AC-3 access units of a 440 Hz tone - test_engine_network_group.cpp's own programme.
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
                0.1 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0));
        }
        const std::vector<std::span<const float>> views(2, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

// Calls tick() and prints what changed, until `done` holds or `timeout` passes.
bool drive(iclforge::hearth::NetworkSinks& sinks, const std::function<bool(const iclforge::hearth::NetworkStatus&)>& done,
           std::chrono::seconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    std::uint64_t seen = 0;
    while (true) {
        sinks.tick();
        for (const std::string& line : sinks.take_log()) {
            std::printf("  host: %s\n", line.c_str());
        }
        const iclforge::hearth::NetworkStatus status = sinks.status();
        if (status.generation != seen) {
            seen = status.generation;
            for (const iclforge::hearth::SinkFacts& facts : status.sinks) {
                std::printf("  %-16s %-8s %-28s %s\n", facts.id.c_str(),
                            facts.pair_state == iclforge::hearth::PairState::kPaired ? "paired" : "unpaired",
                            iclforge::hearth::link_text(facts).c_str(), facts.notice.c_str());
            }
            std::fflush(stdout);
        }
        if (done(status)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(100ms);
    }
}

[[nodiscard]] const iclforge::hearth::SinkFacts* row(const iclforge::hearth::NetworkStatus& status, const std::string& id) {
    const auto found = std::find_if(status.sinks.begin(), status.sinks.end(),
                                    [&](const iclforge::hearth::SinkFacts& facts) { return facts.id == id; });
    return found == status.sinks.end() ? nullptr : &*found;
}

// Withdraws this test's pairings however the case ends, a failed REQUIRE included, so no board is
// left holding a record for an identity nothing will use again.
class Unpair {
   public:
    explicit Unpair(iclforge::hearth::NetworkSinks& sinks) : sinks_(&sinks) {}
    ~Unpair() {
        for (const std::string& id : paired_) {
            sinks_->forget_pairing(id);
        }
        if (!paired_.empty()) {
            // server/unpair goes out as the call returns; give each board a moment to act on it.
            std::this_thread::sleep_for(1s);
        }
    }
    Unpair(const Unpair&) = delete;
    Unpair& operator=(const Unpair&) = delete;
    Unpair(Unpair&&) = delete;
    Unpair& operator=(Unpair&&) = delete;

    void paired(const std::string& id) { paired_.push_back(id); }
    void clear() { paired_.clear(); }

   private:
    iclforge::hearth::NetworkSinks* sinks_;
    std::vector<std::string> paired_;
};

}  // namespace

TEST_CASE("network sinks live: the sinks on this network are found, paired, played to as one group, and unpaired",
          "[.][hearth-network-live]") {
    const std::optional<std::string> named = environment("ICLFORGE_HEARTH_LIVE_SINKS");
    if (!named) {
        SKIP("ICLFORGE_HEARTH_LIVE_SINKS names no sinks");
    }
    const std::vector<std::string> wanted = split(*named);
    REQUIRE_FALSE(wanted.empty());
    const int seconds = environment("ICLFORGE_HEARTH_LIVE_SECONDS") ? std::atoi(environment("ICLFORGE_HEARTH_LIVE_SECONDS")->c_str()) : 10;
    REQUIRE(seconds > 0);

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("live"); }};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    // Browsing, unlike every other NetworkSinks test: finding the real sinks is this case's point.
    iclforge::hearth::NetworkSinks sinks{*identity, "Hearth live test", store, {.request_firewall_exception = false}};
    REQUIRE(sinks.started());

    // 1. Found over mDNS, and read: each has said hello at least once.
    std::printf("finding %zu sink(s)\n", wanted.size());
    REQUIRE(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && !facts->roles.empty();
            });
        },
        60s));

    // 2. Paired, one at a time, by the code each shows on its own page.
    Unpair unpair(sinks);
    for (const std::string& id : wanted) {
        const iclforge::hearth::NetworkStatus listed = sinks.status();
        const iclforge::hearth::SinkFacts* const found = row(listed, id);
        REQUIRE(found != nullptr);
        const std::string address = found->address;
        std::printf("pairing %s at %s\n", id.c_str(), address.c_str());
        // A board's page keeps showing the code of an attempt that ended with its connection;
        // this attempt's is a new one.
        const std::optional<std::string> before = board_status(address);
        const std::string stale = before ? sendspin_field(*before, "pairing_code").value_or("") : "";
        sinks.select_sink(id);
        sinks.pair_sink(id);
        std::optional<std::string> code;
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                if (facts == nullptr || !facts->wants_code) {
                    return false;
                }
                if (const std::optional<std::string> text = board_status(address)) {
                    code = sendspin_field(*text, "pairing_code");
                }
                return code.has_value() && code->size() == 6 && *code != stale;
            },
            45s));
        std::printf("  %s shows %s\n", id.c_str(), code->c_str());
        sinks.submit_pairing_code(id, *code);
        unpair.paired(id);
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr &&
                       facts->pair_state == iclforge::hearth::PairState::kPaired &&
                       facts->link == iclforge::hearth::SinkLink::kConnected &&
                       facts->iclforge_support.has_value();
            },
            45s));
    }

    // 3. One group, every sink a member, each with its clock synchronised.
    const std::string group_id = sinks.create_group("Live");
    REQUIRE_FALSE(group_id.empty());
    for (const std::string& id : wanted) {
        sinks.add_group_member(group_id, id);
    }
    REQUIRE(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->clock_converged;
            });
        },
        45s));

    // 4. The app's own engine plays to the group.
    const int frames = seconds * 48000 / 1536;
    const std::vector<std::byte> programme = eac3_stream(frames);
    const iclforge::hearth::ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<iclforge::hearth::LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such item: " + path);
        }
        return iclforge::hearth::LoadedItem{.bytes = programme};
    };
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    iclforge::hearth::EngineOutputs outputs{
        .group = iclforge::hearth::make_group_sink([&sinks](const std::string& id) { return sinks.group(id); })};
    {
        iclforge::hearth::Engine engine(std::move(outputs), loader, *layout, iclforge::hearth::DecoderSettings{},
                                   iclforge::hearth::EngineTiming{});
        engine.set_output_preferences(iclforge::hearth::OutputPreferences{.pinned = iclforge::hearth::OutputMode::kNetworkGroup,
                                                                     .follow_sink = true,
                                                                     .group_name = group_id,
                                                                     .group_ready = true});
        engine.add({iclforge::hearth::QueueItem{.path = "programme", .title = "Live programme"}});
        engine.play();
        std::printf("playing %d s to the group\n", seconds);
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus&) {
                const iclforge::hearth::EngineStatus status = engine.status();
                return status.state == iclforge::hearth::TransportState::kStopped && !status.history.empty();
            },
            std::chrono::seconds(seconds + 60)));
        const iclforge::hearth::EngineStatus finished = engine.status();
        INFO("output_reason: " << finished.output_reason << " / note: " << finished.note << " / error: "
                               << finished.error);
        REQUIRE(finished.history.size() == 1);
        CHECK(finished.history.front().frames == finished.history.front().expected_frames);
    }

    // 5. Every sink played the programme's bursts, as it reports over this computer's own connection
    //    (its client/state counters - its page's counters are the last stream's, whoever sent it).
    const auto counters = [&](const iclforge::hearth::NetworkStatus& status,
                              const std::string& id) {
        const iclforge::hearth::SinkFacts* facts = row(status, id);
        return facts != nullptr && facts->iclforge_state ? std::optional(facts->iclforge_state->counters) : std::nullopt;
    };
    (void)drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const auto reported = counters(status, id);
                return reported && reported->bursts_played >= static_cast<std::uint64_t>(frames);
            });
        },
        15s);
    const iclforge::hearth::NetworkStatus played = sinks.status();
    for (const std::string& id : wanted) {
        const auto reported = counters(played, id);
        REQUIRE(reported.has_value());
        std::printf("  %s played %llu of %d bursts: %llu underruns, %llu late, %llu dropped, %llu invalid\n", id.c_str(),
                    static_cast<unsigned long long>(reported->bursts_played), frames,
                    static_cast<unsigned long long>(reported->underruns),
                    static_cast<unsigned long long>(reported->late_chunks),
                    static_cast<unsigned long long>(reported->dropped_chunks),
                    static_cast<unsigned long long>(reported->invalid_chunks));
        CHECK(reported->bursts_played == static_cast<std::uint64_t>(frames));
        CHECK(reported->invalid_chunks == 0);
    }

    // 6. Each board forgets this test's pairing.
    unpair.clear();
    for (const std::string& id : wanted) {
        sinks.forget_pairing(id);
    }
    CHECK(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr &&
                       facts->pair_state != iclforge::hearth::PairState::kPaired;
            });
        },
        30s));
}

// ---- a karaoke stream on a board that decodes it ------------------------------------------------
//
// planning/karaoke-phase2.md, W7: a board that lists the karaoke keys is sent the stream as it is,
// told how to reproduce it, and lays its speakers out from the reproduction. This is that against a
// real board, hidden (it needs the board): it pairs the one sink named, sets it to a 3.0 layout (so
// that the reproduced frame is not a fold, and the layout comes from ac3::output_mode()), plays a
// 3/2 karaoke programme to it as the engine would, once per reproduction, and compares what the
// board reports at its outputs with the same programme decoded here.
//
//   ICLFORGE_HEARTH_LIVE_KARAOKE_SINK  the board's mDNS instance name (hearth-eb2c64); the case is
//                                      skipped without it
//   ICLFORGE_HEARTH_LIVE_SECONDS       how long each play runs (default 10)

namespace {

// `frames` AC-3 syncframes of a karaoke 3/2+LFE stream with a tone in each channel, each its own
// pitch and level.
std::vector<std::vector<std::byte>> karaoke_units(int frames) {
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
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < frames; ++f) {
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
        units.push_back(*frame);
    }
    return units;
}

[[nodiscard]] double to_db(double mean_square) {
    return mean_square > 1e-12 ? 10.0 * std::log10(mean_square) : -120.0;
}

// The median of what a board reported for one output, windows of silence apart.
[[nodiscard]] double median_db(std::vector<double> reported) {
    reported.erase(std::remove_if(reported.begin(), reported.end(), [](double db) { return db < -100.0; }),
                   reported.end());
    if (reported.empty()) {
        return -120.0;
    }
    std::sort(reported.begin(), reported.end());
    return reported[reported.size() / 2];
}

}  // namespace

TEST_CASE("network sinks live: a karaoke stream is reproduced by the board that decodes it",
          "[.][hearth-network-live]") {
    const std::optional<std::string> named = environment("ICLFORGE_HEARTH_LIVE_KARAOKE_SINK");
    if (!named) {
        SKIP("ICLFORGE_HEARTH_LIVE_KARAOKE_SINK names no sink");
    }
    const std::string id = *named;
    const int seconds = environment("ICLFORGE_HEARTH_LIVE_SECONDS")
                            ? std::atoi(environment("ICLFORGE_HEARTH_LIVE_SECONDS")->c_str())
                            : 10;
    REQUIRE(seconds > 0);

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("live"); }};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Hearth live test", store, {.request_firewall_exception = false}};
    REQUIRE(sinks.started());

    REQUIRE(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            const iclforge::hearth::SinkFacts* facts = row(status, id);
            return facts != nullptr && !facts->roles.empty();
        },
        60s));

    // Paired by the code the board shows on its own page, and withdrawn however the case ends.
    Unpair unpair(sinks);
    {
        const iclforge::hearth::NetworkStatus listed = sinks.status();
        const iclforge::hearth::SinkFacts* const found = row(listed, id);
        REQUIRE(found != nullptr);
        const std::string address = found->address;
        const std::optional<std::string> before = board_status(address);
        const std::string stale = before ? sendspin_field(*before, "pairing_code").value_or("") : "";
        sinks.select_sink(id);
        sinks.pair_sink(id);
        std::optional<std::string> code;
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                if (facts == nullptr || !facts->wants_code) {
                    return false;
                }
                if (const std::optional<std::string> text = board_status(address)) {
                    code = sendspin_field(*text, "pairing_code");
                }
                return code.has_value() && code->size() == 6 && *code != stale;
            },
            45s));
        sinks.submit_pairing_code(id, *code);
        unpair.paired(id);
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->pair_state == iclforge::hearth::PairState::kPaired &&
                       facts->link == iclforge::hearth::SinkLink::kConnected &&
                       facts->iclforge_support.has_value() && facts->clock_converged;
            },
            45s));
    }

    // The board says it takes the keys, and has the settings command.
    const iclforge::hearth::SinkFacts support_facts = *row(sinks.status(), id);
    REQUIRE(support_facts.iclforge_support.has_value());
    const auto& takes = support_facts.iclforge_support->decoder_settings;
    REQUIRE(std::find(takes.begin(), takes.end(), "karaoke") != takes.end());
    REQUIRE(std::find(takes.begin(), takes.end(), "karaoke_vocals") != takes.end());
    // And, from W10, the three part levels.
    for (const char* key : {"karaoke_melody_db", "karaoke_vocal1_db", "karaoke_vocal2_db"}) {
        REQUIRE(std::find(takes.begin(), takes.end(), key) != takes.end());
    }
    const std::size_t outputs = static_cast<std::size_t>(support_facts.iclforge_support->outputs.count);
    REQUIRE(outputs >= 3);

    // A 3.0 layout, so that the board does not fold and lays out from the reproduction.
    const auto layout = iclforge::render::OutputLayout::parse("3.0");
    REQUIRE(layout.has_value());
    ss::player::Settings base;
    base.layout = "3.0";
    if (support_facts.iclforge_support->management.routing) {
        std::array<char, iclforge::render::Routing::kTextBytes> text{};
        const auto identity_routing = iclforge::render::Routing::identity(layout->slots(), outputs);
        REQUIRE(identity_routing.has_value());
        REQUIRE(identity_routing->format(text) > 0);
        base.routing = std::string(text.data());
    }
    base.trim_db = std::vector<double>(outputs, 0.0);
    base.delay_ms = std::vector<double>(outputs, 0.0);
    base.crossover_hz = 80.0;
    REQUIRE(sinks.push_sink_settings(id, base));

    const std::string group_id = sinks.create_group("Live karaoke");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, id);

    const int frames = seconds * 48000 / 1536;
    const std::vector<std::vector<std::byte>> units = karaoke_units(frames);
    std::vector<std::byte> programme;
    for (const auto& unit : units) {
        programme.insert(programme.end(), unit.begin(), unit.end());
    }
    const iclforge::hearth::ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<iclforge::hearth::LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such item: " + path);
        }
        return iclforge::hearth::LoadedItem{.bytes = programme};
    };
    iclforge::hearth::NetworkSinks* const planner = &sinks;
    iclforge::hearth::EngineOutputs outputs_for_engine{
        .group = iclforge::hearth::make_group_sink(
            [planner](const std::string& group) { return planner->group(group); },
            [planner](const iclforge::hearth::GroupPlanRequest& request) { return planner->plan_group(request); },
            [planner](const std::string& group, const iclforge::hearth::KaraokeChoice& choice) {
                planner->apply_karaoke(group, choice);
            })};
    iclforge::hearth::Engine engine(std::move(outputs_for_engine), loader, *layout,
                                    iclforge::hearth::DecoderSettings{}, iclforge::hearth::EngineTiming{});
    engine.set_output_preferences(
        iclforge::hearth::OutputPreferences{.pinned = iclforge::hearth::OutputMode::kNetworkGroup,
                                            .follow_sink = true,
                                            .group_name = group_id,
                                            .group_ready = true});

    struct Case {
        const char* name;
        iclforge::ac3::KaraokeReproduction reproduction;
        iclforge::ac3::KaraokeVocals vocals;
        // Annex C.2.3's part levels, dB against the table (W10): the capable decoder's.
        double melody_db = 0.0;
        double vocal1_db = 0.0;
    };
    const std::array<Case, 5> cases{{
        {"as coded", iclforge::ac3::KaraokeReproduction::kOff, iclforge::ac3::KaraokeVocals::kBoth},
        {"aware", iclforge::ac3::KaraokeReproduction::kAware, iclforge::ac3::KaraokeVocals::kBoth},
        {"capable, vocal 1", iclforge::ac3::KaraokeReproduction::kCapable, iclforge::ac3::KaraokeVocals::kV1},
        {"capable, no vocals", iclforge::ac3::KaraokeReproduction::kCapable, iclforge::ac3::KaraokeVocals::kNone},
        {"capable, both, melody -12 dB, vocal 1 +6 dB", iclforge::ac3::KaraokeReproduction::kCapable,
         iclforge::ac3::KaraokeVocals::kBoth, -12.0, 6.0},
    }};

    // The host's own decode of the programme, as the same settings would have it, at each slot.
    const auto host_db = [&](const Case& c) {
        iclforge::hearth::DecoderSettings reference_settings;
        reference_settings.karaoke = c.reproduction;
        reference_settings.karaoke_vocals = c.vocals;
        reference_settings.karaoke_melody_db = c.melody_db;
        reference_settings.karaoke_vocal1_db = c.vocal1_db;
        iclforge::hearth::StreamDecoder reference{*layout, 48000, reference_settings};
        std::vector<double> sum(layout->slots(), 0.0);
        std::size_t count = 0;
        const auto deliver = [&](std::span<const std::span<const float>> slots, std::size_t n) {
            for (std::size_t slot = 0; slot < slots.size() && slot < sum.size(); ++slot) {
                for (std::size_t k = 0; k < n; ++k) {
                    sum[slot] += static_cast<double>(slots[slot][k]) * static_cast<double>(slots[slot][k]);
                }
            }
            count += n;
        };
        for (const auto& unit : units) {
            REQUIRE(reference.decode(unit, deliver).has_value());
        }
        reference.finish(deliver);
        std::vector<double> db;
        for (const double s : sum) {
            db.push_back(to_db(count > 0 ? s / static_cast<double>(count) : 0.0));
        }
        return db;
    };

    std::vector<std::vector<double>> board_results;
    std::size_t plays = 0;
    for (const Case& c : cases) {
        iclforge::hearth::DecoderSettings wanted;
        wanted.karaoke = c.reproduction;
        wanted.karaoke_vocals = c.vocals;
        wanted.karaoke_melody_db = c.melody_db;
        wanted.karaoke_vocal1_db = c.vocal1_db;
        engine.set_decoder_settings(wanted);
        engine.add({iclforge::hearth::QueueItem{.path = "programme", .title = "Live karaoke"}});
        ++plays;
        // The queue holds the earlier plays too: play this one, the last added.
        engine.play_item(plays - 1);
        std::printf("playing %d s, %s\n", seconds, c.name);

        std::vector<std::vector<double>> reported(outputs);
        std::vector<ss::player::Level> last;
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                if (const iclforge::hearth::SinkFacts* facts = row(status, id);
                    facts != nullptr && facts->iclforge_state && facts->iclforge_state->levels) {
                    const std::vector<ss::player::Level>& levels = *facts->iclforge_state->levels;
                    const bool changed =
                        levels.size() != last.size() ||
                        !std::equal(levels.begin(), levels.end(), last.begin(),
                                    [](const ss::player::Level& a, const ss::player::Level& b) {
                                        return a.rms_db == b.rms_db && a.peak_db == b.peak_db;
                                    });
                    if (changed) {
                        last = levels;
                        for (const ss::player::Level& level : levels) {
                            if (level.output >= 0 && static_cast<std::size_t>(level.output) < outputs) {
                                reported[static_cast<std::size_t>(level.output)].push_back(level.rms_db);
                            }
                        }
                    }
                }
                const iclforge::hearth::EngineStatus status_now = engine.status();
                return status_now.state == iclforge::hearth::TransportState::kStopped &&
                       status_now.history.size() >= plays;
            },
            std::chrono::seconds(seconds + 60)));
        std::vector<double> board;
        for (std::size_t o = 0; o < layout->slots(); ++o) {
            board.push_back(median_db(reported[o]));
        }
        const std::vector<double> host = host_db(c);
        std::printf("  %-20s", c.name);
        for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
            std::printf("  slot %zu: board %7.2f dB  host %7.2f dB", slot, board[slot], host[slot]);
        }
        std::printf("\n");
        std::fflush(stdout);
        for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
            // A slot with something in it is where the board says the same, to a level meter's
            // window and a float's rounding; one with nothing in it stays silent.
            if (host[slot] > -80.0) {
                CHECK(std::abs(board[slot] - host[slot]) < 1.5);
            } else {
                CHECK(board[slot] < -80.0);
            }
        }
        board_results.push_back(board);
    }

    // Reproducing it made a difference the board's outputs show.
    REQUIRE(board_results.size() == cases.size());
    double moved = 0.0;
    for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
        moved = std::max(moved, std::abs(board_results[0][slot] - board_results[1][slot]));
    }
    CHECK(moved > 1.0);
}

// ---- the lyrics of an item on a board that holds them --------------------------------------------
//
// planning/karaoke-phase2.md, W7d: a board that lists the lyric commands is sent the item's document
// and where the sound is in it, follows them on its own playout clock and reports the line. This is
// that against a real board, hidden: it pairs the one sink named, plays a karaoke programme with a
// lyrics file beside it (lines at 2, 5 and 8 seconds), and watches the line the board reports against
// the moment its outputs first carry sound.
//
//   ICLFORGE_HEARTH_LIVE_LYRICS_SINK  the board's mDNS instance name; the case is skipped without it
//   ICLFORGE_HEARTH_LIVE_SECONDS      how long the programme plays (default 14)

TEST_CASE("network sinks live: a board that holds lyrics follows them with the sound",
          "[.][hearth-network-live]") {
    const std::optional<std::string> named = environment("ICLFORGE_HEARTH_LIVE_LYRICS_SINK");
    if (!named) {
        SKIP("ICLFORGE_HEARTH_LIVE_LYRICS_SINK names no sink");
    }
    const std::string id = *named;
    const int seconds = environment("ICLFORGE_HEARTH_LIVE_SECONDS")
                            ? std::atoi(environment("ICLFORGE_HEARTH_LIVE_SECONDS")->c_str())
                            : 14;
    REQUIRE(seconds >= 11);

    iclforge::hearth::MemorySettingsStore settings;
    iclforge::hearth::PairingStore store{settings, [] { return std::string("live"); }};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    iclforge::hearth::NetworkSinks sinks{*identity, "Hearth live test", store, {.request_firewall_exception = false}};
    REQUIRE(sinks.started());

    REQUIRE(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            const iclforge::hearth::SinkFacts* facts = row(status, id);
            return facts != nullptr && !facts->roles.empty();
        },
        60s));

    Unpair unpair(sinks);
    {
        const iclforge::hearth::NetworkStatus listed = sinks.status();
        const iclforge::hearth::SinkFacts* const found = row(listed, id);
        REQUIRE(found != nullptr);
        const std::string address = found->address;
        const std::optional<std::string> before = board_status(address);
        const std::string stale = before ? sendspin_field(*before, "pairing_code").value_or("") : "";
        sinks.select_sink(id);
        sinks.pair_sink(id);
        std::optional<std::string> code;
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                if (facts == nullptr || !facts->wants_code) {
                    return false;
                }
                if (const std::optional<std::string> text = board_status(address)) {
                    code = sendspin_field(*text, "pairing_code");
                }
                return code.has_value() && code->size() == 6 && *code != stale;
            },
            45s));
        sinks.submit_pairing_code(id, *code);
        unpair.paired(id);
        REQUIRE(drive(
            sinks,
            [&](const iclforge::hearth::NetworkStatus& status) {
                const iclforge::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->pair_state == iclforge::hearth::PairState::kPaired &&
                       facts->link == iclforge::hearth::SinkLink::kConnected &&
                       facts->iclforge_support.has_value() && facts->clock_converged;
            },
            45s));
    }

    // The board says it holds lyrics, and lists the two commands.
    const iclforge::hearth::SinkFacts support_facts = *row(sinks.status(), id);
    REQUIRE(support_facts.iclforge_support.has_value());
    REQUIRE(support_facts.iclforge_support->lyrics_max_bytes > 0);
    REQUIRE(support_facts.iclforge_state.has_value());
    const auto& commands = support_facts.iclforge_state->supported_commands;
    REQUIRE(std::find(commands.begin(), commands.end(), ss::player::Command::kLyrics) != commands.end());
    REQUIRE(std::find(commands.begin(), commands.end(), ss::player::Command::kLyricsPosition) != commands.end());

    const std::string group_id = sinks.create_group("Live lyrics");
    REQUIRE_FALSE(group_id.empty());
    sinks.add_group_member(group_id, id);

    // The item is a file under the temp folder, with its lyrics beside it.
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "iclforge_live_lyrics";
    fs::create_directories(folder);
    const fs::path item = folder / "song.ac3";
    {
        std::ofstream lrc(fs::path(item).replace_extension(".lrc"), std::ios::binary);
        lrc << "[00:02.00]First line\n[00:05.00]Second line\n[00:08.00]Third line\n";
    }

    const int frames = seconds * 48000 / 1536;
    const std::vector<std::vector<std::byte>> units = karaoke_units(frames);
    std::vector<std::byte> programme;
    for (const auto& unit : units) {
        programme.insert(programme.end(), unit.begin(), unit.end());
    }
    const std::string item_path = item.string();
    const iclforge::hearth::ItemLoader loader =
        [&programme, item_path](const std::string& path) -> std::expected<iclforge::hearth::LoadedItem, std::string> {
        if (path != item_path) {
            return std::unexpected("no such item: " + path);
        }
        return iclforge::hearth::LoadedItem{.bytes = programme};
    };
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    iclforge::hearth::NetworkSinks* const planner = &sinks;
    iclforge::hearth::EngineOutputs outputs_for_engine{
        .group = iclforge::hearth::make_group_sink(
            [planner](const std::string& group) { return planner->group(group); },
            [planner](const iclforge::hearth::GroupPlanRequest& request) { return planner->plan_group(request); },
            [planner](const std::string& group, const iclforge::hearth::KaraokeChoice& choice) {
                planner->apply_karaoke(group, choice);
            },
            [planner](const std::string& group, const iclforge::hearth::LyricsUpdate& update) {
                planner->apply_lyrics(group, update);
            })};
    iclforge::hearth::Engine engine(std::move(outputs_for_engine), loader, *layout,
                                    iclforge::hearth::DecoderSettings{}, iclforge::hearth::EngineTiming{});
    engine.set_output_preferences(
        iclforge::hearth::OutputPreferences{.pinned = iclforge::hearth::OutputMode::kNetworkGroup,
                                            .follow_sink = true,
                                            .group_name = group_id,
                                            .group_ready = true});
    engine.add({iclforge::hearth::QueueItem{.path = item_path, .title = "Live lyrics"}});
    engine.play_item(0);
    std::printf("playing %d s, lyrics at 2, 5 and 8 s\n", seconds);

    // What the board reports, against the clock of this test: when its outputs first carry sound, and
    // when each line first came.
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> sound;
    std::map<int, Clock::time_point> line_at;
    std::optional<std::int64_t> reported_item;
    REQUIRE(drive(
        sinks,
        [&](const iclforge::hearth::NetworkStatus& status) {
            const Clock::time_point now = Clock::now();
            if (const iclforge::hearth::SinkFacts* facts = row(status, id);
                facts != nullptr && facts->iclforge_state) {
                const auto& state = *facts->iclforge_state;
                if (!sound && state.levels) {
                    for (const ss::player::Level& level : *state.levels) {
                        if (level.rms_db > -70.0) {
                            sound = now;
                        }
                    }
                }
                if (state.lyrics) {
                    reported_item = state.lyrics->item;
                    line_at.emplace(state.lyrics->line, now);
                }
            }
            const iclforge::hearth::EngineStatus status_now = engine.status();
            return status_now.state == iclforge::hearth::TransportState::kStopped && !status_now.history.empty();
        },
        std::chrono::seconds(seconds + 60)));

    REQUIRE(reported_item.has_value());
    REQUIRE(sound.has_value());
    // The three lines came, in order, at the times the file gives them: against when the sound began,
    // within what polling and the state's own cadence allow.
    REQUIRE(line_at.count(0) == 1);
    REQUIRE(line_at.count(1) == 1);
    REQUIRE(line_at.count(2) == 1);
    const auto seconds_between = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double>(to - from).count();
    };
    const double first = seconds_between(*sound, line_at.at(0));
    const double second = seconds_between(line_at.at(0), line_at.at(1));
    const double third = seconds_between(line_at.at(1), line_at.at(2));
    std::printf("  sound -> line 0: %.2f s (2.00), line 0 -> 1: %.2f s (3.00), line 1 -> 2: %.2f s (3.00)\n",
                first, second, third);
    std::fflush(stdout);
    CHECK(std::abs(first - 2.0) < 0.6);
    CHECK(std::abs(second - 3.0) < 0.5);
    CHECK(std::abs(third - 3.0) < 0.5);
}
