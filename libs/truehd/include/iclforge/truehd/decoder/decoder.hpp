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

// Settings for the lab tool (iclforge-truehd-ss3-lab); the decoder does not use them otherwise.
struct DecoderLab {
    // The output sample from which the lab keeps the first 51,200 samples of the object substream
    // (DecoderCounters::object_first_out); 0 keeps the first ones.
    std::uint64_t record_from = 0;
    // Keep every block header that sends matrices or output shifts, in every substream and with no
    // cap (DecoderCounters::block_header_log). Off by default: the log grows with the stream.
    bool log_block_headers = false;
    friend bool operator==(const DecoderLab&, const DecoderLab&) = default;
};

// The other reading of a point the specification leaves open or contradicts, one for each reading
// that is marked provisional until a stream scores it (ERRATA.md questions 5, 6 and 18; entries 43
// and 45). They exist so that a stream can be decoded both ways and the readings told apart by
// whether the decode is the source: a stream that decodes to its source with the library's reading
// and to something else with the alternative is evidence for the reading, with a count of the
// values that differ. They are for scoring and are never the default; `forge decode` takes them
// only by name (`truehd-reading=`).
struct AlternativeReadings {
    // §17.2: `seed_shr7` is the 32 bits of the seed from bit 7 up, as the text shows, and not the
    // 16 bits FFmpeg's decoder takes (question 5).
    bool seed_shr7_32_bit = false;
    // §7.1: a restart resets the filter histories, as the text says, and does not keep them
    // (question 18).
    bool zero_histories_at_restart = false;
    // §8.2: `output_shift` is indexed by the output position, and not by the matrix channel the
    // output takes (question 6; entry 43).
    bool output_shift_by_position = false;
    // §7.3: the lossless check's fold shifts a sample by the output position, and not by the matrix
    // channel (question 6; entries 12 and 43).
    bool fold_by_position = false;
};

struct DecoderConfig {
    Presentation presentation = Presentation::kBest;
    Strictness strictness = Strictness::kStrict;
    DecoderLab lab = {};
    AlternativeReadings readings{};
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
        std::uint8_t ext_width = 0;
        std::array<std::int32_t, kMaxChannels> ext_value{};  // the extension's deltas as sent
    };
    std::vector<ObjectMatrixHeader> object_first_matrices;
    // Every matrix section of the object substream sent in the first ten intervals: the output
    // sample it was sent at and its matrices (for the lab; no audio).
    struct ObjectSection {
        std::uint32_t sample = 0;
        bool extension = false;
        std::vector<ObjectMatrixHeader> items;
    };
    std::vector<ObjectSection> object_sections;
    // Every block header of every substream that sent matrices or output shifts, in stream order
    // and with no cap, when DecoderLab::log_block_headers is set (`forge probe detail=matrix`).
    // The matrices are as sent: coefficients raw, the extension's deltas raw.
    struct BlockHeaderLog {
        struct Matrix {
            std::uint8_t out = 0;
            std::uint8_t frac_bits = 0;
            bool lsb_bypass = false;
            std::uint8_t noise_shift = 0;
            std::int8_t f_shift = 0;
            std::uint8_t field_2b = 0;
            std::uint8_t field_m4 = 0;
            std::uint8_t ext_width = 0;
            std::uint8_t ext_field2 = 0;
            std::uint32_t mask = 0;  // bit c: coeff[c] was sent; c >= matrix_channels is a noise channel
            std::array<std::int32_t, kMaxChannels + kNoiseChannels> coeff{};
            std::array<std::int32_t, kMaxChannels> ext_value{};
        };
        std::uint64_t unit = 0;            // access unit index, from 0
        std::uint8_t substream = 0;
        std::uint16_t block = 0;           // index of the block in its segment, from 0
        std::uint16_t sample_in_unit = 0;  // samples of the segment before this block
        bool restart = false;              // the block carries a restart header
        std::uint8_t matrix_channels = 0;  // of the restart header in force
        bool has_matrices = false;
        bool extension = false;
        bool coeffs_sent = false;
        bool deltas_sent = false;
        bool has_output_shift = false;
        // Indexed by matrix channel (ERRATA.md entry 43), matrix_channels of them; the decoder
        // applies them as sent.
        std::array<std::int8_t, kMaxChannels> output_shift{};
        // The restart header's ch_assign: the output channel each matrix channel goes to.
        std::array<std::uint8_t, kMaxChannels> ch_assign{};
        std::vector<Matrix> matrices;
    };
    std::vector<BlockHeaderLog> block_header_log;
    // Every block of the object substream in the first ten intervals: the output sample it starts
    // at, its size, whether its header carries matrices (and how many) and whether it has the
    // extension (for the lab).
    struct ObjectBlock {
        std::uint32_t sample = 0;
        std::uint16_t size = 0;
        std::uint8_t matrices = 0;
        bool header = false;  // the block has a header at all
        bool carries = false;
        bool extension = false;
    };
    std::vector<ObjectBlock> object_blocks;
    // Output samples of the object substream decoded so far (for the lab's positions).
    std::uint64_t object_samples_total = 0;
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

    // Forgets the stream: the next unit has to carry a major sync. The decoder's buffers are kept
    // and cleared where they are, so this allocates nothing and cannot fail; a device restarts a
    // stream at a seek or a format change without holding the decoder's memory twice.
    void reset() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::truehd
