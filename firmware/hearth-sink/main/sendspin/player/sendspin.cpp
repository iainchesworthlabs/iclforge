// The board as a Sendspin player. See ../../sendspin.hpp.

#include "sendspin.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "iclforge/lyrics/follower.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/trim_delay.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/base64url.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "iclforge/sendspin/pairing.hpp"
#include "iclforge/sendspin/player_session.hpp"
#include "iclforge/sendspin/websocket.hpp"
#include "iclforge/burst_player.hpp"
#include "iclforge/log.hpp"
#include "iclforge/playout.hpp"
#include "iclforge/sendspin_host.hpp"
#include "iclforge/title_card.hpp"

#include "audio_sink.hpp"
#include "network.hpp"
#include "psram_scope.hpp"
#include "settings.hpp"

namespace player {
namespace {

namespace ss = iclforge::sendspin;
namespace ac = ss::player;
namespace m = ss::messages;

constexpr std::uint32_t kSampleRate = 48000;
// The one port, which a build without the player cannot name through
// libs/sendspin.
static_assert(kSendspinPort == ss::transport::websocket::kClientPort, "a player listens on Sendspin's client port");
// From Kconfig (main/Kconfig.projbuild), as plain constants.
constexpr std::size_t kRingBytes = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_RING_BYTES;
constexpr std::size_t kMaxChunkBytes = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CHUNK_BYTES;
constexpr std::uint32_t kDecodeStackBytes = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES;
constexpr bool kDecodeStackInPsram = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_IN_PSRAM != 0;
constexpr std::size_t kServerStackBytes = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_SERVER_STACK_BYTES;
constexpr int kMaxDelayMs = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_DELAY_MS;
// The delay lines are one buffer per output of this length, in PSRAM. A part without any has no room for them in
// the internal RAM the network and the decoder leave (sixteen outputs of 20 ms is 61 KB), so it has none, whatever
// the build asks: the support object says 0, and a trim of distance is unavailable with that as the reason.
[[nodiscard]] bool delay_lines_possible() {
    return kMaxDelayMs > 0 && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}
[[nodiscard]] double max_delay_ms() { return delay_lines_possible() ? static_cast<double>(kMaxDelayMs) : 0.0; }
constexpr std::size_t kMaxCodedChannels = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS;
// Whether the board lists AC-4 (where it decodes it, which needs CONFIG_ICLFORGE_AC4), and the most
// channels of it a frame may decode to.
constexpr bool kListAc4 = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4 != 0;
constexpr std::size_t kAc4MaxCodedChannels =
    CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_CODED_CHANNELS;
// The most frames a second of an AC-4 stream the board keeps up with (0 = no limit).
constexpr std::uint32_t kAc4MaxFrameRate = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_FRAME_RATE;
// The objects of an E-AC-3 stream the board places in real time; -1 states none.
constexpr int kMaxObjects = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS;
// The most bytes of lyric document the board holds for the item playing (0 = takes none: the commands
// are not listed, so a server sends none).
constexpr std::uint32_t kLyricsMaxBytes = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_LYRICS_MAX_BYTES;
// What a lyric document needs of the heap it is made in, beyond a reserve, to be read and held:
// measured at about ten times its text at the peak, on S3 and P4 (planning/hearth-sendspin-
// extension.md, Lyrics). The internal heap always keeps `kLyricsHeapReserve`, and PSRAM, where the
// document goes when there is room, `kLyricsPsramReserve`.
constexpr std::size_t kLyricsHeapFactor = 12;
constexpr std::size_t kLyricsTransientFactor = 3;
constexpr std::size_t kLyricsHeapReserve = 8192;
constexpr std::size_t kLyricsPsramReserve = 262144;
static_assert(kLyricsMaxBytes <= ac::kMaxLyricsBytes, "a lyric document fits one message");
constexpr std::uint32_t kReportEveryChunks = CONFIG_ICLFORGE_EXAMPLE_REPORT_EVERY_FRAMES;
constexpr std::int32_t kLeadMs = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_LEAD_MS;
constexpr std::int32_t kBufferMs = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_BUFFER_MS;
constexpr bool kUnpairedAccess = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_UNPAIRED_ACCESS != 0;
constexpr bool kOfferPcm = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_PCM != 0;
// Whether the hello lists metadata@v1, which only a board with a screen to draw it on does: the title
// card (planning/p4x-now-playing.md). A board with none would be sent a message at every track and pause
// for nothing, and would keep a title it never shows.
#if defined(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI) || defined(CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL)
constexpr bool kListMetadata = true;
#else
constexpr bool kListMetadata = false;
#endif
constexpr int kSuite = CONFIG_ICLFORGE_SENDSPIN_SUITE;
constexpr BaseType_t kDecodeCore = CONFIG_ICLFORGE_EXAMPLE_DECODE_CORE < 0 ? tskNO_AFFINITY
                                                                           : CONFIG_ICLFORGE_EXAMPLE_DECODE_CORE;
constexpr iclforge::ac3::OperatingMode kMode = CONFIG_ICLFORGE_EXAMPLE_DRC_MODE == 1   ? iclforge::ac3::OperatingMode::kRf
                                     : CONFIG_ICLFORGE_EXAMPLE_DRC_MODE == 2 ? iclforge::ac3::OperatingMode::kCustom
                                                                             : iclforge::ac3::OperatingMode::kLine;
constexpr iclforge::ac3::DownmixTarget kStereoFold =
    CONFIG_ICLFORGE_EXAMPLE_STEREO_FOLD != 0 ? iclforge::ac3::DownmixTarget::kLtRt : iclforge::ac3::DownmixTarget::kLoRo;
constexpr iclforge::ac3::render::ObjectsPolicy kObjects = CONFIG_ICLFORGE_EXAMPLE_OBJECTS == 1   ? iclforge::ac3::render::ObjectsPolicy::kNever
                                                : CONFIG_ICLFORGE_EXAMPLE_OBJECTS == 2 ? iclforge::ac3::render::ObjectsPolicy::kAlways
                                                                                       : iclforge::ac3::render::ObjectsPolicy::kAuto;
constexpr iclforge::objects::oba::joc::Domain kJocDomain =
    CONFIG_ICLFORGE_EXAMPLE_JOC_DOMAIN != 0 ? iclforge::objects::oba::joc::Domain::kMdctBand : iclforge::objects::oba::joc::Domain::kQmf;

// The decoder settings this board takes from a server: every one the
// extension page names that the library has a setting for. drc_cut and
// drc_boost wait for the library's separate cut and boost scales. karaoke and
// karaoke_vocals are Annex C's reproduction of a karaoke AC-3 stream: a server
// that sees them listed sends the stream as it is, and the board lays its
// speakers out from what the reproduction makes (burst_player.cpp). The three
// karaoke_*_db levels move the melody and each vocal against the table; a
// server that moves one and does not see its key listed sends PCM instead.
const std::vector<std::string> kDecoderSettings{"mode",    "heavy_compression", "dialnorm",   "downmix",   "ltrt_phase_shift",
                                                "mix_lfe", "programme",         "objects",    "concealment", "karaoke",
                                                "karaoke_vocals", "karaoke_melody_db", "karaoke_vocal1_db",
                                                "karaoke_vocal2_db"};

// The sink seam as the burst player's sink.
class SeamSink final : public iclforge::ScheduledSink {
   public:
    [[nodiscard]] std::optional<iclforge::PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        return sink_write_timed(outputs);
    }
    [[nodiscard]] bool open(std::uint32_t sample_rate, std::size_t outputs) override {
        return sink_open(sample_rate, static_cast<int>(outputs));
    }
    [[nodiscard]] std::size_t max_outputs() override { return static_cast<std::size_t>(std::max(sink_slots(), 1)); }
    void begin_stream() override { sink_begin_play(); }
};

// What the board reports, beside what the burst player measures.
struct Reported {
    std::int32_t volume = 100;
    bool muted = false;
    std::int32_t output_delay_ms = 0;
    std::optional<ac::SettingsError> settings_error;
};

SeamSink g_sink;

// The player and its host. start_player makes and starts both on the key
// task, and only then hands them to every other task through g_running: one
// load gives both or neither, and nothing takes them down afterwards, so a
// pointer a task has loaded stays good. A start that fails frees what it made
// before anything else could reach it. The server's events and the player's
// clock have pointers of their own (Events, BurstPlayerConfig::local_time),
// since both can run before the hand-over.
struct Running {
    std::unique_ptr<iclforge::BurstPlayer> player;
    std::unique_ptr<iclforge::SendspinHost> host;
};
Running g_started;  // start_player's until g_running points at it
std::atomic<const Running*> g_running{nullptr};
// The tap the screen has asked the player to show its blocks to (sendspin_set_block_tap), under g_mutex with the player's start.
iclforge::BlockTap* g_block_tap = nullptr;
// The host whose clock the player reads, from when start_player() has made it.
std::atomic<const iclforge::SendspinHost*> g_clock{nullptr};

std::atomic<bool> g_external{false};
std::mutex g_mutex;  // guards what follows
Reported g_player_reported;
Reported g_role_reported;
std::optional<ac::State> g_last_role_state;
std::int64_t g_last_role_report_us = 0;
// The lyrics of the item playing, when the board holds any (kLyricsMaxBytes above 0): the document as
// the server sent it, and the playhead its anchors move, on the server's clock. There is no screen to
// draw them on yet (the panel waits for a board with one), so the board reports the line it has the
// sound in and logs each change; a screen reads this, with the cursor's word. Made when the first
// document arrives, and not as a static: an S3 with Wi-Fi up has no internal block over 31,744 bytes
// to spare for the decode task's 32 KB stack, and a few hundred bytes of .bss is what that margin is.
std::unique_ptr<iclforge::lyrics::Follower> g_lyrics;
// What the server says is playing (metadata@v1), when the hello lists the role: made when the first
// object arrives, in PSRAM where the part has it (its strings keep their capacity, so the heap a decoder
// leaves little of is spent once, and not at all by an update or a read).
std::unique_ptr<iclforge::nowview::TitleStore> g_title;
// What the control surface asked for while no player was running, for
// start_player to take up: a layout, on the heap only while one waits (a
// layout is some 600 bytes, which internal RAM would otherwise hold for
// good); and a change to the board, which a hello made before it would not
// show.
std::unique_ptr<iclforge::render::OutputLayout> g_pending_layout;
bool g_board_changed = false;
// A rename that waits for the stream that is playing to end (sendspin_name_changed()).
std::atomic<bool> g_name_pending{false};

// The player and its host, or nothing until both have started.
[[nodiscard]] const Running* running() { return g_running.load(); }

[[nodiscard]] m::PlayerState player_state(const Reported& r) {
    return m::PlayerState{.volume = r.volume,
                          .muted = r.muted,
                          .output_delay_ms = r.output_delay_ms,
                          .required_lead_time_ms = kLeadMs,
                          .min_buffer_ms = kBufferMs,
                          .supported_commands = std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume,
                                                                              m::PlayerCommand::kMute,
                                                                              m::PlayerCommand::kSetOutputDelay},
                          .format = std::nullopt};
}

[[nodiscard]] ac::Support support(const iclforge::BurstPlayer& player) {
    ac::Support s;
    s.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    // The codecs the board reproduces karaoke for are those it decodes of Annex C's: AC-3 and
    // E-AC-3, which is what a server reads it as, whatever else is listed after them.
    const std::vector<ac::DataType> karaoke_codecs = s.data_types;
    const bool ac4 = kListAc4 && iclforge::BurstPlayer::decodes_ac4();
    if (ac4) {
        s.data_types.push_back(ac::DataType::kAc4);
    }
    s.sample_rates = {static_cast<std::int32_t>(kSampleRate)};
    s.outputs.count = sink_slots();
    s.outputs.bit_depth = sink_slot_bits();
    // The widths the board's own page can set: an I2S bus takes either, a
    // sink with no hardware behind it keeps the one it was built with.
    if (std::string_view(sink_name()) == "i2s") {
        s.outputs.bit_depths = {16, 32};
    } else {
        s.outputs.bit_depths = {sink_slot_bits()};
    }
    s.layout_grammar = 1;
    s.management.routing = true;
    s.management.trim_db = {iclforge::render::TrimDelay::kMinTrimDb,
                            iclforge::render::TrimDelay::kMaxTrimDb};
    s.management.max_delay_ms = max_delay_ms();
    s.management.crossover_hz = {iclforge::render::LayoutRenderer::kMinCrossoverHz,
                                 iclforge::render::LayoutRenderer::kMaxCrossoverHz};
    s.management.identify = true;
    s.decoder_settings = kDecoderSettings;
    s.buffer_capacity = player.buffer_capacity();
    // The largest chunk the player's ring and receive buffer take (the build's
    // CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CHUNK_BYTES): a server chunks to it, for PCM over
    // player@v1 as well as for bursts.
    s.max_chunk_bytes = static_cast<std::uint32_t>(kMaxChunkBytes);
    // The widest syncframe the player decodes (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS):
    // past it a syncframe is refused, so a server is told the figure rather than finding out a
    // burst at a time. 0 in the build states nothing.
    for (const ac::DataType type : s.data_types) {
        const std::size_t limit = type == ac::DataType::kAc4 && kAc4MaxCodedChannels != 0
                                      ? kAc4MaxCodedChannels
                                      : kMaxCodedChannels;
        if (limit != 0) {
            s.max_coded_channels[static_cast<std::size_t>(type)] = static_cast<std::uint8_t>(limit);
        }
    }
    // The frames a second of AC-4 it keeps up with, where it lists AC-4 and the build says: a
    // server sends a faster stream another way, as it does one with more channels.
    if (kAc4MaxFrameRate != 0 &&
        std::ranges::find(s.data_types, ac::DataType::kAc4) != s.data_types.end()) {
        s.max_frame_rate[static_cast<std::size_t>(ac::DataType::kAc4)] =
            static_cast<std::uint16_t>(kAc4MaxFrameRate);
    }
    // How many objects it places (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS), for the E-AC-3
    // type where the build lists it: a server then renders a stream with more of them itself,
    // rather than finding that this board played its bed.
    if (kMaxObjects >= 0 &&
        std::ranges::find(s.data_types, ac::DataType::kEac3) != s.data_types.end()) {
        s.max_objects[static_cast<std::size_t>(ac::DataType::kEac3)] =
            static_cast<std::uint8_t>(kMaxObjects);
    }
    // The decoder hands the player an AC-4 presentation's channels alone (ac4_stream.hpp), so the
    // board places none of its objects: a server sends a presentation that has them as PCM it
    // decodes itself, and a presentation of objects alone is never sent as it is.
    if (ac4) {
        s.max_objects[static_cast<std::size_t>(ac::DataType::kAc4)] = 0;
    }
    s.lyrics_max_bytes = kLyricsMaxBytes;
    // The codecs it reproduces karaoke for when told to (the `karaoke` setting): every one it
    // decodes. burst_player.cpp lays a karaoke AC-3 syncframe and an E-AC-3 programme of one
    // independent substream out from the reproduction alike, so a server sends either as it is
    // (planning/karaoke-eac3.md, D6); a board that said nothing here would be taken for AC-3's
    // alone.
    s.karaoke_codecs = karaoke_codecs;
    return s;
}

// The commands the role's state lists: the lyric pair only on a board that holds lyrics.
[[nodiscard]] std::vector<ac::Command> role_commands() {
    std::vector<ac::Command> commands{ac::Command::kVolume, ac::Command::kMute, ac::Command::kSetOutputDelay,
                                      ac::Command::kSettings, ac::Command::kIdentify};
    if (kLyricsMaxBytes != 0) {
        commands.push_back(ac::Command::kLyrics);
        commands.push_back(ac::Command::kLyricsPosition);
    }
    return commands;
}

[[nodiscard]] std::string mac_text() {
    std::array<std::uint8_t, 6> mac{};
    if (!board_mac(mac)) {
        return {};
    }
    std::array<char, 18> text{};
    (void)std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                        mac[4], mac[5]);
    return text.data();
}

[[nodiscard]] ss::PlayerConfig player_config(const iclforge::BurstPlayer& player) {
    ss::PlayerConfig config;
    config.suite = kSuite == 1 ? ss::noise::Suite::kAesGcmSha256 : ss::noise::Suite::kChaChaPolySha256;
    config.name = settings().name.data();
    const esp_app_desc_t* app = esp_app_get_description();
    config.device_info = m::DeviceInfo{.product_name = "Hearth sink",
                                       .manufacturer = "ICL Forge",
                                       .software_version = app != nullptr ? app->version : "",
                                       .mac_address = mac_text()};
    config.supported_roles = {std::string(ac::kRole)};
    if (kOfferPcm) {
        config.supported_roles.emplace_back("player@v1");
    }
    if (kListMetadata) {
        config.supported_roles.emplace_back(ss::metadata::kRole);
    }
    const std::uint64_t capacity = player.buffer_capacity();
    // PCM, and Opus where the build decodes it (CONFIG_ICLFORGE_SENDSPIN_OPUS): FLAC would need its
    // decoder on the board, which the player has not measured room for
    // (planning/hearth-reference-player.md, B3). Stereo at 48 kHz, which is the only rate the
    // board plays. Opus is listed last, so that a server that is not asked for it sends PCM.
    // The depth of the sink's own slots first: a server sends the first format it can, and 24-bit
    // PCM to a 16-bit slot is half as much again in the air, in the ring and through the player
    // for the same audio (planning/esp32-sink-compatibility-matrix.md, the C6 PCM runs).
    const m::AudioFormat pcm24{
        .codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 24};
    const m::AudioFormat pcm16{
        .codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    std::vector<m::AudioFormat> formats = sink_slot_bits() <= 16
                                              ? std::vector<m::AudioFormat>{pcm16, pcm24}
                                              : std::vector<m::AudioFormat>{pcm24, pcm16};
    if (iclforge::BurstPlayer::decodes_opus()) {
        formats.push_back(
            {.codec = m::Codec::kOpus, .channels = 2, .sample_rate = 48000, .bit_depth = 16});
    }
    config.player_support =
        m::PlayerSupport{.supported_formats = std::move(formats),
                         .buffer_capacity = capacity,
                         .commands = {m::PlayerCommand::kVolume, m::PlayerCommand::kMute}};
    config.iclforge_support = support(player);
    config.pair_methods = {
        {.method = m::PairMethod::kPairingPsk,
         .locations = {m::SecretLocation::kOperator},
         .out_channels = {},
         .formats = {},
         .min_pin_length = 0},
        {.method = m::PairMethod::kDynamicCode,
         .locations = {},
         .out_channels = {m::OutChannel::kDisplay},
         .formats = {m::CodeFormat::kDigits},
         .min_pin_length = 6},
    };
    config.unpaired_access = kUnpairedAccess;
    {
        const std::lock_guard lock(g_mutex);
        config.player_state = player_state(g_player_reported);
    }
    config.iclforge_state.volume = 100;
    config.iclforge_state.muted = false;
    config.iclforge_state.required_lead_time_ms = kLeadMs;
    config.iclforge_state.min_buffer_ms = kBufferMs;
    config.iclforge_state.supported_commands = role_commands();
    config.max_message_bytes = kMaxChunkBytes + 64;
    // The tokens one JSON message may hold, which sizes the one block the session takes for it:
    // 8 KB at most, where the host's default of 4,096 tokens is 64 KB and the C6 has no internal
    // block above 31,744 bytes with Wi-Fi up (a valid 4 KB text of 2,000 one-digit values would
    // need 32 KB; a failed allocation aborts with exceptions off). The largest message a server
    // sends is 98 tokens, and no message comes within twice the cap
    // (libs/sendspin/tests/test_sessions.cpp, "no message a server sends comes within twice a
    // sink's token cap"). The one figure that grows with the board is the settings command, two
    // numbers an output: measured for sixteen, so more slots than that call for the cap again.
    static_assert(iclforge::render::OutputLayout::kMaxSlots <= 16,
                  "ss::kSinkMaxJsonTokens was sized for settings commands of sixteen outputs");
    config.max_json_tokens = ss::kSinkMaxJsonTokens;
    return config;
}

// The role's client/state, from what the burst player measured and what the
// board was told.
[[nodiscard]] ac::State role_state(const iclforge::BurstPlayerStatus& status, bool with_levels) {
    ac::State s;
    {
        const std::lock_guard lock(g_mutex);
        s.volume = g_role_reported.volume;
        s.muted = g_role_reported.muted;
        s.output_delay_ms = g_role_reported.output_delay_ms;
        s.settings_error = g_role_reported.settings_error;
    }
    s.required_lead_time_ms = kLeadMs;
    s.min_buffer_ms = kBufferMs;
    s.supported_commands = role_commands();
    s.settings_revision = status.settings_revision;
    if (status.have_decoder) {
        s.decoder = status.decoder;
    }
    if (with_levels && status.have_levels) {
        std::vector<ac::Level> levels;
        for (std::size_t o = 0; o < status.outputs; ++o) {
            levels.push_back(ac::Level{.output = static_cast<std::int32_t>(o),
                                       .peak_db = static_cast<double>(status.peak_db[o]),
                                       .rms_db = static_cast<double>(status.rms_db[o])});
        }
        s.levels = std::move(levels);
    }
    s.counters = status.counters;
    // The layout in force, as the player holds it: set by the board's page or by the last settings
    // command. A server that renders for this sink reads it here.
    if (status.layout[0] != '\0') {
        s.layout = std::string(status.layout.data());
    }
    // The lyrics held, and the line the board's own clock has the sound in: the anchors are on the
    // server's clock, which the host's time filter gives for the board's.
    if (kLyricsMaxBytes != 0) {
        const iclforge::SendspinHost* const clock = g_clock.load();
        const std::optional<std::int64_t> server =
            clock != nullptr ? clock->server_time(esp_timer_get_time()) : std::nullopt;
        const std::lock_guard lock(g_mutex);
        if (g_lyrics && g_lyrics->has_lyrics() && g_lyrics->item()) {
            const std::int32_t line = server ? g_lyrics->line(*server).value_or(-1) : -1;
            s.lyrics = ac::LyricsReport{.item = *g_lyrics->item(), .line = line};
        }
    }
    return s;
}

[[nodiscard]] bool same_report(const ac::State& a, const ac::State& b) {
    return a.levels.has_value() == b.levels.has_value() && a.volume == b.volume &&
           a.muted == b.muted && a.output_delay_ms == b.output_delay_ms &&
           a.settings_revision == b.settings_revision &&
           a.settings_error.has_value() == b.settings_error.has_value() && a.decoder == b.decoder &&
           a.layout == b.layout && a.lyrics == b.lyrics &&
           a.counters.bursts_played == b.counters.bursts_played &&
           a.counters.underruns == b.counters.underruns &&
           a.counters.late_chunks == b.counters.late_chunks &&
           a.counters.dropped_chunks == b.counters.dropped_chunks &&
           a.counters.invalid_chunks == b.counters.invalid_chunks;
}

class Events final : public iclforge::SendspinEvents {
   public:
    // Set by start_player before the host starts, and so before its first
    // event, which can come before the two are handed to the other tasks.
    iclforge::BurstPlayer* player = nullptr;
    iclforge::SendspinHost* host = nullptr;

    void on_stream_start(const m::PlayerStream& stream) override { player->start_pcm(stream.format); }
    void on_stream_clear() override { player->clear(); }
    void on_stream_end() override { player->end(); }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t server_time, std::int64_t local_time) override {
        player->pcm(frame, server_time, local_time);
    }
    void on_player_command(const m::PlayerCommandMessage& command) override {
        m::PlayerState state;
        {
            const std::lock_guard lock(g_mutex);
            apply(g_player_reported, command.command == m::PlayerCommand::kVolume   ? Change::kVolume
                                     : command.command == m::PlayerCommand::kMute ? Change::kMute
                                                                                  : Change::kDelay,
                  command.volume, command.mute, command.output_delay_ms);
            state = player_state(g_player_reported);
            player->set_volume(g_player_reported.volume, g_player_reported.muted);
        }
        host->set_player_state(state);
    }

    void on_burst_stream_start(const ac::StreamStart& stream) override { player->start_bursts(stream); }
    // A flush moves the sound: the document stays for the item, the position waits for the new
    // stream's first anchor. The end of the stream is nothing playing.
    void on_burst_stream_clear() override {
        player->clear();
        const std::lock_guard lock(g_mutex);
        if (g_lyrics) {
            g_lyrics->clear_position();
        }
    }
    void on_burst_stream_end() override {
        player->end();
        const std::lock_guard lock(g_mutex);
        if (g_lyrics) {
            g_lyrics->reset();
        }
    }
    void on_burst(const ss::BurstChunk& chunk, std::int64_t local_time) override { player->burst(chunk, local_time); }
    void on_invalid_burst() override { player->invalid_chunk(); }

    void on_iclforge_command(const ac::CommandMessage& command) override {
        switch (command.command) {
            case ac::Command::kVolume:
            case ac::Command::kMute:
            case ac::Command::kSetOutputDelay: {
                const std::lock_guard lock(g_mutex);
                apply(g_role_reported, command.command == ac::Command::kVolume ? Change::kVolume
                                       : command.command == ac::Command::kMute ? Change::kMute
                                                                               : Change::kDelay,
                      command.volume, command.mute, command.output_delay_ms);
                player->set_volume(g_role_reported.volume, g_role_reported.muted);
                break;
            }
            case ac::Command::kSettings: {
                const std::optional<std::string> why = player->settings(command.settings, support(*player));
                const std::lock_guard lock(g_mutex);
                if (why) {
                    g_role_reported.settings_error = ac::SettingsError{.revision = command.settings.revision, .why = *why};
                    std::printf("sendspin: settings %lld refused: %s\n",
                                static_cast<long long>(command.settings.revision), why->c_str());
                } else {
                    g_role_reported.settings_error.reset();
                }
                break;
            }
            case ac::Command::kIdentify:
                player->identify(command.identify);
                break;
            case ac::Command::kLyrics: {
                if (kLyricsMaxBytes == 0) {
                    break;
                }
                const std::lock_guard lock(g_mutex);
                if (!g_lyrics) {
                    g_lyrics = std::make_unique<iclforge::lyrics::Follower>();
                }
                // The last document goes first, so that what is free is what this one has to fit in.
                g_lyrics->set_document(command.lyrics.item, std::string_view{}, kLyricsMaxBytes);
                // A parsed document is lines and words: measured on boards, about ten times its text
                // at the peak and six times once held, of whichever heap it is made in. The internal
                // heap a stream leaves is what Wi-Fi's receive buffers and the decoder's scratch come
                // out of (an S3 playing E-AC-3 has a few hundred bytes to spare), so a part with PSRAM
                // reads and holds it there (iclforge::player::PsramScope), and a document is taken
                // only while what it costs of the heap it goes to is free beyond a reserve; otherwise
                // the board holds none, and says why.
                const std::size_t size = command.lyrics.text.size();
                const std::size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                const std::size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                const bool in_psram = free_psram >= kLyricsHeapFactor * size + kLyricsPsramReserve;
                // In PSRAM the internal heap still holds the message as it arrives and is read (the
                // channel's buffer and the command's text, a few times its size at once).
                const std::size_t needed =
                    (in_psram ? kLyricsTransientFactor : kLyricsHeapFactor) * size + kLyricsHeapReserve;
                const bool affordable = size == 0 || free_internal >= needed;
                bool held = false;
                const std::int64_t began_us = esp_timer_get_time();
                if (affordable) {
                    std::optional<iclforge::player::PsramScope> scope;
                    if (in_psram) {
                        scope.emplace();
                    }
                    held = g_lyrics->set_document(command.lyrics.item, command.lyrics.text, kLyricsMaxBytes);
                }
                std::printf("sendspin: lyrics for item %lld: %s (%u bytes, in %s, read in %lld us)\n",
                            static_cast<long long>(command.lyrics.item), held ? "held" : "none",
                            static_cast<unsigned>(size), in_psram ? "PSRAM" : "internal RAM",
                            static_cast<long long>(esp_timer_get_time() - began_us));
                if (!affordable) {
                    std::printf("sendspin: lyrics not taken: %u bytes of internal heap free, %u needed\n",
                                static_cast<unsigned>(free_internal), static_cast<unsigned>(needed));
                }
                break;
            }
            case ac::Command::kLyricsPosition: {
                if (kLyricsMaxBytes == 0) {
                    break;
                }
                const std::lock_guard lock(g_mutex);
                const ac::LyricsAnchor& anchor = command.lyrics_position;
                if (g_lyrics) {
                    g_lyrics->anchor(anchor.item, anchor.position_ms, anchor.at_us, anchor.speed);
                }
                break;
            }
        }
        report_now();
    }

    void on_settings_refused(const ac::SettingsError& error) override {
        {
            const std::lock_guard lock(g_mutex);
            g_role_reported.settings_error = error;
        }
        report_now();
    }

    // metadata@v1: what the server says is playing, for the title card (planning/p4x-now-playing.md).
    // The store takes the object as the server's dialect means it (title_card.hpp): the specification's
    // (and aiosendspin 10's, which Music Assistant runs now) is the whole state, and aiosendspin 9.1.1's only
    // what changed, a cleared field as null. A field is `kSet` when it came with a value, `kClear` when it
    // came as null and `kLeave` when the object did not name it. The console says what arrived, which is
    // how the first run with a server shows what it sends this role.
    void on_metadata(const m::ServerState& state, ss::Dialect dialect) override {
        if (!state.metadata) {
            return;
        }
        namespace md = ss::metadata;
        namespace nv = iclforge::nowview;
        nv::TitleUpdate update;
        update.whole = dialect == ss::Dialect::kSpecification;
        const std::optional<md::State>& object = *state.metadata;
        std::array<char, 200> said{};
        if (!object) {
            update.clear_all = true;
            std::snprintf(said.data(), said.size(), " none");
        } else {
            const std::uint8_t nulls = state.metadata_nulls;
            const auto text = [nulls](const std::optional<std::string>& value, std::uint8_t bit) {
                if (value) {
                    return nv::TitleUpdate::Text{.say = nv::Say::kSet, .value = *value};
                }
                return nv::TitleUpdate::Text{.say = (nulls & bit) != 0 ? nv::Say::kClear : nv::Say::kLeave, .value = {}};
            };
            update.timestamp_us = object->timestamp;
            update.title = text(object->title, md::kNullTitle);
            update.artist = text(object->artist, md::kNullArtist);
            update.album = text(object->album, md::kNullAlbum);
            if (object->year) {
                // A year a server has not got is 0 or nonsense, and is no year.
                const bool real = *object->year >= 1000 && *object->year <= 9999;
                update.year = {.say = real ? nv::Say::kSet : nv::Say::kClear, .value = *object->year};
            } else if ((nulls & md::kNullYear) != 0) {
                update.year = {.say = nv::Say::kClear, .value = 0};
            }
            if (object->progress) {
                update.progress = {.say = nv::Say::kSet,
                                   .position_ms = object->progress->track_progress_ms,
                                   .duration_ms = object->progress->track_duration_ms,
                                   .speed = object->progress->playback_speed};
            } else if ((nulls & md::kNullProgress) != 0) {
                update.progress = {.say = nv::Say::kClear};
            }
            // A line of the console: what the object named, a title cut short, and where its time is
            // against the board's clock (a stamp in the future is a scheduled change).
            const iclforge::SendspinHost* const clock = g_clock.load();
            const std::optional<std::int64_t> server =
                clock != nullptr ? clock->server_time(esp_timer_get_time()) : std::nullopt;
            std::array<char, 80> piece{};
            const auto append = [&said, &piece] {
                const std::size_t have = std::strlen(said.data());
                std::snprintf(said.data() + have, said.size() - have, "%s", piece.data());
            };
            const auto named = [&](const char* name, const nv::TitleUpdate::Text& field) {
                if (field.say == nv::Say::kSet) {
                    std::snprintf(piece.data(), piece.size(), " %s \"%.*s\"", name,
                                  static_cast<int>(std::min<std::size_t>(field.value.size(), 40)), field.value.data());
                    append();
                } else if (field.say == nv::Say::kClear) {
                    std::snprintf(piece.data(), piece.size(), " %s null", name);
                    append();
                }
            };
            named("title", update.title);
            named("artist", update.artist);
            named("album", update.album);
            if (update.year.say == nv::Say::kSet) {
                std::snprintf(piece.data(), piece.size(), " year %d", static_cast<int>(update.year.value));
                append();
            } else if (update.year.say == nv::Say::kClear) {
                std::snprintf(piece.data(), piece.size(), " year null");
                append();
            }
            if (update.progress.say == nv::Say::kSet) {
                std::snprintf(piece.data(), piece.size(), " progress %lld/%lld x%d",
                              static_cast<long long>(update.progress.position_ms),
                              static_cast<long long>(update.progress.duration_ms),
                              static_cast<int>(update.progress.speed));
                append();
            } else if (update.progress.say == nv::Say::kClear) {
                std::snprintf(piece.data(), piece.size(), " progress null");
                append();
            }
            if (server) {
                std::snprintf(piece.data(), piece.size(), ", stamped %+lld ms",
                              static_cast<long long>((update.timestamp_us - *server) / 1000));
                append();
            }
        }
        {
            const std::lock_guard lock(g_mutex);
            if (!g_title) {
                std::optional<iclforge::player::PsramScope> scope;
                scope.emplace();
                g_title = std::make_unique<iclforge::nowview::TitleStore>();
            }
            g_title->apply(update);
        }
        std::printf("sendspin: metadata (%s):%s\n", dialect == ss::Dialect::kSpecification ? "whole state" : "changes", said.data());
    }

    void on_metadata_gone() override {
        const std::lock_guard lock(g_mutex);
        if (g_title) {
            g_title->clear();
        }
    }

    void on_pairing_code(std::string_view /*digits*/) override {}
    void on_pairing_held_back() override {}
    void on_pairing_ended(std::string_view /*outcome*/) override {}

   private:
    enum class Change : std::uint8_t { kVolume, kMute, kDelay };

    static void apply(Reported& reported, Change change, std::int32_t volume, bool mute, std::int32_t delay_ms) {
        switch (change) {
            case Change::kVolume:
                reported.volume = std::clamp<std::int32_t>(volume, 0, 100);
                break;
            case Change::kMute:
                reported.muted = mute;
                break;
            case Change::kDelay:
                reported.output_delay_ms = std::clamp<std::int32_t>(delay_ms, 0, 5000);
                break;
        }
    }

    // The next poll sends the state whatever it measured.
    static void report_now() {
        const std::lock_guard lock(g_mutex);
        g_last_role_state.reset();
    }
};

Events g_events;

void print_token(const iclforge::SendspinHost& host) {
    const iclforge::SendspinStore& store = host.store();
    std::array<std::uint8_t, 64> payload{};
    std::copy(store.identity().public_key().begin(), store.identity().public_key().end(), payload.begin());
    std::copy(store.pairing_psk().begin(), store.pairing_psk().end(), payload.begin() + 32);
    // The console is the board's own display: whoever reads it is holding the
    // board, which is what the token asks of them. So it stays out of GET
    // /log, which anyone on the network can read.
    {
        const iclforge::ConsoleOnly console_only;
        std::printf("sendspin: pairing token %s\n",
                    ss::pairing::encode_token(ss::pairing::TokenVersion::kPairingPsk, payload).c_str());
    }
    ss::crypto::wipe(payload);
}

// A stack for the work that makes or reads the player's keys: starting it
// (a Noise identity to make on the first boot, the store to read, and the
// configuration its sessions copy) and forgetting every pairing (a new
// identity). That is more than app_main's 8 KB, or the control server's or the
// console's task, leaves. The task is created in internal RAM, which NVS
// writes need, and goes when the work is done.
constexpr std::uint32_t kKeyWorkStackBytes = 16384;

struct KeyWork {
    std::function<void()> work;
    SemaphoreHandle_t done = nullptr;
    UBaseType_t unused = 0;
};

void key_work_task(void* argument) {
    auto* job = static_cast<KeyWork*>(argument);
    job->work();
    job->unused = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

// Runs `work` on its own stack and waits for it; false when there was no
// memory for the task.
bool on_key_stack(const char* what, std::function<void()> work) {
    KeyWork job{.work = std::move(work), .done = xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        return false;
    }
    if (xTaskCreatePinnedToCore(&key_work_task, "sendspin-keys", kKeyWorkStackBytes, &job, tskIDLE_PRIORITY + 5,
                                nullptr, tskNO_AFFINITY) != pdPASS) {
        vSemaphoreDelete(job.done);
        std::printf("sendspin: no memory for a %u-byte stack to %s on\n", static_cast<unsigned>(kKeyWorkStackBytes),
                    what);
        return false;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    std::printf("sendspin: %s left %u of a %u-byte stack unused\n", what, static_cast<unsigned>(job.unused),
                static_cast<unsigned>(kKeyWorkStackBytes));
    return true;
}

void start_player(const iclforge::render::OutputLayout& layout);

}  // namespace

bool sendspin_built() { return true; }

void sendspin_start(const iclforge::render::OutputLayout& layout) {
    if (running() != nullptr || !network_ready()) {
        if (!network_ready()) {
            std::printf("sendspin: no network, so no player\n");
        }
        return;
    }
    (void)on_key_stack("starting the player", [&layout] { start_player(layout); });
}

namespace {

void start_player(const iclforge::render::OutputLayout& layout) {
    // The player starts first, and the host after it. The player's task
    // stack (kDecodeStackBytes, 32 KB on an ESP32-S3) is the largest block
    // anything here asks internal RAM for. By now the network, mDNS, the
    // control surface and the sink's DMA buffers have left that RAM in
    // pieces: on 2026-09-25 an ESP32-S3 had 104,319 bytes free but no block
    // above 31,744 once the host's state had been made first, and the player
    // did not start. The player reads the host's clock only once a server
    // plays to it, which is after the host has started; until then g_clock
    // is null and there is no server time to convert.
    iclforge::BurstPlayerConfig config;
    config.sample_rate = kSampleRate;
    config.ring_bytes = kRingBytes;
    config.max_chunk_bytes = kMaxChunkBytes;
    // An I2S bus's ceiling moves with its slot width and wiring, so the
    // buffers are sized once for the most the sink can reach at any setting:
    // sixteen on an ESP32-S3's two lines, eight on an ESP32-C6's one, where
    // every per-output buffer is internal RAM. A sink with no hardware behind
    // it keeps the slots it was built with.
    config.max_outputs = std::min<std::size_t>(iclforge::Playout::kMaxOutputs,
                                               static_cast<std::size_t>(std::max(sink_max_slots(), 1)));
    config.max_delay_ms = max_delay_ms();
    // The listener's own level and distance for each speaker; a change before the player is up is taken
    // again below, once it can be handed over.
    config.speaker_trim = settings().speaker_trim;
    config.max_coded_channels = kMaxCodedChannels;
    config.max_ac4_channels = kAc4MaxCodedChannels;
    config.max_ac4_frame_rate = kAc4MaxFrameRate;
    // A stream declaring more objects than the board states it places is played as its bed
    // (iclforge/object_limit.hpp): the figure the support object carries is the one it keeps to.
    if (kMaxObjects >= 0) {
        config.max_objects = static_cast<std::uint8_t>(kMaxObjects);
    }
    config.core = kDecodeCore;
    config.stack_bytes = kDecodeStackBytes;
    config.stack_in_psram = kDecodeStackInPsram;
    config.decode_lane = CONFIG_ICLFORGE_EXAMPLE_DECODE_LANE != 0;
    config.report_every_chunks = kReportEveryChunks;
    config.local_time = [](std::int64_t server_us) -> std::optional<std::int64_t> {
        const iclforge::SendspinHost* const clock = g_clock.load();
        return clock != nullptr ? clock->local_time(server_us) : std::nullopt;
    };
    config.layout = layout;
    {
        // A layout the control surface set after the caller read its own:
        // app_main applies the command carrying it only after this returns.
        const std::lock_guard lock(g_mutex);
        if (g_pending_layout) {
            config.layout = *g_pending_layout;
            g_pending_layout.reset();
        }
    }
    config.decoder.output.mode = kMode;
    config.decoder.joc_domain = kJocDomain;
    config.stereo_fold = kStereoFold;
    config.objects = kObjects;
    config.dynamic_range = settings().dynamic_range;
    auto player = std::make_unique<iclforge::BurstPlayer>(config, g_sink);
    if (!player->start()) {
        std::printf("sendspin: no player: it could not start (the lines above say why)\n");
        return;
    }
    auto host = std::make_unique<iclforge::SendspinHost>();
    g_clock.store(host.get());
    g_events.player = player.get();
    g_events.host = host.get();
    iclforge::SendspinHostConfig host_config;
    host_config.port = kSendspinPort;
    host_config.stack_bytes = kServerStackBytes;
    host_config.core = 0;
    {
        // The hello is made from the board as it is now; a change from here
        // on is taken up below.
        const std::lock_guard lock(g_mutex);
        g_board_changed = false;
    }
    host_config.player = player_config(*player);
    if (!host->start(std::move(host_config), g_events)) {
        // The player's task reads the host's clock, so it goes first.
        g_clock.store(nullptr);
        player.reset();
        return;
    }

    // Handed to the other tasks, with what the control surface asked for
    // meanwhile: a layout for the player, and a changed board for the host's
    // hello. Both requests take the lock this publishes under, so each one
    // lands either here or on the running player.
    g_started.player = std::move(player);
    g_started.host = std::move(host);
    while (true) {
        {
            const std::lock_guard lock(g_mutex);
            // A trim the control surface stored while the player was starting.
            g_started.player->set_speaker_trim(settings().speaker_trim);
            if (g_pending_layout) {
                if (!g_started.player->set_layout(*g_pending_layout)) {
                    std::printf("sendspin: the player cannot take layout %s\n", g_pending_layout->text().data());
                }
                g_pending_layout.reset();
            }
            // A choice made while the player started: the setting is stored before the route calls
            // sendspin_set_dynamic_range(), so the stored one is the latest either way.
            g_started.player->set_dynamic_range(settings().dynamic_range);
            g_started.player->set_block_tap(g_block_tap);
            if (!std::exchange(g_board_changed, false)) {
                g_running.store(&g_started);
                break;
            }
        }
        // Outside the lock, which player_config() takes.
        g_started.host->set_player_config(player_config(*g_started.player));
    }
    print_token(*g_started.host);
    // What is left once the player is up and nothing plays: a first unit's
    // decoder and a server's connection buffers come out of this. On a part
    // with no PSRAM every buffer above is internal RAM too.
    std::printf("sendspin: player up with internal heap %u free, largest block %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

}  // namespace

bool sendspin_running() { return running() != nullptr; }

void sendspin_leave() {
    const Running* const r = running();
    if (r == nullptr) {
        return;
    }
    r->host->leave();
    if (!r->player->hold(true)) {
        std::printf("sendspin: the player did not stop writing in time\n");
    }
}

bool sendspin_playing() {
    const Running* const r = running();
    return r != nullptr && r->player->active();
}

void sendspin_set_external(bool external) {
    const Running* const r = running();
    if (r == nullptr || g_external.exchange(external) == external) {
        return;
    }
    // The servers are told the board is not available, and their sessions
    // drop what they are still sent. The player stops writing before this
    // returns, so whatever takes the sink next has it to itself.
    r->host->set_external_source(external);
    if (!r->player->hold(external)) {
        std::printf("sendspin: the player did not stop writing in time\n");
    }
}

bool sendspin_set_layout(const iclforge::render::OutputLayout& layout) {
    const std::lock_guard lock(g_mutex);
    if (const Running* const r = running()) {
        return r->player->set_layout(layout);
    }
    // For a player that has yet to start, or is starting.
    if (g_pending_layout) {
        *g_pending_layout = layout;
    } else {
        g_pending_layout = std::make_unique<iclforge::render::OutputLayout>(layout);
    }
    return true;
}

void sendspin_set_dynamic_range(iclforge::ac3::DynamicRange range) {
    if (const Running* const r = running()) {
        r->player->set_dynamic_range(range);
    }
    // A player that has yet to start reads the stored setting as it does.
}

void sendspin_set_block_tap(iclforge::BlockTap* tap) {
    // Under the lock the player's start takes, so that the tap lands either on the player that is being made or on the one that runs.
    const std::lock_guard lock(g_mutex);
    g_block_tap = tap;
    if (const Running* const r = running()) {
        r->player->set_block_tap(tap);
    }
}

void sendspin_board_changed() {
    const Running* r = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        r = running();
        if (r == nullptr) {
            // For a player that has yet to start, or is starting.
            g_board_changed = true;
            return;
        }
    }
    r->host->set_player_config(player_config(*r->player));
}

SoundCaps sendspin_sound_caps() {
    SoundCaps caps;
    caps.player = true;
    caps.identify_reason = "";
    caps.trim = iclforge::render::speaker_trim::trim_caps(
        delay_lines_possible() ? iclforge::render::TrimDelay::samples_for_ms(static_cast<double>(kMaxDelayMs), kSampleRate) : 0,
        kSampleRate);
    if (!caps.trim.delay) {
        caps.trim.delay_reason = kMaxDelayMs == 0 ? "this build has no delay lines"
                                                  : "this board has no PSRAM to hold delay lines";
    }
    return caps;
}

void sendspin_set_speaker_trim(const iclforge::render::speaker_trim::SpeakerTrimTable& table) {
    const std::lock_guard lock(g_mutex);
    if (const Running* const r = running()) {
        r->player->set_speaker_trim(table);
    }
    // A player that has yet to start takes settings().speaker_trim as it starts.
}

std::optional<SpeakerTrimStatus> sendspin_speaker_trim_status() {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    const iclforge::BurstPlayerTrim play = r->player->trim_status();
    SpeakerTrimStatus out;
    out.layout = play.layout;
    out.server_level = play.server_level;
    out.server_delay = play.server_delay;
    out.farthest_cm = play.farthest_cm;
    for (std::size_t o = 0; o < out.rows.size(); ++o) {
        if (play.role[o] == iclforge::render::speaker_trim::kNoRole) {
            continue;
        }
        out.rows[out.count++] = SpeakerTrimStatus::Row{.role = play.role[o],
                                                       .output = static_cast<std::uint8_t>(o),
                                                       .applied_level_db = play.level_db[o],
                                                       .applied_delay_samples = play.delay_samples[o],
                                                       .delay_clamped = ((play.clamped >> o) & 1U) != 0};
    }
    return out;
}

iclforge::render::IdentifyOutcome sendspin_identify(const iclforge::render::IdentifyRequest& request) {
    const Running* const r = running();
    if (r == nullptr) {
        return iclforge::render::IdentifyOutcome::kNoPlayer;
    }
    // A play from the control surface holds the player: it is not available for a tone.
    if (g_external.load() && request.shape != iclforge::render::IdentifyRequest::Shape::kOff) {
        return iclforge::render::IdentifyOutcome::kBusy;
    }
    return r->player->identify_local(request);
}

IdentifyStatus sendspin_identify_status() {
    IdentifyStatus out;
    const Running* const r = running();
    if (r == nullptr) {
        return out;
    }
    const iclforge::BurstPlayerStatus play = r->player->status();
    out.active = play.identify_local;
    out.output = play.identify_output;
    out.role = play.identify_role;
    out.remaining_ms = play.identify_remaining_ms;
    return out;
}

void sendspin_name_changed() {
    if (sendspin_playing()) {
        g_name_pending.store(true);
        std::printf("sendspin: renamed; the servers hear the new name when the stream that plays has ended\n");
        return;
    }
    sendspin_board_changed();
}

std::optional<iclforge::ControlSendspin> sendspin_status() {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    const iclforge::SendspinStatus host = r->host->status();
    const iclforge::BurstPlayerStatus play = r->player->status();
    iclforge::ControlSendspin s;
    s.server = host.server_name.data();
    s.server_id = host.server_id.data();
    s.dialect = host.dialect;
    s.psk = host.psk;
    s.activity = host.activity;
    s.role = host.role;
    s.clock_converged = host.clock_converged;
    s.clock_error_us = host.clock_error_us;
    s.clock_updates = host.clock_updates;
    s.clock_rejected = host.clock_rejected;
    s.connections = host.connections;
    s.client_id = host.client_id.data();
    s.paired = host.paired_servers;
    s.pairing_code = host.pairing_code.data();
    s.pairing_held = host.pairing_held_back;
    s.pairing_rounds = host.pairing_rounds;
    s.pairing_outcome = host.pairing_outcome.data();
    s.lost_pairing = host.server_has_lost_pairing;
    s.stream = play.stream;
    s.bursts = play.counters.bursts_played;
    s.underruns = play.counters.underruns;
    s.late = play.counters.late_chunks;
    s.dropped = play.counters.dropped_chunks;
    s.invalid = play.counters.invalid_chunks;
    s.resyncs = play.playout.resyncs;
    s.error_us = play.playout.smoothed_error_us;
    s.worst_error_us = play.playout.worst_error_us;
    if (play.have_play) {
        s.play_frame = play.play_frame;
        if (const std::optional<std::int64_t> server = r->host->server_time(play.play_local_us)) {
            s.play_server_us = *server;
            // When frame 0 played, by the same measure: what two boards in a
            // group compare, whichever frames each last reported.
            s.origin_server_us =
                *server - static_cast<std::int64_t>((play.play_frame * 1'000'000 + (kSampleRate / 2)) / kSampleRate);
        }
    }
    if (play.have_levels) {
        for (std::size_t o = 0; o < play.outputs; ++o) {
            s.peak_db.push_back(play.peak_db[o]);
            s.rms_db.push_back(play.rms_db[o]);
            s.stream_rms.push_back(play.stream_rms[o]);
        }
    }
    s.burst_us = play.burst_us;
    s.worst_burst_us = play.worst_burst_us;
    s.decode_stack_free = static_cast<unsigned long>(play.stack_free);
    s.server_stack_free = static_cast<unsigned long>(host.stack_free);
    s.settings_revision = play.settings_revision;
    s.identifying = play.identifying;
    return s;
}

SendspinLevels sendspin_now_levels() {
    SendspinLevels out;
    const Running* const r = running();
    if (r == nullptr) {
        return out;
    }
    const iclforge::BurstPlayerLevels play = r->player->levels();
    out.have = play.have;
    out.serial = play.serial;
    out.outputs = std::min(play.outputs, out.peak_db.size());
    for (std::size_t o = 0; o < out.outputs; ++o) {
        out.peak_db[o] = play.peak_db[o];
        out.rms_db[o] = play.rms_db[o];
    }
    return out;
}

SendspinPlaying sendspin_now_playing() {
    SendspinPlaying out;
    const Running* const r = running();
    if (r == nullptr) {
        return out;
    }
    const iclforge::BurstPlayerStatus play = r->player->status();
    const std::string_view stream = play.stream != nullptr ? std::string_view(play.stream) : std::string_view();
    out.active = !stream.empty() && stream != "idle";
    if (!out.active) {
        return out;
    }
    out.layout = play.layout;
    out.outputs = std::min(play.outputs, out.peak_db.size());
    out.have_levels = play.have_levels;
    out.coded_bytes = play.coded_bytes;
    for (std::size_t o = 0; o < out.outputs; ++o) {
        out.peak_db[o] = play.peak_db[o];
        out.rms_db[o] = play.rms_db[o];
    }
    namespace nv = iclforge::nowview;
    if (stream == "pcm") {
        out.facts.codec = nv::Codec::kPcm;
    } else if (play.have_decoder) {
        switch (play.decoder.data_type) {
            case ac::DataType::kAc3:
                out.facts.codec = nv::Codec::kAc3;
                break;
            case ac::DataType::kEac3:
                out.facts.codec = nv::Codec::kEac3;
                break;
            case ac::DataType::kAc4:
                out.facts.codec = nv::Codec::kAc4;
                break;
        }
        out.facts.have_decoder = true;
        out.facts.acmod = play.decoder.acmod;
        out.facts.lfe = play.decoder.lfe;
        out.facts.objects = play.decoder.objects;
    } else {
        // A burst stream before its first unit has decoded.
        out.facts.codec = nv::Codec::kUnknown;
    }
    return out;
}

bool sendspin_read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                          const iclforge::lyrics::StageOptions& options) {
    // The position is on the server's clock, which the host's time filter gives for the board's
    // (as the report of the line does). Without a converged clock there is no position, and the
    // stage is "waiting" with the first lines to show.
    const iclforge::SendspinHost* const clock = g_clock.load();
    const std::optional<std::int64_t> server =
        clock != nullptr ? clock->server_time(esp_timer_get_time()) : std::nullopt;
    const std::lock_guard lock(g_mutex);
    const iclforge::lyrics::Lyrics* const lyrics = g_lyrics ? g_lyrics->lyrics() : nullptr;
    std::optional<std::int64_t> position;
    if (g_lyrics && server) {
        position = g_lyrics->position_ms(*server);
    }
    const iclforge::lyrics::Stage stage = iclforge::lyrics::make_stage(lyrics, position, delay_ms, options);
    const std::int64_t item = (g_lyrics && g_lyrics->item()) ? *g_lyrics->item() : -1;
    reader.on_lyrics(item, lyrics, stage);
    return true;
}

bool sendspin_read_title(TitleReader& reader) {
    // The song's position is on the server's clock, which the host's time filter gives for the board's.
    // Without a converged clock the song has no position, and the card has no time to show.
    const iclforge::SendspinHost* const clock = g_clock.load();
    const std::optional<std::int64_t> server =
        clock != nullptr ? clock->server_time(esp_timer_get_time()) : std::nullopt;
    const std::lock_guard lock(g_mutex);
    if (!g_title || !g_title->known()) {
        reader.on_title(nullptr, std::nullopt);
        return true;
    }
    reader.on_title(g_title.get(), server ? g_title->position_ms(*server) : std::nullopt);
    return true;
}

bool sendspin_pairing(std::string_view action) {
    const Running* const r = running();
    if (r == nullptr) {
        return false;
    }
    if (action == "reset") {
        r->host->reset_pairing_rounds();
        return true;
    }
    if (action == "cancel") {
        r->host->cancel_pairing();
        return true;
    }
    if (action == "forget") {
        bool forgotten = false;
        if (!on_key_stack("forgetting every pairing",
                          [r, &forgotten] { forgotten = r->host->forget_pairings(); })) {
            return false;
        }
        std::printf("sendspin: pairings %s\n", forgotten ? "forgotten; a new identity" : "could not all be forgotten");
        if (forgotten) {
            print_token(*r->host);
        }
        return forgotten;
    }
    return false;
}

std::optional<iclforge::ControlPairings> sendspin_pairings() {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    const iclforge::SendspinPairings pairings = r->host->pairings();
    iclforge::ControlPairings out;
    out.capacity = static_cast<unsigned>(iclforge::SendspinStore::kRecordCapacity);
    out.servers.reserve(pairings.count);
    for (std::size_t i = 0; i < pairings.count; ++i) {
        const iclforge::SendspinPairing& p = pairings.servers[i];
        out.servers.push_back(iclforge::ControlPairing{.server_id = ss::base64url::encode(p.server_key),
                                                       .name = p.name.data(),
                                                       .connected = p.connected,
                                                       .last_playback = p.last_playback,
                                                       .seen = p.seen});
    }
    return out;
}

std::optional<bool> sendspin_forget_server(std::string_view server_id) {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    ss::crypto::Key32 key{};
    if (!ss::base64url::decode_exact(server_id, key)) {
        return false;
    }
    return r->host->forget_server(key);
}

namespace {

// `pair list`: each record's server_id as the console shows it, its name, and
// what it is doing.
void print_pairings(const iclforge::SendspinHost& host) {
    const iclforge::SendspinPairings pairings = host.pairings();
    std::printf("sendspin: %u of %u pairing records, the most recently used first\n",
                static_cast<unsigned>(pairings.count), static_cast<unsigned>(iclforge::SendspinStore::kRecordCapacity));
    for (std::size_t i = 0; i < pairings.count; ++i) {
        const iclforge::SendspinPairing& p = pairings.servers[i];
        std::printf("sendspin:   %s  %s%s%s%s\n", ss::base64url::encode(p.server_key).substr(0, 8).c_str(),
                    p.name[0] != '\0' ? p.name.data() : "(no name yet)", p.connected ? ", connected" : "",
                    p.last_playback ? ", the last to play" : "", p.seen ? "" : ", not seen since the board started");
    }
}

// `pair forget ID`: the one record whose server_id starts with `id`, which is
// the whole of it or the first eight or more of its characters, as
// `pair list` prints them. Nothing when none does, or more than one.
[[nodiscard]] std::optional<ss::crypto::Key32> record_for(const iclforge::SendspinPairings& pairings,
                                                          std::string_view id) {
    if (id.size() < 8) {
        return std::nullopt;
    }
    std::optional<ss::crypto::Key32> found;
    for (std::size_t i = 0; i < pairings.count; ++i) {
        if (ss::base64url::encode(pairings.servers[i].server_key).starts_with(id)) {
            if (found) {
                return std::nullopt;
            }
            found = pairings.servers[i].server_key;
        }
    }
    return found;
}

}  // namespace

bool sendspin_console(std::string_view line) {
    const Running* const r = running();
    if (r == nullptr) {
        return false;
    }
    if (line == "pair reset") {
        return sendspin_pairing("reset");
    }
    if (line == "pair cancel") {
        return sendspin_pairing("cancel");
    }
    if (line == "pair forget") {
        return sendspin_pairing("forget");
    }
    // Both read the servers' names from NVS, which the console's own 4 KB
    // task has too little stack for.
    if (line == "pair list") {
        (void)on_key_stack("listing the pairings", [r] { print_pairings(*r->host); });
        return true;
    }
    constexpr std::string_view kForgetOne = "pair forget ";
    if (line.starts_with(kForgetOne)) {
        const std::string_view id = line.substr(kForgetOne.size());
        (void)on_key_stack("forgetting a pairing", [r, id] {
            const std::optional<ss::crypto::Key32> key = record_for(r->host->pairings(), id);
            if (!key || !r->host->forget_server(*key)) {
                std::printf("sendspin: no one pairing has a server_id starting '%.*s'; 'pair list' shows them\n",
                            static_cast<int>(id.size()), id.data());
            }
        });
        return true;
    }
    if (line == "pair token") {
        print_token(*r->host);
        return true;
    }
    if (line == "sendspin") {
        const std::optional<iclforge::ControlSendspin> s = sendspin_status();
        if (!s) {
            return true;
        }
        std::printf("sendspin: server '%s' (%s, %s, %s), role %s, clock %s (%lld us), %u connection(s), %u paired, "
                    "playing %s: %llu bursts, %llu underruns, %llu late, error %lld us\n",
                    s->server.c_str(), s->dialect.c_str(), s->psk.c_str(), s->activity.c_str(), s->role.c_str(),
                    s->clock_converged ? "converged" : "converging", s->clock_error_us, s->connections, s->paired,
                    s->stream.c_str(), s->bursts, s->underruns, s->late, s->error_us);
        return true;
    }
    return false;
}

void sendspin_poll() {
    const Running* const r = running();
    if (r == nullptr) {
        return;
    }
    const iclforge::BurstPlayerStatus status = r->player->status();
    const bool playing = status.stream != std::string_view("idle");
    // A rename waited for the stream to end: it has.
    if (!playing && g_name_pending.exchange(false)) {
        sendspin_board_changed();
    }
    const std::int64_t now = esp_timer_get_time();
    ac::State state = role_state(status, playing);
    bool send = false;
    bool lyric_line_changed = false;
    {
        const std::lock_guard lock(g_mutex);
        // While a stream plays, fresh levels at most ten times a second; any
        // other change at once (the extension page, State object).
        if (!g_last_role_state || !same_report(*g_last_role_state, state)) {
            send = true;
        } else if (playing && state.levels && now - g_last_role_report_us >= 100'000) {
            send = true;
        }
        if (send) {
            lyric_line_changed = state.lyrics.has_value() &&
                                 (!g_last_role_state || g_last_role_state->lyrics != state.lyrics);
            g_last_role_state = state;
            g_last_role_report_us = now;
        }
    }
    if (lyric_line_changed) {
        // What a screen would show, until there is one: the line the sound is in.
        std::printf("sendspin: lyrics item %lld line %d\n", static_cast<long long>(state.lyrics->item),
                    static_cast<int>(state.lyrics->line));
    }
    if (send) {
        r->host->set_iclforge_state(state);
    }
}

}  // namespace player
