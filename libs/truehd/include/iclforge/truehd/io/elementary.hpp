#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// A raw .thd / .mlp stream: access units back to back (TH spec §3.1), with a major sync in some of
// them.

namespace iclforge::truehd {

// What a stream is, from its first major sync and the restart headers of the units that follow it.
struct StreamInfo {
    MajorSync major_sync;
    std::uint32_t sample_rate = 0;
    int num_substreams = 0;
    // The channels each substream's restart header says a decode of it delivers (max_matrix_chan +
    // 1), 0 for a substream whose restart header the probe did not reach.
    std::array<int, kMaxSubstreams> channels{};
    int samples_per_access_unit = 0;  // nominal, from the sample rate
    std::size_t access_units = 0;     // counted in the data given
    std::size_t first_unit_offset = 0;
    bool has_object_substream = false;  // a fourth substream, or substream_info's Atmos bit
};

// How far ahead of its first access unit a stream may start in `data` (a timecode or other packet
// some muxers put in front of it, bytes of another stream): the search for the first major sync
// stops there.
inline constexpr std::size_t kMaxLeadingBytes = 65536;

// Where a TrueHD or MLP stream starts in `data`: the offset of its first access unit, which may
// have up to `max_leading` bytes of something else ahead of it. At offset 0 the unit has to read
// (its header's check nibble, its major sync with a good CRC, its directory). Anywhere else it has
// to read and the unit after it has to read too, when `data` holds that one whole (a stream's bytes
// that run out inside it do not count against it), so that bytes which only look like a major sync
// are not taken for a stream.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::optional<std::size_t> find_stream_start(
    std::span<const std::byte> data, std::size_t max_leading = kMaxLeadingBytes) noexcept;

// Reads the first major sync in `data` (find_stream_start: the data may start up to
// kMaxLeadingBytes before it) and the restart headers after it. kBadSync if there is none.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<StreamInfo, Error> probe_stream(
    std::span<const std::byte> data);

// Hands out the access units of a stream held in memory, one at a time. It starts at the first
// major sync (find_stream_start; the 4 bytes before it are the unit's header). After a unit that
// does not read it looks for the next major sync, and counts the bytes it passed.
class ICLFORGE_TRUEHD_EXPORT ElementaryReader {
   public:
    explicit ElementaryReader(std::span<const std::byte> data) noexcept;

    // The next unit's bytes, or nullopt at the end. A unit whose length runs past the data ends the
    // stream.
    [[nodiscard]] std::optional<std::span<const std::byte>> next();

    // Bytes passed over, the ones ahead of the first unit included.
    [[nodiscard]] std::size_t skipped_bytes() const noexcept { return skipped_; }
    // The part of skipped_bytes() that came before the first unit: not a loss of the stream.
    [[nodiscard]] std::size_t leading_bytes() const noexcept { return leading_; }
    [[nodiscard]] std::size_t units_read() const noexcept { return units_; }
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

   private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    std::size_t skipped_ = 0;
    std::size_t leading_ = 0;
    std::size_t units_ = 0;
    int substreams_ = 0;
    bool started_ = false;
};

}  // namespace iclforge::truehd
