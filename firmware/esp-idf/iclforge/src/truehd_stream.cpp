// The TrueHD decoder as the burst player uses it. See truehd_stream.hpp.

#include "truehd_stream.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

#include "sdkconfig.h"

#include "esp_heap_caps.h"

#include "iclforge/render/render.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

namespace iclforge {

namespace {

namespace th = iclforge::truehd;
using iclforge::ac3::eac3::chanmap::Location;

// The least heap a TrueHD play asks for: the decoder's 158,020 B (firmware/truehd-probe, the same on
// the ESP32-C6, -P4 and -S3) and the unit buffers beside it, with 24 KB for the rest of the stream's
// own allocations.
constexpr std::size_t kHeapFloorBytes = 200 * 1024;

// The probe's high-water mark was 60,944 B; the owner's decision is at least 64 KiB.
constexpr std::size_t kStackBytes = 64 * 1024;

// A TrueHD channel as the renderer's location. The same mapping apps/hearth/engine/src/
// stream_decoder.cpp (truehd_bed) reads a unit's layout with, which a firmware component cannot link.
[[nodiscard]] Location location_of(th::Channel channel) {
    switch (channel) {
        case th::Channel::kL: return Location::kLeft;
        case th::Channel::kR: return Location::kRight;
        case th::Channel::kC: return Location::kCentre;
        case th::Channel::kLfe: return Location::kLfe;
        case th::Channel::kLs: return Location::kLeftSurround;
        case th::Channel::kRs: return Location::kRightSurround;
        case th::Channel::kLvh: return Location::kVhl;
        case th::Channel::kRvh: return Location::kVhr;
        case th::Channel::kLc: return Location::kLc;
        case th::Channel::kRc: return Location::kRc;
        case th::Channel::kLrs: return Location::kLrs;
        case th::Channel::kRrs: return Location::kRrs;
        case th::Channel::kCs: return Location::kCs;
        case th::Channel::kTs: return Location::kTs;
        case th::Channel::kLsd: return Location::kLsd;
        case th::Channel::kRsd: return Location::kRsd;
        case th::Channel::kLw: return Location::kLw;
        case th::Channel::kRw: return Location::kRw;
        case th::Channel::kCvh: return Location::kVhc;
        case th::Channel::kLfe2: return Location::kLfe2;
    }
    return Location::kLeft;
}

// The bed a unit's channels make; empty when the major sync names none, or a speaker twice (the
// renderer has no place for the second).
[[nodiscard]] std::optional<iclforge::ac3::eac3::chanmap::Layout> bed_of(
    std::span<const th::Channel> channels) {
    if (channels.empty() || channels.size() > iclforge::render::LayoutRenderer::kMaxCoded) {
        return std::nullopt;
    }
    iclforge::ac3::eac3::chanmap::Layout bed;
    for (const th::Channel channel : channels) {
        const Location location = location_of(channel);
        if (bed.index_of(location) >= 0) {
            return std::nullopt;
        }
        bed.items[static_cast<std::size_t>(bed.count++)] = location;
    }
    return bed;
}

// The A/52 audio coding mode the front and surround speakers make (the same reading as
// stream_decoder.cpp's acmod_of_bed).
[[nodiscard]] int acmod_of(const iclforge::ac3::eac3::chanmap::Layout& bed) {
    if (bed.index_of(Location::kLeft) < 0) {
        return 1;
    }
    const bool centre = bed.index_of(Location::kCentre) >= 0;
    if (bed.index_of(Location::kLeftSurround) >= 0) {
        return centre ? 7 : 6;
    }
    return centre ? 3 : 2;
}

// The samples of one unit at a rate: 40 at 48 and 44.1 kHz, doubled at each doubling.
[[nodiscard]] std::size_t samples_per_unit(std::uint32_t rate_hz) {
    if (rate_hz >= 176400) {
        return 160;
    }
    return rate_hz >= 88200 ? 80 : 40;
}

}  // namespace

struct TruehdStream::Impl {
    std::unique_ptr<th::Decoder> decoder;
    Config config;
    std::string_view reason;
    std::size_t unit_samples = 0;
    Report report;
    // The layout of the last unit that decoded, which a unit that gives nothing is silence in.
    std::optional<iclforge::ac3::eac3::chanmap::Layout> bed;
    // One channel's samples of the unit being handed over, and the spans that name them.
    std::array<std::array<float, kMaxUnitSamples>, iclforge::render::LayoutRenderer::kMaxCoded>
        planes{};
    std::array<std::span<const float>, iclforge::render::LayoutRenderer::kMaxCoded> spans{};
};

TruehdStream::TruehdStream() : impl_(std::make_unique<Impl>()) {}

TruehdStream::~TruehdStream() = default;

bool TruehdStream::available() {
    return true;
}

std::size_t TruehdStream::heap_floor_bytes() {
    return kHeapFloorBytes;
}

std::size_t TruehdStream::stack_bytes_needed() {
    return kStackBytes;
}

std::size_t TruehdStream::chunk_samples(std::span<const std::byte> chunk,
                                        std::uint32_t sample_rate_hz) {
    std::size_t units = 0;
    for (std::size_t at = 0; at < chunk.size();) {
        const std::size_t length = th::access_unit_length_bytes(chunk.subspan(at));
        if (length < 4 || at + length > chunk.size()) {
            return 0;
        }
        ++units;
        at += length;
    }
    return units * samples_per_unit(sample_rate_hz);
}

bool TruehdStream::open(const Config& config) {
    Impl& im = *impl_;
    im.config = config;
    if (im.decoder) {
        return true;
    }
    const std::size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < kHeapFloorBytes) {
        im.reason = "not enough free heap for a TrueHD decoder";
        return false;
    }
    im.decoder = std::make_unique<th::Decoder>(th::DecoderConfig{});
    return true;
}

bool TruehdStream::is_open() const {
    return impl_->decoder != nullptr;
}

void TruehdStream::reset() {
    Impl& im = *impl_;
    if (im.decoder) {
        im.decoder->reset();
    }
    im.reason = {};
    im.bed.reset();
    im.unit_samples = 0;
}

void TruehdStream::close() {
    impl_->decoder.reset();
    impl_->bed.reset();
}

TruehdStream::Result TruehdStream::decode(std::span<const std::byte> chunk, BlockFn fn,
                                          void* context) {
    Impl& im = *impl_;
    im.reason = {};
    if (!im.decoder) {
        im.reason = "no decoder is open";
        return Result::kInvalid;
    }
    // Whole units first: a chunk that is not is refused before any of it plays.
    std::array<std::span<const std::byte>, kMaxUnitsPerChunk> units;
    std::size_t count = 0;
    for (std::size_t at = 0; at < chunk.size();) {
        const std::size_t length = th::access_unit_length_bytes(chunk.subspan(at));
        if (length < 4 || at + length > chunk.size() || count == kMaxUnitsPerChunk) {
            im.reason = "not whole TrueHD access units";
            return Result::kInvalid;
        }
        units[count++] = chunk.subspan(at, length);
        at += length;
    }
    if (count == 0) {
        im.reason = "an empty TrueHD chunk";
        return Result::kInvalid;
    }

    bool any = false;
    for (std::size_t n = 0; n < count; ++n) {
        const auto decoded = im.decoder->decode(units[n]);
        if (decoded && decoded->has_value()) {
            const th::DecodedAccessUnit& unit = **decoded;
            if (unit.sample_rate != im.config.sample_rate_hz) {
                im.reason = "a sample rate the player does not play";
                return Result::kRefused;
            }
            const auto bed = bed_of(unit.layout);
            if (!bed || unit.samples <= 0 ||
                static_cast<std::size_t>(unit.samples) > kMaxUnitSamples ||
                static_cast<std::size_t>(unit.channels) != unit.layout.size()) {
                im.reason = "a channel layout the renderer does not place";
                return Result::kRefused;
            }
            if (im.config.max_channels != 0 &&
                static_cast<std::size_t>(unit.channels) > im.config.max_channels) {
                im.reason = "more channels than this board decodes of TrueHD";
                return Result::kRefused;
            }
            const std::size_t samples = static_cast<std::size_t>(unit.samples);
            const std::size_t channels = static_cast<std::size_t>(unit.channels);
            // 24-bit values, sign-extended in an int32, to the renderer's floats; interleaved to a
            // plane a channel.
            constexpr float kScale = 1.0F / 8388608.0F;
            for (std::size_t s = 0; s < samples; ++s) {
                for (std::size_t c = 0; c < channels; ++c) {
                    im.planes[c][s] = static_cast<float>(unit.pcm[s * channels + c]) * kScale;
                }
            }
            for (std::size_t c = 0; c < channels; ++c) {
                im.spans[c] = std::span<const float>(im.planes[c].data(), samples);
            }
            im.bed = bed;
            im.unit_samples = samples;
            im.report.channels = unit.channels;
            im.report.lfe = bed->index_of(Location::kLfe) >= 0;
            im.report.acmod = acmod_of(*bed);
            fn(context, Block{.channels = std::span<const std::span<const float>>(im.spans.data(), channels),
                              .bed = *bed,
                              .samples = samples,
                              .index = static_cast<int>(n)});
            any = true;
            continue;
        }
        // A unit that gave nothing, or that did not read: silence for its samples, in the layout
        // the stream last had, so the output is as long as the chunk says. Without a unit decoded
        // yet the rate gives the length and the pair of front speakers the layout.
        const std::size_t samples =
            im.unit_samples != 0 ? im.unit_samples : samples_per_unit(im.config.sample_rate_hz);
        iclforge::ac3::eac3::chanmap::Layout bed;
        if (im.bed) {
            bed = *im.bed;
        } else {
            bed.items[0] = Location::kLeft;
            bed.items[1] = Location::kRight;
            bed.count = 2;
        }
        const std::size_t channels = static_cast<std::size_t>(bed.count);
        for (std::size_t c = 0; c < channels; ++c) {
            std::fill_n(im.planes[c].begin(), samples, 0.0F);
            im.spans[c] = std::span<const float>(im.planes[c].data(), samples);
        }
        fn(context, Block{.channels = std::span<const std::span<const float>>(im.spans.data(), channels),
                          .bed = bed,
                          .samples = samples,
                          .index = static_cast<int>(n)});
        if (!decoded) {
            im.reason = "a unit the decoder refused";
        }
    }
    return any ? Result::kDecoded : Result::kWaiting;
}

std::string_view TruehdStream::reason() const {
    return impl_->reason;
}

std::size_t TruehdStream::unit_samples() const {
    return impl_->unit_samples;
}

const TruehdStream::Report& TruehdStream::report() const {
    return impl_->report;
}

}  // namespace iclforge
