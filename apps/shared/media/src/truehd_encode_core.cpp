#include "truehd_encode_core.hpp"

#include <fmt/format.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iclforge::apps::truehd_encode {

namespace th = iclforge::truehd;

namespace {

constexpr std::size_t kChunkFrames = 65536;
constexpr double kFullScale = 8388608.0;  // 2^23: a 24-bit sample against the WAV reader's [-1, 1)

// The TrueHD channel a WAVE_FORMAT_EXTENSIBLE speaker bit (0-based) stands for: the inverse of what
// `decode` writes in its channel mask.
std::optional<th::Channel> channel_of_speaker_bit(unsigned bit, bool has_side_pair) {
    switch (bit) {
        case 0:
            return th::Channel::kL;
        case 1:
            return th::Channel::kR;
        case 2:
            return th::Channel::kC;
        case 3:
            return th::Channel::kLfe;
        case 4:
            return has_side_pair ? th::Channel::kLrs : th::Channel::kLs;
        case 5:
            return has_side_pair ? th::Channel::kRrs : th::Channel::kRs;
        case 6:
            return th::Channel::kLc;
        case 7:
            return th::Channel::kRc;
        case 8:
            return th::Channel::kCs;
        case 9:
            return th::Channel::kLs;
        case 10:
            return th::Channel::kRs;
        case 11:
            return th::Channel::kTs;
        case 12:
            return th::Channel::kLvh;
        case 13:
            return th::Channel::kCvh;
        case 14:
            return th::Channel::kRvh;
        default:
            return std::nullopt;
    }
}

// The WAV reader's [-1, 1) floats back to the integers they were read from: a 16 or 24 bit PCM
// sample survives the trip exactly (a float32 holds 24 bits), and a wider one or a float is rounded
// to 24 bits. Stream channel k comes from WAV channel `wav_index[k]`.
void interleave(const iclforge::ac3::io::WavData& wav, std::span<const std::size_t> wav_index,
                std::size_t begin, std::size_t end, std::vector<std::int32_t>& out) {
    const std::size_t channels = wav.channels.size();
    out.resize((end - begin) * channels);
    for (std::size_t i = begin; i < end; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            const double scaled =
                std::nearbyint(static_cast<double>(wav.channels[wav_index[c]][i]) * kFullScale);
            out[(i - begin) * channels + c] =
                static_cast<std::int32_t>(std::clamp(scaled, -kFullScale, kFullScale - 1.0));
        }
    }
}

}  // namespace

std::expected<Layout, std::string> layout_of(const iclforge::ac3::io::WavData& wav) {
    const std::size_t count = wav.channels.size();
    Layout layout;
    layout.wav_index.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        layout.wav_index[i] = i;
    }
    if (wav.channel_mask == 0) {
        return layout;
    }
    if (static_cast<std::size_t>(std::popcount(wav.channel_mask)) != count) {
        layout.warnings.push_back(fmt::format(
            "the WAV's channel mask names {} speakers for {} channels; the channels are taken in "
            "the order decode writes them",
            std::popcount(wav.channel_mask), count));
        return layout;
    }
    const bool has_side_pair = (wav.channel_mask & 0x600U) != 0;
    std::vector<th::Channel> speakers;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if ((wav.channel_mask & (1U << bit)) == 0) {
            continue;
        }
        const auto channel = channel_of_speaker_bit(bit, has_side_pair);
        if (!channel) {
            return std::unexpected(fmt::format(
                "the WAV's channel mask has a speaker (bit {}) that TrueHD has no channel for",
                bit));
        }
        speakers.push_back(*channel);
    }
    const auto has = [&](th::Channel channel) {
        return std::ranges::find(speakers, channel) != speakers.end();
    };
    std::size_t covered = 0;
    for (unsigned bit = 0; bit < th::kChannelAssignmentBits; ++bit) {
        const std::vector<th::Channel> named = th::channels_of_assignment(1U << bit);
        if (!named.empty() && std::ranges::all_of(named, has)) {
            layout.assignment |= 1U << bit;
            covered += named.size();
        }
    }
    if (covered != speakers.size()) {
        return std::unexpected(
            std::string{"the WAV's speakers are not a TrueHD channel assignment (L R, C, LFE, Ls "
                        "Rs, Lvh Rvh, Lc Rc, Lrs Rrs, Cs, Ts, Lsd Rsd, Lw Rw, Cvh, LFE2 as pairs "
                        "and singles)"});
    }
    const std::vector<th::Channel> order = th::channels_of_assignment(layout.assignment);
    for (std::size_t k = 0; k < order.size() && k < count; ++k) {
        layout.wav_index[k] =
            static_cast<std::size_t>(std::ranges::find(speakers, order[k]) - speakers.begin());
    }
    return layout;
}

std::expected<Outcome, Failure> encode_wav(const iclforge::ac3::io::WavData& wav,
                                           const Layout& layout, th::EncoderConfig config,
                                           const Progress& progress) {
    config.channel_assignment = layout.assignment;
    config.sample_rate = wav.sample_rate;
    config.channels = static_cast<int>(wav.channels.size());
    config.bits = 24;
    auto encoder = th::Encoder::create(config);
    if (!encoder) {
        return std::unexpected(Failure{.error = encoder.error()});
    }
    Outcome out;
    out.frames = wav.frame_count();
    out.channels = config.channels;
    out.sample_rate = config.sample_rate;
    const auto take = [&out](const std::vector<th::EncodedAccessUnit>& units) {
        for (const th::EncodedAccessUnit& unit : units) {
            out.stream.insert(out.stream.end(), unit.bytes.begin(), unit.bytes.end());
            ++out.access_units;
        }
    };
    std::vector<std::int32_t> pcm;
    const std::size_t frames = wav.frame_count();
    for (std::size_t at = 0; at < frames; at += kChunkFrames) {
        const std::size_t end = std::min(frames, at + kChunkFrames);
        interleave(wav, layout.wav_index, at, end, pcm);
        const auto units = encoder->encode(pcm);
        if (!units) {
            return std::unexpected(Failure{.error = units.error()});
        }
        take(*units);
        if (progress && !progress(end, frames)) {
            return std::unexpected(
                Failure{.error = {th::EncodeErrorKind::kInternal, "stopped by the caller"},
                        .cancelled = true});
        }
    }
    const auto last = encoder->flush();
    if (!last) {
        return std::unexpected(Failure{.error = last.error()});
    }
    take(*last);
    out.stats = encoder->stats();
    out.substreams = encoder->substreams();
    return out;
}

}  // namespace iclforge::apps::truehd_encode
