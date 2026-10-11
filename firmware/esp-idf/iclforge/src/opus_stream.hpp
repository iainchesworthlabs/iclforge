#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

// One Opus decoder for a player@v1 stream: 20 ms packets, 48 kHz, one or two channels, to
// interleaved 16-bit samples. It is libopus 1.6.1 built for fixed point, which is what a part
// without a floating-point unit needs, by CONFIG_ICLFORGE_SENDSPIN_OPUS (opus.cmake). A build
// without that option links opus_stream_none.cpp instead: available() is false and nothing
// opens, so the player lists no Opus and refuses a stream of it.
//
// Measured on an ESP32-C6 (docs/platforms/bare-metal/esp32-c6.md, "Opus from a server"): 7.5 ms
// to decode a 20 ms stereo packet at 128 kbit/s, 27,060 bytes of decoder state, about 16.4 KB of
// the decoding task's stack.

namespace iclforge {

class OpusStream {
   public:
    // The most frames one packet may decode to: 20 ms at 48 kHz, which is what a server sends.
    static constexpr std::size_t kMaxFrames = 960;

    OpusStream();
    ~OpusStream();
    OpusStream(const OpusStream&) = delete;
    OpusStream& operator=(const OpusStream&) = delete;
    OpusStream(OpusStream&&) = delete;
    OpusStream& operator=(OpusStream&&) = delete;

    // Whether this build can decode Opus at all.
    [[nodiscard]] static bool available();

    // Allocates the decoder's state, from internal RAM where there is room: 27 KB for stereo.
    // False when the rate or channel count is not one it takes, or the heap has no room.
    [[nodiscard]] bool open(std::int32_t sample_rate, std::int32_t channels);

    // Back to the state a stream starts in, as after a gap in the packets.
    void reset();

    // One packet to interleaved samples in `out`, which holds at least kMaxFrames frames: the
    // frames decoded, or a negative number when the packet does not decode.
    [[nodiscard]] int decode(std::span<const std::uint8_t> packet, std::span<std::int16_t> out);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge
