// The Sendspin player's audio. See ../include/iclforge/burst_player.hpp.

#include "iclforge/burst_player.hpp"
#include "lane_executor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

#include "ac4_stream.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/dynamic_range.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/identify.hpp"
#include "iclforge/render/identify_sequence.hpp"
#include "iclforge/render/output_trim.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/render/trim_delay.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "opus_stream.hpp"

#include "iclforge/access_units.hpp"
#include "iclforge/decoder_keep.hpp"
#include "iclforge/object_limit.hpp"
#include "iclforge/pcm_unpack.hpp"

namespace iclforge {
namespace {

namespace ss = iclforge::sendspin;
namespace ac = ss::player;
namespace m = ss::messages;
using iclforge::render::IdentifySequence;
using iclforge::render::IdentifyTone;
using iclforge::render::LayoutRenderer;
using iclforge::render::OutputLayout;
using iclforge::render::OutputTrim;
using iclforge::render::Routing;
using iclforge::render::SpeakerTrimTable;
using iclforge::render::TrimDelay;
namespace speaker_trim = iclforge::render::speaker_trim;
using Location = iclforge::ac3::eac3::chanmap::Location;

constexpr std::size_t kBlock = Playout::kBlockFrames;
static_assert(kBlock == iclforge::ac3::kSamplesPerBlock, "the player writes the decoder's block");
constexpr std::size_t kMaxSlots = OutputLayout::kMaxSlots;
// A burst is always 1,536 samples (planning/hearth-sendspin-extension.md).
constexpr std::uint64_t kBurstFrames = 1536;
// An AC-4 chunk is one frame, which lasts what its frame rate says: 2,048 samples at frame rate 13
// and 48 kHz, until the first decoded frame says.
constexpr std::uint64_t kAc4FrameFrames = 2048;
// What waiting for an I-frame costs a stream whose decoder has lost its state: the average of the
// time to the next one, taken as half of a second (DEE's streams and the encoder's default have
// one every 24 frames, 1 s; broadcast and streaming content a second or two). A late AC-4 chunk is
// decoded and its output thrown away, rather than dropped, when that loses less than dropping it
// and waiting would (late_ac4_is_kept()).
constexpr std::int64_t kExpectedIFrameWaitUs = 500'000;
// What a frame costs of its duration before any has been timed, for each stream: between the P4's
// 0.2 to 0.7 and the S3's 0.5 to 0.8 (docs/platforms/bare-metal/esp32-p4.md and esp32-s3.md, AC-4
// from a server), where the first chunks of a play, which are the ones that are late, are
// weighed. Not kept from the stream before, whose frames may have cost another share.
constexpr double kAc4PriorRatio = 0.6;
// The ratio above which a decoder does not catch up with a stream in any time worth waiting for.
constexpr double kAc4CatchUpRatioLimit = 0.95;
// Ring bytes a chunk may not take, so a stream start, clear or end always
// fits behind it.
constexpr std::size_t kControlReserve = 512;
// Levels are measured over 100 ms at 48 kHz.
constexpr std::uint32_t kLevelWindow = 4800;
// Units a decoder can hold before their blocks arrive.
constexpr std::size_t kMaxPendingBeds = 8;
// Bursts whose times are remembered for the frames their decode releases
// late.
constexpr std::size_t kMaxMarks = 16;

constexpr EventBits_t kExited = BIT0;
constexpr EventBits_t kHeld = BIT1;

enum class Kind : std::uint8_t {
    kStartBursts,
    kStartPcm,
    kClear,
    kEnd,
    kBurst,
    kPcm,
};

// A chunk that arrived before the clock map said anything.
constexpr std::int64_t kNoMap = std::numeric_limits<std::int64_t>::min();

// Every ring entry starts with this, and a chunk's bytes follow it.
struct Header {
    Kind kind = Kind::kBurst;
    std::uint8_t data_type = 0;
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    std::uint16_t reserved = 0;
    std::uint32_t generation = 0;
    std::int32_t sample_rate = 0;
    std::int32_t channels = 0;
    std::int32_t bit_depth = 0;
    std::int64_t local_us = 0;
    std::int64_t server_us = 0;
    // BurstPlayerConfig::local_time for server_us when the chunk arrived.
    std::int64_t map_local_us = kNoMap;
};
static_assert(std::is_trivially_copyable_v<Header>);

enum class Stream : std::uint8_t { kIdle, kBursts, kPcm };

[[nodiscard]] bool same_layout(const iclforge::ac3::eac3::chanmap::Layout& a, const iclforge::ac3::eac3::chanmap::Layout& b) {
    if (a.count != b.count) {
        return false;
    }
    for (int i = 0; i < a.count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] float db_of(float linear) {
    if (!(linear > 1e-6F)) {
        return static_cast<float>(ac::kSilenceDb);
    }
    return std::max(20.0F * std::log10(linear), static_cast<float>(ac::kSilenceDb));
}

// PSRAM when the part has it, for the player's big buffers; internal RAM
// otherwise. "Has it" is asked, not learned from a failed allocation, which a
// failed-allocation hook would read as running out.
[[nodiscard]] std::uint32_t bulk_caps() {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                                            : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

// The sink, with the time spent in its writes kept apart from the decode's.
class TimedSink final : public PlayoutSink {
   public:
    explicit TimedSink(ScheduledSink& sink) : sink_(&sink) {}
    [[nodiscard]] std::optional<PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        const std::int64_t entered = esp_timer_get_time();
        std::optional<PlayoutWrite> written = sink_->write(outputs);
        spent_us_ += static_cast<std::uint64_t>(esp_timer_get_time() - entered);
        return written;
    }
    [[nodiscard]] std::uint64_t take_spent() { return std::exchange(spent_us_, 0); }

   private:
    ScheduledSink* sink_;
    std::uint64_t spent_us_ = 0;
};

struct BurstPlayer::Impl {
    Impl(const BurstPlayerConfig& cfg, ScheduledSink& snk)
        : config(cfg), sink(snk), timed(snk), layout(cfg.layout), renderer(cfg.layout, cfg.sample_rate),
          tone(cfg.sample_rate), local_identify(cfg.sample_rate) {
        applied_range = cfg.dynamic_range;
        wanted_range.store(static_cast<std::uint8_t>(cfg.dynamic_range));
    }

    BurstPlayerConfig config;
    ScheduledSink& sink;
    TimedSink timed;

    // The ring, allocated by hand and created with the Static API, as the
    // Player's is (player.cpp says why).
    MessageBufferHandle_t ring = nullptr;
    std::uint8_t* ring_storage = nullptr;
    StaticMessageBuffer_t* ring_struct = nullptr;
    // The session task's scratch for one entry, and the decode task's.
    std::uint8_t* send_buffer = nullptr;
    std::uint8_t* receive_buffer = nullptr;
    std::size_t entry_bytes = 0;
    std::atomic<std::uint32_t> generation{1};

    TaskHandle_t task = nullptr;
    // Whether the stack was made with caps (BurstPlayerConfig::stack_in_psram): freed only by
    // vTaskDeleteWithCaps(), from another task.
    bool task_with_caps = false;
    EventGroupHandle_t events = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> held{false};

    // Every float buffer, one allocation: the rendered slots, the outputs,
    // the playout's staging, the PCM path's channels and the delay lines.
    float* floats = nullptr;
    std::array<std::span<float>, kMaxSlots> rendered{};
    std::array<std::span<float>, Playout::kMaxOutputs> outputs{};
    std::array<std::span<const float>, kMaxSlots> rendered_views{};
    std::array<std::span<const float>, Playout::kMaxOutputs> output_views{};
    std::array<std::span<float>, 2> pcm_channels{};
    std::array<std::span<const float>, 2> pcm_views{};
    std::span<float> delay_storage;
    std::size_t max_delay_samples = 0;
    std::optional<Playout> playout;

    // --- the decode task's own, and nobody else's ---------------------------
    Stream stream = Stream::kIdle;
    ac::DataType data_type = ac::DataType::kEac3;
    m::AudioFormat pcm_format;
    // The stream playing declares more objects than config.max_objects: it is played as its bed,
    // and the console said so once.
    bool bed_only = false;
    bool bed_only_noted = false;
    // The listener chose objects "always": a stream is not held to config.max_objects.
    bool objects_always = false;
    // The decoder of an Opus stream, open from its start to its end, and the packet's samples.
    std::unique_ptr<OpusStream> opus;
    // The decoder of an AC-4 stream of bursts: made when one starts, kept from one such stream to
    // the next as the AC-3 ones are, and let go when another kind of stream starts or the settings
    // it was made with change. A stream's chunks are one sync frame each.
    std::unique_ptr<Ac4Stream> ac4;
    // This stream could not open its decoder, or has had a frame refused: said once, and its
    // chunks counted invalid.
    bool ac4_failed = false;
    // The samples the last decoded AC-4 frame lasted, for the late check.
    std::uint64_t ac4_frame_samples = 0;
    // What the decode and render of an AC-4 frame has cost of the frame's own duration, smoothed
    // over the chunks timed of this stream (the first, which makes the decoder's tables and
    // buffers, is not): what a late chunk is weighed with (play_burst).
    double ac4_decode_ratio = kAc4PriorRatio;
    std::uint32_t ac4_cost_samples = 0;
    // Late AC-4 chunks decoded for their state and not played.
    std::uint64_t late_kept_chunks = 0;
    // The stream's frames that were never handed to the sink: where the timeline is anchored again
    // after the frames it last pushed (the chunks dropped for being late, the ones the decoder
    // waited an I-frame through or decoded and discarded), counted from the stream's first chunk.
    // What a stall costs the listener, which `late` and the playout's silence do not say: the
    // playout pads a gap only where it had pushed nothing.
    std::uint64_t lost_frames = 0;
    // The stream frame up to which every frame is accounted for: pushed, or counted lost. A decoder
    // that waits for an I-frame anchors the timeline again at every chunk it waits through, so
    // what an anchor adds is the distance from here, once.
    std::uint64_t accounted_frame = 0;
    std::array<std::int16_t, OpusStream::kMaxFrames * 2> opus_pcm{};
    std::optional<iclforge::ac3::FrameDecoder> ac3_decoder;
    // The decode lane (BurstPlayerConfig::decode_lane): started by the task, given back by it.
    std::optional<lanes::TaskExecutor> lane;
    std::optional<iclforge::ac3::Eac3Decoder> eac3_decoder;
    std::optional<int> programme;
    std::array<iclforge::ac3::eac3::chanmap::Layout, kMaxPendingBeds> beds{};
    std::size_t bed_head = 0;
    std::size_t bed_count = 0;
    std::optional<iclforge::ac3::eac3::chanmap::Layout> renderer_bed;

    OutputLayout layout;
    iclforge::ac3::render::Serving serving;
    // The decoder settings as the board and the server left them (the server's replace the board's
    // whole), and what the decoders are made with: those, served for the layout, and the listener's
    // dynamic range over them (rebuild_decoder_config).
    iclforge::ac3::DecoderConfig decoder_base;
    iclforge::ac3::DecoderConfig decoder_config;
    // The dynamic range the decoders were made for, and the one the listener last chose: the
    // decoders are made again, at a stream's start, when the two differ.
    iclforge::base::DynamicRange applied_range = iclforge::base::kDefaultDynamicRange;
    std::atomic<std::uint8_t> wanted_range{static_cast<std::uint8_t>(iclforge::base::kDefaultDynamicRange)};
    LayoutRenderer renderer;
    Routing routing;
    // The level and delay of every output: the listener's table, or a server's settings where they speak. Kept
    // across a reopening of the sink for another output count.
    OutputTrim trim;
    std::size_t outputs_open = 0;
    // A server's identify tone, which it starts and stops, and the board's own sequence (identify_local()),
    // which stops itself.
    IdentifyTone tone;
    IdentifySequence local_identify;
    std::optional<ac::Identify> identifying;
    std::optional<ac::Identify> identify_applied;

    // Where decoded frames go in time: the stream's first burst's server
    // time, the frame the decoder hands over next, and the recent bursts'
    // local times by frame.
    bool have_origin = false;
    std::int64_t origin_server_us = 0;
    std::uint64_t decoded_frame = 0;
    bool decoded_frame_set = false;
    struct Mark {
        std::uint64_t frame = 0;
        std::int64_t local_us = 0;
    };
    std::array<Mark, kMaxMarks> marks{};
    std::size_t mark_head = 0;
    std::size_t mark_count = 0;

    // The levels of the PCM block being emitted, measured in integers while it was unpacked
    // (play_pcm()), for the meter to use in place of measuring the floats again. Null for anything
    // else: a burst's blocks have no source of this kind.
    const PcmLevel* source_levels = nullptr;
    std::size_t source_channels = 0;
    // The output each source channel's level is for, or -1 where the level is not the output's own: the
    // slot the renderer puts the channel on, and the output that slot is patched to (play_pcm()). A channel
    // is not on the output of its own number: a stereo bed in a 5.1 room puts R on slot 2, which is C's
    // number in the room's order.
    std::array<int, 2> source_outputs{-1, -1};
    unsigned source_bits = 16;

    // The meter.
    std::array<float, Playout::kMaxOutputs> window_peak{};
    std::array<double, Playout::kMaxOutputs> window_sum{};
    std::uint32_t window_frames = 0;
    std::array<double, Playout::kMaxOutputs> stream_sum{};
    std::uint64_t stream_frames = 0;

    // Volume, in by any task, ramped by the decode task.
    std::atomic<float> target_gain{1.0F};
    float gain = 1.0F;

    // What the screen has asked to see of the output (BurstPlayer::set_block_tap); read once a block.
    std::atomic<BlockTap*> block_tap{nullptr};

    // Burst timing.
    std::uint64_t decode_us_total = 0;
    std::uint64_t sink_us_total = 0;
    std::uint64_t timed_bursts = 0;
    std::uint32_t worst_burst_us = 0;
    // The most the ring has held since the stream began, in bytes: how far
    // behind the network the decode fell at worst. Measured against what an
    // empty ring has free, which the buffer's own bookkeeping takes from.
    std::size_t ring_high = 0;
    std::size_t ring_free_empty = 0;

    std::uint64_t late_chunks = 0;
    std::uint64_t bursts_played = 0;
    // The coded bytes of the chunks played, for the screen's bitrate.
    std::uint64_t coded_bytes = 0;
    bool have_decoder_report = false;
    ac::DecoderReport decoder_report;
    // Whether this stream has already printed why it refused a syncframe
    // over config.max_coded_channels: once per stream, not once per burst.
    bool channels_refused_warned = false;

    // --- from other tasks -------------------------------------------------------
    std::atomic<std::uint64_t> dropped_chunks{0};
    std::atomic<std::uint64_t> invalid_chunks{0};
    std::atomic<bool> active{false};
    std::mutex settings_mutex;
    std::optional<ac::Settings> pending_settings;
    std::optional<OutputLayout> pending_layout;
    bool have_server_settings = false;
    std::optional<ac::Identify> pending_identify;
    bool identify_changed = false;
    // From the control surface: the listener's table, and the board's own identify request.
    std::optional<SpeakerTrimTable> pending_trim;
    std::optional<IdentifyRequest> pending_local_identify;
    std::int64_t settings_revision = 0;

    mutable std::mutex status_mutex;
    BurstPlayerStatus status;
    BurstPlayerTrim trim_report;

    // ---------------------------------------------------------------------------

    void send(const Header& header, std::span<const std::uint8_t> payload, bool control) {
        if (ring == nullptr) {
            return;
        }
        const std::size_t bytes = sizeof(Header) + payload.size();
        if (bytes > entry_bytes) {
            dropped_chunks.fetch_add(1);
            return;
        }
        // A chunk leaves room behind it for a start, clear or end.
        if (!control && xMessageBufferSpacesAvailable(ring) < bytes + kControlReserve) {
            dropped_chunks.fetch_add(1);
            return;
        }
        std::memcpy(send_buffer, &header, sizeof(Header));
        if (!payload.empty()) {
            std::memcpy(send_buffer + sizeof(Header), payload.data(), payload.size());
        }
        const std::size_t sent = xMessageBufferSend(ring, send_buffer, bytes, control ? pdMS_TO_TICKS(100) : 0);
        if (sent != bytes && !control) {
            dropped_chunks.fetch_add(1);
        } else if (sent != bytes) {
            std::printf("burst player: a stream control message did not fit the ring\n");
        }
    }

    // --- settings -----------------------------------------------------------

    // The outputs the sink is opened for: the layout's slots, or as many as
    // the routing reaches, and the identified output.
    [[nodiscard]] std::size_t outputs_needed() const {
        std::size_t needed = routing.outputs() > 0 ? 0 : layout.slots();
        for (std::size_t c = 0; c < routing.channels(); ++c) {
            const int output = routing.output_of(c);
            if (output != Routing::kUnassigned) {
                needed = std::max(needed, static_cast<std::size_t>(output) + 1);
            }
        }
        if (identifying) {
            needed = std::max(needed, static_cast<std::size_t>(identifying->output) + 1);
        }
        return std::clamp<std::size_t>(needed, 1, std::min(config.max_outputs, Playout::kMaxOutputs));
    }

    // Everything that follows from the layout and the decoder's settings.
    void configure(const OutputLayout& new_layout, const iclforge::ac3::DecoderConfig& decoder, iclforge::ac3::DownmixTarget fold,
                   iclforge::ac3::render::ObjectsPolicy objects, double crossover_hz, const std::optional<Routing>& new_routing,
                   std::optional<std::span<const double>> trims, std::optional<std::span<const double>> delays) {
        layout = new_layout;
        serving = iclforge::ac3::render::serve(layout, fold, objects);
        objects_always = objects == iclforge::ac3::render::ObjectsPolicy::kAlways;
        decoder_base = decoder;
        rebuild_decoder_config();
        renderer = LayoutRenderer{layout, config.sample_rate, crossover_hz};
        renderer_bed.reset();
        if (new_routing) {
            routing = *new_routing;
        } else {
            routing = Routing::identity(layout.slots(), layout.slots()).value_or(Routing{});
        }
        // A server's trims and delays replace the listener's table whole, each as the server sent it; a key it
        // left out is the table's.
        trim.set_server(trims, delays);
        // Given back before the sink is opened: a layout with more outputs makes the sink's DMA
        // queue larger (a TDM line's holds the outputs it carries), and an AC-4 decoder keeps
        // nearly all of internal RAM between plays.
        drop_decoders();  // made with the settings these are
        open_outputs(false);
        reset_decoding();
    }

    // The configuration the AC-3 and E-AC-3 decoders are made with: the board's and the server's
    // settings, the layout's serving, and the listener's dynamic range over all of it.
    void rebuild_decoder_config() {
        decoder_config = decoder_base;
        iclforge::ac3::render::configure_decoder(serving, decoder_config);
        decoder_config.executor = lane.has_value() ? &*lane : nullptr;
        iclforge::ac3::apply_dynamic_range(applied_range, decoder_config);
    }

    // The listener's latest choice, taken up at a stream's start: the decoders made for the one
    // before are let go, and made again (AC-4's with the new range in its config) by the stream.
    void take_up_dynamic_range() {
        const auto wanted = static_cast<iclforge::base::DynamicRange>(wanted_range.load());
        if (wanted == applied_range) {
            return;
        }
        applied_range = wanted;
        rebuild_decoder_config();
        drop_decoders();
        std::printf("burst player: dynamic range %s\n", iclforge::base::dynamic_range_name(applied_range).data());
    }

    // The decoders given back, for a change of the configuration they were made with.
    void drop_decoders() {
        ac3_decoder.reset();
        eac3_decoder.reset();
        // The AC-4 decoder is made with the fold and the lane it has now: it goes, and the stream's
        // next chunk makes another. The object stays, for the stream's own placement.
        if (ac4) {
            ac4->close();
        }
    }

    // The sink opened for as many outputs as the routing and the identify
    // tone reach, and the trims and delays set on them. `always` opens it
    // even when the count has not changed, for a stream that begins after
    // something else may have opened the sink its own way.
    void open_outputs(bool always) {
        const std::size_t needed = outputs_needed();
        if (always || needed != outputs_open) {
            if (playout && needed != outputs_open) {
                playout->flush(esp_timer_get_time(), timed);
            }
            if (sink.open(config.sample_rate, needed)) {
                outputs_open = needed;
            } else {
                std::printf("burst player: the sink cannot open %u outputs\n", static_cast<unsigned>(needed));
            }
            if (playout) {
                playout->set_outputs(outputs_open);
                playout->realign();
            }
        }
        trim.configure(layout, routing, outputs_open);
    }

    void configure_defaults(const OutputLayout& new_layout) {
        configure(new_layout, config.decoder, config.stereo_fold, config.objects, LayoutRenderer::kDefaultCrossoverHz,
                  std::nullopt, std::nullopt, std::nullopt);
    }

    // The server's settings, which replace every earlier one whole: a key
    // left out takes the board's own value. Checked when they arrived.
    void apply(const ac::Settings& s) {
        OutputLayout new_layout = config.layout;
        if (s.layout) {
            if (const auto parsed = OutputLayout::parse(*s.layout)) {
                new_layout = *parsed;
            }
        }
        iclforge::ac3::DecoderConfig decoder = config.decoder;
        const ac::DecoderSettings& d = s.decoder;
        if (d.mode) {
            decoder.output.mode =
                *d.mode == ac::DecoderMode::kRf     ? iclforge::ac3::OperatingMode::kRf
                : *d.mode == ac::DecoderMode::kLine ? iclforge::ac3::OperatingMode::kLine
                                                    : iclforge::ac3::OperatingMode::kCustom;
        }
        if (d.heavy_compression) {
            decoder.heavy_compression = *d.heavy_compression;
        }
        if (d.dialnorm) {
            decoder.output.apply_dialnorm = *d.dialnorm;
        }
        if (d.ltrt_phase_shift) {
            decoder.output.ltrt_phase_shift = *d.ltrt_phase_shift;
        }
        if (d.mix_lfe) {
            decoder.output.mix_lfe = *d.mix_lfe;
        }
        if (d.has_programme) {
            decoder.programme = d.programme;
        }
        if (d.concealment) {
            decoder.concealment = *d.concealment == ac::Concealment::kRepeatFade ? iclforge::ac3::ConcealmentPolicy::kRepeatFade
                                  : *d.concealment == ac::Concealment::kMute     ? iclforge::ac3::ConcealmentPolicy::kMute
                                                                                  : iclforge::ac3::ConcealmentPolicy::kNone;
        }
        if (d.karaoke) {
            decoder.output.karaoke = *d.karaoke == ac::Karaoke::kCapable ? iclforge::ac3::KaraokeReproduction::kCapable
                                     : *d.karaoke == ac::Karaoke::kAware ? iclforge::ac3::KaraokeReproduction::kAware
                                                                         : iclforge::ac3::KaraokeReproduction::kOff;
        }
        if (d.karaoke_vocals) {
            decoder.output.karaoke_vocals = *d.karaoke_vocals == ac::KaraokeVocals::kNone ? iclforge::ac3::KaraokeVocals::kNone
                                            : *d.karaoke_vocals == ac::KaraokeVocals::kV1 ? iclforge::ac3::KaraokeVocals::kV1
                                            : *d.karaoke_vocals == ac::KaraokeVocals::kV2 ? iclforge::ac3::KaraokeVocals::kV2
                                                                                          : iclforge::ac3::KaraokeVocals::kBoth;
        }
        // The capable reproduction's part levels, in dB against Table C.2.3; the library's own
        // span and conversion, so a level is what Hearth's page and its decode mean by it.
        if (d.karaoke_melody_db) {
            decoder.output.karaoke_melody_gain = iclforge::ac3::karaoke_gain_from_db(*d.karaoke_melody_db);
        }
        if (d.karaoke_vocal1_db) {
            decoder.output.karaoke_vocal1_gain = iclforge::ac3::karaoke_gain_from_db(*d.karaoke_vocal1_db);
        }
        if (d.karaoke_vocal2_db) {
            decoder.output.karaoke_vocal2_gain = iclforge::ac3::karaoke_gain_from_db(*d.karaoke_vocal2_db);
        }
        iclforge::ac3::DownmixTarget fold = config.stereo_fold;
        if (d.downmix) {
            fold = *d.downmix == ac::Downmix::kLtRt ? iclforge::ac3::DownmixTarget::kLtRt : iclforge::ac3::DownmixTarget::kLoRo;
        }
        iclforge::ac3::render::ObjectsPolicy objects = config.objects;
        if (d.objects) {
            objects = *d.objects == ac::ObjectsPolicy::kAlways
                          ? iclforge::ac3::render::ObjectsPolicy::kAlways
                      : *d.objects == ac::ObjectsPolicy::kNever
                          ? iclforge::ac3::render::ObjectsPolicy::kNever
                          : iclforge::ac3::render::ObjectsPolicy::kAuto;
        }
        std::optional<Routing> new_routing;
        if (s.routing) {
            new_routing = Routing::parse(*s.routing, std::min(config.max_outputs, Playout::kMaxOutputs));
        }
        configure(new_layout, decoder, fold, objects, s.crossover_hz.value_or(LayoutRenderer::kDefaultCrossoverHz),
                  new_routing,
                  s.trim_db ? std::optional<std::span<const double>>(std::span<const double>(*s.trim_db)) : std::nullopt,
                  s.delay_ms ? std::optional<std::span<const double>>(std::span<const double>(*s.delay_ms)) : std::nullopt);
        std::printf("burst player: settings %lld applied: layout %s, %u outputs\n",
                    static_cast<long long>(s.revision), layout.text().data(), static_cast<unsigned>(outputs_open));
    }

    // At a burst boundary, or while nothing plays.
    void apply_pending() {
        std::optional<ac::Settings> settings;
        std::optional<OutputLayout> own_layout;
        std::optional<ac::Identify> identify;
        bool identify_update = false;
        std::optional<SpeakerTrimTable> new_trim;
        std::optional<IdentifyRequest> local_request;
        {
            const std::lock_guard lock(settings_mutex);
            settings.swap(pending_settings);
            own_layout.swap(pending_layout);
            identify = pending_identify;
            identify_update = std::exchange(identify_changed, false);
            new_trim.swap(pending_trim);
            local_request.swap(pending_local_identify);
        }
        if (new_trim) {
            // Before the settings below, which configure everything from scratch with the table in force.
            trim.set_table(*new_trim);
        }
        if (settings) {
            apply(*settings);
            const std::lock_guard lock(settings_mutex);
            settings_revision = settings->revision;
        } else if (own_layout) {
            configure_defaults(*own_layout);
        }
        if (identify_update) {
            identifying = identify;
            if (identifying) {
                (void)tone.set_level_db(identifying->level_db);
                tone.reset();
            }
            if (outputs_needed() != outputs_open) {
                open_outputs(false);
            }
            if (identifying) {
                // A server's tone replaces the board's own.
                local_identify.stop(true);
            }
        }
        if (local_request) {
            start_local_identify(*local_request);
        }
    }

    // The board's own identify request, at a moment nothing else is using the sink: the tone out of the speaker
    // of the layout in force that has the role asked for, or each in turn. The outputs are the layout's channels
    // as the patch has them.
    void start_local_identify(const IdentifyRequest& request) {
        using Shape = IdentifyRequest::Shape;
        if (request.shape == Shape::kOff) {
            local_identify.stop();
            return;
        }
        if (stream != Stream::kIdle || identifying || held.load()) {
            return;
        }
        std::array<IdentifySequence::Target, IdentifySequence::kMaxTargets> targets{};
        std::size_t count = 0;
        const std::size_t channels = std::min(layout.slots(), routing.channels());
        for (std::size_t c = 0; c < channels && count < targets.size(); ++c) {
            const int output = routing.output_of(c);
            const speaker_trim::RoleCode role = speaker_trim::role_of_slot(layout, c);
            if (output < 0 || static_cast<std::size_t>(output) >= outputs_open || role == speaker_trim::kNoRole) {
                continue;
            }
            if (request.shape != Shape::kEach && role != request.role) {
                continue;
            }
            targets[count++] = IdentifySequence::Target{
                .output = static_cast<std::uint8_t>(output),
                .band = carries_lfe(static_cast<std::size_t>(output)) ? IdentifyTone::Band::kLow : IdentifyTone::Band::kFull};
        }
        const IdentifySequence::Mode mode = request.shape == Shape::kEach   ? IdentifySequence::Mode::kEach
                                            : request.shape == Shape::kLoop ? IdentifySequence::Mode::kLoop
                                                                            : IdentifySequence::Mode::kOnce;
        if (local_identify.start(mode, std::span<const IdentifySequence::Target>(targets.data(), count), request.level_db)) {
            std::printf("burst player: identify %.*s: %u speaker%s, at most %d dB\n",
                        request.shape == Shape::kEach ? 4 : static_cast<int>(speaker_trim::role_name(request.role).size()),
                        request.shape == Shape::kEach ? "each" : speaker_trim::role_name(request.role).data(),
                        static_cast<unsigned>(request.shape == Shape::kEach ? count : 1), count == 1 ? "" : "s",
                        static_cast<int>(IdentifySequence::clamp_level_db(request.level_db)));
        }
    }

    // --- decoding ---------------------------------------------------------------

    // The decoders start the next stream where a new one would, and keep what they
    // have allocated: made again, they ask a heap that Wi-Fi has cut up for the
    // scratch they just gave back, and on a part with no PSRAM it is not there
    // (an ESP32-C6 aborted on a 36 KB and then a 7 KB request, on a track change
    // and on a late chunk). Only a change of what they are made for - the
    // settings, or the other codec - drops them (drop_decoders).
    void reset_decoding() {
        if (opus) {
            opus->reset();
        }
        if (ac3_decoder) {
            ac3_decoder->reset();
        }
        if (eac3_decoder) {
            eac3_decoder->reset();
        }
        if (ac4) {
            ac4->reset();
        }
        reset_output();
    }

    // What follows the decoders: the programme and the bed it chose, the renderer's and the trim's
    // history, and the timeline, which the next chunk anchors again. A late AC-4 chunk that is
    // decoded for its state and not played makes it too (play_burst), since the next frame that is
    // played does not follow the last one that was.
    void reset_output() {
        programme.reset();
        bed_head = 0;
        bed_count = 0;
        renderer_bed.reset();
        renderer.reset();
        trim.reset();
        decoded_frame_set = false;
        mark_count = 0;
        mark_head = 0;
    }

    void new_stream(Stream kind) {
        take_up_dynamic_range();
        reset_decoding();
        stream = kind;
        have_origin = false;
        have_decoder_report = false;
        bursts_played = 0;
        coded_bytes = 0;
        late_chunks = 0;
        late_kept_chunks = 0;
        lost_frames = 0;
        accounted_frame = 0;
        ac4_decode_ratio = kAc4PriorRatio;
        ac4_cost_samples = 0;
        dropped_chunks.store(0);
        invalid_chunks.store(0);
        channels_refused_warned = false;
        bed_only = false;
        bed_only_noted = false;
        decode_us_total = 0;
        sink_us_total = 0;
        timed_bursts = 0;
        worst_burst_us = 0;
        ring_high = 0;
        window_peak.fill(0.0F);
        window_sum.fill(0.0);
        window_frames = 0;
        stream_sum.fill(0.0);
        stream_frames = 0;
        playout->restart();
        // The least free internal heap from here to the stream's end, which
        // its closing line reports. Already running when the last stream was
        // never closed: its start, then, is the earlier one.
        (void)heap_caps_monitor_local_minimum_free_size_start();
        // An Opus stream's decoder is kept from one Opus stream to the next, as the AC-3 ones are,
        // and let go when another kind of stream starts; the AC-3 ones are let go when an Opus
        // stream does, and when a PCM stream does on a part with PSRAM to make them again (iclforge/
        // decoder_keep.hpp). Each is 27 KB or more, and a heap that WiFi has cut up has room for one.
        // An AC-4 stream's decoder is the largest by far, 286 KB at 2.0: it goes with the AC-3
        // ones when any other kind starts, and they go when it does. Its allocations go where
        // CONFIG_ICLFORGE_AC4_INTERNAL_BELOW puts them for as long as the stream plays.
        const bool ac4_stream = kind == Stream::kBursts && data_type == ac::DataType::kAc4;
        const bool opus_stream = kind == Stream::kPcm && pcm_format.codec == m::Codec::kOpus;
        const DecoderNeed need = ac4_stream    ? DecoderNeed::kAc4
                                 : opus_stream ? DecoderNeed::kOpus
                                 : kind == Stream::kPcm ? DecoderNeed::kNone
                                                        : DecoderNeed::kAc3Family;
        const DecodersKept keep = decoders_kept(need, heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
        ac4_failed = false;
        ac4_frame_samples = 0;
        if (!keep.ac3_family) {
            ac3_decoder.reset();
            eac3_decoder.reset();
        }
        if (!keep.ac4) {
            ac4.reset();
        } else {
            if (!ac4) {
                ac4 = std::make_unique<Ac4Stream>();
            }
            ac4->begin_stream();
        }
        if (!opus_stream) {
            opus.reset();
        } else if (!opus) {
            auto decoder = std::make_unique<OpusStream>();
            if (decoder->open(pcm_format.sample_rate, pcm_format.channels)) {
                opus = std::move(decoder);
            } else {
                std::printf(
                    "burst player: could not open an Opus decoder for %d channels (heap free %u)\n",
                    static_cast<int>(pcm_format.channels),
                    static_cast<unsigned>(
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            }
        }
        // Opened again whatever it was: a play from the control surface may
        // have opened the sink its own way since the last stream.
        open_outputs(true);
        sink.begin_stream();
        active.store(kind != Stream::kIdle);
    }

    // The console's verdict on a stream, as the Player's plays end with one:
    // its counters, the playout's figures, the least free internal heap while
    // it played (heap_free, which CI holds to a floor), and each output's RMS
    // over the whole stream.
    void report_stream_end() {
        if (stream == Stream::kIdle) {
            return;
        }
        const std::size_t least_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        (void)heap_caps_monitor_local_minimum_free_size_stop();
        const Playout::Stats& p = playout->stats();
        std::printf(
            "sendspin.stream=%s bursts=%llu late=%llu late_kept=%llu lost_frames=%llu dropped=%llu "
            "invalid=%llu underruns=%llu "
            "resyncs=%u silence_frames=%llu skipped_frames=%llu dropped_frames=%llu "
            "repeated_frames=%llu "
            "worst_error_us=%lld burst_us=%llu sink_us=%llu ring_high=%lu heap_free=%lu\n",
            stream == Stream::kBursts ? "bursts" : "pcm",
            static_cast<unsigned long long>(bursts_played),
            static_cast<unsigned long long>(late_chunks),
            static_cast<unsigned long long>(late_kept_chunks),
            static_cast<unsigned long long>(lost_frames),
            static_cast<unsigned long long>(dropped_chunks.load()),
            static_cast<unsigned long long>(invalid_chunks.load()),
            static_cast<unsigned long long>(p.underruns), static_cast<unsigned>(p.resyncs),
            static_cast<unsigned long long>(p.silence_frames),
            static_cast<unsigned long long>(p.skipped_frames),
            static_cast<unsigned long long>(p.dropped_frames),
            static_cast<unsigned long long>(p.repeated_frames),
            static_cast<long long>(p.worst_error_us),
            static_cast<unsigned long long>(timed_bursts > 0 ? decode_us_total / timed_bursts : 0),
            static_cast<unsigned long long>(timed_bursts > 0 ? sink_us_total / timed_bursts : 0),
            static_cast<unsigned long>(ring_high), static_cast<unsigned long>(least_heap));
        for (std::size_t o = 0; o < outputs_open; ++o) {
            std::printf("sendspin.rms[%u]=%lu\n", static_cast<unsigned>(o),
                        static_cast<unsigned long>(stream_rms(o)));
        }
    }

    [[nodiscard]] std::uint32_t stream_rms(std::size_t output) const {
        if (stream_frames == 0) {
            return 0;
        }
        const double rms = std::sqrt(stream_sum[output] / static_cast<double>(stream_frames));
        return static_cast<std::uint32_t>((rms * 1e6) + 0.5);
    }

    void end_stream() {
        playout->flush(esp_timer_get_time(), timed);
        report_stream_end();
        reset_decoding();
        if (ac4) {
            ac4->end_stream();
        }
        stream = Stream::kIdle;
        have_decoder_report = false;
        active.store(false);
    }

    // config.local_time for `server_us`, or kNoMap.
    [[nodiscard]] std::int64_t map_now(std::int64_t server_us) const {
        if (!config.local_time) {
            return kNoMap;
        }
        return config.local_time(server_us).value_or(kNoMap);
    }

    // A chunk's local time, moved by as much as the clock map has moved since
    // the chunk arrived (BurstPlayerConfig::local_time).
    [[nodiscard]] std::int64_t local_of(const Header& header) const {
        if (header.map_local_us == kNoMap) {
            return header.local_us;
        }
        const std::int64_t now = map_now(header.server_us);
        return now == kNoMap ? header.local_us : header.local_us + (now - header.map_local_us);
    }

    [[nodiscard]] std::uint64_t frame_of(std::int64_t server_us) const {
        const std::int64_t since = server_us - origin_server_us;
        if (since <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(((since * config.sample_rate) + 500'000) / 1'000'000);
    }

    void mark(std::uint64_t frame, std::int64_t local_us) {
        const std::size_t at = (mark_head + mark_count) % kMaxMarks;
        marks[at] = Mark{.frame = frame, .local_us = local_us};
        if (mark_count < kMaxMarks) {
            ++mark_count;
        } else {
            mark_head = (mark_head + 1) % kMaxMarks;
        }
    }

    // When decoded frame `frame` should play: its burst's time, and the
    // frames since.
    [[nodiscard]] std::optional<std::int64_t> target_of(std::uint64_t frame) {
        std::optional<Mark> best;
        for (std::size_t i = 0; i < mark_count; ++i) {
            const Mark& candidate = marks[(mark_head + i) % kMaxMarks];
            if (candidate.frame <= frame && (!best || candidate.frame >= best->frame)) {
                best = candidate;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const std::uint64_t since = frame - best->frame;
        return best->local_us + static_cast<std::int64_t>(((since * 1'000'000) + (config.sample_rate / 2)) / config.sample_rate);
    }

    void push_bed(const iclforge::ac3::eac3::chanmap::Layout& bed) {
        if (bed_count == kMaxPendingBeds) {
            bed_head = (bed_head + 1) % kMaxPendingBeds;
            --bed_count;
        }
        beds[(bed_head + bed_count) % kMaxPendingBeds] = bed;
        ++bed_count;
    }

    // One decoded block: placed, routed, trimmed, delayed, metered and
    // scheduled.
    void deliver(const iclforge::ac3::PcmBlock& block) {
        std::size_t n = block.channels.empty() ? 0 : block.channels.front().size();
        if (n == 0 && !block.objects.empty()) {
            n = block.objects.front().size();
        }
        n = std::min(n, kBlock);
        if (n == 0) {
            return;
        }
        const std::size_t slots = layout.slots();
        const std::span<const std::span<float>> out(rendered.data(), slots);
        if (block.index == 0 && bed_count > 0) {
            const iclforge::ac3::eac3::chanmap::Layout bed = beds[bed_head];
            bed_head = (bed_head + 1) % kMaxPendingBeds;
            --bed_count;
            if (!serving.fold && (!renderer_bed || !same_layout(*renderer_bed, bed))) {
                renderer.set_bed(bed);
                renderer_bed = bed;
            }
        }
        std::uint32_t reached = renderer.all_slots();
        if (serving.fold) {
            renderer.render_folded(block, 1.0F, out);
        } else {
            if (serving.reconstruct && block.index == 0) {
                renderer.set_objects(block.object_metadata, block.objects.size());
            }
            reached = renderer.render_reached(block, serving.reconstruct, 1.0F, out);
        }
        emit(n, reached);
    }

    // The outputs that can carry audio in a block that reached the slots in `reached`
    // (LayoutRenderer::render_reached()), bit n for output n: the ones the patch feeds from them. Every
    // other output is silence, and costs nothing from here to the sink: it is not routed, trimmed,
    // scaled or metered, and the playout hands the sink the same block of zeros for all of them. An
    // output with a delay keeps a line that holds audio past the block that gave it, so it stays live
    // whatever reached it, and a server's identify tone writes every output.
    [[nodiscard]] std::uint32_t live_outputs(std::uint32_t reached) const {
        const std::uint32_t open = outputs_open >= 32 ? ~std::uint32_t{0} : (std::uint32_t{1} << outputs_open) - 1U;
        if (identifying) {
            return open;
        }
        return (routing.outputs_fed_by(reached) | trim.delayed_outputs()) & open;
    }

    // An output that is live though no slot reached by this block feeds it (a delay line's tail, a server's
    // identify tone) reads its channel's slot, which render_reached() left as it was: the audio of an earlier
    // block. Those slots are zeroed first, so that the output takes silence.
    void silence_unfed_slots(std::uint32_t live, std::uint32_t reached, std::size_t n) {
        const std::uint32_t unfed = live & ~routing.outputs_fed_by(reached);
        for (std::size_t o = 0; unfed != 0 && o < outputs_open; ++o) {
            if (((unfed >> o) & 1U) == 0) {
                continue;
            }
            const int channel = routing.channel_of(o);
            if (channel != Routing::kUnassigned && static_cast<std::size_t>(channel) < layout.slots()) {
                std::fill_n(rendered[static_cast<std::size_t>(channel)].data(), n, 0.0F);
            }
        }
    }

    // The rendered slots from here on: `n` frames of them, `reached` the slots that hold any.
    void emit(std::size_t n, std::uint32_t reached) {
        const std::size_t slots = layout.slots();
        for (std::size_t s = 0; s < slots; ++s) {
            rendered_views[s] = std::span<const float>(rendered[s].data(), n);
        }
        std::array<std::span<float>, Playout::kMaxOutputs> view{};
        for (std::size_t o = 0; o < outputs_open; ++o) {
            view[o] = outputs[o].first(n);
            output_views[o] = view[o];
        }
        const std::span<const std::span<float>> out(view.data(), outputs_open);
        const std::uint32_t live = live_outputs(reached);
        silence_unfed_slots(live, reached, n);
        routing.apply(std::span<const std::span<const float>>(rendered_views.data(), slots), out, live);
        // Outputs past the patch's own, which only the identify tone opens.
        for (std::size_t o = routing.outputs(); o < outputs_open; ++o) {
            if (((live >> o) & 1U) != 0) {
                std::fill(view[o].begin(), view[o].end(), 0.0F);
            }
        }
        trim.process(out, live);
        if (identifying) {
            // Every output carries the tone or silence; the stream goes on
            // underneath, decoded and discarded, and keeps its timeline.
            const std::size_t output = identifying->output >= 0 ? static_cast<std::size_t>(identifying->output)
                                                                : outputs_open;
            tone.fill(out, output, carries_lfe(output) ? IdentifyTone::Band::kLow : IdentifyTone::Band::kFull);
        }
        const bool scaled = apply_gain(out, n, live);
        tap_block(out, n, live);
        if (scaled || !meter_from_source(n)) {
            meter(out, n, live);
        }

        const std::uint64_t first = decoded_frame;
        decoded_frame += n;
        accounted_frame = std::max(accounted_frame, decoded_frame);
        std::optional<std::int64_t> target = target_of(first);
        if (!target) {
            return;  // frames from before any burst this decode knows of
        }
        playout->push(std::span<const std::span<const float>>(output_views.data(), outputs_open), n, *target, first,
                      esp_timer_get_time(), timed, ~live);
    }

    [[nodiscard]] bool carries_lfe(std::size_t output) const {
        const int channel = routing.channel_of(output);
        if (channel == Routing::kUnassigned || static_cast<std::size_t>(channel) >= layout.slots()) {
            return false;
        }
        return layout.slot(static_cast<std::size_t>(channel)).kind == iclforge::render::Speaker::Kind::kLfe;
    }

    // The block as it is about to be heard, to whatever is looking at it (iclforge/block_tap.hpp): nothing when nothing is.
    void tap_block(std::span<const std::span<float>> out, std::size_t n, std::uint32_t live) {
        BlockTap* const tap = block_tap.load(std::memory_order_acquire);
        if (tap != nullptr && tap->wants_blocks()) {
            tap->block(out, n, live);
        }
    }

    // True when the block was scaled: a block at unity is left as it is, and costs nothing. Only the
    // outputs in `live` are scaled: the others are silence, which scaling does not change.
    bool apply_gain(std::span<const std::span<float>> out, std::size_t n, std::uint32_t live = ~std::uint32_t{0}) {
        const float target = target_gain.load();
        if (target == 1.0F && gain == 1.0F) {
            return false;
        }
        const float start = gain;
        const float step = (target - start) / static_cast<float>(n);
        for (std::size_t o = 0; o < out.size(); ++o) {
            if (((live >> o) & 1U) == 0) {
                continue;
            }
            const std::span<float>& samples = out[o];
            float g = start;
            for (std::size_t k = 0; k < n; ++k) {
                g += step;
                samples[k] *= g;
            }
        }
        gain = target;
        return true;
    }

    // The outputs in `live` measured over `n` frames. The others are silence: a peak of 0 and a sum of
    // 0 change nothing the window or the stream has, so they are not read, and the report says them
    // all the same (publish_levels() has every open output).
    void meter(std::span<const std::span<float>> out, std::size_t n, std::uint32_t live = ~std::uint32_t{0}) {
        for (std::size_t o = 0; o < out.size(); ++o) {
            if (((live >> o) & 1U) == 0) {
                continue;
            }
            const std::span<float> samples = out[o];
            float peak = window_peak[o];
            double sum = 0.0;
            std::size_t k = 0;
            // Squared in float sixteen at a time and summed in double, as
            // the example's MeteredSink does: every double operation on
            // this part is a software call.
            for (; k + 16 <= n; k += 16) {
                float partial = 0.0F;
                for (std::size_t j = 0; j < 16; ++j) {
                    const float x = samples[k + j];
                    partial += x * x;
                    peak = std::max(peak, std::fabs(x));
                }
                sum += static_cast<double>(partial);
            }
            for (; k < n; ++k) {
                const float x = samples[k];
                sum += static_cast<double>(x * x);
                peak = std::max(peak, std::fabs(x));
            }
            window_peak[o] = peak;
            window_sum[o] += sum;
            stream_sum[o] += sum;
        }
        meter_commit(n);
    }

    // `n` frames more in the window and the stream, and the window published once it is full.
    void meter_commit(std::size_t n) {
        window_frames += static_cast<std::uint32_t>(n);
        stream_frames += n;
        if (window_frames >= kLevelWindow) {
            publish_levels();
        }
    }

    // The block's levels from the integers its PCM source was unpacked from (play_pcm()), where the
    // outputs carry that source and nothing else: unity gain, every source channel on one output
    // (source_outputs, which is where the renderer and the patch put it, not the channel's number), no
    // trim or delay, no identify tone (and, which the caller checks, no gain applied to the block). The same peak and mean square the floats would give, without a
    // soft-float operation a sample. False when any of that does not hold, and the caller measures
    // the floats.
    [[nodiscard]] bool meter_from_source(std::size_t n) {
        if (source_levels == nullptr || identifying || trim.active()) {
            return false;
        }
        for (std::size_t c = 0; c < source_channels; ++c) {
            if (source_outputs[c] < 0) {
                return false;
            }
        }
        for (std::size_t c = 0; c < source_channels; ++c) {
            const std::size_t o = static_cast<std::size_t>(source_outputs[c]);
            window_peak[o] =
                std::max(window_peak[o], pcm_level_peak(source_levels[c], source_bits));
            const double sum = pcm_level_square_sum(source_levels[c], source_bits);
            window_sum[o] += sum;
            stream_sum[o] += sum;
        }
        meter_commit(n);
        return true;
    }

    void publish_levels() {
        const std::lock_guard lock(status_mutex);
        BurstPlayerStatus& s = status;
        s.outputs = outputs_open;
        for (std::size_t o = 0; o < outputs_open; ++o) {
            s.peak_db[o] = db_of(window_peak[o]);
            s.rms_db[o] = db_of(static_cast<float>(std::sqrt(window_sum[o] / std::max<double>(window_frames, 1.0))));
            s.stream_rms[o] = stream_rms(o);
        }
        s.have_levels = true;
        ++s.levels_serial;
        window_peak.fill(0.0F);
        window_sum.fill(0.0);
        window_frames = 0;
    }

    void decode_unit(std::span<const std::byte> unit) {
        const auto header = iclforge::ac3::io::read_frame_header(unit);
        if (!header) {
            restart_after_error();
            return;
        }
        if (config.max_coded_channels != 0 &&
            static_cast<std::size_t>(header->coded_channels()) > config.max_coded_channels) {
            if (!channels_refused_warned) {
                channels_refused_warned = true;
                std::printf("burst player: refusing a %d-channel syncframe: more than the %u this part decodes "
                            "in this build\n",
                            header->coded_channels(), static_cast<unsigned>(config.max_coded_channels));
            }
            restart_after_error();
            return;
        }
        if (header->kind == iclforge::ac3::io::StreamKind::kEac3) {
            if (!programme) {
                programme = header->substreamid;
            } else if (header->substreamid != *programme) {
                return;
            }
        }
        if (!serving.fold) {
            const std::optional<iclforge::ac3::eac3::chanmap::Layout> bed = peek_unit_layout(unit);
            if (!bed) {
                restart_after_error();
                return;
            }
            // The bed is the layout the decoder's channels are in, which for an AC-3 syncframe is
            // the coded one except under Annex C's reproduction: a karaoke stream comes out as L,
            // C, R and the LFE, three or four channels for the four to six coded, and laid out
            // from the coded acmod its vocals would sit where the surrounds were. An E-AC-3
            // programme of one independent substream is reproduced the same way
            // (iclforge::ac3::is_karaoke_programme), so a unit that is one syncframe asks the
            // same question. (A fold has no bed: the renderer takes its channels as they come.)
            iclforge::ac3::eac3::chanmap::Layout unit_bed = *bed;
            if (header->bytes == unit.size()) {
                const auto bsmod = static_cast<iclforge::ac3::meta::BitstreamMode>(header->bsmod);
                const bool karaoke =
                    header->kind == iclforge::ac3::io::StreamKind::kAc3
                        ? iclforge::ac3::meta::is_karaoke(bsmod, header->acmod)
                        : iclforge::ac3::is_karaoke_programme(
                              bsmod, header->acmod, 1, header->oba_complexity_index.has_value());
                const iclforge::ac3::OutputMode mode =
                    iclforge::ac3::output_mode(decoder_config.output, header->acmod, header->lfe, karaoke);
                if (mode.acmod != header->acmod) {
                    unit_bed = iclforge::ac3::eac3::chanmap::expand(
                        iclforge::ac3::eac3::chanmap::acmod_map(mode.acmod, mode.lfe));
                }
            }
            push_bed(unit_bed);
        }
        const auto deliver_block = [this](const iclforge::ac3::PcmBlock& block) { deliver(block); };
        if (header->kind == iclforge::ac3::io::StreamKind::kAc3 && header->bytes == unit.size()) {
            if (!ac3_decoder) {
                // One codec's scratch at a time: an E-AC-3 decoder's is the larger.
                eac3_decoder.reset();
                ac3_decoder.emplace(decoder_config);
            }
            const auto decoded = ac3_decoder->decode_frame_by_block(unit, deliver_block);
            if (!decoded) {
                restart_after_error();
                return;
            }
            if (!have_decoder_report) {
                have_decoder_report = true;
                decoder_report = ac::DecoderReport{.data_type = data_type,
                                                   .acmod = static_cast<std::int32_t>(decoded->acmod),
                                                   .lfe = decoded->lfe,
                                                   .substreams = 1,
                                                   .objects = 0,
                                                   .objects_placed = false,
                                                   .dialnorm = -static_cast<double>(decoded->dialnorm)};
            }
            return;
        }
        if (!eac3_decoder) {
            ac3_decoder.reset();
            eac3_decoder.emplace(decoder_config);
        }
        // A stream that declares more objects than this board places in real time is played as
        // its bed, decided from the header before the object reconstruction allocates anything.
        // Set on the decoder this stream uses, which may be one kept from an earlier stream.
        bed_only = serving.reconstruct && !objects_always &&
                   iclforge::objects_past_limit(header->oba_complexity_index, config.max_objects);
        if (bed_only && !bed_only_noted) {
            bed_only_noted = true;
            std::printf(
                "burst player: a stream declaring %d objects is past the %u this board places in "
                "real time: playing its bed\n",
                iclforge::declared_objects(header->oba_complexity_index),
                static_cast<unsigned>(config.max_objects.value_or(0)));
        }
        eac3_decoder->set_skip_object_reconstruction(!serving.reconstruct || bed_only);
        const auto decoded = eac3_decoder->decode_access_unit_by_block(unit, deliver_block);
        if (!decoded) {
            restart_after_error();
            return;
        }
        // The report comes from the first unit that decodes, and again from
        // the first to carry objects if that one did not, as the test sink's
        // does.
        if (!*decoded || (have_decoder_report && (decoder_report.objects > 0 || !(*decoded)->object_metadata))) {
            return;
        }
        const iclforge::ac3::DecodedAccessUnit& au = **decoded;
        const std::int32_t objects =
            au.object_metadata ? static_cast<std::int32_t>(iclforge::objects::oba::describe_objects(*au.object_metadata).size()) : 0;
        have_decoder_report = true;
        decoder_report =
            ac::DecoderReport{.data_type = data_type,
                              .acmod = static_cast<std::int32_t>(au.acmod),
                              .lfe = au.layout.index_of(Location::kLfe) >= 0,
                              .substreams = au.substream_count,
                              .objects = objects,
                              .objects_placed = objects > 0 && serving.reconstruct && !bed_only,
                              .dialnorm = -static_cast<double>(au.dialnorm)};
    }

    // One block of an AC-4 frame onto the layout and into the sink: the bed its channels are in, as
    // deliver() sets one for an AC-3 block, and then the same render and emit.
    static void ac4_block(void* self, const Ac4Stream::Block& block) {
        static_cast<Impl*>(self)->ac4_deliver(block);
    }

    void ac4_deliver(const Ac4Stream::Block& block) {
        if (!serving.fold && (!renderer_bed || !same_layout(*renderer_bed, block.bed))) {
            renderer.set_bed(block.bed);
            renderer_bed = block.bed;
        }
        const iclforge::ac3::PcmBlock pcm{.index = block.index,
                                          .blocks = 0,
                                          .channels = block.channels,
                                          .objects = {},
                                          .object_indices = {},
                                          .object_metadata = nullptr};
        const std::span<const std::span<float>> out(rendered.data(), layout.slots());
        std::uint32_t reached = renderer.all_slots();
        if (serving.fold) {
            renderer.render_folded(pcm, 1.0F, out);
        } else {
            reached = renderer.render_reached(pcm, false, 1.0F, out);
        }
        emit(block.samples, reached);
    }

    // The AC-4 decoder made, when it is not: false, with the stream refused for good and its chunk
    // counted invalid, when it cannot be.
    [[nodiscard]] bool ensure_ac4_open() {
        if (ac4->is_open()) {
            return true;
        }
        if (ac4->open({.fold = serving.fold,
                       .executor = lane.has_value() ? &*lane : nullptr,
                       .max_channels = config.max_ac4_channels,
                       .sample_rate_hz = config.sample_rate,
                       .max_frame_rate = config.max_ac4_frame_rate,
                       .dynamic_range = applied_range})) {
            return true;
        }
        ac4_failed = true;
        std::printf("burst player: cannot decode this AC-4 stream: %.*s\n",
                    static_cast<int>(ac4->reason().size()), ac4->reason().data());
        invalid_chunks.fetch_add(1);
        return false;
    }

    // One chunk of an AC-4 stream: its sync frame decoded. False when it was not one, which starts
    // the decoder again.
    [[nodiscard]] bool decode_ac4(std::span<const std::byte> sync_frame) {
        if (ac4_failed || !ac4) {
            invalid_chunks.fetch_add(1);
            return true;
        }
        if (!ensure_ac4_open()) {
            return true;
        }
        switch (ac4->decode(sync_frame, &Impl::ac4_block, this)) {
            case Ac4Stream::Result::kDecoded: {
                ac4_frame_samples = ac4->frame_samples();
                if (!have_decoder_report) {
                    have_decoder_report = true;
                    const Ac4Stream::Report& report = ac4->report();
                    decoder_report = ac::DecoderReport{.data_type = data_type,
                                                       .acmod = report.acmod,
                                                       .lfe = report.lfe,
                                                       .substreams = 1,
                                                       .objects = 0,
                                                       .objects_placed = false,
                                                       .dialnorm = report.dialnorm_db};
                }
                return true;
            }
            case Ac4Stream::Result::kWaiting:
                // Nothing came out for this chunk: the decoder is waiting for an I-frame, after a
                // start or a reset, and its output will not begin at this chunk. The timeline is
                // anchored again by the next chunk. Kept, decoded_frame would stay at the chunk
                // the wait began at while the marks moved on, and every frame decoded after the
                // I-frame would find no mark at or before it and be dropped for good.
                decoded_frame_set = false;
                mark_count = 0;
                mark_head = 0;
                return true;
            case Ac4Stream::Result::kRefused:
                ac4_failed = true;
                std::printf("burst player: refusing this AC-4 stream: %.*s\n",
                            static_cast<int>(ac4->reason().size()), ac4->reason().data());
                return false;
            case Ac4Stream::Result::kInvalid:
                break;
        }
        return false;
    }

    // A unit that does not decode: the decoders start again, and the next
    // burst is the start of a stream.
    void restart_after_error() {
        invalid_chunks.fetch_add(1);
        reset_decoding();
    }

    // `us` of decode and render for one AC-4 frame, folded into what a frame costs of its duration.
    void note_ac4_cost(std::uint64_t us) {
        if (ac4_cost_samples++ == 0) {
            return;
        }
        const std::uint64_t frames = ac4_frame_samples != 0 ? ac4_frame_samples : kAc4FrameFrames;
        const double frame_us =
            static_cast<double>(frames) * 1e6 / static_cast<double>(config.sample_rate);
        ac4_decode_ratio += 0.125 * ((static_cast<double>(us) / frame_us) - ac4_decode_ratio);
    }

    // Whether a chunk `late_us` past the sink's next play time is worth decoding for the state it
    // leaves the decoder in. Each chunk decoded takes a fraction `r` of the time the next one's
    // audio lasts, so the lateness falls by 1 - r of a frame with each, and the audio lost by
    // decoding through it is the lateness over 1 - r. Dropping the chunk loses the lateness itself
    // and then everything up to the next I-frame, which averages kExpectedIFrameWaitUs. Keeping
    // is the better when late / (1 - r) <= late + wait, that is when late * r <= (1 - r) * wait.
    // A stream whose decoder could not be made or has had a frame refused has no state to keep;
    // one not yet open is made for the chunk, as a first chunk that is late is the stream's
    // I-frame and the one most worth keeping.
    [[nodiscard]] bool late_ac4_is_kept(std::int64_t late_us, std::int64_t frame_us) const {
        if (!ac4 || ac4_failed || frame_us <= 0) {
            return false;
        }
        if (ac4_decode_ratio >= kAc4CatchUpRatioLimit) {
            return false;
        }
        return static_cast<double>(late_us) * ac4_decode_ratio <=
               (1.0 - ac4_decode_ratio) * static_cast<double>(kExpectedIFrameWaitUs);
    }

    static void ac4_discard_block(void* /*self*/, const Ac4Stream::Block& /*block*/) {}

    // The chunk decoded and its output thrown away: nothing is rendered or handed to the sink.
    // What follows is anchored again by the next chunk that is played, with the renderer's history
    // cleared, as after a drop; the decoder keeps its state.
    void keep_late_ac4(std::span<const std::byte> sync_frame) {
        if (!ensure_ac4_open()) {
            return;
        }
        const std::int64_t started = esp_timer_get_time();
        const Ac4Stream::Result result = ac4->decode(sync_frame, &Impl::ac4_discard_block, this);
        const std::uint64_t spent = static_cast<std::uint64_t>(esp_timer_get_time() - started);
        reset_output();
        switch (result) {
            case Ac4Stream::Result::kDecoded:
                ac4_frame_samples = ac4->frame_samples();
                ++late_kept_chunks;
                note_ac4_cost(spent);
                return;
            case Ac4Stream::Result::kWaiting:
                // Waiting for an I-frame still, and this chunk was not one: the same as before.
                return;
            case Ac4Stream::Result::kRefused:
                ac4_failed = true;
                std::printf("burst player: refusing this AC-4 stream: %.*s\n",
                            static_cast<int>(ac4->reason().size()), ac4->reason().data());
                invalid_chunks.fetch_add(1);
                return;
            case Ac4Stream::Result::kInvalid:
                break;
        }
        restart_after_error();
    }

    void play_burst(const Header& header, std::span<const std::uint8_t> payload) {
        if (stream != Stream::kBursts) {
            return;
        }
        if (static_cast<ss::BurstDataType>(header.pc & 0x1FU) != ac::burst_data_type(data_type)) {
            invalid_chunks.fetch_add(1);
            return;
        }
        if (!have_origin) {
            have_origin = true;
            origin_server_us = header.server_us;
        }
        const std::uint64_t frame = frame_of(header.server_us);
        const std::int64_t local_us = local_of(header);
        // Too late to play any of it: dropped before it is decoded, and the
        // decoder starts again with the next.
        if (const std::optional<std::int64_t> next = playout->next_play_us(esp_timer_get_time())) {
            const std::uint64_t frames =
                data_type == ac::DataType::kAc4
                    ? (ac4_frame_samples != 0 ? ac4_frame_samples : kAc4FrameFrames)
                    : kBurstFrames;
            const std::int64_t frame_us =
                static_cast<std::int64_t>((frames * 1'000'000) / config.sample_rate);
            const std::int64_t end = local_us + frame_us;
            if (end < *next) {
                ++late_chunks;
                // An AC-4 frame depends on the ones before it, so a dropped chunk costs the decoder
                // its state and the stream everything up to the next I-frame, which can be a second
                // or more away for a chunk that was a few milliseconds late. One the decoder
                // catches up on soon enough is decoded and discarded instead.
                if (data_type == ac::DataType::kAc4 && late_ac4_is_kept(*next - end, frame_us)) {
                    keep_late_ac4(std::as_bytes(payload));
                    return;
                }
                reset_decoding();
                return;
            }
        }
        if (!decoded_frame_set) {
            decoded_frame_set = true;
            decoded_frame = frame;
            if (frame > accounted_frame) {
                lost_frames += frame - accounted_frame;
                accounted_frame = frame;
            }
        }
        mark(frame, local_us);
        ++bursts_played;
        coded_bytes += payload.size();
        const std::int64_t started = esp_timer_get_time();
        // An AC-4 chunk is one sync frame, whole: it is not cut into units as AC-3's and E-AC-3's
        // bursts are.
        const bool whole = data_type == ac::DataType::kAc4
                               ? decode_ac4(std::as_bytes(payload))
                               : for_each_access_unit(std::as_bytes(payload),
                                                      [this](std::span<const std::byte> unit) {
                                                          decode_unit(unit);
                                                      });
        if (!whole) {
            restart_after_error();
        }
        count_time(started);
    }

    // A chunk's time, from `started` to now: the sink's part and the rest.
    // A progress line follows every config.report_every_chunks chunks.
    void count_time(std::int64_t started) {
        const std::uint64_t spent = static_cast<std::uint64_t>(esp_timer_get_time() - started);
        const std::uint64_t in_sink = timed.take_spent();
        const std::uint64_t own = spent > in_sink ? spent - in_sink : 0;
        decode_us_total += own;
        sink_us_total += in_sink;
        ++timed_bursts;
        if (data_type == ac::DataType::kAc4) {
            note_ac4_cost(own);
        }
        worst_burst_us = std::max(worst_burst_us, static_cast<std::uint32_t>(own));
        if (config.report_every_chunks > 0 && timed_bursts % config.report_every_chunks == 0) {
            report_progress();
        }
    }

    // What the closing line will say, so far, with the internal heap free
    // now, its largest block and the least since the stream began: the
    // network's receive buffers come from there, and a stream that runs it
    // out loses packets.
    void report_progress() const {
        const Playout::Stats& p = playout->stats();
        std::printf("sendspin.progress chunks=%llu late=%llu underruns=%llu resyncs=%u burst_us=%llu "
                    "worst_burst_us=%lu sink_us=%llu ring_high=%lu heap_free=%lu heap_largest=%lu heap_least=%lu\n",
                    static_cast<unsigned long long>(bursts_played), static_cast<unsigned long long>(late_chunks),
                    static_cast<unsigned long long>(p.underruns), static_cast<unsigned>(p.resyncs),
                    static_cast<unsigned long long>(decode_us_total / timed_bursts),
                    static_cast<unsigned long>(worst_burst_us),
                    static_cast<unsigned long long>(sink_us_total / timed_bursts), static_cast<unsigned long>(ring_high),
                    static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    }

    void play_pcm(const Header& header, std::span<const std::uint8_t> payload) {
        if (stream != Stream::kPcm) {
            return;
        }
        const std::int64_t started = esp_timer_get_time();
        const std::size_t channels = static_cast<std::size_t>(std::clamp<std::int32_t>(pcm_format.channels, 1, 2));
        // An Opus packet is decoded to 16-bit samples, and from there is a PCM chunk of its own.
        std::span<const std::uint8_t> samples = payload;
        std::size_t width = pcm_format.bit_depth == 24 ? 3 : 2;
        if (pcm_format.codec == m::Codec::kOpus) {
            const int decoded = opus ? opus->decode(payload, opus_pcm) : -1;
            if (decoded <= 0) {
                invalid_chunks.fetch_add(1);
                return;
            }
            width = 2;
            samples = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(opus_pcm.data()),
                static_cast<std::size_t>(decoded) * channels * width);
        }
        const std::size_t frame_bytes = channels * width;
        if (samples.size() % frame_bytes != 0) {
            invalid_chunks.fetch_add(1);
            return;
        }
        if (!have_origin) {
            have_origin = true;
            origin_server_us = header.server_us;
        }
        const std::uint64_t first = frame_of(header.server_us);
        mark(first, local_of(header));
        decoded_frame = first;
        ++bursts_played;
        const std::size_t frames = samples.size() / frame_bytes;
        const iclforge::ac3::eac3::chanmap::Layout bed =
            iclforge::ac3::eac3::chanmap::expand(iclforge::ac3::eac3::chanmap::acmod_map(channels == 1 ? iclforge::ac3::Acmod::k1_0 : iclforge::ac3::Acmod::k2_0, false));
        if (!serving.fold && (!renderer_bed || !same_layout(*renderer_bed, bed))) {
            renderer.set_bed(bed);
            renderer_bed = bed;
        }
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t n = std::min(kBlock, frames - done);
            const std::span<const std::uint8_t> bytes =
                samples.subspan(done * frame_bytes, n * frame_bytes);
            // Integer operations only: a chunk's conversion to float and its meter were the whole
            // of the time it took to render on a part without a floating-point unit.
            std::array<PcmLevel, 2> levels{};
            const std::array<std::span<float>, 2> planar{pcm_channels[0].first(n),
                                                         pcm_channels[1].first(n)};
            if (width == 2) {
                unpack_pcm16(bytes, channels, n, planar, levels);
            } else {
                unpack_pcm24(bytes, channels, n, planar, levels);
            }
            std::size_t used = channels;
            if (serving.fold && channels == 2 && layout.speaker_count() == 1) {
                // A one-speaker room hears both channels.
                for (std::size_t k = 0; k < n; ++k) {
                    pcm_channels[0][k] = 0.5F * (pcm_channels[0][k] + pcm_channels[1][k]);
                }
                used = 1;
            } else if (serving.fold && channels == 1 && layout.speaker_count() == 2) {
                // And a two-speaker room hears one on both.
                std::copy_n(pcm_channels[0].begin(), n, pcm_channels[1].begin());
                used = 2;
            }
            for (std::size_t c = 0; c < used; ++c) {
                pcm_views[c] = std::span<const float>(pcm_channels[c].data(), n);
            }
            const iclforge::ac3::PcmBlock block{.index = 0,
                                      .blocks = 1,
                                      .channels = std::span<const std::span<const float>>(pcm_views.data(), used),
                                      .objects = {},
                                      .object_indices = {},
                                      .object_metadata = nullptr};
            const std::span<const std::span<float>> out(rendered.data(), layout.slots());
            std::uint32_t reached = renderer.all_slots();
            if (serving.fold) {
                renderer.render_folded(block, 1.0F, out);
            } else {
                reached = renderer.render_reached(block, false, 1.0F, out);
            }
            // The levels stand for the outputs only while the channels are the source's own: not
            // for a two-channel stream folded to one speaker, or a mono one given to two.
            if (used == channels) {
                source_levels = levels.data();
                source_channels = channels;
                source_bits = width == 2 ? 16U : 24U;
                for (std::size_t c = 0; c < source_outputs.size(); ++c) {
                    source_outputs[c] = c < channels ? output_of_slot_only(serving.fold ? renderer.folded_slot(c, channels)
                                                                                     : renderer.unity_slot(c))
                                                     : -1;
                }
            }
            emit(n, reached);
            source_levels = nullptr;
            done += n;
        }
        count_time(started);
    }

    // The output a slot is patched to, when the slot's samples are all that output carries; else -1.
    [[nodiscard]] int output_of_slot_only(int slot) const {
        if (slot < 0) {
            return -1;
        }
        const int output = routing.output_of(static_cast<std::size_t>(slot));
        if (output < 0 || static_cast<std::size_t>(output) >= outputs_open ||
            routing.channel_of(static_cast<std::size_t>(output)) != slot) {
            return -1;
        }
        return output;
    }

    // The identify tone with nothing playing: blocks of it straight to the
    // sink, paced by the sink.
    void identify_idle() {
        const std::span<const std::span<float>> out(outputs.data(), outputs_open);
        if (local_identify.active()) {
            (void)local_identify.fill(out);
            // The tone takes the speaker's level trim, as music does, so that a level set by ear is heard.
            for (std::size_t o = 0; o < outputs_open; ++o) {
                const float g = trim.gain(o);
                if (g != 1.0F) {
                    for (std::size_t k = 0; k < kBlock; ++k) {
                        outputs[o][k] *= g;
                    }
                }
            }
        } else {
            const std::size_t output =
                identifying->output >= 0 ? static_cast<std::size_t>(identifying->output) : outputs_open;
            tone.fill(out, output, carries_lfe(output) ? IdentifyTone::Band::kLow : IdentifyTone::Band::kFull);
        }
        apply_gain(out, kBlock);
        tap_block(out, kBlock, ~std::uint32_t{0});
        meter(out, kBlock);
        for (std::size_t o = 0; o < outputs_open; ++o) {
            output_views[o] = outputs[o];
        }
        (void)sink.write(std::span<const std::span<const float>>(output_views.data(), outputs_open));
    }

    void publish_status() {
        const std::lock_guard lock(status_mutex);
        BurstPlayerStatus& s = status;
        s.stream = stream == Stream::kBursts ? "bursts" : stream == Stream::kPcm ? "pcm" : "idle";
        s.have_decoder = have_decoder_report;
        s.decoder = decoder_report;
        s.outputs = outputs_open;
        s.counters = ac::Counters{.bursts_played = bursts_played,
                                  .underruns = playout->stats().underruns,
                                  .late_chunks = late_chunks,
                                  .dropped_chunks = dropped_chunks.load(),
                                  .invalid_chunks = invalid_chunks.load()};
        s.coded_bytes = coded_bytes;
        s.playout = playout->stats();
        if (s.playout.have_last) {
            s.have_play = true;
            s.play_frame = s.playout.last_frame;
            s.play_local_us = s.playout.last_play_us;
        } else {
            s.have_play = false;
        }
        s.burst_us = static_cast<std::uint32_t>(timed_bursts > 0 ? decode_us_total / timed_bursts : 0);
        s.worst_burst_us = worst_burst_us;
        s.stack_free = static_cast<std::size_t>(uxTaskGetStackHighWaterMark(nullptr));
        {
            const std::lock_guard settings_lock(settings_mutex);
            s.settings_revision = settings_revision;
        }
        const std::string_view text = layout.text();
        const std::size_t length = std::min(text.size(), s.layout.size() - 1);
        std::memcpy(s.layout.data(), text.data(), length);
        s.layout[length] = '\0';
        s.identifying = identifying.has_value();
        s.identify_local = local_identify.active();
        const int tone_output = local_identify.output();
        s.identify_output = static_cast<std::int8_t>(tone_output);
        s.identify_role = tone_output >= 0 ? trim.role(static_cast<std::size_t>(tone_output)) : speaker_trim::kNoRole;
        s.identify_remaining_ms = local_identify.remaining_ms();
        BurstPlayerTrim& t = trim_report;
        t.layout = s.layout;
        for (std::size_t o = 0; o < Playout::kMaxOutputs; ++o) {
            t.role[o] = trim.role(o);
            t.level_db[o] = static_cast<float>(trim.level_db(o));
            t.delay_samples[o] = trim.delay_samples(o);
        }
        t.clamped = trim.clamped();
        t.server_level = trim.server_level();
        t.server_delay = trim.server_delay();
        t.max_delay_samples = static_cast<std::uint32_t>(trim.max_delay_samples());
        t.farthest_cm = trim.farthest_cm();
        if (stream == Stream::kIdle) {
            s.have_levels = false;
        }
    }

    // Something else has the sink: nothing is written until it is given back,
    // and then the sink is opened again the player's own way.
    void wait_while_held() {
        // Whatever takes the sink next has it to itself: not even a quiet tone is left in it.
        local_identify.stop(true);
        playout->restart();
        reset_decoding();
        have_origin = false;
        xEventGroupSetBits(events, kHeld);
        while (held.load() && !stopping.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        xEventGroupClearBits(events, kHeld);
        open_outputs(true);
    }

    void run() {
#if SOC_CPU_CORES_NUM > 1
        if (config.decode_lane && (config.core == 0 || config.core == 1)) {
            lane.emplace();
            const bool in_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
            if (lane->start(1 - config.core, config.priority, in_psram ? 16384 : 12288, in_psram)) {
                decoder_config.executor = &*lane;
                std::printf("burst player: decode stages on cores %d and %d\n", static_cast<int>(config.core),
                            static_cast<int>(1 - config.core));
            } else {
                std::printf("burst player: no second lane for the decode, one core\n");
                lane.reset();
            }
        }
#endif
        std::int64_t last_status = 0;
        while (!stopping.load()) {
            if (held.load()) {
                wait_while_held();
                continue;
            }
            if (stream == Stream::kIdle) {
                apply_pending();
            }
            const TickType_t wait = (identifying || local_identify.active()) && stream == Stream::kIdle ? 0 : pdMS_TO_TICKS(50);
            const std::size_t free_now = xMessageBufferSpacesAvailable(ring);
            const std::size_t held = ring_free_empty > free_now ? ring_free_empty - free_now : 0;
            const std::size_t got = xMessageBufferReceive(ring, receive_buffer, entry_bytes, wait);
            if (stream != Stream::kIdle) {
                ring_high = std::max(ring_high, held);
            }
            const std::int64_t now = esp_timer_get_time();
            if (now - last_status >= 100'000) {
                last_status = now;
                publish_status();
            }
            if (got < sizeof(Header)) {
                if ((identifying || local_identify.active()) && stream == Stream::kIdle) {
                    identify_idle();
                }
                continue;
            }
            Header header;
            std::memcpy(&header, receive_buffer, sizeof(Header));
            const std::span<const std::uint8_t> payload(receive_buffer + sizeof(Header), got - sizeof(Header));
            const bool stale = header.generation != generation.load();
            switch (header.kind) {
                case Kind::kStartBursts:
                case Kind::kStartPcm: {
                    // A play begins: the board's own tone is over before its first block, not after it.
                    local_identify.stop(true);
                    const Stream kind = header.kind == Kind::kStartBursts ? Stream::kBursts : Stream::kPcm;
                    const ac::DataType type = static_cast<ac::DataType>(header.data_type);
                    const m::AudioFormat format{.codec = kind == Stream::kPcm && header.pc == 1
                                                             ? m::Codec::kOpus
                                                             : m::Codec::kPcm,
                                                .channels = header.channels,
                                                .sample_rate = header.sample_rate,
                                                .bit_depth = header.bit_depth};
                    // A start for the stream already running changes it in
                    // place: what is buffered still plays.
                    const bool in_place = stream == kind &&
                                          (kind == Stream::kBursts ? type == data_type : format == pcm_format);
                    apply_pending();
                    if (!in_place) {
                        if (stream != Stream::kIdle) {
                            report_stream_end();
                        }
                        data_type = type;
                        pcm_format = format;
                        new_stream(kind);
                    } else if (kind == Stream::kBursts && type != data_type) {
                        data_type = type;
                        reset_decoding();
                    }
                    break;
                }
                case Kind::kClear:
                    // The sink stays as it is: the stream this clears is the
                    // one it was opened for.
                    reset_decoding();
                    playout->restart();
                    have_origin = false;
                    break;
                case Kind::kEnd:
                    end_stream();
                    apply_pending();
                    break;
                case Kind::kBurst:
                    if (!stale) {
                        apply_pending();
                        play_burst(header, payload);
                    }
                    break;
                case Kind::kPcm:
                    if (!stale) {
                        apply_pending();
                        play_pcm(header, payload);
                    }
                    break;
            }
        }
        // The decoders hold a pointer to the lane: they go before it does.
        drop_decoders();
        decoder_config.executor = nullptr;
        lane.reset();
        xEventGroupSetBits(events, kExited);
        if (task_with_caps) {
            vTaskSuspend(nullptr);
        }
        vTaskDelete(nullptr);
    }

    static void entry(void* self) { static_cast<Impl*>(self)->run(); }

    void release() {
        if (ring != nullptr) {
            vMessageBufferDelete(ring);
            ring = nullptr;
        }
        heap_caps_free(ring_struct);
        heap_caps_free(ring_storage);
        heap_caps_free(send_buffer);
        heap_caps_free(receive_buffer);
        heap_caps_free(floats);
        ring_struct = nullptr;
        ring_storage = nullptr;
        send_buffer = nullptr;
        receive_buffer = nullptr;
        floats = nullptr;
        playout.reset();
        if (events != nullptr) {
            vEventGroupDelete(events);
            events = nullptr;
        }
    }
};

BurstPlayer::BurstPlayer(const BurstPlayerConfig& config, ScheduledSink& sink)
    : impl_(std::make_unique<Impl>(config, sink)) {}

BurstPlayer::~BurstPlayer() { stop(); }

bool BurstPlayer::start() {
    Impl& im = *impl_;
    if (im.task != nullptr) {
        return true;
    }
    im.config.max_outputs = std::clamp<std::size_t>(im.config.max_outputs, 1, Playout::kMaxOutputs);
    const std::uint32_t caps = bulk_caps();
    im.entry_bytes = sizeof(Header) + im.config.max_chunk_bytes;
    im.ring_struct = static_cast<StaticMessageBuffer_t*>(heap_caps_malloc(sizeof(StaticMessageBuffer_t), caps));
    im.ring_storage = static_cast<std::uint8_t*>(heap_caps_malloc(im.config.ring_bytes, caps));
    im.send_buffer = static_cast<std::uint8_t*>(heap_caps_malloc(im.entry_bytes, caps));
    im.receive_buffer = static_cast<std::uint8_t*>(heap_caps_malloc(im.entry_bytes, caps));
    im.max_delay_samples = TrimDelay::samples_for_ms(im.config.max_delay_ms, im.config.sample_rate);
    const std::size_t outputs = im.config.max_outputs;
    const std::size_t float_count = (kMaxSlots * kBlock) + (outputs * kBlock) + (outputs * kBlock) + (2 * kBlock) +
                                    TrimDelay::storage_floats(outputs, im.max_delay_samples);
    im.floats = static_cast<float*>(heap_caps_calloc(float_count, sizeof(float), caps));
    im.events = xEventGroupCreate();
    if (im.ring_struct == nullptr || im.ring_storage == nullptr || im.send_buffer == nullptr ||
        im.receive_buffer == nullptr || im.floats == nullptr || im.events == nullptr) {
        std::printf("burst player: no memory for its buffers (%u floats, a %u-byte ring)\n",
                    static_cast<unsigned>(float_count), static_cast<unsigned>(im.config.ring_bytes));
        im.release();
        return false;
    }
    im.ring = xMessageBufferCreateStatic(im.config.ring_bytes, im.ring_storage, im.ring_struct);
    if (im.ring == nullptr) {
        std::printf("burst player: could not create the ring\n");
        im.release();
        return false;
    }
    im.ring_free_empty = xMessageBufferSpacesAvailable(im.ring);
    std::span<float> all(im.floats, float_count);
    std::size_t at = 0;
    for (std::size_t s = 0; s < kMaxSlots; ++s, at += kBlock) {
        im.rendered[s] = all.subspan(at, kBlock);
    }
    for (std::size_t o = 0; o < outputs; ++o, at += kBlock) {
        im.outputs[o] = all.subspan(at, kBlock);
    }
    const std::span<float> staging = all.subspan(at, outputs * kBlock);
    at += outputs * kBlock;
    for (std::size_t c = 0; c < 2; ++c, at += kBlock) {
        im.pcm_channels[c] = all.subspan(at, kBlock);
    }
    im.delay_storage = all.subspan(at);
    im.playout.emplace(staging, outputs, im.config.tuning);
    im.trim.setup(im.delay_storage, outputs, im.max_delay_samples, im.config.sample_rate);
    im.trim.set_table(im.config.speaker_trim);

    im.configure_defaults(im.config.layout);
    std::printf("burst player: ring %u bytes and buffers in %s, layout %s, %u outputs, delay up to %u samples\n",
                static_cast<unsigned>(im.config.ring_bytes),
                (caps & MALLOC_CAP_SPIRAM) != 0 ? "PSRAM" : "internal SRAM", im.layout.text().data(),
                static_cast<unsigned>(im.outputs_open), static_cast<unsigned>(im.max_delay_samples));

    im.stopping.store(false);
    // The stack in PSRAM is asked for and checked for presence as the ring is, not learned from a
    // failed allocation.
    BaseType_t created = pdFAIL;
    if (im.config.stack_in_psram && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        created = xTaskCreatePinnedToCoreWithCaps(&Impl::entry, "sendspin-play", im.config.stack_bytes, &im,
                                                  im.config.priority, &im.task, im.config.core,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        im.task_with_caps = created == pdPASS;
        if (im.task_with_caps) {
            std::printf("burst player: decode stack %u bytes in PSRAM\n",
                        static_cast<unsigned>(im.config.stack_bytes));
        }
    }
    if (created != pdPASS) {
        created = xTaskCreatePinnedToCore(&Impl::entry, "sendspin-play", im.config.stack_bytes, &im,
                                          im.config.priority, &im.task, im.config.core);
    }
    if (created != pdPASS) {
        std::printf("burst player: could not start its task\n");
        im.task = nullptr;
        im.release();
        return false;
    }
    return true;
}

void BurstPlayer::stop() {
    Impl& im = *impl_;
    if (im.task == nullptr) {
        return;
    }
    im.stopping.store(true);
    const EventBits_t bits = xEventGroupWaitBits(im.events, kExited, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    if ((bits & kExited) == 0) {
        std::printf("burst player: its task did not stop; leaving it\n");
        return;
    }
    if (im.task_with_caps && im.task != nullptr) {
        vTaskDeleteWithCaps(im.task);
    }
    im.task_with_caps = false;
    im.task = nullptr;
    im.release();
}

std::size_t BurstPlayer::buffer_capacity() const {
    const std::size_t ring = impl_->config.ring_bytes;
    return ring > kControlReserve * 2 ? ring - (kControlReserve * 2) : 0;
}

void BurstPlayer::start_bursts(const ac::StreamStart& stream) {
    Header header;
    header.kind = Kind::kStartBursts;
    header.data_type = static_cast<std::uint8_t>(stream.data_type);
    header.sample_rate = stream.sample_rate;
    header.generation = impl_->generation.load();
    impl_->send(header, {}, true);
}

void BurstPlayer::start_pcm(const m::AudioFormat& format) {
    Header header;
    header.kind = Kind::kStartPcm;
    // The codec the stream is in: 0 PCM, 1 Opus.
    header.pc = format.codec == m::Codec::kOpus ? 1U : 0U;
    header.sample_rate = format.sample_rate;
    header.channels = format.channels;
    header.bit_depth = format.bit_depth;
    header.generation = impl_->generation.load();
    impl_->send(header, {}, true);
}

void BurstPlayer::clear() {
    Header header;
    header.kind = Kind::kClear;
    // Whatever the ring holds from before this is dropped as it comes out.
    header.generation = impl_->generation.fetch_add(1) + 1;
    impl_->send(header, {}, true);
}

void BurstPlayer::end() {
    Header header;
    header.kind = Kind::kEnd;
    header.generation = impl_->generation.fetch_add(1) + 1;
    impl_->send(header, {}, true);
}

void BurstPlayer::burst(const ss::BurstChunk& chunk, std::int64_t local_us) {
    Header header;
    header.kind = Kind::kBurst;
    header.pc = chunk.pc;
    header.pd = chunk.pd;
    header.generation = impl_->generation.load();
    header.local_us = local_us;
    header.server_us = chunk.chunk.timestamp_us;
    header.map_local_us = impl_->map_now(header.server_us);
    impl_->send(header, chunk.chunk.data, false);
}

void BurstPlayer::pcm(std::span<const std::uint8_t> frame, std::int64_t server_us, std::int64_t local_us) {
    Header header;
    header.kind = Kind::kPcm;
    header.generation = impl_->generation.load();
    header.local_us = local_us;
    header.server_us = server_us;
    header.map_local_us = impl_->map_now(server_us);
    impl_->send(header, frame, false);
}

void BurstPlayer::invalid_chunk() { impl_->invalid_chunks.fetch_add(1); }

std::optional<std::string> BurstPlayer::settings(const ac::Settings& settings, const ac::Support& support) {
    if (std::optional<std::string> why = ac::check_settings(settings, support)) {
        return why;
    }
    if (settings.layout) {
        const std::optional<OutputLayout> parsed = OutputLayout::parse(*settings.layout);
        if (!parsed) {
            return std::string("the layout is not one this board reads: ") + *settings.layout;
        }
        if (!settings.routing && parsed->slots() > impl_->sink.max_outputs()) {
            return std::string("the layout has more speakers than this board has outputs");
        }
    }
    if (settings.routing && !Routing::parse(*settings.routing, std::min(impl_->config.max_outputs,
                                                                        static_cast<std::size_t>(support.outputs.count)))) {
        return std::string("the routing is not one this board can patch: ") + *settings.routing;
    }
    if (settings.delay_ms) {
        for (const double ms : *settings.delay_ms) {
            if (TrimDelay::samples_for_ms(ms, impl_->config.sample_rate) > impl_->max_delay_samples) {
                return std::string("a delay is longer than this board holds");
            }
        }
    }
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->pending_settings = settings;
    impl_->pending_layout.reset();
    impl_->have_server_settings = true;
    return std::nullopt;
}

bool BurstPlayer::set_layout(const OutputLayout& layout) {
    if (layout.slots() > impl_->sink.max_outputs()) {
        return false;
    }
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->config.layout = layout;
    impl_->pending_layout = layout;
    return true;
}

void BurstPlayer::set_dynamic_range(iclforge::base::DynamicRange range) {
    impl_->wanted_range.store(static_cast<std::uint8_t>(range));
}

void BurstPlayer::identify(std::optional<ac::Identify> tone) {
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->pending_identify = tone;
    impl_->identify_changed = true;
}

void BurstPlayer::set_speaker_trim(const SpeakerTrimTable& table) {
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->config.speaker_trim = table;
    impl_->pending_trim = table;
}

IdentifyOutcome BurstPlayer::identify_local(const IdentifyRequest& request) {
    Impl& im = *impl_;
    if (request.shape == IdentifyRequest::Shape::kOff) {
        const std::lock_guard lock(im.settings_mutex);
        im.pending_local_identify = request;
        return IdentifyOutcome::kStopped;
    }
    if (im.held.load() || im.stopping.load() || im.task == nullptr) {
        return IdentifyOutcome::kBusy;
    }
    if (im.active.load()) {
        return IdentifyOutcome::kPlaying;
    }
    if (status().identifying) {
        return IdentifyOutcome::kServer;
    }
    // The speakers the layout in force has at an output, as the last report said.
    const BurstPlayerTrim now = trim_status();
    bool any = false;
    bool found = false;
    for (std::size_t o = 0; o < Playout::kMaxOutputs; ++o) {
        if (now.role[o] != speaker_trim::kNoRole) {
            any = true;
            found = found || now.role[o] == request.role;
        }
    }
    if (!any || (request.shape != IdentifyRequest::Shape::kEach && !found)) {
        return IdentifyOutcome::kNoSuchSpeaker;
    }
    const std::lock_guard lock(im.settings_mutex);
    im.pending_local_identify = request;
    return IdentifyOutcome::kStarted;
}

void BurstPlayer::set_block_tap(BlockTap* tap) { impl_->block_tap.store(tap, std::memory_order_release); }

void BurstPlayer::set_volume(int volume, bool muted) {
    const float linear = static_cast<float>(std::clamp(volume, 0, 100)) / 100.0F;
    impl_->target_gain.store(muted ? 0.0F : linear * std::sqrt(linear));
}

bool BurstPlayer::hold(bool held) {
    Impl& im = *impl_;
    // Chunks from before a hold, and from during it, are not played after it.
    im.generation.fetch_add(1);
    im.held.store(held);
    if (im.task == nullptr || im.events == nullptr) {
        return true;
    }
    if (!held) {
        return true;
    }
    const EventBits_t bits = xEventGroupWaitBits(im.events, kHeld, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    return (bits & kHeld) != 0;
}

BurstPlayerStatus BurstPlayer::status() const {
    const std::lock_guard lock(impl_->status_mutex);
    return impl_->status;
}

BurstPlayerLevels BurstPlayer::levels() const {
    BurstPlayerLevels out;
    const std::lock_guard lock(impl_->status_mutex);
    const BurstPlayerStatus& s = impl_->status;
    out.have = s.have_levels && impl_->active.load();
    out.serial = s.levels_serial;
    out.outputs = std::min(s.outputs, out.peak_db.size());
    for (std::size_t o = 0; o < out.outputs; ++o) {
        out.peak_db[o] = s.peak_db[o];
        out.rms_db[o] = s.rms_db[o];
    }
    return out;
}

BurstPlayerTrim BurstPlayer::trim_status() const {
    const std::lock_guard lock(impl_->status_mutex);
    return impl_->trim_report;
}

bool BurstPlayer::active() const { return impl_->active.load(); }

bool BurstPlayer::decodes_ac4() {
    return Ac4Stream::available();
}

bool BurstPlayer::decodes_opus() {
    return OpusStream::available();
}

}  // namespace iclforge
