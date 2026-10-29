#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/export.hpp"
#include "iclforge/mlp/io/extra_data.hpp"

// The TrueHD (MLP) encoder: PCM in, an elementary stream of access units out, always lossless
// (truehd-spec.md section 19). The decoder reproduces every input sample exactly; with `verify`
// on (the default) the encoder proves that for every access unit it writes by decoding it, and
// fails with kVerificationFailed rather than emit one that does not.
//
// Per restart interval it chooses, from the signal:
//   - the output shift that carries the source's trailing zeros (a 16-bit source in 24-bit words
//     codes 16-bit samples);
//   - the presentations' downmix matrices, which make the 2-channel presentation of a 5.1 or 7.1
//     stream a fold-down of all its channels and the 6-channel presentation of a 7.1 stream a
//     fold-down of its rear pairs, each limited so that no presentation clips (10.5);
//   - inter-channel decorrelation: a primitive matrix per channel whose least-squares prediction
//     from the channels before it saves bits, within each substream's matrix budget (10.3);
//   - per channel and access unit, the prediction filter: FIR orders 1 to 8 by Levinson-Durbin on
//     the access unit and on the whole interval, FIR + IIR pairs by two-stage least squares, and
//     the filter already in place, each costed exactly with the decoder's arithmetic (9.7's
//     filter pool and order selection);
//   - per channel and access unit, huff_type, huff_lsbs and huff_offset, by exact bit count;
// and then schedules the access units' input timing against peak_data_rate through the decoder's
// FIFO (18), failing with kRateExceeded when the lossless stream cannot be delivered at that rate.
//
// Channel layouts: 1 to 8 channels in the order of section 13.1's assignment (2: L R; 6: L R C
// LFE Ls Rs; 8: L R C LFE Ls Rs Lrs Rrs). Two channels make one substream; six make two (the
// 2-channel and 6-channel presentations); eight make three (2, 6 and 8). Up to 16 channels make a
// fourth, 16-channel substream (0x31EC), the Atmos presentation, whose first eight channels are
// the 7.1 bed the lower presentations are built from (iclforge/mlp/encoder/atmos.hpp).
//
// Samples are integers at 24-bit scale (a 16-bit source shifted left by 8), interleaved.

namespace iclforge::mlp {

struct EncoderConfig {
    std::uint32_t sample_rate = 48000;  // 44.1, 48, 88.2, 96, 176.4 or 192 kHz
    int channels = 2;                   // 1 to 8, or up to 16 with a 16-channel substream
    // Section 13.1's assignment of the top channel-based presentation; 0 picks the usual one for
    // the channel count (stereo, 5.1, 7.1).
    std::uint16_t channel_assignment = 0;
    // Build the 2-channel (and for 7.1 the 6-channel) presentation as a fold-down of the full
    // mix. Off: each lower presentation is the first channels of the one above, as FFmpeg's
    // encoder writes them.
    bool downmix = true;
    // Access units between restart headers (7.5: 8 to 128). Each restart carries a major sync.
    int restart_interval = 16;
    // The peak data rate the stream must be deliverable at, in bits per second (18 Mbps is the
    // Blu-ray ceiling, 21.6).
    std::uint64_t peak_data_rate = 18'000'000;
    int max_fir_order = 8;  // 0 to 8
    int max_iir_order = 4;  // 0 to 4; filters A and B together at most 8
    bool decorrelate = true;
    bool verify = true;
    // Channel meaning (14.1) written into every major sync.
    ChannelMeaning channel_meaning;
    // The 16-channel presentation's description, for a 16-channel substream.
    std::optional<ExtraChannelMeaning> extra_channel_meaning;
    // A content signature payload per access unit (io/extra_data.hpp); not used when empty.
    SignatureHook signature_hook;
};

struct EncoderStats {
    std::uint64_t access_units = 0;
    std::uint64_t bytes = 0;
    std::uint64_t restarts = 0;
    // The highest rate any access unit is delivered at under the FIFO schedule (18), and the
    // mean rate, in bits per second.
    std::uint64_t peak_bits_per_second = 0;
    std::uint64_t mean_bits_per_second = 0;
    // How the per-channel filters were chosen, over every channel of every access unit.
    std::uint64_t fir_blocks = 0;
    std::uint64_t iir_blocks = 0;
    std::uint64_t unfiltered_blocks = 0;
    std::uint64_t filter_changes = 0;
    std::uint64_t decorrelation_matrices = 0;  // over every restart interval
    std::uint64_t limited_downmixes = 0;       // intervals whose downmix the limiter reduced
};

class ICLFORGE_MLP_EXPORT Encoder {
   public:
    [[nodiscard]] static std::expected<Encoder, Error> create(EncoderConfig config);
    ~Encoder();
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Takes interleaved samples (a whole number of frames) and returns the access units completed
    // so far, as bytes. A restart interval is coded once all of it has arrived, so output lags
    // input by up to restart_interval access units.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> encode(
        std::span<const std::int32_t> interleaved);

    // Evolution payloads (an Atmos frame's object audio metadata, say) for the access unit that
    // holds the next sample given to encode(). Several calls before one sample add up.
    void attach(EvolutionPayload payload);

    // A 16-channel stream's object render (the Atmos path, encoder/atmos.hpp): from the access
    // unit that holds the next sample given to encode(), object o (channel 8 + o) folds into bed
    // channel c (0 to 7, 13.1's 7.1 order) of the 8-channel presentation with gain gains[o][c],
    // and the 16-channel substream's matrices undo that fold exactly. Before the first call no
    // object is folded in. Gains above 1 in magnitude are not representable (10.2) and fail the
    // next interval with kInvalidConfiguration.
    void render_objects(std::span<const std::array<double, 8>> gains);

    // Codes what is left, padding the last access unit and marking the padding in its
    // termination word (P7), and returns those access units.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> flush();

    [[nodiscard]] const EncoderStats& stats() const noexcept;
    [[nodiscard]] const EncoderConfig& config() const noexcept;

   private:
    struct State;
    explicit Encoder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// A whole stream in one call.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<std::vector<std::byte>, Error> encode_stream(
    std::span<const std::int32_t> interleaved, const EncoderConfig& config,
    EncoderStats* stats = nullptr);

}  // namespace iclforge::mlp
