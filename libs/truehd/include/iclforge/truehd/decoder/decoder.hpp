#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// A TrueHD / MLP decoder, written from the owner's Dolby TrueHD (MLP) Reference Specification
// (v3.5, "TH spec") and checked, sample for sample, against the streams FFmpeg's encoder writes and
// FFmpeg's decoder. The codec is lossless: what comes out is the PCM that went in, 24 bits wide,
// bit for bit.
//
// What it does: it reads access units (TH spec §3) of both stream types, 0xF8726FBA TrueHD and
// 0xF8726FBB MLP; the major sync with its CRC-16, the substream directory, the segments with their
// parity byte and CRC-8, and the restart headers with theirs. It reconstructs substreams 0 to 2 (up
// to eight channels) and substream 3 (sixteen channels) the same way: the Huffman or raw residuals
// (§11), the FIR and IIR prediction filters (§9), the primitive matrices with their LSB bypass
// (§10) and the output shifts, at 44.1 to 192 kHz. It checks the lossless check of every restart
// interval against the next restart header (§7.3). A presentation with several substreams reads
// them in order, each seeing the channels the ones before it decoded (§2.3).
//
// What it refuses, with ErrorKind::kUnsupported and a reason: a stream whose major sync extension the
// CRC does not locate, and a channel count that changes inside an access unit. Noise generators 0 and 1
// (§17.2 to §17.4; ERRATA.md entry 44) and LSB bypass (§10.2.1) are decoded under either restart sync.
// The extra data (§16) and the object audio metadata in it are not interpreted: the bytes come back
// with the unit.
//
// ERRATA.md, beside this library, records every place the specification and the streams disagree,
// the reading taken for each, and the questions only the owner can answer.

namespace iclforge::truehd {

// Which substreams a decode reads (TH spec §2.2, §20.2). Reading fewer is cheaper; the PCM is the
// last one's.
enum class Presentation : std::uint8_t {
    kBest,  // the highest of substreams 0..2 the stream has: the stereo, 5.1 or 7.1 bed
    k2ch,   // substream 0
    k6ch,   // substreams 0..1
    k8ch,   // substreams 0..2
    kAll,   // every substream, the sixteen-channel one last
};

// What a failed check does. Either way a unit that cannot be read at all (its header, its
// directory or a segment's framing) is an error, and from then until a substream's next restart
// header that substream gives no samples and no error, because what it carries from unit to unit
// no longer follows the stream. A decode that has to be exact therefore resumes at the next
// restart header: the samples of the units in between are lost, not wrong.
enum class Strictness : std::uint8_t {
    kStrict,   // a check that fails (nibble, CRC, parity, lossless check) is an error, the unit is
               // dropped, and its substream waits for its next restart header
    kLenient,  // the failed check is counted and the unit is decoded as far as it reads. Its
               // samples can be wrong: the lossless check at the next restart header says so
               // (DecoderCounters::lossless_check_failures), and it is not made for the last
               // restart interval of a stream, which no restart header follows
};

// Switches for trials on the points of the object substream (restart sync 0x31EC) that no stream
// has settled (ERRATA.md entry 69). All off is the decoder. A stream whose lossless checks pass
// only with one on says what the point is; nothing else should set them.
struct DecoderLab {
    // The object substream reads the bed rows as the substream before it output them, after that
    // substream's matrices, not before.
    bool lower_rows_after_matrices = false;
    bool no_quantiser_mask = false;  // the object matrices do not mask their output's low bits
    bool reverse_matrices = false;   // the object matrices run last to first
    bool inverse_f_shift = false;    // the coefficient is raw / 2^(frac_bits + f_shift)
    // What the object substream's lossless check folds: 0 the output of every matrix channel;
    // 1 and 2 the output of the coded channels (shifted left by the matrix channel, or by its
    // place among the coded ones); 3 and 4 the same for the channels below them; 5 and 6 the
    // coded samples before the matrices; 10 + N the output of the matrix channels from N up.
    int fold_scope = 0;
    // The extension's deltas: 0 refine the coefficients by 2^-(width + 1) of their last place; 1 not
    // at all; 2 by 2^-width; 3 for the block that sends them only; 4 and 5 with a ramp over the
    // block, (i + 1) / block or i / block; 7 and 8 the same with 2^-width.
    int object_extension = 0;
    // Added to the number of bits the deltas are read below their last place (a negative number
    // weighs the deltas more), and whether their sign is turned over.
    int object_extension_bias = 0;
    bool object_extension_negate = false;
    // When not 0, the deltas are read at 2^-(this - 16) below their last place whatever their width.
    int object_extension_fixed = 0;
    // The place the deltas count from: 0 their coefficient's last place; 1 2^-frac; 2 to 5 one
    // absolute place for every matrix (2^-13, 2^-14, 2^-15, 2^-12).
    int object_extension_unit = 0;
    // How an object matrix's sum exactly between two outputs rounds: 0 down, 1 up, 2 to even;
    // 3 rounds every sum to nearest (a control).
    int object_tie_mode = 0;
    // The object matrices' noise draw is shifted by: 1 minus the output shift; 2 minus the
    // quantiser step; 3 minus both; 4 plus the output shift (of the matrix's output row).
    int object_noise_scale = 0;
    // The rows whose matrix output the quantiser step masks: 0 all, 1 the coded rows only (8 and
    // up), 2 the bed rows only.
    int mask_rows = 0;
    bool object_restart_clears = false;  // an object restart clears the filter histories
    // The object matrices' noise index: bit 0 counts the samples from the start of the block, not
    // of the access unit; bit 1 numbers the matrices from the first, not from the last; bit 2
    // fills the noise buffer again for every block, not once for the access unit.
    int object_noise_index = 0;
    int object_noise_cap = 0;          // the matrix number in the noise index is at most this
    int object_noise_stride_mask = 0;  // the noise stride is masked with this (0 no mask)
    int object_noise_left = 0;  // 1: only the matrices that draw noise count towards a start index
    friend bool operator==(const DecoderLab&, const DecoderLab&) = default;
};

struct DecoderConfig {
    Presentation presentation = Presentation::kBest;
    Strictness strictness = Strictness::kStrict;
    DecoderLab lab = {};
};

// One decoded access unit. The views point into the decoder and stay valid until its next decode()
// or reset().
struct DecodedAccessUnit {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int samples = 0;    // per channel
    int substream = 0;  // the substream the PCM is from
    // Interleaved, `samples` x `channels`. Each value is 24 bits, sign-extended into the int32.
    std::span<const std::int32_t> pcm;
    // The speakers of the channels in order, when the major sync names them for this channel count;
    // else empty.
    std::span<const Channel> layout;
    std::span<const std::byte>
        extra_data;          // the unit's bytes after its last segment, uninterpreted
    bool restarted = false;  // a restart header was read in the substream decoded
    // The last access unit of a stream that does not end on a unit boundary carries an
    // end-of-stream marker counting the samples at its end that are padding. They are not in
    // `samples` or `pcm`.
    int padding_samples = 0;
};

// What follows the last block of a substream's segments, counted over the segments decoded
// (TH spec §16.5; ERRATA.md question 3). `bits` is the distance from the end of the last block to
// the end of the segment's payload (its parity and CRC bytes excluded).
struct SegmentTailCounters {
    std::uint64_t segments = 0;
    std::uint64_t zero = 0;                 // only zero bits follow the last block
    std::uint64_t terminator_a = 0;         // terminatorA (0xD234E or 0x348D3) follows it at once
    std::uint64_t terminator_b = 0;         // 0x1234 follows it at once (substream 3)
    std::uint64_t terminator_b_at_end = 0;  // the segment ends with 0x1234 (substream 3)
    std::uint64_t end_marker = 0;           // the segment ends with terminatorA and a 12-bit count
    std::uint64_t other = 0;                // anything else
    std::uint64_t bits_min = 0;
    std::uint64_t bits_max = 0;
};

struct DecoderCounters {
    std::uint64_t units = 0;          // units decode() was given
    std::uint64_t units_decoded = 0;  // ... that gave samples
    std::uint64_t units_waiting = 0;  // ... that gave none because no restart header had come yet
    std::uint64_t restarts = 0;
    std::uint64_t lossless_checks_ok = 0;
    std::uint64_t lossless_check_failures = 0;
    // The same, by substream.
    std::array<std::uint64_t, kMaxSubstreams> lossless_ok_by_substream{};
    std::array<std::uint64_t, kMaxSubstreams> lossless_failed_by_substream{};
    // The first checks of the object substream: the byte the restart header stored and the one the
    // decoder folded (ERRATA.md entry 69; for the trials of DecoderLab).
    static constexpr std::size_t kObjectChecksKept = 12;
    std::array<std::uint8_t, kObjectChecksKept> object_check_stored{};
    std::array<std::uint8_t, kObjectChecksKept> object_check_folded{};
    std::size_t object_checks_kept = 0;
    // The first 256 checks of the object substream, in order: bit 0 set when the check passed,
    // the upper bits the number of matrix sections the interval's blocks carried (at most 127).
    static constexpr std::size_t kObjectIntervalsKept = 256;
    std::vector<std::uint8_t> object_interval_log;
    // For the same intervals: the matrices of the last section, the largest noise shift among
    // them, and the bits the largest coded sample of channels 8 and up, of channels below 8
    // (before the matrices) and of every output row took (no sample value).
    struct ObjectIntervalFacts {
        std::uint8_t matrices = 0;
        std::uint8_t max_noise_shift = 0;
        std::uint8_t coded_bits = 0;
        std::uint8_t bed_bits = 0;
        std::uint8_t out_bits = 0;
        std::uint32_t samples_so_far = 0;  // output samples kept when the interval ended
    };
    std::vector<ObjectIntervalFacts> object_interval_facts;
    std::string object_error_context;  // the first object block header that failed (lab)
    // The quantiser steps and output shifts, by matrix channel, at the first interval's check.
    std::array<std::uint8_t, kMaxChannels> object_quant_step{};
    std::array<std::int8_t, kMaxChannels> object_output_shift{};
    // For the same intervals, the fold of each matrix channel's own samples (the check byte of the
    // fold of that channel alone): columns 0 to 11 after the matrices, 12 to 23 before them.
    static constexpr std::size_t kObjectColumns = 24;
    // The headers of the first matrix set the object substream sent (no coefficient).
    struct ObjectMatrixHeader {
        std::uint8_t out = 0;
        std::uint8_t frac_bits = 0;
        std::int8_t f_shift = 0;
        std::uint8_t field_2b = 0;
        std::uint8_t field_m4 = 0;
        std::uint16_t mask = 0;
        std::array<std::int32_t, kMaxChannels> coeff{};  // as sent (raw)
    };
    std::vector<ObjectMatrixHeader> object_first_matrices;
    std::array<std::array<std::uint8_t, kObjectColumns>, kObjectChecksKept> object_column_bytes{};
    // The first three intervals of the object substream, by matrix channel: the smallest and the
    // largest value and the number of samples that differ from the one before, in the rows as
    // decoded (before the matrices) and in the rows as output (after them). Counts and ranges only.
    struct ObjectStats {
        std::array<std::int32_t, kMaxChannels> coded_min{};
        std::array<std::int32_t, kMaxChannels> coded_max{};
        std::array<std::uint32_t, kMaxChannels> coded_changes{};
        std::array<std::int32_t, kMaxChannels> out_min{};
        std::array<std::int32_t, kMaxChannels> out_max{};
        std::array<std::uint32_t, kMaxChannels> out_changes{};
        std::array<std::int32_t, kMaxChannels> coded_last{};
        std::array<std::int32_t, kMaxChannels> out_last{};
        std::uint64_t samples = 0;
    } object_stats;
    // The output rows (after the matrices, before the output shift) of the same first intervals,
    // 12 values a sample, for the lab to find the samples that differ in a stream whose objects
    // are constant.
    std::vector<std::int32_t> object_first_out;
    std::uint64_t check_failures =
        0;  // nibble, major sync CRC, restart CRC, parity and segment CRC together
    // Block headers that send matrices, and the matrices among them with lsb_bypass set (TH spec
    // §10.2; ERRATA.md question 11).
    std::uint64_t matrix_headers = 0;
    std::uint64_t lsb_bypass_matrices = 0;
    // Per substream: what follows the last block of its segments.
    std::array<SegmentTailCounters, kMaxSubstreams> tails{};
};

class ICLFORGE_TRUEHD_EXPORT Decoder {
   public:
    Decoder();
    explicit Decoder(const DecoderConfig& config);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Decodes one access unit, whole (from its 4-byte header). The decoder holds the state a unit
    // continues: the stream's major sync, each substream's parameters, filters' histories and
    // lossless check. Nothing for a unit that cannot give samples yet: one that arrives before a
    // major sync, or whose substreams have not had a restart header (a stream joined partway).
    // After an error the decoder stays usable, but the substream that failed waits for its next
    // restart header; so do all of them after a unit that does not read at all.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, Error> decode(
        std::span<const std::byte> access_unit);

    // For a caller that dropped input between the unit it decoded last and the next one it will
    // decode (a container packet lost, bytes a reader skipped to find the next unit): the decoder
    // cannot see a missing unit, so this makes every substream wait for its next restart header,
    // as after an error. The stream's major sync and the counters are kept.
    void lost_data() noexcept;

    // The latest major sync, once one has been read.
    [[nodiscard]] const std::optional<MajorSync>& major_sync() const noexcept;

    [[nodiscard]] const DecoderCounters& counters() const noexcept;

    // Forgets the stream: the next unit has to carry a major sync.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::truehd
