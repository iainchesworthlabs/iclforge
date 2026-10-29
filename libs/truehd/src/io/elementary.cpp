#include "iclforge/truehd/io/elementary.hpp"

#include "iclforge/base/bitreader.hpp"

namespace iclforge::truehd {

namespace {

constexpr std::size_t kAccessUnitHeaderBytes = 4;
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);
constexpr std::uint8_t kObjectSubstreamBit = 0x80;  // TH spec §6.4: substream_info bit 7

// The offset of the first major sync word at or after `from`, or kNotFound.
[[nodiscard]] std::size_t find_major_sync(std::span<const std::byte> data,
                                          std::size_t from) noexcept {
    for (std::size_t i = from; i + 4 <= data.size(); ++i) {
        if (std::to_integer<std::uint8_t>(data[i]) != kMajorSyncByte0) {
            continue;
        }
        if (starts_with_major_sync(data.subspan(i))) {
            return i;
        }
    }
    return kNotFound;
}

// The offset of the first access unit that begins with a major sync at or after `from` and reads
// (its header, its major sync and its directory are sound), or kNotFound.
[[nodiscard]] std::size_t find_unit_with_major_sync(std::span<const std::byte> data,
                                                    std::size_t from) noexcept {
    std::size_t search_from = from + kAccessUnitHeaderBytes;
    while (true) {
        const std::size_t sync = find_major_sync(data, search_from);
        if (sync == kNotFound) {
            return kNotFound;
        }
        const std::size_t start = sync - kAccessUnitHeaderBytes;
        if (parse_access_unit(data.subspan(start), 0, true)) {
            return start;
        }
        search_from = sync + 1;
    }
}

// The restart header, when the segment begins with one: its first block's two flag bits, then the
// header.
[[nodiscard]] std::optional<RestartHeader> restart_of_segment(std::span<const std::byte> segment) {
    BitReader reader(segment);
    if (reader.read(1) == 0 || reader.read(1) == 0) {
        return std::nullopt;
    }
    auto header = read_restart_header(reader, segment, true);
    if (!header) {
        return std::nullopt;
    }
    return *header;
}

}  // namespace

std::expected<StreamInfo, Error> probe_stream(std::span<const std::byte> data) {
    const std::size_t first = find_unit_with_major_sync(data, 0);
    if (first == kNotFound) {
        return std::unexpected(Error{ErrorKind::kBadSync, "no access unit with a major sync"});
    }
    auto first_unit = parse_access_unit(data.subspan(first), 0, true);
    if (!first_unit) {
        return std::unexpected(first_unit.error());
    }
    StreamInfo info;
    info.major_sync = *first_unit->major_sync;
    info.sample_rate = info.major_sync.sample_rate();
    info.num_substreams = info.major_sync.num_substreams;
    info.samples_per_access_unit = nominal_samples_per_access_unit(info.major_sync.ratebits());
    info.first_unit_offset = first;
    info.has_object_substream = info.num_substreams == kMaxSubstreams ||
                                (info.major_sync.substream_info & kObjectSubstreamBit) != 0;
    for (int i = 0; i < info.num_substreams; ++i) {
        if (const auto restart =
                restart_of_segment(first_unit->segments[static_cast<std::size_t>(i)])) {
            info.channels[static_cast<std::size_t>(i)] = restart->matrix_channels();
        }
    }
    // The units after the first are counted by their length words alone.
    std::size_t offset = first;
    while (offset + 2 <= data.size()) {
        const std::size_t length = access_unit_length_bytes(data.subspan(offset));
        if (length < kAccessUnitHeaderBytes || offset + length > data.size()) {
            break;
        }
        ++info.access_units;
        offset += length;
    }
    return info;
}

ElementaryReader::ElementaryReader(std::span<const std::byte> data) noexcept : data_(data) {}

std::optional<std::span<const std::byte>> ElementaryReader::next() {
    if (!started_) {
        started_ = true;
        const std::size_t first = find_unit_with_major_sync(data_, 0);
        if (first == kNotFound) {
            skipped_ += data_.size();
            pos_ = data_.size();
            return std::nullopt;
        }
        skipped_ += first;
        pos_ = first;
    }
    while (pos_ + kAccessUnitHeaderBytes <= data_.size()) {
        const std::span<const std::byte> rest = data_.subspan(pos_);
        const std::size_t length = access_unit_length_bytes(rest);
        if (length >= kAccessUnitHeaderBytes && pos_ + length > data_.size()) {
            // A unit that runs past the data is the end of the stream.
            skipped_ += data_.size() - pos_;
            pos_ = data_.size();
            return std::nullopt;
        }
        if (length >= kAccessUnitHeaderBytes) {
            if (const auto unit = parse_access_unit(rest.first(length), substreams_, true)) {
                if (unit->major_sync) {
                    substreams_ = unit->major_sync->num_substreams;
                }
                pos_ += length;
                ++units_;
                return rest.first(length);
            }
        }
        // The unit does not read: take the next major sync as the way back in.
        const std::size_t next_unit = find_unit_with_major_sync(data_, pos_ + 1);
        if (next_unit == kNotFound) {
            skipped_ += data_.size() - pos_;
            pos_ = data_.size();
            return std::nullopt;
        }
        skipped_ += next_unit - pos_;
        pos_ = next_unit;
        substreams_ = 0;
    }
    skipped_ += data_.size() - pos_;
    pos_ = data_.size();
    return std::nullopt;
}

}  // namespace iclforge::truehd
