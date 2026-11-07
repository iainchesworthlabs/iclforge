#include "truehd_encode_core.hpp"

#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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


namespace {

// The assignment bits whose channels are all among `speakers`, and how many channels they cover.
struct Cover {
    std::uint32_t assignment = 0;
    std::size_t covered = 0;
};

Cover cover_of(std::span<const th::Channel> speakers) {
    Cover cover;
    const auto has = [&](th::Channel channel) {
        return std::ranges::find(speakers, channel) != speakers.end();
    };
    for (unsigned bit = 0; bit < th::kChannelAssignmentBits; ++bit) {
        const std::vector<th::Channel> named = th::channels_of_assignment(1U << bit);
        if (!named.empty() && std::ranges::all_of(named, has)) {
            cover.assignment |= 1U << bit;
            cover.covered += named.size();
        }
    }
    return cover;
}

constexpr std::uint32_t kEveryAssignmentBit = (1U << th::kChannelAssignmentBits) - 1U;

// `channels` as their names in an assignment's order, space separated.
std::string names_of(std::span<const th::Channel> channels) {
    std::string out;
    for (const th::Channel channel : th::channels_of_assignment(kEveryAssignmentBit)) {
        if (std::ranges::find(channels, channel) != channels.end()) {
            out += out.empty() ? "" : " ";
            out += th::channel_name(channel);
        }
    }
    return out;
}

// The sentence for a set of places that is not an assignment: which group lacks a member, and the
// two sets nearest to it (every such group completed, and every such group left out).
std::string not_an_assignment(std::span<const th::Channel> speakers) {
    const auto in_set = [&](th::Channel channel) {
        return std::ranges::find(speakers, channel) != speakers.end();
    };
    std::vector<th::Channel> completed(speakers.begin(), speakers.end());
    std::vector<th::Channel> trimmed(speakers.begin(), speakers.end());
    std::string needs;
    std::string groups;
    for (unsigned bit = 0; bit < th::kChannelAssignmentBits; ++bit) {
        const std::vector<th::Channel> group = th::channels_of_assignment(1U << bit);
        groups += fmt::format("{}{}", groups.empty() ? "" : ", ", names_of(group));
        const auto present = static_cast<std::size_t>(std::ranges::count_if(group, in_set));
        if (present == 0 || present == group.size()) {
            continue;
        }
        for (const th::Channel channel : group) {
            if (in_set(channel)) {
                std::erase(trimmed, channel);
            } else {
                completed.push_back(channel);
            }
        }
        needs += fmt::format("{}{} needs {}", needs.empty() ? "" : "; ",
                             th::channel_name(*std::ranges::find_if(group, in_set)),
                             th::channel_name(*std::ranges::find_if_not(group, in_set)));
    }
    std::string nearest = names_of(completed);
    if (!trimmed.empty()) {
        nearest += " or " + names_of(trimmed);
    }
    return fmt::format(
        "map= places {}, which is not a TrueHD channel set: the channels come in these groups, "
        "each whole or not at all ({}). {}. The nearest sets are {}",
        names_of(speakers), groups, needs, nearest);
}

}  // namespace

std::optional<th::Channel> channel_named(std::string_view name) {
    for (const th::Channel channel : th::channels_of_assignment(kEveryAssignmentBit)) {
        const std::string_view candidate = th::channel_name(channel);
        if (std::ranges::equal(candidate, name, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return channel;
        }
    }
    return std::nullopt;
}

std::expected<std::vector<Placement>, std::string> parse_map(std::string_view spec) {
    std::vector<Placement> out;
    std::size_t at = 0;
    while (at <= spec.size()) {
        const std::size_t comma = std::min(spec.find(',', at), spec.size());
        const std::string_view entry = spec.substr(at, comma - at);
        at = comma + 1;
        const auto bad = [&entry](std::string_view why) {
            return std::unexpected(fmt::format(
                "map= entry '{}': {} (each is <source>.<channel>:<place>, as in 0.0:L,0.1:R,1.0:C)",
                entry, why));
        };
        const std::size_t dot = entry.find('.');
        const std::size_t colon = entry.find(':');
        if (dot == std::string_view::npos || colon == std::string_view::npos || colon < dot) {
            return bad("needs a source, a channel and a place");
        }
        Placement placement;
        const auto number = [](std::string_view text, std::size_t& value) {
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && ec == std::errc{} && ptr == text.data() + text.size();
        };
        if (!number(entry.substr(0, dot), placement.source) ||
            !number(entry.substr(dot + 1, colon - dot - 1), placement.channel)) {
            return bad("the source and the channel are numbers from 0");
        }
        const auto place = channel_named(entry.substr(colon + 1));
        if (!place) {
            return std::unexpected(fmt::format(
                "map= entry '{}': '{}' is not a TrueHD channel (L R C LFE Ls Rs Lvh Rvh Lc Rc Lrs "
                "Rrs Cs Ts Lsd Rsd Lw Rw Cvh LFE2)",
                entry, entry.substr(colon + 1)));
        }
        placement.place = *place;
        out.push_back(placement);
    }
    return out;
}

std::expected<Assembled, std::string> assemble(std::span<const iclforge::ac3::io::WavData> sources,
                                               std::span<const std::size_t> offset_samples,
                                               std::span<const Placement> placements) {
    if (placements.empty()) {
        return std::unexpected(std::string{"map= names no channel"});
    }
    std::vector<th::Channel> places;
    std::vector<bool> used(sources.size(), false);
    for (const Placement& p : placements) {
        if (p.source >= sources.size()) {
            return std::unexpected(fmt::format(
                "map= names source {}, but there {} {} (the positional input is source 0, each src= "
                "the next)",
                p.source, sources.size() == 1 ? "is" : "are", sources.size()));
        }
        if (p.channel >= sources[p.source].channels.size()) {
            return std::unexpected(fmt::format("map= names channel {} of source {}, which has {}",
                                               p.channel, p.source,
                                               sources[p.source].channels.size()));
        }
        if (std::ranges::find(places, p.place) != places.end()) {
            return std::unexpected(fmt::format("map= places {} twice", th::channel_name(p.place)));
        }
        places.push_back(p.place);
        used[p.source] = true;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!used[i]) {
            return std::unexpected(fmt::format(
                "source {} has no channel in map=: a source that is given and not placed is a "
                "mistake, not a default",
                i));
        }
        if (sources[i].sample_rate != sources.front().sample_rate) {
            return std::unexpected(fmt::format(
                "source {} is {} Hz, but source 0 is {} Hz - every source must share a sample rate",
                i, sources[i].sample_rate, sources.front().sample_rate));
        }
    }
    const Cover cover = cover_of(places);
    if (cover.covered != places.size()) {
        return std::unexpected(not_an_assignment(places));
    }
    Assembled out;
    out.layout.assignment = cover.assignment;
    out.wav.sample_rate = sources.front().sample_rate;
    const auto offset_of = [&](std::size_t source) {
        return source < offset_samples.size() ? offset_samples[source] : std::size_t{0};
    };
    std::size_t total = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        total = std::max(total, offset_of(i) + sources[i].frame_count());
    }
    for (const th::Channel place : th::channels_of_assignment(cover.assignment)) {
        const Placement& p = *std::ranges::find(placements, place, &Placement::place);
        std::vector<float> channel(total, 0.0F);
        std::ranges::copy(sources[p.source].channels[p.channel],
                          channel.begin() + static_cast<std::ptrdiff_t>(offset_of(p.source)));
        out.wav.channels.push_back(std::move(channel));
        out.layout.wav_index.push_back(out.wav.channels.size() - 1);
    }
    return out;
}

void pad_leading(iclforge::ac3::io::WavData& wav, std::size_t samples) {
    for (auto& channel : wav.channels) {
        channel.insert(channel.begin(), samples, 0.0F);
    }
}

namespace {

std::optional<std::uint32_t> parse_unsigned(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_real(std::string_view text) {
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

std::unexpected<std::string> refuse(std::string sentence) {
    return std::unexpected(std::move(sentence));
}

}  // namespace

namespace {

using MeaningEdit = th::ChannelMeaningEdit;

struct MeaningNumber {
    std::string_view name;
    int low;
    int high;
    std::optional<int> MeaningEdit::*field;
};

constexpr std::array<MeaningNumber, 10> kMeaningNumbers{{
    {"dialnorm-2ch", 0, 63, &MeaningEdit::twoch_dialogue_norm},
    {"dialnorm-6ch", 0, 31, &MeaningEdit::sixch_dialogue_norm},
    {"dialnorm-8ch", 0, 31, &MeaningEdit::eightch_dialogue_norm},
    {"mix-level-2ch", 0, 63, &MeaningEdit::twoch_mix_level},
    {"mix-level-6ch", 0, 63, &MeaningEdit::sixch_mix_level},
    {"mix-level-8ch", 0, 63, &MeaningEdit::eightch_mix_level},
    {"source-format-6ch", 0, 31, &MeaningEdit::sixch_source_format},
    {"source-format-8ch", 0, 63, &MeaningEdit::eightch_source_format},
    {"heavy-drc-start-up-gain", -32, 31, &MeaningEdit::heavy_drc_start_up_gain},
    {"drc-start-up-gain", -64, 63, &MeaningEdit::drc_start_up_gain},
}};

struct MeaningFlag {
    std::string_view name;
    std::optional<bool> MeaningEdit::*field;
};

constexpr std::array<MeaningFlag, 3> kMeaningFlags{{
    {"control-2ch", &MeaningEdit::twoch_control_enabled},
    {"control-6ch", &MeaningEdit::sixch_control_enabled},
    {"control-8ch", &MeaningEdit::eightch_control_enabled},
}};

// The order the options are written in: the dialogue norms, the mix levels, the source formats, the
// control bits, the two gains.
constexpr std::array<std::string_view, 13> kMeaningNames{
    "dialnorm-2ch",      "dialnorm-6ch",      "dialnorm-8ch",
    "mix-level-2ch",     "mix-level-6ch",     "mix-level-8ch",
    "source-format-6ch", "source-format-8ch", "control-2ch",
    "control-6ch",       "control-8ch",       "heavy-drc-start-up-gain",
    "drc-start-up-gain"};

// A whole number with nothing else in the text; a minus sign is allowed, a plus sign is not.
std::optional<int> parse_whole(std::string_view text) {
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::span<const std::string_view> meaning_option_names() noexcept {
    return kMeaningNames;
}

bool is_meaning_option(std::string_view name) noexcept {
    return std::ranges::find(kMeaningNames, name) != kMeaningNames.end();
}

std::expected<void, std::string> set_meaning_option(th::ChannelMeaningEdit& edit,
                                                    std::string_view name, std::string_view value) {
    for (const MeaningNumber& number : kMeaningNumbers) {
        if (name != number.name) {
            continue;
        }
        const auto parsed = parse_whole(value);
        if (!parsed || *parsed < number.low || *parsed > number.high) {
            return refuse(fmt::format("truehd-{} is a whole number from {} to {}", name, number.low,
                                      number.high));
        }
        edit.*number.field = *parsed;
        return {};
    }
    for (const MeaningFlag& flag : kMeaningFlags) {
        if (name != flag.name) {
            continue;
        }
        if (value != "on" && value != "off") {
            return refuse(fmt::format("truehd-{} is on or off", name));
        }
        edit.*flag.field = value == "on";
        return {};
    }
    return refuse(fmt::format("truehd-{} is not a channel meaning option", name));
}

std::expected<void, std::string> set_option(Settings& settings, std::string_view name,
                                            std::string_view value) {
    if (is_meaning_option(name)) {
        return set_meaning_option(settings.meaning, name, value);
    }
    if (name == "downmix") {
        if (value != "none" && value != "standard") {
            return refuse("truehd-downmix is none or standard");
        }
        settings.standard_downmix = value == "standard";
        return {};
    }
    if (name == "restart") {
        const auto units = parse_unsigned(value);
        if (!units || *units == 0 || *units > static_cast<std::uint32_t>(kRestartMaxUnits)) {
            return refuse(fmt::format(
                "truehd-restart is the access units between restart headers, 1 or 8 to {}",
                kRestartMaxUnits));
        }
        settings.restart_units = static_cast<int>(*units);
        return {};
    }
    if (name == "peak-rate") {
        const auto kbps = parse_unsigned(value);
        if (!kbps || *kbps == 0 || *kbps > kPeakRateMaxKbps) {
            return refuse(
                fmt::format("truehd-peak-rate is a rate in kbit/s, 1 to {}", kPeakRateMaxKbps));
        }
        settings.peak_rate_kbps = *kbps;
        return {};
    }
    if (name == "rate-check") {
        if (value != "strict" && value != "lenient") {
            return refuse("truehd-rate-check is strict or lenient");
        }
        settings.rate_lenient = value == "lenient";
        return {};
    }
    if (name == "bitrate") {
        const auto kbps = parse_unsigned(value);
        if (!kbps || *kbps < kBitrateMinKbps || *kbps > kBitrateMaxKbps) {
            return refuse(fmt::format("truehd-bitrate is a rate in kbit/s, {} to {}", kBitrateMinKbps,
                                      kBitrateMaxKbps));
        }
        settings.bitrate_kbps = *kbps;
        return {};
    }
    if (name == "major-sync") {
        const auto units = parse_unsigned(value);
        if (!units || *units > static_cast<std::uint32_t>(kMajorSyncMaxUnits)) {
            return refuse(fmt::format(
                "truehd-major-sync is the access units between major syncs, 0 (one at every "
                "restart header) or a multiple of truehd-restart= up to {}",
                kMajorSyncMaxUnits));
        }
        settings.major_sync_units = static_cast<int>(*units);
        return {};
    }
    if (name == "fir-order") {
        if (value.size() != 1 || value[0] < '0' || value[0] > '0' + kFirOrderMax) {
            return refuse(fmt::format("truehd-fir-order is 0 to {}", kFirOrderMax));
        }
        settings.fir_order = value[0] - '0';
        return {};
    }
    if (name == "matrix") {
        if (value != "on" && value != "off") {
            return refuse("truehd-matrix is on or off");
        }
        settings.matrix = value == "on";
        return {};
    }
    if (name == "stereo-mode") {
        if (value == "stereo") {
            settings.stereo_mode = 0;
        } else if (value == "ltrt") {
            settings.stereo_mode = 1;
        } else if (value == "headphone") {
            settings.stereo_mode = 2;
        } else {
            return refuse("truehd-stereo-mode is stereo, ltrt or headphone");
        }
        return {};
    }
    if (name == "downmix-gains") {
        std::array<double, 4> gains{};
        std::string_view rest = value;
        bool good = true;
        for (std::size_t i = 0; i < gains.size() && good; ++i) {
            const std::size_t comma = rest.find(',');
            const std::string_view part = rest.substr(0, comma);
            if (part == "off") {
                gains[i] = 0.0;
            } else if (const auto db = parse_real(part);
                       db && std::isfinite(*db) && *db <= 0.0 && *db >= kDownmixGainFloorDb) {
                gains[i] = std::pow(10.0, *db / 20.0);
            } else {
                good = false;
            }
            if (i + 1 < gains.size()) {
                good = good && comma != std::string_view::npos;
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            } else {
                good = good && comma == std::string_view::npos;
            }
        }
        if (!good) {
            return refuse(
                "truehd-downmix-gains is four gains in dB, 0 to -60 or off, for C, Ls and Rs, LFE, "
                "and Lrs and Rrs, e.g. -3,-3,off,-3");
        }
        settings.downmix_gains = gains;
        return {};
    }
    return refuse(fmt::format("truehd-{} is not an option of the TrueHD encode", name));
}

std::expected<void, std::string> apply(const Settings& settings, th::EncoderConfig& config) {
    config.downmix = settings.standard_downmix ? th::DownmixMode::kStandard : th::DownmixMode::kNone;
    if (settings.downmix_gains) {
        const std::array<double, 4>& g = *settings.downmix_gains;
        config.downmix_gains = th::DownmixGains{
            .centre = g[0], .surround = g[1], .lfe = g[2], .rear_to_surround = g[3]};
    }
    if (settings.restart_units) {
        config.restart_interval_units = *settings.restart_units;
    }
    if (settings.peak_rate_kbps) {
        config.peak_data_rate_bps = *settings.peak_rate_kbps * 1000U;
    }
    config.allow_rate_violations = settings.rate_lenient;
    if (settings.bitrate_kbps) {
        config.rate_control.target_bitrate_bps = *settings.bitrate_kbps * 1000U;
    }
    if (settings.major_sync_units) {
        config.major_sync_interval_units = *settings.major_sync_units;
    }
    if (settings.fir_order) {
        config.max_fir_order = *settings.fir_order;
    }
    config.matrix_search = settings.matrix;
    config.stereo_mode = settings.stereo_mode;
    config.verify = settings.verify;
    // The channel meaning (TH spec §14.1): the fields named, the rest zero as FFmpeg's streams have
    // them.
    if (const std::string_view field = th::apply_edit(settings.meaning, config.channel_meaning);
        !field.empty()) {
        return refuse(std::string{field});
    }
    return {};
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
