#include "reference_melody.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"

namespace iclforge::hearth {

namespace {

constexpr std::size_t kNoChannel = static_cast<std::size_t>(-1);

// Where each part is among a frame's decoded channels, as coded (Table C.2.1: 3/0 L M R, 2/1 L R
// V1, 3/1 L M R V1, 2/2 L R V1 V2, 3/2 L M R V1 V2). The decoder's order is the AC-3 one, LFE last.
struct Layout {
    std::size_t melody = kNoChannel;
    std::size_t vocal1 = kNoChannel;
    std::size_t vocal2 = kNoChannel;
};

Layout layout_of(ac3::Acmod acmod) {
    switch (acmod) {
        case ac3::Acmod::k3_0:
            return {1, kNoChannel, kNoChannel};
        case ac3::Acmod::k2_1:
            return {kNoChannel, 2, kNoChannel};
        case ac3::Acmod::k3_1:
            return {1, 3, kNoChannel};
        case ac3::Acmod::k2_2:
            return {kNoChannel, 2, 3};
        case ac3::Acmod::k3_2:
            return {1, 3, 4};
        default:
            return {};
    }
}

std::size_t channel_of(const Layout& layout, ReferencePart part) {
    switch (part) {
        case ReferencePart::kMelodyGuide:
            return layout.melody;
        case ReferencePart::kVocal1:
            return layout.vocal1;
        case ReferencePart::kVocal2:
            return layout.vocal2;
    }
    return kNoChannel;
}

const char* describe_part(ReferencePart part) {
    switch (part) {
        case ReferencePart::kMelodyGuide:
            return "the melody guide (M)";
        case ReferencePart::kVocal1:
            return "vocal 1 (V1)";
        case ReferencePart::kVocal2:
            return "vocal 2 (V2)";
    }
    return "";
}

}  // namespace

std::expected<ReferenceMelody, std::string> reference_from_karaoke(
    std::span<const std::byte> ac3_stream, std::optional<ReferencePart> prefer,
    const std::atomic<bool>* cancel) {
    const auto frames = ac3::split_frames(ac3_stream);
    if (!frames || frames->empty()) {
        return std::unexpected(std::string{"The stream has no AC-3 frames to take a melody from."});
    }

    // As coded: the library's defaults leave the channels where the stream puts them.
    ac3::FrameDecoder decoder{ac3::DecoderConfig{}};
    std::vector<float> samples;
    samples.reserve(frames->size() * static_cast<std::size_t>(ac3::kSamplesPerFrame));

    std::size_t channel = kNoChannel;
    ReferencePart part = ReferencePart::kMelodyGuide;
    std::uint32_t rate = 0;
    std::size_t index = 0;
    for (const auto& frame_bytes : *frames) {
        if (cancel != nullptr && (index % 256U) == 0U && cancel->load(std::memory_order_relaxed)) {
            return std::unexpected(std::string{"Reading the melody was cancelled."});
        }
        ++index;
        const auto decoded = decoder.decode_frame(frame_bytes);
        if (!decoded) {
            // A frame that does not decode is a frame of silence: the timeline must not move.
            if (channel != kNoChannel) {
                samples.insert(samples.end(), static_cast<std::size_t>(ac3::kSamplesPerFrame),
                               0.0F);
            }
            continue;
        }
        if (channel == kNoChannel) {
            if (!ac3::meta::is_karaoke(decoded->info.bsmod, decoded->acmod)) {
                return std::unexpected(
                    std::string{"The stream is not a karaoke stream, so it has no melody part."});
            }
            const Layout layout = layout_of(decoded->acmod);
            const std::array<ReferencePart, 3> order = {
                ReferencePart::kMelodyGuide, ReferencePart::kVocal1, ReferencePart::kVocal2};
            if (prefer && channel_of(layout, *prefer) != kNoChannel) {
                part = *prefer;
                channel = channel_of(layout, *prefer);
            } else {
                for (const ReferencePart candidate : order) {
                    if (channel_of(layout, candidate) != kNoChannel) {
                        part = candidate;
                        channel = channel_of(layout, candidate);
                        break;
                    }
                }
            }
            if (channel == kNoChannel) {
                return std::unexpected(
                    std::string{"The karaoke stream carries no melody or vocal part."});
            }
            rate = ac3::sample_rate_hz(decoded->sample_rate);
        }
        if (channel >= decoded->channels.size()) {
            return std::unexpected(
                std::string{"A frame of the stream has fewer channels than its first."});
        }
        const std::vector<float>& pcm = decoded->channels[channel];
        samples.insert(samples.end(), pcm.begin(), pcm.end());
    }
    if (channel == kNoChannel || rate == 0 || samples.empty()) {
        return std::unexpected(std::string{"No frame of the stream could be decoded."});
    }

    ReferenceMelody reference;
    reference.part = part;
    reference.description = describe_part(part);
    reference.contour = pitch::contour_of_signal(samples, rate);
    const std::size_t voiced = reference.contour.voiced_frames();
    if (voiced < 20) {
        return std::unexpected(std::string{"The "} + reference.description +
                               " has no stretch of clear pitch in it.");
    }
    reference.voiced_share =
        static_cast<double>(voiced) /
        static_cast<double>(std::max<std::size_t>(reference.contour.hz.size(), 1));
    return reference;
}

}  // namespace iclforge::hearth
