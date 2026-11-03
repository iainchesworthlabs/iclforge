#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/sink.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"

// A burst output that decodes what it is sent (planning/hearth-android-phases-4-5.md, phase 4):
// each stream's bursts of AC-3 or E-AC-3 with any object layer, or of AC-4, decoded and rendered to
// the sink's speaker layout by iclforge::render, and the samples handed to a PcmSink. It was the
// test sink's output, which writes a float WAV file and a play-time log; the Android app's is the
// same decode with a sink that plays.
//
// AC-4 (planning/ac4.md, D11): a burst is one AC-4 sync frame (IEC 61937-14 Annex A), zero-padded
// to a whole 8-byte unit in an HBR16 burst, which iclforge::ac4::Decoder decodes to its own
// channels; those are rendered by the bed ac4_bed() makes of the decoder's speakers, in blocks of
// 256 samples, a two-speaker layout included, since the AC-4 decoder has no fold of its own yet. A
// burst's duration is its frame's at the base sampling frequency, which IEC 61937-14 Tables 5 and
// 6 give as the AC-4 data-burst's repetition period. The decoder decodes 48 kHz at
// frame_rate_index 13 only, and refuses the other rates, which count as undecodable.
//
// How it decodes, which a local reference decode repeats to compare: DecoderConfig's defaults in
// line mode, served by render::serve(layout, Lo/Ro, ObjectsPolicy::kAuto) and
// render::configure_decoder, so objects are placed when the layout has heights and a two-speaker
// layout is folded by the decoder. An E-AC-3 stream plays the programme of its first access unit.
// The E-AC-3 decoder releases a unit it holds back (§3.7) during the next unit's call; blocks are
// placed and written in the order they arrive, and a unit still held when the stream ends is not
// written. A burst that does not decode resets the decoders, and the stream carries on from the
// next.
//
// TrueHD (planning/truehd-parity-hearth-player.md, H5): a chunk is one to 24 whole access units,
// which iclforge::truehd::Decoder decodes (all the substreams its presentation `best` takes, strict
// checking) to 24-bit integers, and which are rendered by the bed the stream's channel names make
// (truehd_bed()), a unit at a time (40, 80 or 160 samples). A chunk's duration is its units' at the
// stream's rate. A unit that does not decode, or comes before the stream's first restart header,
// plays as silence for its samples, and the decoder waits for the next restart header on its own;
// the sample count of the output is the chunks' whatever is lost.
//
// Not thread-safe: the sink calls it from the session's callbacks, under the session's lock.

namespace iclforge::sink {

// Where a decoding output's samples go: one stream at a time, as interleaved float frames of the
// layout's slots, with the time each burst is due. The test sink's is a float WAV file and a
// play-time log; the Android app's plays them.
class PcmSink {
   public:
    PcmSink() = default;
    virtual ~PcmSink() = default;
    PcmSink(const PcmSink&) = delete;
    PcmSink& operator=(const PcmSink&) = delete;
    PcmSink(PcmSink&&) = delete;
    PcmSink& operator=(PcmSink&&) = delete;

    // A stream began, or changed in place: `channels` interleaved float samples a frame at
    // `sample_rate` follow. False for a stream this sink cannot take, which the output then does
    // not take either.
    virtual bool open(std::uint32_t sample_rate, std::uint16_t channels) = 0;
    // Whether write() is wanted: false for a sink that only counts, which spares the interleave.
    [[nodiscard]] virtual bool taking() const = 0;
    // A burst is due, to be heard from `local_time_us` on the sink's steady clock. Its first frame
    // is `first_frame` of the stream and it lasts `frames`, the count of the samples the burst
    // stands for, which are made by the write()s that follow it. Called before them.
    virtual void burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t frames) = 0;
    // The next frames of the stream, interleaved.
    virtual void write(std::span<const float> interleaved) = 0;
    // stream/clear: what was buffered is dropped. The stream had made `frames`.
    virtual void clear(std::uint64_t frames) = 0;
    // The stream ended, or another began.
    virtual void close() = 0;
    // The file the current or last stream went to, for the log; empty when it went to none.
    [[nodiscard]] virtual const std::filesystem::path& file() const = 0;
};

// The coded layout an AC-4 decoder's channels make, for the renderer: each speaker at the E-AC-3
// channel map's location of the same place. Ls and Rs stay the surround pair, which a 7.X mode
// keeps at the sides; Lb and Rb, Lw and Rw, and Tfl and Tfr are the rear, wide and front height
// pairs (ETSI TS 103 190-1 clause D.1; A/52 Table E2.5).
[[nodiscard]] ac3::eac3::chanmap::Layout ac4_bed(std::span<const iclforge::ac4::Speaker> speakers);

// Whether the layout renderer can place `speakers` as a bed: it takes sixteen coded channels, and
// Table E2.5 has no location for 22.2's bottom channels or 9.X.4's screen pair. A frame of a wider
// layout, 22.2's, is refused rather than placed on some of its channels.
[[nodiscard]] bool ac4_placeable(std::span<const iclforge::ac4::Speaker> speakers);

// The coded layout a TrueHD stream's channels make, for the renderer: each speaker at the location
// of the same place in Table E2.5. Nothing for no channels, more than the renderer takes, or a
// channel named twice. (Hearth's engine has the same function for its own decode,
// apps/hearth/engine/src/stream_decoder.hpp: it does not link this library.)
[[nodiscard]] std::optional<ac3::eac3::chanmap::Layout> truehd_bed(
    std::span<const iclforge::truehd::Channel> channels);

class DecodingBurstOutput : public BurstStreamOutput {
   public:
    // Decodes to `layout`'s slots and hands the samples to `pcm`, which the output owns.
    DecodingBurstOutput(const render::OutputLayout& layout, std::unique_ptr<PcmSink> pcm);
    ~DecodingBurstOutput() override;
    DecodingBurstOutput(const DecodingBurstOutput&) = delete;
    DecodingBurstOutput& operator=(const DecodingBurstOutput&) = delete;
    DecodingBurstOutput(DecodingBurstOutput&&) = delete;
    DecodingBurstOutput& operator=(DecodingBurstOutput&&) = delete;

    // Annex C's reproduction of a karaoke AC-3 stream, and which vocals the capable one plays, as
    // a settings command's decoder object says; the decoders are made again with them, at the
    // next burst. A karaoke stream is laid out from ac3::output_mode(), as a board does, since
    // the reproduced frame is L, C, R and the LFE and not the coded layout.
    // `levels_db` are the melody's, vocal 1's and vocal 2's, in dB against the table: absent
    // keys of the command are 0, the table's.
    void set_karaoke(sendspin::player::Karaoke reproduction, sendspin::player::KaraokeVocals vocals,
                     const std::array<double, 3>& levels_db = {0.0, 0.0, 0.0}) override;

    // A stream began, or changed in place: new decoders, and the sink opened again.
    bool start(const sendspin::player::StreamStart& stream) override;
    void clear() override;
    void end() override;
    // One burst to be played from `local_time`.
    void write(const sendspin::BurstChunk& chunk, std::int64_t local_time) override;

    [[nodiscard]] std::uint64_t bursts() const { return bursts_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    [[nodiscard]] std::uint64_t undecodable() const { return undecodable_; }
    [[nodiscard]] std::uint32_t streams() const { return streams_; }
    // The file the current or last stream went to; empty when the sink has none.
    [[nodiscard]] const std::filesystem::path& file() const override { return pcm_->file(); }
    [[nodiscard]] OutputCounts counts() const override { return {streams_, bursts_, frames_}; }
    // What the decoder found in the current stream, once a unit of it has decoded; updated once
    // more by the first unit to carry objects, if the first did not.
    [[nodiscard]] const std::optional<sendspin::player::DecoderReport>& decoder() const override {
        return decoder_;
    }
    // This output decodes what it is sent and plays the samples.
    [[nodiscard]] std::optional<sendspin::player::Output> output() const override {
        return sendspin::player::Output::kPcm;
    }

   private:
    void reset_decoding();
    void decode_unit(std::span<const std::byte> unit);
    void place(const ac3::PcmBlock& block);
    void write_ac4(const sendspin::BurstChunk& chunk, std::int64_t local_time);
    void write_truehd(const sendspin::BurstChunk& chunk, std::int64_t local_time);
    void decode_truehd_unit(std::span<const std::byte> unit, std::size_t samples);
    void place_truehd(const iclforge::truehd::DecodedAccessUnit& decoded);
    // `samples` frames of silence on every slot, in blocks.
    void emit_silence(std::size_t samples);
    void render_ac4(const iclforge::ac4::DecodedFrame& frame);
    // Hands the first `n` samples of every slot of block_ to the sink.
    void emit(std::size_t n);

    std::unique_ptr<PcmSink> pcm_;
    render::OutputLayout layout_;
    ac3::render::Serving serving_;
    ac3::DecoderConfig config_;
    render::LayoutRenderer renderer_;
    std::optional<sendspin::player::StreamStart> stream_;
    std::optional<ac3::FrameDecoder> ac3_decoder_;
    std::optional<ac3::Eac3Decoder> eac3_decoder_;
    std::optional<iclforge::ac4::Decoder> ac4_decoder_;
    std::optional<iclforge::truehd::Decoder> truehd_decoder_;
    std::vector<float> truehd_planar_;
    std::vector<std::span<const float>> truehd_spans_;
    // One span per decoded AC-4 channel, a block's worth of it at a time.
    std::vector<std::span<const float>> ac4_block_;
    std::optional<int> programme_;
    // The bed each unit given to the decoder is placed by, oldest first: a unit's first block takes
    // the oldest, whichever call delivers it.
    std::deque<ac3::eac3::chanmap::Layout> beds_;
    std::optional<ac3::eac3::chanmap::Layout> renderer_bed_;
    std::optional<sendspin::player::DecoderReport> decoder_;

    std::array<std::array<float, ac3::kSamplesPerBlock>, render::OutputLayout::kMaxSlots> block_{};
    std::vector<float> interleaved_;
    std::uint64_t bursts_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t undecodable_ = 0;
    std::uint64_t stream_frames_ = 0;
    std::uint32_t streams_ = 0;
};

}  // namespace iclforge::sink
