#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/base/dynamic_range.hpp"
#include "iclforge/base/executor.hpp"

// The AC-4 decoder as the Sendspin player's burst path uses it (planning/ac4.md, I6): one AC-4
// sync frame a chunk, to blocks of 256 samples a channel in the order and layout the renderer
// takes, as AC-3's and E-AC-3's decoders hand them over. It is libs/ac4's decoder in the
// component's decode arithmetic, built by CONFIG_ICLFORGE_AC4; a build without that option links
// ac4_stream_none.cpp instead, where available() is false and nothing opens, so the player lists
// no AC-4 and refuses a stream of it (the way opus_stream.hpp does for Opus).
//
// What it does not do: objects. decode_by_block() hands over a presentation's channels alone, and
// a presentation of object audio alone has none, so the board states that it places none
// (max_objects of 0) and a server sends such a presentation as PCM it decodes itself
// (apps/hearth/engine/src/sink_form.cpp). A presentation that has channels besides its objects is
// played as those channels, with its objects left undecoded (Config::skip_objects).

namespace iclforge {

class Ac4Stream {
   public:
    // One block of the decoder's output, for the player's renderer.
    struct Block {
        // One span a channel, `samples` long, in the order of `bed`; valid for the call.
        std::span<const std::span<const float>> channels;
        // The layout the channels are in, as the renderer's set_bed() takes it.
        iclforge::ac3::eac3::chanmap::Layout bed;
        std::size_t samples = 0;
        // The block's number in its frame, from 0.
        int index = 0;
    };
    using BlockFn = void (*)(void* context, const Block& block);

    enum class Result : std::uint8_t {
        // A frame came out, as blocks.
        kDecoded,
        // Nothing yet: a frame whose substreams need configuration no I-frame has sent.
        kWaiting,
        // Not a sync frame, or one the decoder refused (reason() says).
        kInvalid,
        // A rate or a set of channels the player cannot place or may not decode (reason() says).
        kRefused,
    };

    struct Config {
        // The decoder's own fold, for the player's serving of a stereo or mono layout.
        std::optional<iclforge::ac3::DownmixTarget> fold{};
        // The second lane's executor (BurstPlayerConfig::decode_lane), not owned; null for one
        // core.
        iclforge::base::Executor* executor = nullptr;
        // The most channels a frame may decode to; 0 refuses none.
        std::size_t max_channels = 0;
        // The one rate the player plays; a frame at another is refused.
        std::uint32_t sample_rate_hz = 48000;
        // The most frames a second a stream may have, which the board states as max_frame_rate
        // (the S3's 25): a frame of a stream above it is refused once the decoder has said how
        // long a frame is, which a server that read the figure never sends. 0 refuses none.
        std::uint32_t max_frame_rate = 0;
        // The decoder leaves a presentation's object audio undecoded (DecoderConfig::skip_objects):
        // the board places none, and states so (max_objects of 0), so what it would reconstruct is
        // dropped by decode_by_block(). A presentation of objects alone is then refused, which
        // the server's own choice of PCM for it makes rare.
        bool skip_objects = true;
        // The listener's dynamic range (iclforge/ac4/decoder/dynamic_range.hpp). Standard leaves
        // the decoder as it is made: no output level, and so no dialnorm and no DRC. Late night
        // brings it to a DRC mode the stream configures, at the stream's own dialnorm, from the
        // frame after the first one that says what the dialnorm is.
        iclforge::base::DynamicRange dynamic_range = iclforge::base::kDefaultDynamicRange;
    };

    // What the decoder found in the last frame that decoded.
    struct Report {
        int channels = 0;
        bool lfe = false;
        // The A/52 audio coding mode the front and surround speakers make (1/0, 2/0, 3/0, 2/2 or
        // 3/2), as DecoderReport::acmod says it for AC-4.
        int acmod = 0;
        double dialnorm_db = 0.0;
    };

    Ac4Stream();
    ~Ac4Stream();
    Ac4Stream(const Ac4Stream&) = delete;
    Ac4Stream& operator=(const Ac4Stream&) = delete;
    Ac4Stream(Ac4Stream&&) = delete;
    Ac4Stream& operator=(Ac4Stream&&) = delete;

    // Whether this build decodes AC-4 at all (CONFIG_ICLFORGE_AC4).
    [[nodiscard]] static bool available();

    // The least free heap a play of any AC-4 stream asks for before a decoder is made, in bytes
    // (the 2.0 fixtures' peak, planning/ac4.md, D14f): a part under it refuses the stream with the
    // figures, where it would otherwise meet an abort() inside a frame.
    [[nodiscard]] static std::size_t heap_floor_bytes();

    // From the stream's start to its end: where the decoder's allocations go
    // (CONFIG_ICLFORGE_AC4_INTERNAL_BELOW, which puts an ESP32-S3's decoder state in PSRAM), set
    // for as long as a stream plays and ESP-IDF's own value put back after it, so that AC-3 and
    // E-AC-3 plays keep theirs. Idempotent.
    void begin_stream();
    void end_stream();

    // Makes the decoder. False, with reason() saying why, when the heap is under the floor or the
    // decoder could not be made. Made once and kept across streams until the object goes.
    [[nodiscard]] bool open(const Config& config);
    [[nodiscard]] bool is_open() const;

    // Back to the state a stream starts in: the next frame waits for an I-frame.
    void reset();
    // The decoder let go, with its state: for a change of what it is made with (the fold, the
    // lane), or before the lane it holds a pointer to goes. open() makes another.
    void close();

    // One sync frame (0xAC40 or 0xAC41, its size, the frame, the CRC word where there is one, and
    // zeros after it that an HBR16 burst pads with) decoded, its blocks handed to `fn` as they are
    // completed.
    [[nodiscard]] Result decode(std::span<const std::byte> sync_frame, BlockFn fn, void* context);

    // The last decode's refusal or error, a string literal; empty after one that decoded.
    [[nodiscard]] std::string_view reason() const;
    // The samples the last decoded frame lasted at the output rate: 2,048 at frame rate 13 and
    // 48 kHz, and the integer sequence of the 29.97, 59.94 and 119.88 fps rates. 0 before one.
    [[nodiscard]] std::size_t frame_samples() const;
    [[nodiscard]] const Report& report() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge
