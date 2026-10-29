#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
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
// What it refuses, with ErrorKind::kUnsupported and a reason: under a restart header with
// noise_type 1 (sync 0x31EB), a matrix with LSB bypass or a non-zero noise shift, because the
// 256-entry noise table of generator 1 is not in the specification yet (§17.3, §17.4; ERRATA.md
// entry 32); a stream whose major sync extension the CRC does not locate. A noise_type 1 restart
// without either decodes, and noise generator 0 (§17.2) is decoded. The extra data (§16) and the
// object audio metadata in it are not interpreted: the bytes come back with the unit.
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

struct DecoderConfig {
    Presentation presentation = Presentation::kBest;
    Strictness strictness = Strictness::kStrict;
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
    std::uint64_t zero = 0;          // only zero bits follow the last block
    std::uint64_t terminator_a = 0;  // terminatorA (0xD234E or 0x348D3) follows it at once
    std::uint64_t terminator_b = 0;  // 0x1234 follows it at once (substream 3)
    std::uint64_t terminator_b_at_end = 0;  // the segment ends with 0x1234 (substream 3)
    std::uint64_t end_marker = 0;    // the segment ends with terminatorA and a 12-bit count
    std::uint64_t other = 0;         // anything else
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
