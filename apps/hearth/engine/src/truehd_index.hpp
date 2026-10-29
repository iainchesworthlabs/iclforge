#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "item_source.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// A TrueHD or MLP stream as Hearth plays it (planning/truehd-parity-hearth-player.md, rows 5 and
// 14): an index of the places a decoder can join the stream, and a cursor that hands out access
// units from any of them.
//
// What the stream is made of decides the shape. An access unit holds 40, 80 or 160 samples (at 48,
// 96 or 192 kHz; the 44.1 kHz family the same), which is 1,200 units a second, so keeping a view
// and a start for each unit the way a Session keeps an AC-3 frame's would take 24 bytes and 8 more
// for every one: 207 MB for a two hour film, 38 times what the same film's E-AC-3 takes. The
// length of every unit is the same, though, so a unit's start is its number times that length and
// needs no record. What does need one is where each unit lies in the file, and a decoder can only
// start at a unit with a major sync (it has no state before a restart header, and a unit with a
// major sync restarts every substream: test_truehd_index.cpp checks that on every stream in
// testdata/truehd). So the index lists those units, thinned to one every kRestartSpacing units at
// most, with their byte offsets; reaching a place in the stream is a read from the last of them
// before it.
//
// Only the last access unit of a stream that does not end on a unit boundary is shorter, by the
// padding its end-of-stream marker counts (TH spec section 16.5), and that marker is in the
// segments, so it is read by decoding the last restart interval once, when the index is made.

namespace iclforge::hearth {

// A unit a decoder can start at: its number counted from the first unit, and where it begins.
struct TruehdRestart {
    std::uint64_t unit = 0;
    std::uint64_t offset = 0;
};

struct TruehdIndex {
    iclforge::truehd::StreamKind kind = iclforge::truehd::StreamKind::kTrueHd;
    std::uint32_t sample_rate = 0;
    // The samples in each access unit, from the sample rate.
    std::uint32_t samples_per_unit = 0;
    int substreams = 0;
    // What each substream's first restart header says it decodes to; 0 where it did not reach one.
    std::array<int, iclforge::truehd::kMaxSubstreams> channels{};
    // A fourth substream, or an object bit in the major sync: the stream has an object layer the
    // decoder does not render.
    bool object_substream = false;
    // Units that read, counting from the first one that has a major sync.
    std::uint64_t units = 0;
    // Their bytes together, and the bytes of the source that were not part of a unit that read:
    // before the first unit, between two, and after the last.
    std::uint64_t unit_bytes = 0;
    std::uint64_t skipped_bytes = 0;
    // Units that carry a major sync, all of them and not only those the index keeps.
    std::uint64_t major_syncs = 0;
    // The samples at the end of the last unit that are padding.
    std::uint32_t padding_samples = 0;
    // The units to join at, by number, with the first among them and none closer to the last than
    // the spacing the index was made with.
    std::vector<TruehdRestart> restarts;

    // Every sample the stream holds.
    [[nodiscard]] std::uint64_t samples() const {
        const std::uint64_t whole = units * samples_per_unit;
        return whole > padding_samples ? whole - padding_samples : 0;
    }

    // The last restart at or before `unit`; the first unit is one.
    [[nodiscard]] const TruehdRestart& restart_at_or_before(std::uint64_t unit) const;

    // How many channels a decode of `presentation` delivers, and whether the stream has that
    // presentation: kBest is the highest of substreams 0 to 2, as the decoder takes it.
    [[nodiscard]] int channels_of(iclforge::truehd::Presentation presentation) const;
};

// Units are kept as joining points at least this far apart (0.21 s at 48 kHz), whatever the stream
// offers: a stream that restarts at every unit would otherwise cost an entry for each.
inline constexpr std::uint64_t kTruehdRestartSpacing = 256;

// Hands out the access units of a stream in a source one at a time, as ElementaryReader does for a
// stream in memory and with the same rules: the first unit is the first that has a major sync and
// reads; one that does not read sends it looking for the next such unit, and the bytes it passed
// are counted; a unit that runs past the end of the source is the end of the stream. What differs
// is that it reads the source a window at a time.
class TruehdCursor {
public:
    // The most the cursor holds of the source at once, by default; a window is never smaller than
    // two units, whatever is asked for.
    static constexpr std::size_t kDefaultWindowBytes = 256 * 1024;

    // `source` must outlive the cursor.
    explicit TruehdCursor(ByteSource& source, std::size_t window_bytes = kDefaultWindowBytes);

    // Goes to the first unit of the stream. False when there is none.
    bool rewind();
    // Goes to `at`, a unit with a major sync (an entry of the index).
    void seek(const TruehdRestart& at);

    // The next unit's bytes, valid until the next call, or nothing at the end of the stream.
    [[nodiscard]] std::optional<std::span<const std::byte>> next();

    // The number and the offset of the unit next() returned last, and whether it opens with a
    // major sync.
    [[nodiscard]] std::uint64_t last_unit() const { return last_unit_; }
    [[nodiscard]] std::uint64_t last_offset() const { return last_offset_; }
    [[nodiscard]] bool last_has_major_sync() const { return last_has_major_sync_; }
    // The number of the unit next() returns next.
    [[nodiscard]] std::uint64_t unit() const { return unit_; }
    // The bytes passed over since the cursor was made, rewound or set.
    [[nodiscard]] std::uint64_t skipped_bytes() const { return skipped_; }

private:
    // The bytes from `offset` on, as many as the window holds from there and at least `want` of them
    // while the source has that many. Invalidates what an earlier call returned.
    [[nodiscard]] std::span<const std::byte> view(std::uint64_t offset, std::size_t want);
    // The offset of the first unit at or after `from` that opens with a major sync and reads.
    [[nodiscard]] std::optional<std::uint64_t> find_unit_with_major_sync(std::uint64_t from);

    ByteSource* source_;
    std::uint64_t size_;
    std::vector<std::byte> window_;
    std::uint64_t window_start_ = 0;
    std::size_t window_size_ = 0;
    std::uint64_t pos_ = 0;
    std::uint64_t unit_ = 0;
    std::uint64_t skipped_ = 0;
    int substreams_ = 0;
    std::uint64_t last_unit_ = 0;
    std::uint64_t last_offset_ = 0;
    bool last_has_major_sync_ = false;
};

// Reads the whole source once: the units, the restarts to keep, and the padding at the end. The
// error is a sentence, for a source with no unit that reads.
[[nodiscard]] std::expected<TruehdIndex, std::string> index_truehd(
    ByteSource& source, std::uint64_t restart_spacing = kTruehdRestartSpacing,
    std::size_t window_bytes = TruehdCursor::kDefaultWindowBytes);

}  // namespace iclforge::hearth
