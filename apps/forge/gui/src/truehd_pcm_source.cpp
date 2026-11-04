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
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace forge_gui {

namespace th = iclforge::truehd;

namespace {

// A 24-bit sample, sign-extended in an int32, to +-1.0.
constexpr float kFullScale = 8388608.0F;

// A file read by offset, as Hearth's loader reads one (apps/hearth/ui/src/item_loader.cpp): the
// cursor asks for a window at a time, so a film's lossless track is not held in memory.
class FileByteSource final : public iclforge::apps::ByteSource {
   public:
    explicit FileByteSource(const std::filesystem::path& path) : file_(path, std::ios::binary) {
        if (file_) {
            file_.seekg(0, std::ios::end);
            const std::streamoff end = file_.tellg();
            ok_ = end >= 0;
            size_ = ok_ ? static_cast<std::uint64_t>(end) : 0;
        }
    }

    [[nodiscard]] bool ok() const { return ok_; }

    [[nodiscard]] std::uint64_t size() const override { return size_; }

    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        if (offset >= size_ || out.empty()) {
            return 0;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<std::size_t>(std::max<std::streamsize>(file_.gcount(), 0));
    }

   private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
    bool ok_ = false;
};

}  // namespace

std::expected<std::unique_ptr<TruehdPcmSource>, std::string> TruehdPcmSource::open(
    const std::filesystem::path& path, th::Presentation presentation) {
    std::unique_ptr<TruehdPcmSource> source{new TruehdPcmSource()};
    source->path_ = path;
    source->presentation_ = presentation;
    auto bytes = std::make_unique<FileByteSource>(path);
    if (!bytes->ok()) {
        return std::unexpected("Could not open " + path.string() + ".");
    }
    source->bytes_ = std::move(bytes);
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
    // The index is one pass over the stream (the units that open with a major sync, thinned, and
    // the padding at the end), made by the code Hearth plays from.
    auto index = iclforge::apps::index_truehd(*bytes_);
    if (!index) {
        return path_.string() + " is not a TrueHD or MLP stream (" + index.error() + ").";
    }
    index_ = std::move(*index);
    cursor_ = std::make_unique<iclforge::apps::TruehdCursor>(*bytes_);
    info_.mlp = index_.kind == th::StreamKind::kMlp;
    info_.sample_rate = index_.sample_rate;
    info_.samples_per_unit = static_cast<int>(index_.samples_per_unit);
    info_.units = index_.units;
    info_.restarts = index_.major_syncs;
    info_.frames = index_.samples();

    // TH spec section 18 over the unit lengths: a second walk of the stream, since the index keeps
    // no lengths. The model needs the declared rate, which the first unit's major sync gives.
    if (!cursor_->rewind()) {
        return path_.string() + " does not open with an access unit that carries a major sync.";
    }
    iclforge::apps::truehd_probe::RateModel rate_model;
    bool first = true;
    while (const auto unit = cursor_->next()) {
        std::optional<th::MajorSync> first_sync;
        if (first) {
            if (const auto parsed = th::parse_major_sync(unit->subspan(4), false)) {
                first_sync = *parsed;
            }
            first = false;
        }
        rate_model.add(*unit, first_sync);
    }
    info_.rate = rate_model.report();
    return {};
}

void TruehdPcmSource::seek_to(std::uint64_t frame) {
    const std::uint64_t samples_per_unit = static_cast<std::uint64_t>(info_.samples_per_unit);
    const iclforge::apps::TruehdRestart& restart = index_.restart_at_or_before(frame / samples_per_unit);
    decoder_ = std::make_unique<th::Decoder>(
        th::DecoderConfig{.presentation = presentation_, .strictness = th::Strictness::kLenient});
    cursor_->seek(restart);
    ended_ = false;
    pending_start_ = restart.unit * samples_per_unit;
    decoded_end_ = pending_start_;
    for (auto& channel : pending_) {
        channel.clear();
    }
}

bool TruehdPcmSource::decode_next_unit() {
    if (ended_) {
        return false;
    }
    const auto unit = cursor_->next();
    if (!unit) {
        ended_ = true;
        return false;
    }
    const auto result = decoder_->decode(*unit);
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
        ended_ = true;
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
