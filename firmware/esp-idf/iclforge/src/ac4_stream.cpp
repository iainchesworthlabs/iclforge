// The AC-4 decoder as the burst player uses it. See ac4_stream.hpp.

#include "ac4_stream.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

#include "sdkconfig.h"

#include "esp_heap_caps.h"

#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/decoder/dynamic_range.hpp"
#include "iclforge/render/render.hpp"

#include "ac4_bridge.hpp"
#include "iclforge/ac4_sync_frame.hpp"

namespace iclforge {

namespace {

// The least heap any AC-4 decode has asked for: the 2.0 fixtures' peak since D14f (planning/
// ac4.md), as player.cpp's own floor has it.
constexpr std::size_t kHeapFloorBytes = 286365;

// The A/52 audio coding mode the decoded channels make: what the report says of an AC-4 stream,
// whose channel modes are all one of 1/0, 2/0, 3/0, 2/2 or 3/2 there, with any back, wide or height
// pair left out (apps/hearth/testsink/src/burst_output.cpp's ac4_acmod).
[[nodiscard]] int acmod_of(std::span<const iclforge::ac4::Speaker> speakers) {
    const auto has = [&](iclforge::ac4::Speaker speaker) {
        return std::find(speakers.begin(), speakers.end(), speaker) != speakers.end();
    };
    if (!has(iclforge::ac4::Speaker::kLeft)) {
        return 1;
    }
    if (has(iclforge::ac4::Speaker::kLeftSurround)) {
        return has(iclforge::ac4::Speaker::kCentre) ? 7 : 6;
    }
    return has(iclforge::ac4::Speaker::kCentre) ? 3 : 2;
}

}  // namespace

struct Ac4Stream::Impl {
    std::optional<iclforge::ac4::Decoder> decoder;
    Config config;
    // The output processing the decoder was made with, which the listener's dynamic range is
    // applied over (iclforge::ac4::follow_dynamic_range).
    iclforge::ac4::OutputConfig base_output;
    bool placement = false;
    std::string_view reason;
    std::size_t frame_samples = 0;
    Report report;
};

Ac4Stream::Ac4Stream() : impl_(std::make_unique<Impl>()) {}

Ac4Stream::~Ac4Stream() {
    end_stream();
}

bool Ac4Stream::available() {
    return true;
}

std::size_t Ac4Stream::heap_floor_bytes() {
    return kHeapFloorBytes;
}

void Ac4Stream::begin_stream() {
#if CONFIG_SPIRAM_USE_MALLOC
    if (!impl_->placement) {
        impl_->placement = true;
        heap_caps_malloc_extmem_enable(
            static_cast<std::size_t>(CONFIG_ICLFORGE_AC4_INTERNAL_BELOW));
    }
#endif
}

void Ac4Stream::end_stream() {
#if CONFIG_SPIRAM_USE_MALLOC
    if (impl_ && impl_->placement) {
        impl_->placement = false;
        heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    }
#endif
}

bool Ac4Stream::open(const Config& config) {
    Impl& im = *impl_;
    im.reason = {};
    if (im.decoder) {
        im.config = config;
        return true;
    }
    if (const std::size_t room = heap_caps_get_free_size(MALLOC_CAP_8BIT); room < kHeapFloorBytes) {
        std::printf(
            "burst player: AC-4 needs at least %lu bytes of heap and %lu are free: refused\n",
            static_cast<unsigned long>(kHeapFloorBytes), static_cast<unsigned long>(room));
        im.reason = "less heap than an AC-4 decode needs";
        return false;
    }
    iclforge::ac4::DecoderConfig decoder_config;
    decoder_config.output.downmix = ac4bridge::target(config.fold);
    decoder_config.executor = config.executor;
    decoder_config.skip_objects = config.skip_objects;
    im.base_output = decoder_config.output;
    im.config = config;
    im.decoder.emplace(decoder_config);
    return true;
}

bool Ac4Stream::is_open() const {
    return impl_->decoder.has_value();
}

void Ac4Stream::reset() {
    if (impl_->decoder) {
        impl_->decoder->reset();
        // The next stream's first frame is not played at the level the last one's dialnorm gave
        // (late night only; standard never leaves the base).
        (void)iclforge::ac4::restore_base_output(*impl_->decoder, impl_->base_output);
    }
}

void Ac4Stream::close() {
    impl_->decoder.reset();
}

Ac4Stream::Result Ac4Stream::decode(std::span<const std::byte> sync_frame, BlockFn fn,
                                    void* context) {
    Impl& im = *impl_;
    im.reason = {};
    if (!im.decoder) {
        im.reason = "no AC-4 decoder is open";
        return Result::kInvalid;
    }
    const std::span<const std::byte> raw = ac4_raw_frame(sync_frame);
    if (raw.empty()) {
        im.reason = "not a whole AC-4 sync frame";
        return Result::kInvalid;
    }
    std::string_view refused;
    int index = 0;
    const auto deliver = [&](const iclforge::ac4::PcmBlock& block) {
        if (!refused.empty()) {
            return;
        }
        if (block.sample_rate_hz != static_cast<int>(im.config.sample_rate_hz)) {
            refused = "a sample rate the player does not play";
            return;
        }
        if (block.speakers.size() > iclforge::render::LayoutRenderer::kMaxCoded ||
            !ac4bridge::placeable(block.speakers)) {
            refused = "a channel layout the renderer does not place";
            return;
        }
        if (im.config.max_channels != 0 && block.speakers.size() > im.config.max_channels) {
            refused = "more channels than this board decodes of AC-4";
            return;
        }
        const Block out{.channels = block.channels,
                        .bed = ac4bridge::bed(block.speakers),
                        .samples = block.samples,
                        .index = index++};
        fn(context, out);
    };
    const auto decoded = im.decoder->decode_by_block(raw, deliver);
    if (!refused.empty()) {
        im.reason = refused;
        return Result::kRefused;
    }
    if (!decoded) {
        im.reason = im.decoder->refusal_reason();
        if (im.reason.empty()) {
            im.reason = "the decoder refused the frame";
        }
        return Result::kInvalid;
    }
    if (!decoded->has_value()) {
        return Result::kWaiting;
    }
    const iclforge::ac4::FrameInfo& info = **decoded;
    im.frame_samples = info.samples;
    // Late night follows the stream's dialnorm and DRC modes, which this frame has now said.
    (void)iclforge::ac4::follow_dynamic_range(*im.decoder, im.config.dynamic_range, im.base_output);
    // A stream's frames come faster than the board keeps up with: the converter's cost is the
    // same for a frame at 29.97 a second as at 24, so the play would fall behind and underrun.
    // The slack covers the 1000/1001 rates, whose frames alternate between two lengths.
    constexpr double kRateSlack = 1.005;
    if (im.config.max_frame_rate != 0 && info.samples != 0 &&
        static_cast<double>(im.config.sample_rate_hz) / static_cast<double>(info.samples) >
            static_cast<double>(im.config.max_frame_rate) * kRateSlack) {
        im.reason = "more frames a second than this board decodes of AC-4";
        return Result::kRefused;
    }
    im.report.channels = static_cast<int>(info.speakers.size());
    im.report.lfe = std::find(info.speakers.begin(), info.speakers.end(),
                              iclforge::ac4::Speaker::kLfe) != info.speakers.end();
    im.report.acmod = acmod_of(info.speakers);
    im.report.dialnorm_db = im.decoder->metadata().loudness.dialnorm_dbfs.value_or(0.0);
    return Result::kDecoded;
}

std::string_view Ac4Stream::reason() const {
    return impl_->reason;
}

std::size_t Ac4Stream::frame_samples() const {
    return impl_->frame_samples;
}

const Ac4Stream::Report& Ac4Stream::report() const {
    return impl_->report;
}

}  // namespace iclforge
