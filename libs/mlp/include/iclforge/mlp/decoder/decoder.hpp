#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/export.hpp"
#include "iclforge/mlp/io/extra_data.hpp"

// The TrueHD (MLP) decoder: one access unit in, the PCM of one presentation out, exactly the
// samples the encoder was given (truehd-spec.md section 20).
//
// What it does: the major sync and the substream directory; for each substream the presentation
// needs, the restart headers, the block headers, Huffman and LSB sample decode, FIR and IIR
// inverse prediction, the primitive matrices, the output shifts and the channel assignment; the
// checks of Appendix E as typed errors (the check nibble, every CRC and parity, the lossless
// check of each restart interval, every range the syntax allows); random access from any major
// sync (20.3); the DRC metadata the stream carries, exposed and not applied; and the extra data's
// evolution payloads (Atmos object audio metadata, a content signature) passed through.
//
// What it refuses, by name: a DVD-Audio MLP stream (0xF8726FBB, kUnsupportedStreamType), whose
// format_info is laid out for two sample-rate groups; and a primitive matrix that takes one of
// the two noise channels (kNoiseChannel), since section 17 gives no generator for the noise and a
// decode that guessed it could not be lossless.
//
// Presentations (2.2, 20.2): substream 0 alone gives the 2-channel presentation, substreams 0 and
// 1 the 6-channel one, 0 to 2 the 8-channel one and 0 to 3 the 16-channel (Atmos) one. A stream
// with fewer substreams has fewer presentations; the decoder gives the richest one at or below
// what the configuration asks for. Each substream's presentation is its matrices applied to the
// channels of every substream up to it (planning/truehd.md, P29).
//
// Samples are integers at 24-bit scale, as a 24-bit PCM file holds them: a 16-bit source's
// samples come back shifted left by 8. A 16-channel substream (0x31EC) may carry wider samples;
// those come back as decoded.

namespace iclforge::mlp {

struct DecoderConfig {
    // The presentation to decode, by its channel count: 2, 6, 8 or 16, or 0 for the richest the
    // stream has.
    int presentation_channels = 0;
    // Appendix E's lossless check of each restart interval. Off only for a caller that wants the
    // samples of a stream it knows to be damaged.
    bool verify_lossless_check = true;
    // The major sync, substream and extra data checks (CRCs and parity).
    bool verify_checks = true;
};

// The DRC metadata of section 12 the stream carries for the decoded presentation: the startup
// gains of the channel meaning and the latest update from the substream directory. Not applied.
struct DrcMetadata {
    std::int8_t drc_start_up_gain = 0;
    std::int8_t heavy_drc_start_up_gain = 0;
    std::optional<std::int16_t> drc_gain_update;
    std::uint8_t drc_time_update = 0;
};

struct DecodedAccessUnit {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int samples = 0;  // per channel, padding dropped
    // Samples at the end of the access unit that its termination word marks as padding (P7),
    // dropped from `pcm`: a stream's last access unit holds the rest of a source that is not a
    // whole number of access units long.
    int padding = 0;
    std::vector<std::int32_t> pcm;  // interleaved, channels * samples
    std::uint16_t input_timing = 0;
    bool major_sync = false;  // this access unit carried a major sync
    bool restart = false;     // the presentation's substream restarted in this unit
    int substreams_decoded = 0;
    DrcMetadata drc;
    std::vector<EvolutionPayload> evolution;  // the extra data's payloads, in order
    // Set when the access unit had extra data this library could not read (a Dolby encoder's
    // layout, say); the audio is decoded regardless.
    std::optional<Error> extra_data_error;
};

class ICLFORGE_MLP_EXPORT Decoder {
   public:
    explicit Decoder(DecoderConfig config = {});
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Decodes one access unit (as io/elementary.hpp's scan() frames them). Before the first major
    // sync, and after an error until the next major sync, an access unit decodes to kNoMajorSync.
    [[nodiscard]] std::expected<DecodedAccessUnit, Error> decode(
        std::span<const std::byte> access_unit);

    // Forgets every substream's state, as a seek does; decoding resumes at the next major sync.
    void reset();

    // The stream's last major sync, once one has been seen.
    [[nodiscard]] const std::optional<MajorSync>& major_sync() const noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};

// A whole elementary stream decoded to interleaved PCM: every access unit in order, stopping at
// the first error. `info` receives the stream's first major sync.
struct DecodedStream {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    std::vector<std::int32_t> pcm;
    std::size_t access_units = 0;
    std::optional<MajorSync> major_sync;
    std::vector<std::vector<EvolutionPayload>> evolution;  // per access unit
};
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<DecodedStream, Error> decode_stream(
    std::span<const std::byte> stream, DecoderConfig config = {});

}  // namespace iclforge::mlp
