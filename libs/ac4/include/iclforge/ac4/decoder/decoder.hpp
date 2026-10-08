#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/ac4/decoder/config.hpp"
#include "iclforge/ac4/export.hpp"
#include "iclforge/ac4/decoder/frame.hpp"
#include "iclforge/ac4/decoder/presentation.hpp"

// An AC-4 decoder: ETSI TS 103 190-1 V1.4.1 (2025-07), "Part 1: Channel based
// coding", and ETSI TS 103 190-2 V1.3.1 (2025-07), "Part 2: Immersive and
// personalized audio", written from the published texts. Clause numbers
// below name the part that defines the element; Part 2 clause 6 amends Part 1
// clause 4 for bitstream_version 2, which is what every stream this project
// has seen uses.
//
// What this version does: it reads every syntax element of a raw AC-4
// frame's substreams - the presentation substream, channel-coded audio
// substreams in the Part 1 channel elements (their HSF extension substreams,
// ac4_hsf_ext_substream(), included), and EMDF payload substreams - and
// reports what a frame carries. It decodes to PCM the mono, stereo, 3.0, 5.X
// and 7.X channel elements in every codec mode Part 1 gives them (SIMPLE,
// ASPX and the A-CPL modes), at every frame rate of Part 1 Tables 83 and 84:
// the audio spectral frontend, stereo and multichannel processing, the
// inverse transform with block switching, frame alignment, the QMF domain's
// companding, A-SPX and A-CPL (Part 1 clauses 5.1, 5.3, 5.5, 5.6 and 5.7),
// and at every frame_rate_index but 13 the sample rate converter from the
// internal rate to 48 kHz (clause 6.2.15), its phase locked to
// sequence_counter (Part 2 clause 5.11). It decodes the immersive element of
// the 7.X.4 and 9.X.4 channel modes (Part 2 clause 6.2.4, b_5fronts for the
// 9.X.4 modes) in every codec mode, in full or core decoding (DecodingMode),
// with Part 2's stereo and multichannel processing, S-CPL, A-SPX, A-CPL and
// A-JCC (clauses 5.2 to 5.6), and renders it by Part 2's channel renderer
// (clause 5.10.2, DownmixTarget; a 9.X layout is no target, Tables 35 to 37
// being left out). It decodes
// the 22.2 channel element (Part 2 clause 6.2.4.3) in full decoding, SIMPLE and
// ASPX, to its 24 channels in the order of Part 2 Table A.27's speaker indices:
// no renderer or downmix has a 22.2 input (Tables 35 to 43), so it is delivered
// as coded alone. It decodes the presentation a system chooses (Part 2 clause
// 4.8.2) with all its substreams: music and effects with dialogue, main audio
// with associated audio, both, and a main substream with the dialogue
// enhancement substream the hybrid dialogue enhancement methods take (Part 1
// clauses 5.7.8.9 and 6.2.16, Part 2 clauses 4.8.3.17 to 4.8.4). It decodes
// object audio (Part 2 clauses 4.8.3.4, 4.8.3.13 and 4.8.3.19): A-JOC
// substreams in full and core decoding (clause 5.7), with A-JOC's dialogue
// enhancement (5.8.2.3 and 5.8.2.4), and direct-coded object substreams with
// theirs (5.8.2.5), to each object's PCM and the properties its object audio
// metadata sets (clause 6.3.9, Annex F), for the application to render
// (DecodedFrame::objects); it renders an intermediate spatial format itself
// (clause 5.10.3). The table of contents and the substream framing come from
// iclforge::ac4::parse_raw_frame (the inspector, libs/ac4); this library starts
// where the inspector stops.
//
// A presentation in the efficient high frame rate mode (Part 2 clause 5.1.3)
// spreads one codec frame over frame_rate_fraction (2 or 4) transmission
// frames. The decoder holds their fragments (Figure 8's FIFO) and decodes the
// unit when its last frame arrives: decode() returns no frame for the others,
// parse() a report without substreams, and the frame decode() returns is at
// the audio frame rate of Table 18, with the unit's first sequence_counter
// divided by the fraction as its own (ERRATA.md).
//
// It decodes the speech spectral frontend (Part 1 clause 5.2) for the tracks
// that select it, from the text alone: no stream here uses the tool, and
// ERRATA.md gives the readings the text's defects needed.
//
// What it refuses, with DecodeError::kUnsupported and a reason: core decoding
// of the 22.2 channel element (Table 8 supports full decoding alone) and its
// rendering to any DownmixTarget but kAsCoded, an intermediate spatial format
// mixed into channels Annex A.2.1 has no matrix for, a 96/192 kHz substream
// whose HSF extension substream could not be resolved and read alongside it
// (it is refused rather than decoded at the base rate), and a substream no
// element of the table of contents this decoder reads names (an HSF extension
// substream no ac4_hsf_ext_substream_info() names among them). Refusing is per
// substream and per frame; the next frame is attempted afresh.
//
// At 96 and 192 kHz decode() turns a SIMPLE-mode substream with its HSF
// extension into PCM (Part 1 clauses 4.2.4.3, 5.4 and 6.2.5.2; ERRATA.md,
// "96 and 192 kHz"). There it refuses, the same way, A-SPX and A-CPL, the
// speech spectral frontend, the immersive and 22.2 elements, object audio, the
// mixing of a presentation's substreams, dialogue enhancement where the stream
// sends it and a gain is asked for, and DRC's compression curve and
// transmitted gains.
//
// ERRATA.md beside this library records where the two standards are
// ambiguous or defective and the reading taken for each.

namespace iclforge::ac4 {

// One decoder per stream: configuration sent only in I-frames (A-SPX, A-CPL,
// DRC, dialogue enhancement) persists from one frame to the next, until a
// sequence_counter that does not continue the stream marks a change of
// source (Part 1 clause 4.3.3.2.2), which forgets it, so that frames wait for
// the new source's first I-frame. decode()'s signal carries on across the
// change: the old source's audio still in the decoder comes out to its end,
// overlapping the new source's first frame, which makes a splice or a switch
// of streams at an I-frame seamless (Part 1 clause 6.2.19). A frame that
// returns nothing while it waits drops that signal, so that the first frame
// decoded after the wait starts from silence. A frame whose table of contents
// does not read is taken to be the frame the stream expected, so one damaged
// frame is not a change of source.
//
// A system changes the output processing and the presentation while a stream
// plays with set_output() and set_presentation(), which keep everything the
// decoder has read: a new decoder waits for an I-frame.
class ICLFORGE_AC4_EXPORT Decoder {
   public:
    Decoder();
    explicit Decoder(const DecoderConfig& config);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Reads one raw_ac4_frame - an iclforge::ac4::SyncFrame's raw_ac4_frame, or an MP4
    // sample. An error in one substream is recorded in that substream's
    // report and the others are still read; the frame itself fails only
    // when its table of contents does. Updates presentations() and
    // metadata() as decode() does.
    [[nodiscard]] std::expected<FrameReport, DecodeError> parse(
        std::span<const std::byte> raw_ac4_frame);

    // Reads one raw_ac4_frame as parse() does and decodes the presentation
    // select_presentation() gives for DecoderConfig::presentation: each of its
    // substreams, mixed into the channels of its main or music and effects
    // substream. Nothing for a frame that has no output: one whose substreams
    // need configuration no I-frame has sent yet. The error, when there is
    // one, is a substream's (or the table of contents'), and refusal_reason()
    // says why. Under a concealment policy, a concealed frame in place of
    // either, once a frame has decoded.
    [[nodiscard]] std::expected<std::optional<DecodedFrame>, DecodeError> decode(
        std::span<const std::byte> raw_ac4_frame);

    // decode(), with the output handed to `sink` in blocks of kBlockSamples as
    // it completes them, the samples left over held for the next frame. The
    // decoder keeps the frame's storage, so a stream decoded this way
    // allocates nothing per frame once its layout is set. A change of layout
    // or rate first hands over what is held, as a shorter block. It hands
    // over channels alone: a presentation's objects come from decode().
    [[nodiscard]] std::expected<std::optional<FrameInfo>, DecodeError> decode_by_block(
        std::span<const std::byte> raw_ac4_frame, BlockSink sink);

    // The samples decode_by_block() holds back, handed to `sink` as one
    // shorter block, at the end of a stream; returns how many there were.
    std::size_t flush(BlockSink sink);

    // Why the last decode() failed, returned nothing or returned a concealed
    // frame, a string literal; empty after a decode() that decoded its frame.
    [[nodiscard]] std::string_view refusal_reason() const noexcept;

    // The output processing, from the next frame. The stages take the new
    // values as they take the stream's own from one frame to the next; a new
    // layout starts its channels' synthesis from silence.
    void set_output(const OutputConfig& output);
    [[nodiscard]] const OutputConfig& output() const noexcept;

    // The presentation choice, from the next frame. A presentation's
    // substreams are read in every frame whichever is decoded, so a newly
    // chosen one needs no I-frame; its signal starts from silence.
    void set_presentation(const PresentationChoice& choice);

    // The presentations of the last frame read, in its table of contents'
    // order; empty before one.
    [[nodiscard]] std::span<const PresentationInfo> presentations() const;

    // The metadata of the presentation the last frame selected.
    [[nodiscard]] const PresentationMetadata& metadata() const;

    // The decoder's delay at the output rate for the stream as last decoded:
    // 1 313 samples at frame_rate_index 13, and at the other indices the same
    // at the internal rate and the converter's delay, to the nearest sample; 0
    // before a frame has decoded. At 96 and 192 kHz the frame alignment's d_pcm
    // times 2 or 4 (Part 1 clause 5.6), with the converter's where there is one.
    // decode_by_block() holds back up to kBlockSamples - 1 samples more.
    [[nodiscard]] int latency_samples() const noexcept;

    // Forgets everything carried between frames, the samples decode_by_block()
    // holds back included.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::ac4
