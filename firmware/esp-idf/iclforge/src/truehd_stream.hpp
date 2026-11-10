#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "iclforge/ac3/core/eac3_tables.hpp"

// The TrueHD decoder as the Sendspin player's burst path uses it:
// a chunk of one to 24 whole access units (the Sendspin extension's TrueHD data type), each decoded
// to 40, 80 or 160 samples a channel in the order and layout the renderer takes, as AC-3's,
// E-AC-3's and AC-4's decoders hand them over. The units of a run that share a layout and a length
// are handed over together, as one block of up to Config::max_block_samples (6 units at 48 kHz when
// the player's 256-sample block is the bound), because the player's path from the renderer to the
// sink costs a fixed amount a block that a unit of 40 samples does not pay back. It is libs/truehd's decoder, built by
// CONFIG_ICLFORGE_TRUEHD; a build without that option links truehd_stream_none.cpp instead, where
// available() is false and nothing opens, so the player lists no TrueHD and refuses a stream of it
// (the way ac4_stream.hpp does for AC-4).
//
// A unit that gives no samples (a stream joined before its first major sync, or after a unit that
// did not read) is handed over as silence of its length, so the output is as long as the chunks say.
//
// What it does not do: objects. libs/truehd decodes the bed and the board states that it places no
// objects, and a stream's presentation is the highest one the chunks hold (a server cuts the
// substreams of a lower one for a board that states fewer channels).

namespace iclforge {

class TruehdStream {
   public:
    // A run of units' samples, for the player's renderer.
    struct Block {
        // One span a channel, `samples` long, in the order of `bed`; valid for the call.
        std::span<const std::span<const float>> channels;
        // The layout the channels are in, as the renderer's set_bed() takes it.
        iclforge::ac3::eac3::chanmap::Layout bed;
        std::size_t samples = 0;
        // The samples of each unit the block holds, which `samples` is a whole number of: the
        // block-length state of the player's stages (a level ramp, a meter's window) is kept in
        // units, as it was when each unit was a block of its own.
        std::size_t unit_samples = 0;
        // The first unit's number in its chunk, from 0.
        int index = 0;
    };
    using BlockFn = void (*)(void* context, const Block& block);

    enum class Result : std::uint8_t {
        // The chunk's units came out, as blocks (some of them silence, if the stream was joined).
        kDecoded,
        // Nothing yet: no unit of the chunk could be decoded, and none had a length to play as silence.
        kWaiting,
        // Not whole access units, or a unit the decoder refused (reason() says).
        kInvalid,
        // A rate or a set of channels the player cannot place or may not decode (reason() says).
        kRefused,
    };

    struct Config {
        // The most channels a unit may decode to; 0 refuses none.
        std::size_t max_channels = 0;
        // The one rate the player plays; a unit at another is refused.
        std::uint32_t sample_rate_hz = 48000;
        // The most samples a channel of one block handed to the player may hold: rounded down to whole
        // units of the rate, and at least one. 0 is one unit a block, which is what the player did
        // before units were handed over together.
        std::size_t max_block_samples = 0;
        // The most samples a channel the stream converts ahead of handing them over: a chunk's units
        // are decoded and converted into the planes first, and handed over a block at a time after
        // them, so that the decoder's code runs through the chunk before the player's does (kMaxRunSamples
        // for a chunk's worth, 0 for none: a block is converted, handed over, and the next converted).
        // Rounded down to whole units, held to a chunk and to kPlaneBudgetBytes of planes, and not
        // honoured on a part with no PSRAM, where a run is a block.
        std::size_t max_run_samples = 0;
    };

    // What the decoder found in the last unit that decoded.
    struct Report {
        int channels = 0;
        bool lfe = false;
        // The A/52 audio coding mode the front and surround speakers make (1/0, 2/0, 3/0, 2/2 or
        // 3/2).
        int acmod = 0;
    };

    // The most access units a chunk holds (sendspin::kTruehdUnitsPerChunk).
    static constexpr std::size_t kMaxUnitsPerChunk = 24;
    // The most samples a channel of one unit holds (160, at 192 kHz).
    static constexpr std::size_t kMaxUnitSamples = 160;
    // A whole chunk of the longest units: Config::max_run_samples for no limit but the budget.
    static constexpr std::size_t kMaxRunSamples = kMaxUnitsPerChunk * kMaxUnitSamples;
    // The most bytes the planes hold when they are made to hold more than a block.
    static constexpr std::size_t kPlaneBudgetBytes = 64 * 1024;

    TruehdStream();
    ~TruehdStream();
    TruehdStream(const TruehdStream&) = delete;
    TruehdStream& operator=(const TruehdStream&) = delete;
    TruehdStream(TruehdStream&&) = delete;
    TruehdStream& operator=(TruehdStream&&) = delete;

    // Whether this build decodes TrueHD at all (CONFIG_ICLFORGE_TRUEHD).
    [[nodiscard]] static bool available();

    // The least free heap a play of any TrueHD stream asks for before a decoder is made, in bytes
    // (the probe's decoder, 158 KB, and the buffers beside it): a part under it refuses the stream
    // with the figures, where it would otherwise meet an abort() inside a unit.
    [[nodiscard]] static std::size_t heap_floor_bytes();

    // The stack, in bytes, the task that calls decode() needs for it: 60,944 B were used by the
    // probe on the ESP32-C6, -P4 and -S3, and the owner's decision is at least 64 KiB.
    [[nodiscard]] static std::size_t stack_bytes_needed();

    // The samples a channel of this chunk lasts at `sample_rate_hz`: its units counted by their
    // lengths, times the samples one unit has at that rate (40 at 48 and 44.1 kHz, 80 at 96 and 88.2,
    // 160 at 192 and 176.4). 0 for a chunk that is not whole access units. The player weighs a
    // chunk's lateness with it before it decodes.
    [[nodiscard]] static std::size_t chunk_samples(std::span<const std::byte> chunk,
                                                   std::uint32_t sample_rate_hz);

    // Makes the decoder, and the planes a block's samples are converted into (max_channels, or the
    // renderer's most, planes of the block length: PSRAM where the part has it, internal RAM
    // otherwise). False, with reason() saying why, when the heap is under the floor or either
    // could not be made. Made once and kept across streams until close() or the object goes.
    [[nodiscard]] bool open(const Config& config);
    [[nodiscard]] bool is_open() const;

    // Back to the state a stream starts in: the next unit waits for a major sync. Allocates nothing.
    void reset();
    // The decoder let go, with its state. open() makes another.
    void close();

    // One chunk: whole access units in stream order. Each is decoded, and the samples of the units
    // of a run that share a layout and a length are handed to `fn` as one block, in order; a unit
    // that gives nothing is silence of the length and layout of the one before and goes into the
    // run it would have been a block beside. A unit that is refused ends the run first: what comes
    // before it has been handed over when decode() returns.
    [[nodiscard]] Result decode(std::span<const std::byte> chunk, BlockFn fn, void* context);

    // The last decode's refusal or error, a string literal; empty after one that decoded.
    [[nodiscard]] std::string_view reason() const;
    // The samples one unit lasts at the stream's rate (40, 80 or 160; 0 before a unit decoded).
    [[nodiscard]] std::size_t unit_samples() const;
    [[nodiscard]] const Report& report() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge
