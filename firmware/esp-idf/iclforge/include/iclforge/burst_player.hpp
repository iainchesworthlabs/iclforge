#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "freertos/FreeRTOS.h"

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/base/dynamic_range.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/playout.hpp"

// The Sendspin player's audio (planning/hearth-reference-player.md, B3): what
// the server sends arrives here with the time it should play, and leaves for
// the sink at that time.
//
//   _iclforge_player@v1's bursts (planning/hearth-sendspin-extension.md, Burst
//   chunks) are decoded, AC-3 or E-AC-3 with any object layer, or AC-4's channels
//   where the build has its decoder (ac4_stream.hpp), and rendered onto the
//   board's layout by iclforge::render;
//   player@v1's PCM, which is what Music Assistant sends, is rendered onto the
//   same layout as a stereo or mono bed.
//
// Either way each block is then routed to the sink's outputs, trimmed and
// delayed per output (iclforge::render::Routing and TrimDelay), replaced by the
// identify tone while one plays, metered, scaled by the player's volume, and
// handed to a Playout, which writes it so that its first frame leaves the
// audio port when the server said.
//
// ONE TASK DOES THE WORK. The Sendspin session's task only copies each chunk,
// with its local time, into a ring - PSRAM where the part has it - and the
// decode task takes them in order, so a slow decode never holds up the
// network. Stream starts, clears and ends travel through the same ring, so
// they take effect exactly between the chunks they came between; a clear or
// an end also drops whatever the ring still holds from before it.
//
// A chunk that could not play in time is dropped before it is decoded
// (late_chunks), and so the next is decoded as the start of a stream. What
// the decoder hands over for a unit it held back (§3.7) is placed by the
// unit's own time: every decoded frame is counted from the stream's start,
// and each burst's time says where its frames go.
//
// Settings from the server - layout, routing, trims, delays, the crossover
// and the decoder's own - are checked when they arrive and applied by the
// decode task at the next burst boundary.

namespace iclforge {

// Where a burst player writes: a PlayoutSink that can also be opened for a
// number of outputs. The i2s sink is one; the capture sink is the other.
class ScheduledSink : public PlayoutSink {
   public:
    // Opens for `outputs` outputs at `sample_rate`. False when this sink
    // cannot carry that many.
    [[nodiscard]] virtual bool open(std::uint32_t sample_rate, std::size_t outputs) = 0;
    // The most outputs the sink could be opened for now.
    [[nodiscard]] virtual std::size_t max_outputs() = 0;
    // A stream is beginning: the sink's own figures start again.
    virtual void begin_stream() = 0;
};

struct BurstPlayerConfig {
    std::uint32_t sample_rate = 48000;
    // The ring between the session and the decode task, in bytes, chunks and
    // their headers together. In PSRAM where the part has some. What a
    // server may send ahead is this less a reserve kept for stream starts,
    // clears and ends (buffer_capacity()).
    std::size_t ring_bytes = 256 * 1024;
    // The largest chunk taken, and the decode task's buffer for one: an
    // E-AC-3 burst at the bitstream's ceiling is 24,585 bytes, and 150 ms of
    // 24-bit stereo PCM 21,600.
    std::size_t max_chunk_bytes = 25 * 1024;
    // The most outputs the player will ever route to, for its buffers.
    std::size_t max_outputs = Playout::kMaxOutputs;
    // The longest delay a speaker can be given; delay lines for every output,
    // in PSRAM where the part has some. 0 offers no delay.
    double max_delay_ms = 20.0;
    // A syncframe coding more full-bandwidth channels plus LFE than this
    // (FrameHeader::coded_channels()) is refused before a decoder is ever
    // opened for it, rather than decoded: the decoder's own scratch scales
    // with the channel count, and on a part with no PSRAM and a Sendspin
    // ring resident, running out partway through can fail an allocation the
    // heap has no room left to recover from. 0 refuses nothing - the
    // default, and every board with room to decode whatever `outputs.count`
    // does not already turn away.
    std::size_t max_coded_channels = 0;
    // As max_coded_channels, for an AC-4 stream: the most channels a frame may decode to, which is
    // what the board keeps up with (the P4's 6, the S3's 2), refused once the decoder has said how
    // many a frame has. 0 refuses none.
    std::size_t max_ac4_channels = 0;
    // The most frames a second an AC-4 stream may have, which the support object states as
    // max_frame_rate (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4_MAX_FRAME_RATE): the S3's decode of a
    // frame through the frame-rate converter takes the same time at 29.97 a second as at 24, which
    // is a play it cannot keep up with. A frame of a faster stream is refused, as one with more
    // channels is. 0 refuses none.
    std::uint32_t max_ac4_frame_rate = 0;
    // The most objects an E-AC-3 stream may declare for the board to place them in real time, which
    // the support object states as max_objects (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS), and
    // nothing for a board that states none. A stream that declares more (iclforge/object_limit.hpp:
    // its complexity index less the LFE) is played as its bed, from its first unit and without the
    // object reconstruction's allocations: the objects past what the board keeps up with would
    // have chunks come late, which is worse than a bed on time. A listener who chose objects
    // "always" is not overruled.
    std::optional<std::uint8_t> max_objects{};

    BaseType_t core = 1;
    UBaseType_t priority = 6;
    // A worker on the other core takes a decode's independent per-channel and per-object
    // stages (PlayerConfig::decode_lane): made when the task starts, at the task's priority,
    // its stack in PSRAM where there is PSRAM. The PCM is the same bit for bit.
    bool decode_lane = false;
    // The decode task's stack, which the decoder needs most of
    // (PlayerConfig::decode_stack_bytes has the measurements).
    std::uint32_t stack_bytes = 32768;
    // The decode task's stack in PSRAM where there is PSRAM, for a part whose internal RAM is what
    // Wi-Fi leaves it: an ESP32-S3 with Wi-Fi and the player up has no internal block over 31,744
    // bytes, which is less than `stack_bytes`, and a stack that fits one boot stops fitting when a
    // few bytes more are allocated before the task starts (the Sendspin player's start is later than
    // Wi-Fi's, mDNS's and the control server's). The stack is then read through the data cache
    // (PlayerConfig::decode_stack_in_psram has the measurements). Needs
    // CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM, and falls back to internal RAM without PSRAM. The
    // task parks at its end and stop() deletes it with vTaskDeleteWithCaps().
    bool stack_in_psram = false;
    // A sendspin.progress line on the console every this many chunks while a
    // stream plays, as well as the closing line; 0 for the closing line only.
    std::uint32_t report_every_chunks = 0;
    // The local time a server time plays at by the playing connection's clock
    // as it stands, or nothing; called from any task. A chunk comes with its
    // local time worked out when it arrived, which may be seconds before it
    // plays, and the player moves it by as much as this has moved since. With
    // none, a clock update reaches the playout only with the chunks sent
    // after it.
    std::function<std::optional<std::int64_t>(std::int64_t)> local_time;

    // The layout until a server sends one, and the decoder settings under
    // it: the server's settings replace these whole, as the extension page
    // says, and a stream that sends none plays with these.
    iclforge::render::OutputLayout layout = iclforge::render::OutputLayout::stereo();
    iclforge::ac3::DecoderConfig decoder{.output = {.mode = iclforge::ac3::OperatingMode::kLine}};
    iclforge::ac3::DownmixTarget stereo_fold = iclforge::ac3::DownmixTarget::kLoRo;
    iclforge::ac3::render::ObjectsPolicy objects = iclforge::ac3::render::ObjectsPolicy::kAuto;
    // The listener's choice of dynamic range (iclforge/base/dynamic_range.hpp), which is applied
    // last, over the decoder settings above and over a server's: late night wins over a server's
    // mode and compression keys, standard leaves them be. Changed with set_dynamic_range().
    iclforge::base::DynamicRange dynamic_range = iclforge::base::kDefaultDynamicRange;

    Playout::Tuning tuning;
};

// What the player reports, for the role's client/state and the board's page.
// Fixed-size so a copy never allocates.
struct BurstPlayerStatus {
    // "idle", "bursts" or "pcm".
    const char* stream = "idle";
    // What the decoder found in the current stream, once a unit has decoded.
    bool have_decoder = false;
    iclforge::sendspin::player::DecoderReport decoder;

    // Per output: the last 100 ms, in dB of full scale (kSilenceDb for
    // silence), and the whole stream's RMS scaled by a million, as the
    // console's stream.rms lines print it.
    std::size_t outputs = 0;
    std::array<float, Playout::kMaxOutputs> peak_db{};
    std::array<float, Playout::kMaxOutputs> rms_db{};
    std::array<std::uint32_t, Playout::kMaxOutputs> stream_rms{};
    // The window the levels were measured over has closed at least once.
    bool have_levels = false;
    std::uint32_t levels_serial = 0;

    iclforge::sendspin::player::Counters counters;
    // The coded bytes of the chunks played so far in this stream (0 for PCM): the screen's bitrate.
    std::uint64_t coded_bytes = 0;
    Playout::Stats playout;

    // The newest stream frame with a known play time: its position from the
    // stream's start, and when it plays, locally.
    std::uint64_t play_frame = 0;
    std::int64_t play_local_us = 0;
    bool have_play = false;

    // Decode, render and the rest per burst, averaged over the stream, and
    // the worst.
    std::uint32_t burst_us = 0;
    std::uint32_t worst_burst_us = 0;
    // The least the decode task's stack has had spare.
    std::size_t stack_free = 0;

    std::int64_t settings_revision = 0;
    // The layout in force, as text.
    std::array<char, iclforge::render::OutputLayout::kTextBytes> layout{};
    bool identifying = false;
};

// Just the levels of the last window that closed, for a screen that draws them every 100 ms: what the status
// holds of them, with the count of windows closed so far (so that a reader knows whether it has seen this one),
// and nothing else of the status. Fixed-size, so a copy never allocates.
struct BurstPlayerLevels {
    // A stream is running and its first window has closed.
    bool have = false;
    std::size_t outputs = 0;
    // Windows closed since the player started: moves by one when a new window's levels are published.
    std::uint32_t serial = 0;
    std::array<float, Playout::kMaxOutputs> peak_db{};
    std::array<float, Playout::kMaxOutputs> rms_db{};
};

class BurstPlayer {
   public:
    BurstPlayer(const BurstPlayerConfig& config, ScheduledSink& sink);
    ~BurstPlayer();
    BurstPlayer(const BurstPlayer&) = delete;
    BurstPlayer& operator=(const BurstPlayer&) = delete;

    // Allocates the ring and the buffers and starts the task. False, having
    // said why on the console, when it cannot.
    [[nodiscard]] bool start();
    void stop();

    // What a server may send ahead, for the support objects' buffer_capacity.
    [[nodiscard]] std::size_t buffer_capacity() const;

    // From the Sendspin session's task. Each goes into the ring in order;
    // chunks that do not fit are dropped and counted.
    void start_bursts(const iclforge::sendspin::player::StreamStart& stream);
    void start_pcm(const iclforge::sendspin::messages::AudioFormat& format);
    void clear();
    void end();
    void burst(const iclforge::sendspin::BurstChunk& chunk, std::int64_t local_us);
    void pcm(std::span<const std::uint8_t> frame, std::int64_t server_us, std::int64_t local_us);
    void invalid_chunk();

    // The role's settings: checked against `support` now, and applied at the
    // next burst boundary. The reason they are refused, or nothing.
    [[nodiscard]] std::optional<std::string> settings(const iclforge::sendspin::player::Settings& settings,
                                                      const iclforge::sendspin::player::Support& support);
    // The layout a board's own page sets, for streams with no server settings:
    // applied at the next burst boundary, and replaced by the next settings a
    // server sends. False for a layout the sink cannot carry.
    [[nodiscard]] bool set_layout(const iclforge::render::OutputLayout& layout);
    // The listener's choice of dynamic range, from the next stream's start: a stream that is
    // playing finishes as it began. Safe from any task.
    void set_dynamic_range(iclforge::base::DynamicRange range);
    // The identify tone on one output at `level_db`, or none.
    void identify(std::optional<iclforge::sendspin::player::Identify> tone);
    // The player's volume, 0 to 100, and mute: (volume / 100)^1.5, ramped over
    // a block.
    void set_volume(int volume, bool muted);

    // Something else is about to use the sink: the player drops what it
    // holds and stops writing, and this returns once it has (or after two
    // seconds, false). Released, the player goes on with the chunks that
    // arrive after it, opening the sink again its own way. A stream keeps
    // running across a hold, as far as its server knows.
    bool hold(bool held);

    // Safe from any task.
    [[nodiscard]] BurstPlayerStatus status() const;
    // The levels alone, under the same lock: a lock and two short arrays, where status() copies the whole of it.
    [[nodiscard]] BurstPlayerLevels levels() const;
    // Whether a stream is running.
    [[nodiscard]] bool active() const;
    // Whether this build decodes Opus (CONFIG_ICLFORGE_SENDSPIN_OPUS): a firmware lists the format
    // in its player@v1 support only where it does.
    [[nodiscard]] static bool decodes_opus();
    // Whether this build decodes AC-4 (CONFIG_ICLFORGE_AC4): a firmware lists the data type in its
    // support only where it does, and where it keeps up (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4).
    [[nodiscard]] static bool decodes_ac4();

    struct Impl;

   private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge
