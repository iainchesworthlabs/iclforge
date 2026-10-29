#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// A TrueHD / MLP lossless encoder, written from the owner's Dolby TrueHD (MLP) Reference
// Specification (v3.3, "TH spec") and held to two decoders: this library's and FFmpeg's strict
// decode must both give back the PCM that went in, sample for sample. libs/truehd/ERRATA.md
// records every place the specification and the streams disagree.
//
// What it writes: 1 to 8 channels of 16, 20 or 24 bit PCM at 44.1, 48, 88.2, 96, 176.4 or 192 kHz
// as a TrueHD stream (0xF8726FBA). A stream is cut into restart intervals (TH spec §7.5), each of
// which starts with a major sync and a restart header in every substream and is coded as a
// whole, so the encoder holds one interval of input (128 access units by default, 107 ms at 48
// kHz) before it emits it. In each interval the encoder
//   - finds the low bits every sample leaves clear (a 16 bit source in a 24 bit container) and
//     sends them as the output shift and the quantiser step (TH spec §8.2),
//   - removes the correlation between neighbouring channel pairs with a primitive matrix when
//     that makes the stream smaller (TH spec §10),
//   - predicts each channel with a FIR filter found by windowed autocorrelation and the
//     Levinson-Durbin recursion, quantised to the widths of TH spec §9.3, and
//   - codes each block's residual with one of the three codebooks of TH spec App. C or raw, the
//     one a bit count says is shortest, with the offset and the raw width to match (TH spec §11).
// Channels are spread over substreams the way FFmpeg's TrueHD encoder spreads them: the first two
// in substream 0, the next four in substream 1, the last two in substream 2 (ERRATA.md, entry 31),
// or, with a downmix presentation requested, as the 2 channel, 6 channel and 8 channel
// presentations of TH spec §2.2 whose lower presentations are downmixes that the later
// substreams' matrices undo exactly.
//
// What it does not write: noise generation (the generator is not defined, ERRATA.md open
// question 5), the Atmos substream and its evolution payload (phase T2b; this class only frames a
// payload it is handed, see EncoderConfig::extra_data), and MLP (0xF8726FBB). The block data CRC
// is not written either, because the specification does not say what it covers (ERRATA.md entry
// 21).
//
// Tools no external decoder exercises (FFmpeg's encoder never writes them) are behind
// ExperimentalTools and off by default.

namespace iclforge::truehd {

enum class EncodeErrorKind : std::uint8_t {
    kInvalidConfig,  // a configuration the encoder does not write
    kBadInput,       // PCM that is not whole frames or has a sample outside the declared width
    kUnsupported,    // asked for something this version does not write (detail names it)
    kRateExceeded,   // the stream cannot be sent within the peak data rate and the 75 ms latency
    kVerifyFailed,   // EncoderConfig::verify: the decoder did not give back the input
    kInternal,       // a bug: the encoder produced a stream its own syntax layer refuses
};

struct EncodeError {
    EncodeErrorKind kind = EncodeErrorKind::kInternal;
    // A sentence for a log: a string literal, so it needs no storage and no lifetime.
    std::string_view detail;
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view to_string(EncodeErrorKind kind) noexcept;

// How the lower presentations of a multichannel stream are made (TH spec §2.2, §10.5).
enum class DownmixMode : std::uint8_t {
    // Substream 0 carries the first two channels as they are, substream 1 the next four, substream
    // 2 the last two. A 2 channel decode gives L and R unchanged; there is nothing folded in.
    // This is how FFmpeg's encoder writes a stream, and the one an external oracle reads.
    kNone,
    // Substream 0 carries a stereo downmix, substream 1 a 5.1 downmix of an 8 channel stream (or
    // the 5.1 itself), and the later substreams' matrices turn them back into the source channels
    // exactly. The downmix is quantised and level-limited so that no downmix sample overflows the
    // 24 bit range of a presentation (TH spec §10.5): the encoder lowers the folded-in
    // coefficients until the interval fits and falls back to a downmix with nothing folded in.
    kStandard,
};

// The gains the standard downmix folds in, as linear factors. 0.7071 is -3 dB.
struct DownmixGains {
    double centre = 0.7071067811865476;            // C into L and R
    double surround = 0.7071067811865476;          // Ls into L, Rs into R (5.1 to 2.0)
    double lfe = 0.0;                              // LFE into L and R
    double rear_to_surround = 0.7071067811865476;  // Lrs into Ls, Rrs into Rs (7.1 to 5.1)
};

// Tools that no external decoder exercises: FFmpeg's encoder never writes them, so a stream that
// uses one is read back by this library's decoder (and the Python reference) and by no other
// decoder here. Off by default; tests label them "no external oracle".
struct ExperimentalTools {
    // An IIR filter after the FIR (TH spec §9.5; ERRATA.md entry 13 says what the streams show it
    // predicts from). Not written yet: the encoder refuses the flag with kUnsupported.
    bool iir = false;
    // LSB bypass in the stereo matrix (TH spec §10.2). Not written yet: kUnsupported.
    bool lsb_bypass = false;
};

// The payload of an access unit's extra data (TH spec §16): given the index of the unit, returns
// the bytes of the evolution frame payload to frame into that unit, or nothing. The encoder
// writes the framing of TH spec §16.1 and §16.4 around the bytes and never looks inside them.
using ExtraDataSource = std::function<std::vector<std::byte>(std::uint64_t access_unit_index)>;

struct EncoderConfig {
    StreamKind kind = StreamKind::kTrueHd;  // only kTrueHd is written
    std::uint32_t sample_rate = 48000;
    int channels = 2;  // 1..8
    // The width of the PCM given to encode(): a sample is a signed integer of this many bits (16,
    // 20 or 24), not left-justified.
    int bits = 24;
    // TH spec §13.1: the 13-bit channel assignment, naming the speakers in the order of the
    // PCM's channels. 0 picks the layout for `channels`: mono C, stereo L R, 3.0, 4.0, 5.0, 5.1,
    // 6.0 and 7.1 without LFE as 6 channels and 7.1 as 8 (the table in encoder.cpp).
    std::uint32_t channel_assignment = 0;
    // The access units between restart headers (TH spec §7.5): 1 or at least 8. A stream of fewer
    // units than this has one restart header. A longer interval makes a smaller stream, a shorter
    // one a smaller delay before the first unit leaves the encoder and a quicker random access.
    int restart_interval_units = 128;
    // TH spec §4.4, §18: the rate the stream is declared not to exceed. 18 Mbps is Blu-ray's.
    std::uint32_t peak_data_rate_bps = 18'000'000;
    // false: a stream that cannot be sent within peak_data_rate_bps and the 75 ms of TH spec §18
    // is an error (kRateExceeded). true: it is written anyway and counted in
    // EncoderStats::rate_violations.
    bool allow_rate_violations = false;
    DownmixMode downmix = DownmixMode::kNone;
    DownmixGains downmix_gains{};
    // The 2 channel mode of TH spec §5.4: 0 stereo, 1 Lt/Rt matrix encoded, 2 headphone.
    unsigned stereo_mode = 0;
    // TH spec §14: dialogue normalisation, mix levels and the DRC start-up gains. All zero by
    // default, as in FFmpeg's streams.
    ChannelMeaning channel_meaning{};
    // The largest FIR order tried (TH spec §9.3: 8). 0 turns prediction off.
    int max_fir_order = 8;
    // Decorrelate the channel pairs with a primitive matrix when it makes the stream smaller.
    bool matrix_search = true;
    ExperimentalTools experimental{};
    ExtraDataSource extra_data{};
    // Decode every unit with this library's decoder as it is produced and compare with the input
    // (TH spec §19.2). Costs about as much again as the encode.
    bool verify = false;
};

// One access unit, whole, as it goes into a file.
struct EncodedAccessUnit {
    std::vector<std::byte> bytes;
    int samples = 0;          // PCM samples per channel the unit holds, padding included
    int padding_samples = 0;  // zero samples the last unit was filled up with (ERRATA.md, entry 14)
    bool has_major_sync = false;
};

struct EncoderStats {
    std::uint64_t access_units = 0;
    std::uint64_t restart_intervals = 0;
    std::uint64_t input_samples = 0;  // per channel
    std::uint64_t bytes = 0;
    std::uint64_t rate_violations = 0;
    std::uint32_t max_unit_bytes = 0;
    // The largest rate any access unit would need to arrive at over its own duration, in bits per
    // second; the peak rate is a limit on a longer average (TH spec §18), this is not.
    std::uint64_t max_unit_rate_bps = 0;
    std::uint64_t matrices_used = 0;  // primitive matrices sent, over all intervals
    // Restart intervals whose downmix had its gains lowered, or dropped, to stay within 24 bits.
    std::uint64_t downmix_reduced_intervals = 0;
    std::uint64_t verified_units = 0;
};

class ICLFORGE_TRUEHD_EXPORT Encoder {
   public:
    // Checks the configuration; the encoder holds no state before the first encode().
    [[nodiscard]] static std::expected<Encoder, EncodeError> create(const EncoderConfig& config);
    // The sentence create() would fail with, or an empty view if it would not.
    [[nodiscard]] static std::string_view refusal_reason(const EncoderConfig& config);

    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    ~Encoder();

    // Takes interleaved PCM, `channels` values per frame, and returns the access units that are
    // complete. Most calls return nothing: the encoder keeps the input until a whole restart
    // interval is there.
    [[nodiscard]] std::expected<std::vector<EncodedAccessUnit>, EncodeError> encode(
        std::span<const std::int32_t> pcm);

    // Codes what is held, padding the last access unit with zero samples (the unit then ends with
    // the end-of-stream marker of ERRATA.md entry 14). The encoder starts a new stream if it is
    // given more input afterwards.
    [[nodiscard]] std::expected<std::vector<EncodedAccessUnit>, EncodeError> flush();

    [[nodiscard]] const EncoderStats& stats() const noexcept;
    // The samples per channel in a full access unit: 40 at 44.1 and 48 kHz, 80 at 88.2 and 96,
    // 160 at 176.4 and 192.
    [[nodiscard]] int samples_per_access_unit() const noexcept;
    // The number of substreams the configuration writes.
    [[nodiscard]] int substreams() const noexcept;

   private:
    struct Impl;
    explicit Encoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The default channel assignment for a channel count (TH spec §13.1), 0 for a count with none.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint32_t default_channel_assignment(
    int channels) noexcept;

}  // namespace iclforge::truehd
