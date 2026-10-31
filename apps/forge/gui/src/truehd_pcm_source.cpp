#include "truehd_pcm_source.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace forge_gui {

namespace th = iclforge::truehd;

namespace {

// An access unit is at most 4095 16-bit words (TH spec §3.3: the length is 12 bits).
constexpr std::size_t kMaxUnitBytes = 8192;
// How much of the file the index pass reads at a time.
constexpr std::size_t kScanBlockBytes = std::size_t{4} << 20U;
// What find_stream_start and probe_stream read of the head of the file.
constexpr std::size_t kHeadBytes = th::kMaxLeadingBytes + (std::size_t{256} << 10U);
// A 24-bit sample, sign-extended in an int32, to +-1.0.
constexpr float kFullScale = 8388608.0F;

}  // namespace

std::expected<std::unique_ptr<TruehdPcmSource>, std::string> TruehdPcmSource::open(
    const std::filesystem::path& path, th::Presentation presentation) {
    std::unique_ptr<TruehdPcmSource> source{new TruehdPcmSource()};
    source->path_ = path;
    source->presentation_ = presentation;
    source->file_.open(path, std::ios::binary);
    if (!source->file_) {
        return std::unexpected("Could not open " + path.string() + ".");
    }
    if (const std::string problem = source->scan(); !problem.empty()) {
        return std::unexpected(problem);
    }
    // The first unit says how many channels there are and what they are.
    source->seek_to(0);
    if (!source->decode_next_unit() || source->info_.channels == 0) {
        return std::unexpected(path.string() + " holds no TrueHD or MLP access unit that decodes.");
    }
    source->seek_to(0);
    return source;
}

std::string TruehdPcmSource::scan() {
    file_.seekg(0, std::ios::end);
    file_size_ = static_cast<std::uint64_t>(file_.tellg());
    if (file_size_ < 8) {
        return path_.string() + " is too short to hold a TrueHD or MLP access unit.";
    }
    std::vector<std::byte> head(static_cast<std::size_t>(std::min<std::uint64_t>(file_size_, kHeadBytes)));
    file_.seekg(0);
    file_.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(std::max<std::streamsize>(file_.gcount(), 0)));
    file_.clear();
    const auto start = th::find_stream_start(head);
    if (!start) {
        return path_.string() +
               " is not a TrueHD or MLP stream: it has no access unit with a major sync.";
    }
    first_offset_ = *start;
    const auto probed = th::probe_stream(head);
    if (!probed) {
        return path_.string() + ": " + std::string{th::to_string(probed.error().kind)};
    }
    info_.mlp = probed->major_sync.kind == th::StreamKind::kMlp;
    info_.sample_rate = probed->sample_rate;
    info_.samples_per_unit = probed->samples_per_access_unit;
    if (info_.sample_rate == 0 || info_.samples_per_unit <= 0) {
        return path_.string() + " has a sample rate this build does not read.";
    }

    // The index: every unit's length is in its first two bytes, so the file is walked without
    // decoding it, and the units that open with a major sync are the places a decode can begin.
    std::vector<std::byte> block;
    std::uint64_t block_start = 0;
    std::uint64_t pos = first_offset_;
    std::uint64_t units = 0;
    iclforge::apps::truehd_probe::RateModel rate_model;
    while (pos + 8 <= file_size_) {
        if (pos < block_start || pos + 8 > block_start + block.size()) {
            block.resize(kScanBlockBytes);
            file_.seekg(static_cast<std::streamoff>(pos));
            file_.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
            block.resize(static_cast<std::size_t>(std::max<std::streamsize>(file_.gcount(), 0)));
            file_.clear();
            block_start = pos;
            if (block.size() < 8) {
                break;
            }
        }
        const auto here = std::span<const std::byte>(block).subspan(static_cast<std::size_t>(pos - block_start));
        const std::size_t length = th::access_unit_length_bytes(here);
        if (length < 8 || length > kMaxUnitBytes || pos + length > file_size_) {
            break;  // the end of the stream, or bytes that are not an access unit
        }
        const bool major_sync = th::starts_with_major_sync(here.subspan(4));
        if (major_sync) {
            restarts_.push_back(Restart{.unit = units, .offset = pos});
        }
        // The rate model needs the declared rate, which the first unit's major sync gives.
        std::optional<th::MajorSync> first_sync;
        if (units == 0 && major_sync) {
            if (const auto parsed = th::parse_major_sync(here.subspan(4), false)) {
                first_sync = *parsed;
            }
        }
        rate_model.add(length, first_sync);
        pos += length;
        ++units;
    }
    if (restarts_.empty() || restarts_.front().unit != 0) {
        return path_.string() + " does not open with an access unit that carries a major sync.";
    }
    info_.units = units;
    info_.rate = rate_model.report();
    info_.restarts = restarts_.size();
    info_.frames = units * static_cast<std::uint64_t>(info_.samples_per_unit);
    return {};
}

void TruehdPcmSource::seek_to(std::uint64_t frame) {
    const std::uint64_t unit = frame / static_cast<std::uint64_t>(info_.samples_per_unit);
    auto it = std::ranges::upper_bound(restarts_, unit, {}, &Restart::unit);
    if (it != restarts_.begin()) {
        --it;
    }
    decoder_ = std::make_unique<th::Decoder>(
        th::DecoderConfig{.presentation = presentation_, .strictness = th::Strictness::kLenient});
    next_offset_ = it->offset;
    next_unit_ = it->unit;
    pending_start_ = it->unit * static_cast<std::uint64_t>(info_.samples_per_unit);
    decoded_end_ = pending_start_;
    for (auto& channel : pending_) {
        channel.clear();
    }
}

bool TruehdPcmSource::decode_next_unit() {
    if (next_unit_ >= info_.units) {
        return false;
    }
    unit_buffer_.resize(kMaxUnitBytes);
    file_.seekg(static_cast<std::streamoff>(next_offset_));
    file_.read(reinterpret_cast<char*>(unit_buffer_.data()), static_cast<std::streamsize>(unit_buffer_.size()));
    const auto got = static_cast<std::size_t>(std::max<std::streamsize>(file_.gcount(), 0));
    file_.clear();
    const std::size_t length = th::access_unit_length_bytes(std::span<const std::byte>(unit_buffer_).first(got));
    if (length < 8 || length > got) {
        next_unit_ = info_.units;
        return false;
    }
    const auto unit = std::span<const std::byte>(unit_buffer_).first(length);
    next_offset_ += length;
    ++next_unit_;
    const auto result = decoder_->decode(unit);
    if (!result) {
        ++decode_errors_;
        return true;
    }
    if (!*result) {
        return true;  // waits for a restart header
    }
    const th::DecodedAccessUnit& decoded = **result;
    const auto channels = static_cast<std::size_t>(decoded.channels);
    const auto samples = static_cast<std::size_t>(decoded.samples);
    if (info_.channels == 0) {
        info_.channels = decoded.channels;
        info_.layout.assign(decoded.layout.begin(), decoded.layout.end());
    }
    if (decoded.channels != info_.channels) {
        ++decode_errors_;  // a stream that changes its channel count is not played past the change
        next_unit_ = info_.units;
        return false;
    }
    if (pending_.size() != channels) {
        pending_.assign(channels, {});
    }
    for (std::size_t c = 0; c < channels; ++c) {
        auto& plane = pending_[c];
        plane.reserve(plane.size() + samples);
        for (std::size_t s = 0; s < samples; ++s) {
            plane.push_back(static_cast<float>(decoded.pcm[(s * channels) + c]) / kFullScale);
        }
    }
    decoded_end_ += samples;
    return true;
}

std::size_t TruehdPcmSource::read(std::uint64_t at, std::size_t frames,
                                  std::vector<std::vector<float>>& planar) {
    const std::lock_guard lock{mutex_};
    const auto channels = static_cast<std::size_t>(info_.channels);
    planar.assign(channels, {});
    if (at >= info_.frames || frames == 0) {
        return 0;
    }
    if (!decoder_ || at < pending_start_ || at > decoded_end_) {
        seek_to(at);
    }
    // What is kept is what the caller has not yet taken: the samples before `at` go.
    if (at > pending_start_) {
        const std::uint64_t drop = std::min<std::uint64_t>(at - pending_start_, decoded_end_ - pending_start_);
        for (auto& plane : pending_) {
            plane.erase(plane.begin(), plane.begin() + static_cast<std::ptrdiff_t>(drop));
        }
        pending_start_ += drop;
    }
    while (decoded_end_ < at + frames && decode_next_unit()) {
        // A seek lands on the restart before `at`; the samples ahead of it are dropped as they come.
        if (pending_start_ < at) {
            const std::uint64_t drop = std::min<std::uint64_t>(at - pending_start_, decoded_end_ - pending_start_);
            for (auto& plane : pending_) {
                plane.erase(plane.begin(), plane.begin() + static_cast<std::ptrdiff_t>(drop));
            }
            pending_start_ += drop;
        }
    }
    if (decoded_end_ <= at) {
        return 0;
    }
    const std::size_t delivered = static_cast<std::size_t>(std::min<std::uint64_t>(frames, decoded_end_ - at));
    for (std::size_t c = 0; c < channels; ++c) {
        planar[c].assign(pending_[c].begin(), pending_[c].begin() + static_cast<std::ptrdiff_t>(delivered));
    }
    return delivered;
}

namespace {

// The WAVE_FORMAT_EXTENSIBLE speaker bit of a TrueHD channel, 0 for one the format has no bit for.
std::uint32_t speaker_bit(th::Channel channel) {
    switch (channel) {
        case th::Channel::kL:
            return 0x1;
        case th::Channel::kR:
            return 0x2;
        case th::Channel::kC:
            return 0x4;
        case th::Channel::kLfe:
            return 0x8;
        case th::Channel::kLrs:
            return 0x10;
        case th::Channel::kRrs:
            return 0x20;
        case th::Channel::kLc:
            return 0x40;
        case th::Channel::kRc:
            return 0x80;
        case th::Channel::kCs:
            return 0x100;
        case th::Channel::kLs:
            return 0x200;
        case th::Channel::kRs:
            return 0x400;
        case th::Channel::kTs:
            return 0x800;
        case th::Channel::kLvh:
            return 0x1000;
        case th::Channel::kCvh:
            return 0x2000;
        case th::Channel::kRvh:
            return 0x4000;
        case th::Channel::kLsd:
        case th::Channel::kRsd:
        case th::Channel::kLw:
        case th::Channel::kRw:
        case th::Channel::kLfe2:
            break;
    }
    return 0;
}

void put_le(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
    }
}

}  // namespace

std::uint32_t truehd_wav_channel_mask(std::span<const th::Channel> layout, int channels) {
    if (layout.size() != static_cast<std::size_t>(channels)) {
        return 0;
    }
    std::uint32_t mask = 0;
    std::uint32_t last = 0;
    for (const th::Channel channel : layout) {
        const std::uint32_t bit = speaker_bit(channel);
        if (bit == 0 || bit <= last) {
            return 0;
        }
        mask |= bit;
        last = bit;
    }
    return mask;
}

std::string write_truehd_wav24(TruehdPcmSource& source, const std::filesystem::path& path) {
    const TruehdPcmInfo& info = source.info();
    constexpr std::uint32_t kBits = 24;
    constexpr std::uint32_t kHeaderBytes = 68;
    const auto channels = static_cast<std::uint32_t>(info.channels);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return "Could not write " + path.string() + ".";
    }
    std::vector<std::uint8_t> header;
    const auto tag = [&header](const char* text) {
        for (int i = 0; i < 4; ++i) {
            header.push_back(static_cast<std::uint8_t>(text[i]));
        }
    };
    const std::uint32_t block = channels * kBits / 8;
    tag("RIFF");
    put_le(header, 0xFFFFFFFFU, 4);
    tag("WAVE");
    tag("fmt ");
    put_le(header, 40, 4);
    put_le(header, 0xFFFE, 2);
    put_le(header, channels, 2);
    put_le(header, info.sample_rate, 4);
    put_le(header, info.sample_rate * block, 4);
    put_le(header, block, 2);
    put_le(header, kBits, 2);
    put_le(header, 22, 2);
    put_le(header, kBits, 2);
    put_le(header, truehd_wav_channel_mask(info.layout, info.channels), 4);
    constexpr std::array<std::uint8_t, 16> kPcm{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    header.insert(header.end(), kPcm.begin(), kPcm.end());
    tag("data");
    put_le(header, 0xFFFFFFFFU, 4);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    constexpr std::size_t kWindow = 4096;
    std::vector<std::vector<float>> planar;
    std::vector<std::uint8_t> bytes;
    std::uint64_t at = 0;
    std::uint64_t data_bytes = 0;
    while (true) {
        const std::size_t got = source.read(at, kWindow, planar);
        if (got == 0) {
            break;
        }
        bytes.clear();
        for (std::size_t s = 0; s < got; ++s) {
            for (std::size_t c = 0; c < channels; ++c) {
                const auto value = static_cast<std::int32_t>(std::lround(static_cast<double>(planar[c][s]) * 8388608.0));
                const auto bits = static_cast<std::uint32_t>(value);
                bytes.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>((bits >> 8U) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>((bits >> 16U) & 0xFFU));
            }
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        data_bytes += bytes.size();
        at += got;
    }
    const auto patch = [&out](std::streamoff where, std::uint64_t value) {
        std::vector<std::uint8_t> raw;
        put_le(raw, static_cast<std::uint32_t>(std::min<std::uint64_t>(value, 0xFFFFFFFFU)), 4);
        out.seekp(where);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    };
    patch(4, data_bytes + kHeaderBytes - 8);
    patch(64, data_bytes);
    out.close();
    if (out.fail()) {
        return "Could not write " + path.string() + ".";
    }
    return {};
}

}  // namespace forge_gui
