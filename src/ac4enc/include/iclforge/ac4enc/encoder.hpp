#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac4/toc.hpp"
#include "iclforge/ac4enc/config.hpp"
#include "iclforge/ac4enc/export.hpp"

// An AC-4 encoder: ETSI TS 103 190-1 V1.4.1 (2025-07), "Part 1: Channel based
// coding", and ETSI TS 103 190-2 V1.3.1 (2025-07), "Part 2: Immersive and
// personalized audio", written from the published texts. Clause numbers below
// name the part that defines the element.
//
// What it writes: mono, stereo, 5.0, 5.1, 5.0.4 or 5.1.4 PCM at 48 kHz, at every
// frame rate of Part 1 Table 83, or at 44.1 kHz in frames of 2 048 samples
// (frame_rate_index 13, the one Table 84 has), as one presentation of one
// channel-coded substream, or as several channel-coded substreams, each in a
// substream group of its own, and the presentations of Part 2 Table 53 made of
// them (SubstreamConfig, PresentationConfig), at a constant, average or
// variable bit rate (RateMode). At every frame rate but index 13's the input is
// converted to the rate the frames are coded at, the inverse of the
// decoder's conversion (Tables 83 and 84's resampling ratio). The codec
// mode is SIMPLE, the audio spectral frontend with block switching and
// MDCT-domain stereo processing for each channel pair; or ASPX, which codes
// the spectral frontend up to a crossover and recreates the band above it
// with A-SPX, with companding at the lower rates in mono and stereo; or, in
// 5.0 and 5.1, ASPX_ACPL_2 or ASPX_ACPL_3, which code a downmix in the ASPX
// way and rebuild the channels from it with A-CPL. 5.0 and 5.1 take the 5.X
// element in the form DEE's streams have it (coding_config 0 and 2ch_mode 0:
// L and R as a pair, Ls and Rs as a pair, C alone, and the LFE); its other
// coding configurations, 7.0 and 7.1 in the 7.X element, ASPX_ACPL_1, and
// A-CPL in stereo are experimental. 5.0.4 and 5.1.4 take Part 2's immersive
// element as DEE's streams have it: the 7.0.4 or 7.1.4 channel mode with the
// back pair absent, in SCPL, ASPX_SCPL or ASPX_ACPL_2 by the rate, with
// core_5ch_grouping 0 and 2ch_mode 0; 7.0.4 and 7.1.4 with the back pair,
// ASPX_ACPL_1 and ASPX_AJCC there are experimental. The table of contents is bitstream
// version 2 with presentation version 1, every presentation that carries
// audio with a presentation_id and the least md_compat its tracks need (Part
// 2 Table 55; configuration 6's EMDF payloads alone have neither),
// and each presentation substream carries the dialogue normalisation it is
// given and, as configured, further loudness values, DRC's decoder modes, the
// stereo downmix's values, the substream groups' gains and the associated
// audio's mixing values; each audio substream's metadata() carries its
// dialogue enhancement's parameters and a dialogue substream's mixing values.
// The table of contents keeps CMAF's rules (Part 2 Annex H.1.2): at most 64
// presentations, each that carries audio with a presentation_id of its own,
// and one configuration throughout. A configuration 6 presentation has no
// field for one, so a CMAF track cannot carry a stream that has it
// (iclforge::ac4::cmaf_refusal()), where an MP4 can. With experimental.objects, a
// substream of objects and their metadata (ObjectsConfig): an A-JOC substream,
// a downmix in the ASF and A-SPX tools and the parameters that rebuild the
// objects from it (Part 2 clause 5.7), or direct-coded object substreams in
// Part 1's elements, with object audio metadata either way (Part 2 clause
// 6.2.8). Every configuration outside the rules this header states is
// refused;
// Encoder::refusal_reason() names the rule a configuration breaks. Each frame
// comes out as a raw_ac4_frame, which an MP4 sample holds as it is
// (iclforge::ac4::build_dac4() describes the track from toc()), and which sync_frame()
// wraps for a raw .ac4 file or MPEG-2 TS.
//
// Every field of the configuration structures has a default, so a designated
// initializer names only the fields it sets.
//
// src/ac4enc/ERRATA.md records the readings the writer alone needs; where the
// decoder depends on the same reading, src/ac4dec/ERRATA.md has it.

namespace iclforge::ac4 {

enum class EncodeError : std::uint8_t {
    kInvalidConfig,  // a configuration the encoder does not write: Encoder::refusal_reason() says why
    kInvalidInput,   // a channel count or lengths that do not match, or a sample that is not finite
};

[[nodiscard]] ICLFORGE_AC4ENC_EXPORT std::string_view describe(EncodeError error);

// One coded frame: what an MP4 sample holds as it is, and what sync_frame()
// wraps for a raw .ac4 file or MPEG-2 TS.
struct EncodedFrame {
    std::vector<std::byte> raw_ac4_frame{};
    // PCM samples per channel the frame decodes to, at the input's rate: the
    // frame's length at index 13, and elsewhere what the decoder's converter
    // gives the frame, which at 29.97, 59.94 and 119.88 fps changes from
    // frame to frame in a cycle of five (Part 2 clause 5.11): 1 601, 1 602,
    // 1 601, 1 602 and 1 602 at 29.97.
    int samples = 0;
    bool iframe = false;   // b_iframe_global
};

class ICLFORGE_AC4ENC_EXPORT Encoder {
   public:
    // Fails with EncodeError::kInvalidConfig for a configuration outside what
    // the encoder writes (the rules this header states), or whose rate cannot
    // hold its least frame.
    [[nodiscard]] static std::expected<Encoder, EncodeError> create(const EncoderConfig& config);

    // Why create() refuses `config`: a string literal naming the first rule it
    // breaks, such as "a language tag longer than 63 bytes"; empty where
    // create() makes an encoder of it. It does create()'s work to find out.
    [[nodiscard]] static std::string_view refusal_reason(const EncoderConfig& config);

    ~Encoder();
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Planar samples at full scale 1.0, one span per input channel, all the
    // same length, any length. Returns the frames this input completes, in
    // order; the encoder's delay holds back the frames the last input still
    // needs.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels);
    // With DialogueSource::kStem: the programme and, sample for sample, the
    // dialogue in it, in the programme's channels (in every input channel,
    // with several substreams, a substream without a stem ignoring its own).
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels,
        std::span<const std::span<const float>> dialogue);
    // With an object substream: the objects' PCM and the changes to their
    // metadata within it or after it, in any order. An update for an object
    // the substream lacks, before this input's first sample, or with a
    // property off its range is EncodeError::kInvalidInput.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels,
        std::span<const ObjectMetadataUpdate> updates);

    // Ends the stream: pads the input with silence to the end of its last
    // frame and returns the frames the delay still held, so that a decoder's
    // output covers every input sample. The encoder takes no input after it.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> flush();

    // The table of contents every frame carries. sequence_counter and
    // b_iframe_global change from frame to frame, and substream_sizes with each
    // frame's content; the rest is fixed for the stream, which is what
    // iclforge::ac4::build_dac4() and iclforge::ac4::rfc6381_codec_string() read.
    [[nodiscard]] const Toc& toc() const noexcept;

    // The codec mode the stream is coded in: what kAuto chose from the rate,
    // never kAuto.
    [[nodiscard]] CodecMode codec_mode() const noexcept;

    // Samples of silence the encoder puts before the input, at the input's
    // rate: a frame and a half, 3 072 samples at frame_rate_index 13. At the
    // other frame rates, a frame and a half at the internal rate and the
    // converter's delay, to the nearest sample: the delay itself is a
    // fraction of a sample off it. An input sample at index n is at index n +
    // delay_samples() of the decoded output before the decoder's own delay is
    // added.
    [[nodiscard]] int delay_samples() const noexcept;

    // The delay iclforge::ac4::Decoder adds, at the input's rate: at frame_rate_index
    // 13, 1 313 samples (Part 1 Table 188's d_pcm, 352, the QMF banks' 577 and
    // six QMF slots); at the other frame rates, the same at the internal rate
    // and the decoder's converter's delay, to the nearest sample. An input
    // sample at index n is at index n + delay_samples() +
    // decoder_delay_samples() of iclforge::ac4::Decoder's output, to within a sample.
    [[nodiscard]] int decoder_delay_samples() const noexcept;

   private:
    struct Impl;
    explicit Encoder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Part 2 Annex G.3.1's ac4_syncframe(): the sync word 0xAC40, or 0xAC41 and a
// trailing crc_word (Annex G.4.2) when `crc` is set, then frame_size and the
// raw frame.
[[nodiscard]] ICLFORGE_AC4ENC_EXPORT std::vector<std::byte> sync_frame(std::span<const std::byte> raw_ac4_frame,
                                                              bool crc);

}  // namespace iclforge::ac4
