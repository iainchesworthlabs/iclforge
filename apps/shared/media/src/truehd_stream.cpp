#include "truehd_stream.hpp"

#include <algorithm>
#include <fmt/format.h>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// See truehd_stream.hpp.

namespace iclforge::apps {

namespace th = iclforge::truehd;

namespace {

// An access unit is at most 4,095 16-bit words (TH spec section 3.3's 12-bit length).
constexpr std::size_t kMaxUnitBytes = 8190;
constexpr std::size_t kHeaderBytes = 4;
// How far ahead a search for the next major sync looks at a time.
constexpr std::size_t kScanBytes = 64 * 1024;

// The substreams a decode of `presentation` reads from a stream of `available`
// (decoder.cpp's substreams_wanted).
[[nodiscard]] int wanted_substreams(th::Presentation presentation, int available) {
    int wanted = available;
    switch (presentation) {
        case th::Presentation::kBest:
            wanted = std::min(available, 3);
            break;
        case th::Presentation::k2ch:
            wanted = 1;
            break;
        case th::Presentation::k6ch:
            wanted = 2;
            break;
        case th::Presentation::k8ch:
            wanted = 3;
            break;
        case th::Presentation::kAll:
            break;
    }
    return std::min(wanted, available);
}

}  // namespace

const TruehdRestart& TruehdIndex::restart_at_or_before(std::uint64_t unit) const {
    const auto after = std::ranges::upper_bound(restarts, unit, {}, &TruehdRestart::unit);
    return after == restarts.begin() ? restarts.front() : *std::prev(after);
}

int TruehdIndex::channels_of(th::Presentation presentation) const {
    for (int wanted = wanted_substreams(presentation, substreams); wanted > 0; --wanted) {
        if (const int count = channels[static_cast<std::size_t>(wanted - 1)]; count != 0) {
            return count;
        }
    }
    return 0;
}

TruehdCursor::TruehdCursor(ByteSource& source, std::size_t window_bytes)
    : source_(&source), size_(source.size()), window_(std::max(window_bytes, 2 * kMaxUnitBytes)) {}

std::span<const std::byte> TruehdCursor::view(std::uint64_t offset, std::size_t want) {
    if (offset >= size_) {
        return {};
    }
    const std::uint64_t window_end = window_start_ + window_size_;
    const bool starts_inside = offset >= window_start_ && offset < window_end;
    const bool reaches_end = window_end >= size_;
    if (!starts_inside || (offset + want > window_end && !reaches_end)) {
        window_start_ = offset;
        window_size_ = source_->read(offset, window_);
    }
    if (offset < window_start_ || offset >= window_start_ + window_size_) {
        return {};
    }
    return std::span<const std::byte>(window_).subspan(static_cast<std::size_t>(offset - window_start_),
                                                       window_size_ -
                                                           static_cast<std::size_t>(offset - window_start_));
}

std::optional<std::uint64_t> TruehdCursor::find_unit_with_major_sync(std::uint64_t from) {
    // A unit's major sync word follows its four header bytes: `search` is where one may start.
    std::uint64_t search = from + kHeaderBytes;
    while (search + kHeaderBytes <= size_) {
        const std::span<const std::byte> chunk = view(search, kScanBytes);
        const std::size_t scan = std::min(chunk.size(), kScanBytes);
        if (scan < kHeaderBytes) {
            return std::nullopt;
        }
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i + kHeaderBytes <= scan; ++i) {
            if (std::to_integer<std::uint8_t>(chunk[i]) == th::kMajorSyncByte0 &&
                th::starts_with_major_sync(chunk.subspan(i))) {
                found = i;
                break;
            }
        }
        if (!found) {
            // The last three bytes looked at may begin a sync word that the next look finishes.
            search += scan - (kHeaderBytes - 1);
            continue;
        }
        const std::uint64_t sync = search + *found;
        const std::uint64_t start = sync - kHeaderBytes;
        // The check reads the unit, which may move the window; `chunk` is not used again.
        if (th::parse_access_unit(view(start, kMaxUnitBytes), 0, true)) {
            return start;
        }
        search = sync + 1;
    }
    return std::nullopt;
}

bool TruehdCursor::rewind() {
    pos_ = 0;
    unit_ = 0;
    skipped_ = 0;
    substreams_ = 0;
    last_has_major_sync_ = false;
    const std::optional<std::uint64_t> first = find_unit_with_major_sync(0);
    if (!first) {
        skipped_ = size_;
        pos_ = size_;
        return false;
    }
    skipped_ = *first;
    pos_ = *first;
    return true;
}

void TruehdCursor::seek(const TruehdRestart& at) {
    pos_ = at.offset;
    unit_ = at.unit;
    skipped_ = 0;
    substreams_ = 0;
    last_has_major_sync_ = false;
}

std::optional<std::span<const std::byte>> TruehdCursor::next() {
    while (pos_ + kHeaderBytes <= size_) {
        const std::span<const std::byte> rest = view(pos_, kMaxUnitBytes);
        const std::size_t length = th::access_unit_length_bytes(rest);
        if (length >= kHeaderBytes && length > rest.size()) {
            // A unit that runs past the data is the end of the stream.
            skipped_ += size_ - pos_;
            pos_ = size_;
            return std::nullopt;
        }
        if (length >= kHeaderBytes) {
            const std::span<const std::byte> unit = rest.first(length);
            if (const auto parsed = th::parse_access_unit(unit, substreams_, true)) {
                last_has_major_sync_ = parsed->major_sync.has_value();
                if (parsed->major_sync) {
                    substreams_ = parsed->major_sync->num_substreams;
                }
                last_offset_ = pos_;
                last_unit_ = unit_++;
                pos_ += length;
                return unit;
            }
        }
        // The unit does not read: the next major sync is the way back in.
        const std::optional<std::uint64_t> next_unit = find_unit_with_major_sync(pos_ + 1);
        if (!next_unit) {
            skipped_ += size_ - pos_;
            pos_ = size_;
            return std::nullopt;
        }
        skipped_ += *next_unit - pos_;
        pos_ = *next_unit;
        substreams_ = 0;
    }
    skipped_ += size_ - pos_;
    pos_ = size_;
    return std::nullopt;
}

std::expected<TruehdIndex, std::string> index_truehd(ByteSource& source,
                                                     std::uint64_t restart_spacing,
                                                     std::size_t window_bytes) {
    TruehdCursor cursor(source, window_bytes);
    if (!cursor.rewind()) {
        return std::unexpected(std::string{"no TrueHD or MLP access unit with a major sync"});
    }
    TruehdIndex index;
    {
        // The stream's facts from the first unit and the restart headers in it.
        std::vector<std::byte> head(2 * kMaxUnitBytes);
        const std::uint64_t first_offset = cursor.skipped_bytes();
        head.resize(source.read(first_offset, head));
        const auto info = th::probe_stream(head);
        if (!info) {
            return std::unexpected(fmt::format("the first access unit does not read ({})",
                                               th::to_string(info.error().kind)));
        }
        index.kind = info->major_sync.kind;
        index.sample_rate = info->sample_rate;
        index.samples_per_unit = static_cast<std::uint32_t>(info->samples_per_access_unit);
        index.substreams = info->num_substreams;
        index.channels = info->channels;
        index.object_substream = info->has_object_substream;
        index.variable_rate = info->major_sync.is_vbr;
        index.peak_data_rate = info->major_sync.peak_data_rate;
    }
    if (index.sample_rate == 0 || index.samples_per_unit == 0) {
        return std::unexpected(std::string{"the major sync names no sample rate this player knows"});
    }

    // The last unit with a major sync, kept apart from the thinned list: the padding is read from
    // there.
    TruehdRestart last_sync{};
    std::uint64_t spacing_from = 0;
    while (const auto unit = cursor.next()) {
        index.unit_bytes += unit->size();
        if (!cursor.last_has_major_sync()) {
            continue;
        }
        ++index.major_syncs;
        last_sync = TruehdRestart{.unit = cursor.last_unit(), .offset = cursor.last_offset()};
        if (index.restarts.empty() || last_sync.unit - spacing_from >= restart_spacing) {
            index.restarts.push_back(last_sync);
            spacing_from = last_sync.unit;
        }
    }
    index.units = cursor.unit();
    index.skipped_bytes = cursor.skipped_bytes();

    // The end-of-stream marker is in the last unit's segments: decode from the last restart to see
    // it. A stream that does not decode to its end has none to read.
    th::Decoder decoder;
    TruehdCursor tail(source, window_bytes);
    tail.seek(last_sync);
    while (const auto unit = tail.next()) {
        const auto decoded = decoder.decode(*unit);
        if (decoded && *decoded) {
            index.padding_samples = static_cast<std::uint32_t>((*decoded)->padding_samples);
        } else {
            index.padding_samples = 0;
        }
    }
    return index;
}

}  // namespace iclforge::apps
