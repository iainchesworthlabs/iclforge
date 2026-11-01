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
#include "iclforge/truehd/encoder/ratectl.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// A TrueHD / MLP lossless encoder, written from the owner's Dolby TrueHD (MLP) Reference
// Specification (v3.5, "TH spec") and held to two decoders: this library's and FFmpeg's strict
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
// question 5), the evolution payload (this class only frames a payload it is handed, see
// EncoderConfig::extra_data), and MLP (0xF8726FBB). The Atmos substream is written with
// EncoderConfig::object_channels. The block data CRC
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

// What substream 3 codes (TH spec §15.3; ERRATA.md entries 63 and 70).
enum class ObjectLayout : std::uint8_t {
    // The channels of substream 3 are coded as they are, with no matrix and no prediction from the
    // bed, and `min_chan` 0. The bed is whatever the caller gives. This is not the structure of the
    // owner's streams.
    kIndependent,
    // The structure of the owner's streams: substream 3 codes only the channels from 8 up
    // (`min_chan` 8), and its primitive matrices rebuild all the output channels from the 7.1 bed
    // of substreams 0 to 2 and those coded channels. The 7.1 bed is therefore not an input: it is
    // the first eight objects with the rest mixed in by a fixed render, and the PCM given to
    // encode() is the `object_channels` objects alone. `object_channels` is 9 to 16 and the
    // downmix is DownmixMode::kNone.
    kBedMatrices,
};

// What the encoder does when a unit would arrive later than the model of libs/truehd/ratectl.hpp
// allows (ERRATA.md entry 71).
enum class RateMode : std::uint8_t {
    // Nothing: the stream is lossless, so a unit that does not fit is kRateExceeded (or counted, with
    // EncoderConfig::allow_rate_violations).
    kStrict,
    // The object channels of substream 3 give up low bits, one at a time, in the restart interval
    // that does not fit, until it does (RateControl::max_shape_bits at most). The 7.1 bed of
    // substreams 0 to 2 is never touched. The objects of such an interval are no longer the input's:
    // each sample is the input's rounded to a multiple of 2^k, and EncoderStats::shaped_intervals
    // says how many intervals it happened in. Needs object_channels. PROVISIONAL: whether the owner
    // accepts a lossy object substream at all is ERRATA.md question 71.
    kShape,
};

struct RateControl {
    RateMode mode = RateMode::kStrict;
    // The bitrate the stream is held to, over any 75 ms (the arrival model run at this rate). 0: the
    // declared peak data rate, which is as much as a decoder is told to expect. Above the declared
    // peak rate it changes nothing.
    std::uint32_t target_bitrate_bps = 0;
    // kShape: the most low bits an interval's object channels give up beyond the ones the input
    // leaves clear. 0 to 16.
    int max_shape_bits = 8;
};

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
    // The access units between major syncs: 0 (a major sync at every restart header, the default
    // and what the encoder always wrote) or a multiple of restart_interval_units. The restarts
    // between two major syncs have a restart header in every substream and no major sync
    // (layout.hpp, LayoutCadence).
    int major_sync_interval_units = 0;
    // TH spec §4.4, §18: the rate the stream is declared not to exceed. 18 Mbps is Blu-ray's.
    std::uint32_t peak_data_rate_bps = 18'000'000;
    // What happens to a unit that would arrive late at that rate, or at the bitrate chosen in
    // `rate_control` (ratectl.hpp). The extra_data source may be called more than once for a unit:
    // a restart interval that is coded again asks again.
    RateControl rate_control{};
    // false: a stream that cannot be sent within peak_data_rate_bps and the 75 ms of TH spec §18
    // is an error (kRateExceeded). true: it is written anyway and counted in
    // EncoderStats::rate_violations.
    bool allow_rate_violations = false;
    DownmixMode downmix = DownmixMode::kNone;
    DownmixGains downmix_gains{};
    // The 2 channel mode of TH spec §5.4: 0 stereo, 1 Lt/Rt matrix encoded, 2 headphone.
    unsigned stereo_mode = 0;
    // TH spec §14: dialogue normalisation, mix levels, source formats, the control-enabled bits and
    // the DRC start-up gains, per presentation (2, 6 and 8 channels), written into every major
    // sync. All zero by default, as in FFmpeg's streams, so a configuration that does not set them
    // writes the stream it always wrote. A field wider than its width (channel_meaning_problem())
    // or `extra_present` (the sixteen-channel extension is not written) is kInvalidConfig, the
    // detail naming the field. Provisional, no oracle until the DEE set: what the values mean
    // (units, which presentation applies when, what a control-enabled bit gates) is ERRATA.md
    // questions 51 to 55.
    ChannelMeaning channel_meaning{};
    // TrueHD Atmos (TH spec §15, v3.9): the channels of substream 3, 1 to 16 (the Atmos
    // presentations are 12, 14 and 16, §15.17; the owner's discs have 12), written with restart
    // sync 0x31EC and the 16-channel extension of the major sync (§14.2). 0 writes none. The PCM
    // given to encode() then has `channels` + `object_channels` values a frame: the 7.1 bed first
    // (`channels` must be 8, substreams 0 to 2, 48 kHz only, §15.2, §20.2), then the object
    // channels, which (ObjectLayout::kIndependent) are coded as substream 3 on their own, with no
    // matrix or prediction from the bed; `object_layout` says what kBedMatrices changes. A decode
    // of the best presentation gives the bed; Presentation::kAll gives the object channels. The
    // programme they carry (which are beds, which objects) is the object audio metadata's, written
    // by the caller in the extra data (`extra_data`); this only says in the major sync what
    // `object_meaning` says. PROVISIONAL, no oracle but this library's decoder and the Python
    // reference (ERRATA.md entry 63; questions 61 to 63).
    int object_channels = 0;
    // How substream 3 relates to the bed (ERRATA.md entries 63 and 70).
    ObjectLayout object_layout = ObjectLayout::kIndependent;
    // kBedMatrices only: the noise shift (0 to 15; 0 draws none, 1 is what the owner's silent
    // streams have) of the matrices that mix the objects into the bed. The matrices draw generator
    // 1's noise, which the encoder reproduces to the bit.
    int object_noise_shift = 0;
    // kBedMatrices only: one matrix with the LSB bypass (object_channels - 1, 2.0 on itself), which
    // sends one bit a sample for the low bit the doubling loses. Needs a quantiser step of 0, so
    // the encoder leaves the bits below the output shift's reach in the samples (a 16 bit source
    // loses the one bit of saving it would otherwise have).
    bool object_lsb_bypass = false;
    // The 16-channel fields of the major sync (§14.2). `sixteench_channel_count` is filled in by
    // the encoder: the object channels, less the LFE of a dynamic-object-only programme, which is
    // how the owner's discs count them. Used only with object_channels.
    ExtraChannelMeaning object_meaning{};
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
    // ObjectLayout::kBedMatrices: restart intervals whose mix of the objects into the bed had its
    // gain lowered, or dropped, to keep the bed within 24 bits.
    std::uint64_t object_mix_reduced_intervals = 0;
    // The smallest headroom of any unit under the declared peak rate (ratectl.hpp), in microseconds;
    // below 0 only with allow_rate_violations.
    std::int64_t min_headroom_us = 0;
    // RateMode::kShape: restart intervals whose object channels gave up bits, the most any of them
    // gave up beyond what the input leaves clear, and the intervals coded again on the way.
    std::uint64_t shaped_intervals = 0;
    int max_shape_bits = 0;
    std::uint64_t shape_retries = 0;
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
    // interval is there. kBadInput (a sample outside the range of `bits`, or PCM that is not whole
    // frames) is found before anything is kept and leaves the encoder as it was. After any other
    // error the encoder's stream state is not defined: destroy it and make another.
    [[nodiscard]] std::expected<std::vector<EncodedAccessUnit>, EncodeError> encode(
        std::span<const std::int32_t> pcm);

    // Codes what is held, padding the last access unit with zero samples (the unit then ends with
    // the end-of-stream marker of ERRATA.md entry 14). The encoder starts a new stream if it is
    // given more input afterwards.
    [[nodiscard]] std::expected<std::vector<EncodedAccessUnit>, EncodeError> flush();

    [[nodiscard]] const EncoderStats& stats() const noexcept;
    // The figures of every restart interval written so far, in order (ratectl.hpp): what
    // analyse_rate() finds in the stream the units make.
    [[nodiscard]] std::span<const IntervalRate> interval_rates() const noexcept;
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
