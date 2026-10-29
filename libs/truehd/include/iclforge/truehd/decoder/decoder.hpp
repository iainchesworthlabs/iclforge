#pragma once

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

// A TrueHD / MLP decoder, written from the owner's Dolby TrueHD (MLP) Reference Specification (v3.3, "TH spec")
// and checked, sample for sample, against the streams FFmpeg's encoder writes and FFmpeg's decoder. The codec is
// lossless: what comes out is the PCM that went in, 24 bits wide, bit for bit.
//
// What it does: it reads access units (TH spec §3) of both stream types, 0xF8726FBA TrueHD and 0xF8726FBB MLP;
// the major sync with its CRC-16, the substream directory, the segments with their parity byte and CRC-8, and
// the restart headers with theirs. It reconstructs substreams 0 to 2 (up to eight channels) and substream 3
// (sixteen channels) the same way: the Huffman or raw residuals (§11), the FIR and IIR prediction filters (§9),
// the primitive matrices with their LSB bypass (§10) and the output shifts, at 44.1 to 192 kHz. It checks the
// lossless check of every restart interval against the next restart header (§7.3). A presentation with several
// substreams reads them in order, each seeing the channels the ones before it decoded (§2.3).
//
// What it refuses, with ErrorKind::kUnsupported and a reason: a matrix that draws on the noise channels, and a
// restart header with noise_type set, because the specification does not define the generator (§17;
// ERRATA.md); a stream whose major sync extension the CRC does not locate. The extra data (§16) and the object
// audio metadata in it are not interpreted: the bytes come back with the unit.
//
// ERRATA.md, beside this library, records every place the specification and the streams disagree, the reading
// taken for each, and the questions only the owner can answer.

namespace iclforge::truehd {

// Which substreams a decode reads (TH spec §2.2, §20.2). Reading fewer is cheaper; the PCM is the last one's.
enum class Presentation : std::uint8_t {
    kBest,   // the highest of substreams 0..2 the stream has: the stereo, 5.1 or 7.1 bed
    k2ch,    // substream 0
    k6ch,    // substreams 0..1
    k8ch,    // substreams 0..2
    kAll,    // every substream, the sixteen-channel one last
};

enum class Strictness : std::uint8_t {
    kStrict,   // a check that fails (nibble, CRC, parity, lossless check) is an error and the unit is dropped
    kLenient,  // the failed check is counted and the unit is decoded as far as it reads
};

struct DecoderConfig {
    Presentation presentation = Presentation::kBest;
    Strictness strictness = Strictness::kStrict;
};

// One decoded access unit. The views point into the decoder and stay valid until its next decode() or reset().
struct DecodedAccessUnit {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int samples = 0;   // per channel
    int substream = 0; // the substream the PCM is from
    // Interleaved, `samples` x `channels`. Each value is 24 bits, sign-extended into the int32.
    std::span<const std::int32_t> pcm;
    // The speakers of the channels in order, when the major sync names them for this channel count; else empty.
    std::span<const Channel> layout;
    std::span<const std::byte> extra_data;  // the unit's bytes after its last segment, uninterpreted
    bool restarted = false;                 // a restart header was read in the substream decoded
};

struct DecoderCounters {
    std::uint64_t units = 0;             // units decode() was given
    std::uint64_t units_decoded = 0;     // ... that gave samples
    std::uint64_t units_waiting = 0;     // ... that gave none because no restart header had come yet
    std::uint64_t restarts = 0;
    std::uint64_t lossless_checks_ok = 0;
    std::uint64_t lossless_check_failures = 0;
    std::uint64_t check_failures = 0;    // nibble, major sync CRC, restart CRC, parity and segment CRC together
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

    // Decodes one access unit, whole (from its 4-byte header). The decoder holds the state a unit continues:
    // the stream's major sync, each substream's parameters, filters' histories and lossless check. Nothing for
    // a unit that cannot give samples yet: one that arrives before a major sync, or whose substreams have not
    // had a restart header (a stream joined partway). After an error the decoder stays usable, but the
    // substream that failed waits for its next restart header.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, Error> decode(
        std::span<const std::byte> access_unit);

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
