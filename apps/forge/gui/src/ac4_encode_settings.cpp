#include "ac4_encode_settings.hpp"
#include "iclforge/ac4/io/carriage.hpp"
#include "iclforge/base/number_parse.hpp"

#include <charconv>
#include <cmath>
#include <fmt/format.h>
#include <limits>

#include "ac4_encode_core.hpp"

namespace forge_gui {

namespace {

// Part 1 Tables 149 and 149a in dB, in the order of kAc4CentreLevels and
// kAc4SurroundLevels; "off" is -infinity, as forge reads it.
constexpr double kOff = -std::numeric_limits<double>::infinity();
constexpr std::array<double, 8> kCentreDb{3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0, kOff};
constexpr std::array<double, 6> kSurroundDb{0.0, -1.5, -3.0, -4.5, -6.0, kOff};

constexpr std::array<iclforge::ac4::RateMode, 3> kRateModes{iclforge::ac4::RateMode::kConstant,
                                                            iclforge::ac4::RateMode::kAverage,
                                                            iclforge::ac4::RateMode::kVariable};
constexpr std::array<iclforge::ac4::CodecMode, 6> kCodecModes{
    iclforge::ac4::CodecMode::kAuto,      iclforge::ac4::CodecMode::kSimple,
    iclforge::ac4::CodecMode::kAspx,      iclforge::ac4::CodecMode::kAspxAcpl1,
    iclforge::ac4::CodecMode::kAspxAcpl2, iclforge::ac4::CodecMode::kAspxAcpl3};
constexpr std::array<iclforge::ac4::LoudnessPractice, 7> kPractices{
    iclforge::ac4::LoudnessPractice::kEbuR128,
    iclforge::ac4::LoudnessPractice::kAtscA85,
    iclforge::ac4::LoudnessPractice::kAribTrB32,
    iclforge::ac4::LoudnessPractice::kFreeTvOp59,
    iclforge::ac4::LoudnessPractice::kManual,
    iclforge::ac4::LoudnessPractice::kConsumerLeveller,
    iclforge::ac4::LoudnessPractice::kNotIndicated};
constexpr std::array<iclforge::ac4::DrcProfile, 5> kProfiles{
    iclforge::ac4::DrcProfile::kFilmStandard, iclforge::ac4::DrcProfile::kFilmLight,
    iclforge::ac4::DrcProfile::kMusicStandard, iclforge::ac4::DrcProfile::kMusicLight,
    iclforge::ac4::DrcProfile::kSpeech};
constexpr std::array<iclforge::ac4::PreferredDownmix, 4> kPreferred{
    iclforge::ac4::PreferredDownmix::kLoRo, iclforge::ac4::PreferredDownmix::kLtRt,
    iclforge::ac4::PreferredDownmix::kLtRtProLogicII,
    iclforge::ac4::PreferredDownmix::kNotIndicated};

// The index `at` of a table of `size` entries, or the first where it is past
// the end.
[[nodiscard]] std::size_t within(std::size_t at, std::size_t size) {
    return at < size ? at : 0;
}

[[nodiscard]] std::string dialogue_channels(const Ac4EncodeSettings& s) {
    std::string out;
    const auto add = [&out](bool on, std::string_view name) {
        if (on) {
            out += out.empty() ? "" : ",";
            out += name;
        }
    };
    add(s.dialogue_left, "l");
    add(s.dialogue_right, "r");
    add(s.dialogue_centre, "c");
    return out;
}

}  // namespace

bool ac4_downmix_named(const Ac4EncodeSettings& settings) {
    return settings.centre_level.has_value() || settings.surround_level.has_value() ||
           settings.preferred_downmix.has_value();
}

bool ac4_dialogue_named(const Ac4EncodeSettings& settings) {
    return settings.dialogue_left || settings.dialogue_right || settings.dialogue_centre;
}

std::optional<iclforge::ac4::LoudnessPractice> ac4_loudness_practice(
    const Ac4EncodeSettings& settings) {
    if (!settings.loudness) {
        return std::nullopt;
    }
    return kPractices[within(*settings.loudness, kPractices.size())];
}

std::vector<std::string> ac4_cli_tokens(const Ac4EncodeSettings& s, bool mp4) {
    std::vector<std::string> out;
    const auto put = [&out](std::string_view key, std::string_view value) {
        out.push_back(fmt::format("{}={}", key, value));
    };
    if (const std::size_t rate = within(s.frame_rate, kAc4FrameRates.size());
        rate != kAc4NativeFrameRate) {
        put("frame-rate", kAc4FrameRates[rate].token);
    }
    if (const std::size_t mode = within(s.rate_mode, kAc4RateModes.size()); mode != 0) {
        put("rate-mode", kAc4RateModes[mode].token);
    }
    if (const std::size_t mode = within(s.codec_mode, kAc4CodecModes.size()); mode != 0) {
        put("codec-mode", kAc4CodecModes[mode].token);
    }
    // With loudness= the dialnorm is always named: without it, ac4-encode
    // measures one, and the page would not show which it sends.
    if (s.measure_dialnorm) {
        put("dialnorm", "auto");
    } else if (s.dialnorm_db != 31.0 || s.loudness) {
        put("dialnorm", fmt::format("{:g}", s.dialnorm_db));
    }
    if (s.loudness) {
        put("loudness", kAc4LoudnessPractices[within(*s.loudness, kPractices.size())].token);
    }
    if (s.drc) {
        put("drc", kAc4DrcProfiles[within(*s.drc, kProfiles.size())].token);
    }
    if (s.centre_level) {
        put("cmixlev", kAc4CentreLevels[within(*s.centre_level, kCentreDb.size())].token);
    }
    if (s.surround_level) {
        put("surmixlev", kAc4SurroundLevels[within(*s.surround_level, kSurroundDb.size())].token);
    }
    if (s.preferred_downmix) {
        put("dmixmod",
            kAc4PreferredDownmixes[within(*s.preferred_downmix, kPreferred.size())].token);
    }
    if (ac4_dialogue_named(s)) {
        put("dialogue-channels", dialogue_channels(s));
        if (s.dialogue_mid) {
            put("dialogue-method", "mid");
        }
        if (const int gain =
                kAc4DialogueMaxGains[within(s.dialogue_max_gain, kAc4DialogueMaxGains.size())];
            gain != 9) {
            put("dialogue-max-gain", fmt::format("{}", gain));
        }
    }
    if (s.iframe_interval != 24) {
        put("iframe-interval", fmt::format("{}", s.iframe_interval));
    }
    if (!s.crc && !mp4) {
        put("crc", "off");
    }
    return out;
}

namespace {

// The position of `value` among `choices`' tokens. The token is what forge's parser compares the
// value with, so it is compared here the same way and a project's value is accepted exactly where
// the command line would accept it.
template <std::size_t N>
std::optional<std::size_t> choice_of(const std::array<Ac4Choice, N>& choices,
                                     std::string_view value) {
    for (std::size_t i = 0; i < N; ++i) {
        if (choices[i].token == value) {
            return i;
        }
    }
    return std::nullopt;
}

// A decimal number, read without the process's locale (strtod would read "0.5" as 0 under one that
// writes a comma, which Qt sets from the user's) by iclforge::base::parse_double, which is
// std::from_chars' reader on every toolchain, the NDK's and the macOS wheel's included. It is text a
// person typed into an option, so the syntax is theirs: a leading '+' and leading space are fine.
std::optional<double> number_of(std::string_view text) {
    const auto parsed = iclforge::base::parse_double(text, iclforge::base::kUserNumber);
    if (!parsed || !std::isfinite(parsed->value)) {
        return std::nullopt;
    }
    return parsed->value;
}

std::optional<int> whole_of(std::string_view text) {
    int value = 0;
    const char* const end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || at != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

const std::vector<std::string_view>& ac4_option_keys() {
    static const std::vector<std::string_view> keys{
        "frame-rate",  "rate-mode", "codec-mode", "dialnorm",         "loudness",
        "drc",         "cmixlev",   "surmixlev",  "dmixmod",          "dialogue-channels",
        "dialogue-method", "dialogue-max-gain", "iframe-interval", "crc", "coding"};
    return keys;
}

Ac4OptionResult ac4_apply_option(Ac4EncodeSettings& s, std::string_view key,
                                 std::string_view value) {
    using Result = Ac4OptionResult;
    // The choice lists whose token is the whole answer.
    const auto choose = [&](const auto& choices, std::size_t& into) {
        if (const auto at = choice_of(choices, value)) {
            into = *at;
            return Result::kApplied;
        }
        return Result::kBadValue;
    };
    const auto choose_optional = [&](const auto& choices, std::optional<std::size_t>& into) {
        if (const auto at = choice_of(choices, value)) {
            into = *at;
            return Result::kApplied;
        }
        return Result::kBadValue;
    };

    if (key == "frame-rate") {
        return choose(kAc4FrameRates, s.frame_rate);
    }
    if (key == "rate-mode") {
        return choose(kAc4RateModes, s.rate_mode);
    }
    if (key == "codec-mode") {
        if (choice_of(kAc4CodecModes, value)) {
            return choose(kAc4CodecModes, s.codec_mode);
        }
        // Valid for the command, and without a control on the page.
        for (const std::string_view named : {"scpl", "aspx-scpl", "aspx-ajcc"}) {
            if (value == named) {
                return Result::kNotHeld;
            }
        }
        return Result::kBadValue;
    }
    if (key == "dialnorm") {
        if (value == "auto") {
            s.measure_dialnorm = true;
            return Result::kApplied;
        }
        // Part 1 clause 4.3.12.2.1: 0 to 31.75 dB below full scale in steps of 0.25.
        const auto db = number_of(value);
        if (!db || !(*db >= 0.0 && *db <= 31.75) ||
            std::fabs(*db * 4.0 - std::round(*db * 4.0)) > 1e-9) {
            return Result::kBadValue;
        }
        s.dialnorm_db = *db;
        s.measure_dialnorm = false;
        return Result::kApplied;
    }
    if (key == "loudness") {
        return choose_optional(kAc4LoudnessPractices, s.loudness);
    }
    if (key == "drc") {
        if (value == "none") {
            s.drc.reset();
            return Result::kApplied;
        }
        return choose_optional(kAc4DrcProfiles, s.drc);
    }
    if (key == "cmixlev") {
        return choose_optional(kAc4CentreLevels, s.centre_level);
    }
    if (key == "surmixlev") {
        return choose_optional(kAc4SurroundLevels, s.surround_level);
    }
    if (key == "dmixmod") {
        return choose_optional(kAc4PreferredDownmixes, s.preferred_downmix);
    }
    if (key == "dialogue-channels") {
        bool left = false;
        bool right = false;
        bool centre = false;
        std::string_view rest = value;
        bool ok = !rest.empty();
        while (ok && !rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            if (item == "l") {
                left = true;
            } else if (item == "r") {
                right = true;
            } else if (item == "c") {
                centre = true;
            } else {
                ok = false;
            }
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        if (!ok) {
            return Result::kBadValue;
        }
        s.dialogue_left = left;
        s.dialogue_right = right;
        s.dialogue_centre = centre;
        return Result::kApplied;
    }
    if (key == "dialogue-method") {
        if (value == "mid" || value == "independent") {
            s.dialogue_mid = value == "mid";
            return Result::kApplied;
        }
        // "cross" is the command's, and the page has the two.
        return value == "cross" ? Result::kNotHeld : Result::kBadValue;
    }
    if (key == "dialogue-max-gain") {
        const auto db = whole_of(value);
        for (std::size_t i = 0; db && i < kAc4DialogueMaxGains.size(); ++i) {
            if (kAc4DialogueMaxGains[i] == *db) {
                s.dialogue_max_gain = i;
                return Result::kApplied;
            }
        }
        return Result::kBadValue;
    }
    if (key == "iframe-interval") {
        const auto frames = whole_of(value);
        if (!frames || *frames < 1 || *frames > 100000) {
            return Result::kBadValue;
        }
        s.iframe_interval = *frames;
        return Result::kApplied;
    }
    if (key == "crc") {
        if (value != "on" && value != "off") {
            return Result::kBadValue;
        }
        s.crc = value == "on";
        return Result::kApplied;
    }
    if (key == "coding") {
        return choose(kAc4ObjectCodings, s.object_coding);
    }
    return Result::kNotHeld;
}

std::optional<std::string> ac4_settings_refusal(const Ac4EncodeSettings& settings,
                                                std::size_t channels, int sample_rate_hz) {
    if (sample_rate_hz == 44100 && within(settings.frame_rate, kAc4FrameRates.size()) !=
                                       kAc4NativeFrameRate) {
        return std::string{
            "at 44.1 kHz AC-4 has the native frame rate alone (Part 1 Table 83); frame-rate= "
            "names another"};
    }
    if (ac4_downmix_named(settings) && channels < 5) {
        return fmt::format(
            "the downmix options describe the stereo downmix of 5.0, 5.1, 7.0, 7.1 and the "
            "immersive layouts; the source is {}",
            iclforge::apps::ac4_layout_name(channels, iclforge::ac4::AdditionalPair::kNone));
    }
    return std::nullopt;
}

std::optional<int> ac4_object_dialnorm(const Ac4EncodeSettings& s) {
    if (s.measure_dialnorm || s.dialnorm_db < 1.0 || s.dialnorm_db > 31.0 ||
        s.dialnorm_db != std::floor(s.dialnorm_db)) {
        return std::nullopt;
    }
    return static_cast<int>(s.dialnorm_db);
}

std::vector<std::string> ac4_object_cli_tokens(const Ac4EncodeSettings& s, bool mp4) {
    std::vector<std::string> out;
    if (within(s.object_coding, kAc4ObjectCodings.size()) != 0) {
        out.push_back(fmt::format(
            "coding={}",
            kAc4ObjectCodings[within(s.object_coding, kAc4ObjectCodings.size())].token));
    }
    // atmos-encode's own default is 31, as ac4-encode's.
    if (const auto dialnorm = ac4_object_dialnorm(s); dialnorm && *dialnorm != 31) {
        out.push_back(fmt::format("dialnorm={}", *dialnorm));
    }
    if (!s.crc && !mp4) {
        out.push_back("crc=off");
    }
    return out;
}

std::optional<std::string> ac4_object_settings_refusal(const Ac4EncodeSettings& s) {
    if (s.measure_dialnorm) {
        return std::string{
            "an AC-4 object stream has no bed to measure a loudness on; set the dialnorm by hand, "
            "1 to 31"};
    }
    if (!ac4_object_dialnorm(s)) {
        return fmt::format(
            "an AC-4 object stream takes its dialnorm in whole dB, 1 to 31, as atmos-encode reads "
            "it; the setting is {:g}",
            s.dialnorm_db);
    }
    return std::nullopt;
}

iclforge::apps::Ac4ObjectsParams ac4_objects_params(const Ac4EncodeSettings& s,
                                               std::uint32_t sample_rate_hz, int bitrate_kbps) {
    iclforge::apps::Ac4ObjectsParams params;
    params.sample_rate_hz = sample_rate_hz;
    params.bitrate_kbps = bitrate_kbps;
    params.dialnorm_db = ac4_object_dialnorm(s).value_or(31);
    params.coding = within(s.object_coding, kAc4ObjectCodings.size()) == 0
                        ? iclforge::ac4::ObjectCoding::kAjoc
                        : iclforge::ac4::ObjectCoding::kDirect;
    return params;
}

iclforge::ac4::EncoderConfig ac4_encoder_config(const Ac4EncodeSettings& s, int channels,
                                                int sample_rate_hz, int bitrate_kbps) {
    iclforge::ac4::EncoderConfig config;
    config.channels = channels;
    config.sample_rate_hz = sample_rate_hz;
    config.frame_rate_index = static_cast<int>(within(s.frame_rate, kAc4FrameRates.size()));
    config.bitrate_kbps = bitrate_kbps;
    config.rate_mode = kRateModes[within(s.rate_mode, kRateModes.size())];
    config.codec_mode = kCodecModes[within(s.codec_mode, kCodecModes.size())];
    config.iframe_interval = s.iframe_interval;
    config.dialnorm_db = -s.dialnorm_db;
    if (s.drc) {
        // ac4-encode's drc=: Table 161's four modes on the profile.
        iclforge::ac4::DrcConfig drc;
        drc.profile = kProfiles[within(*s.drc, kProfiles.size())];
        for (int id = 0; id < 4; ++id) {
            drc.modes.push_back(
                iclforge::ac4::DrcModeConfig{.id = id, .gains_config = std::nullopt});
        }
        config.drc = drc;
    }
    if (ac4_downmix_named(s)) {
        iclforge::ac4::DownmixConfig downmix;
        if (s.centre_level) {
            downmix.loro_centre_db = kCentreDb[within(*s.centre_level, kCentreDb.size())];
        }
        if (s.surround_level) {
            downmix.loro_surround_db = kSurroundDb[within(*s.surround_level, kSurroundDb.size())];
        }
        if (s.preferred_downmix) {
            downmix.preferred = kPreferred[within(*s.preferred_downmix, kPreferred.size())];
        }
        config.downmix = downmix;
    }
    if (ac4_dialogue_named(s)) {
        iclforge::ac4::DialogueConfig dialogue;
        dialogue.method = s.dialogue_mid ? iclforge::ac4::DialogueMethod::kMid
                                         : iclforge::ac4::DialogueMethod::kChannelIndependent;
        dialogue.source = iclforge::ac4::DialogueSource::kMarkedChannels;
        dialogue.max_gain_db =
            kAc4DialogueMaxGains[within(s.dialogue_max_gain, kAc4DialogueMaxGains.size())];
        dialogue.left = s.dialogue_left;
        dialogue.right = s.dialogue_right;
        dialogue.centre = s.dialogue_centre;
        config.dialogue = dialogue;
    }
    return config;
}

}  // namespace forge_gui
