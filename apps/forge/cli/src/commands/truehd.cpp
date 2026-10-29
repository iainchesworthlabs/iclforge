#include "truehd.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../json.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/version.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "truehd_evolution.hpp"
#include "truehd_meaning.hpp"
#include "truehd_structure.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

constexpr int kBitsPerSample = 24;
constexpr std::size_t kWavHeaderBytes = 68;

std::optional<th::Presentation> parse_presentation(std::string_view text) {
    if (text.empty() || text == "best") {
        return th::Presentation::kBest;
    }
    if (text == "2ch") {
        return th::Presentation::k2ch;
    }
    if (text == "6ch") {
        return th::Presentation::k6ch;
    }
    if (text == "8ch") {
        return th::Presentation::k8ch;
    }
    if (text == "all") {
        return th::Presentation::kAll;
    }
    return std::nullopt;
}

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

// A WAV lists its channels in the order of the mask's bits. The stream's channels go out in the
// order the stream carries them, so the mask is stated only when that order is the WAV's own (5.1
// is); otherwise it is 0 and the channel names are in the status lines.
std::uint32_t channel_mask(std::span<const th::Channel> layout, int channels) {
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

void put_u16(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

// A 24-bit PCM WAV (WAVE_FORMAT_EXTENSIBLE) written as the samples arrive. A file's sizes are
// patched at the end; stdout's say "unknown" (0xFFFFFFFF), as a stream's do.
class Pcm24Wav {
   public:
    bool open(std::string_view out_path, int channels, std::uint32_t rate, std::uint32_t mask) {
        to_stdout_ = out_path == "-";
        if (to_stdout_) {
            iclforge::cli::platform::set_stdio_binary();
            out_ = &std::cout;
        } else {
            file_.open(std::string{out_path}, std::ios::binary | std::ios::trunc);
            out_ = &file_;
        }
        if (!*out_) {
            return false;
        }
        const auto block = static_cast<std::uint32_t>(channels * kBitsPerSample / 8);
        std::vector<std::uint8_t> header;
        const auto tag = [&header](const char* text) {
            for (int i = 0; i < 4; ++i) {
                header.push_back(static_cast<std::uint8_t>(text[i]));
            }
        };
        tag("RIFF");
        put_u32(header, 0xFFFFFFFFU);
        tag("WAVE");
        tag("fmt ");
        put_u32(header, 40);
        put_u16(header, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
        put_u16(header, static_cast<std::uint32_t>(channels));
        put_u32(header, rate);
        put_u32(header, rate * block);
        put_u16(header, block);
        put_u16(header, kBitsPerSample);
        put_u16(header, 22);
        put_u16(header, kBitsPerSample);
        put_u32(header, mask);
        // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00AA00389B71}
        constexpr std::array<std::uint8_t, 16> kPcm{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        header.insert(header.end(), kPcm.begin(), kPcm.end());
        tag("data");
        put_u32(header, 0xFFFFFFFFU);
        return put(header);
    }

    bool write(std::span<const std::int32_t> pcm) {
        buffer_.clear();
        for (const std::int32_t sample : pcm) {
            const auto bits = static_cast<std::uint32_t>(sample);
            buffer_.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
            buffer_.push_back(static_cast<std::uint8_t>((bits >> 8U) & 0xFFU));
            buffer_.push_back(static_cast<std::uint8_t>((bits >> 16U) & 0xFFU));
        }
        data_bytes_ += buffer_.size();
        return put(buffer_);
    }

    bool close() {
        if (out_ == nullptr) {
            return true;
        }
        bool ok = true;
        if (to_stdout_) {
            out_->flush();
            ok = static_cast<bool>(*out_);
        } else {
            const auto patch = [this](std::streamoff at, std::uint64_t value) {
                std::vector<std::uint8_t> bytes;
                put_u32(bytes,
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(value, 0xFFFFFFFFU)));
                file_.seekp(at);
                return put(bytes);
            };
            ok = patch(4, data_bytes_ + kWavHeaderBytes - 8) && patch(64, data_bytes_);
            file_.close();  // flushes: a full disk shows here
            ok = ok && !file_.fail();
        }
        out_ = nullptr;
        return ok;
    }

   private:
    bool put(const std::vector<std::uint8_t>& bytes) {
        out_->write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(*out_);
    }

    // A file goes through an ofstream like the CLI's other outputs; "-" is std::cout. (It was
    // fopen(path, "wb") until CodeQL's cpp/world-writable-file-creation named it: the only
    // fopen for writing in the CLI, creating the file with mode 0666.)
    std::ofstream file_;
    std::ostream* out_ = nullptr;
    bool to_stdout_ = false;
    std::uint64_t data_bytes_ = 0;
    std::vector<std::uint8_t> buffer_;
};

std::string channel_names(std::span<const th::Channel> layout) {
    std::string out;
    for (const th::Channel channel : layout) {
        out += out.empty() ? "" : " ";
        out += th::channel_name(channel);
    }
    return out;
}

std::string error_text(const th::Error& error) {
    return error.detail.empty() ? std::string{th::to_string(error.kind)}
                                : fmt::format("{}: {}", th::to_string(error.kind), error.detail);
}

const char* codec_name(const th::MajorSync& major_sync) {
    return major_sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP";
}

// The option a token sets, without its value ("drc" for "drc=0.5"), and the same for a spelling the
// help prints ("truehd-check=" is "truehd-check").
std::string_view key_of(std::string_view token) {
    return token.substr(0, token.find('='));
}

std::string_view key_of_spelling(std::string_view spelling) {
    return spelling.ends_with('=') ? spelling.substr(0, spelling.size() - 1) : spelling;
}

// Whether the spelling names one value of an option ("detail=evolution"), which applies that value and
// no other, and not the option ("json=").
bool spells_a_value(std::string_view spelling) {
    const auto equals = spelling.find('=');
    return equals != std::string_view::npos && equals + 1 < spelling.size();
}

// Choices between ways of computing a transform. TrueHD has no transform, so none of these can
// change a stream or a verdict, and they are warned about, not refused.
constexpr std::array<std::string_view, 3> kTransformChoices{"fast-mdct", "fast-imdct", "mode"};

// Options that assert something about a stream (or record what was read of it). One that is not
// applied would let the run pass without having checked anything.
constexpr std::array<std::string_view, 9> kVerifications{
    "verify-content", "check-content", "unanchored-ok", "trust-anchor", "pin",
    "verify-time",    "verify-objects", "gate-objects", "syntax-trace"};

// The tokens, `separator` between them.
std::string join(std::span<const std::string_view> tokens, std::string_view separator) {
    std::string out;
    for (const std::string_view token : tokens) {
        out += out.empty() ? "" : separator;
        out += token;
    }
    return out;
}

template <typename Range>
bool contains(const Range& range, std::string_view key) {
    return std::ranges::find(range, key) != std::ranges::end(range);
}

}  // namespace

int check_options_applied(const Options& meta, std::initializer_list<std::string_view> applied) {
    std::vector<std::string_view> ignorable;
    std::vector<std::string_view> refused;
    bool verification = false;
    for (const std::string& token : meta.given) {
        const std::string_view key = key_of(token);
        if (key == "quiet" || key == "verbose" ||
            std::ranges::any_of(applied, [&token, key](std::string_view spelling) {
                return spells_a_value(spelling) ? spelling == token
                                                : key_of_spelling(spelling) == key;
            })) {
            continue;
        }
        if (contains(kTransformChoices, key)) {
            ignorable.emplace_back(token);
            continue;
        }
        refused.emplace_back(token);
        verification = verification || contains(kVerifications, key);
    }
    if (!ignorable.empty()) {
        fmt::println(stderr,
                     "warning: {} has no effect on TrueHD or MLP, which has no transform; "
                     "ignored",
                     join(ignorable, ", "));
    }
    if (refused.empty()) {
        return kExitOk;
    }
    std::string takes;
    for (const std::string_view spelling : applied) {
        takes += fmt::format("{}, ", spelling);
    }
    fmt::println(stderr,
                 "error: {} does not apply {} to TrueHD or MLP, and an option that is not applied "
                 "is refused, not dropped. For TrueHD or MLP it takes {}quiet and verbose{}",
                 meta.command, join(refused, ", "), takes,
                 verification ? ". Nothing was verified" : "");
    return kExitUsage;
}

void warn_truehd_options_unused(const Options& meta, std::string_view stream_path) {
    std::vector<std::string_view> given;
    for (const std::string& token : meta.given) {
        const std::string_view key = key_of(token);
        if (key == "truehd-presentation" || key == "truehd-check") {
            given.emplace_back(token);
        }
    }
    if (!given.empty()) {
        fmt::println(stderr, "warning: {} is not TrueHD or MLP: {} {} ignored", stream_path,
                     join(given, " and "), given.size() == 1 ? "is" : "are");
    }
}

bool is_truehd_stream(std::span<const std::byte> stream) noexcept {
    // A TrueHD or MLP stream starts with an access unit whose major sync follows its four header
    // bytes, or has some bytes of something else ahead of that (a timecode or other packet some
    // muxers put in front): find_stream_start says which, and not for bytes that only look like a
    // major sync.
    return stream.size() >= 8 && th::find_stream_start(stream).has_value();
}

std::optional<CoreStrip> strip_ac3_core(std::span<const std::byte> stream) {
    CoreStrip out;
    int substreams = 0;  // from the first major sync; 0 until one has been seen
    std::size_t pos = 0;
    while (pos + 4 <= stream.size()) {
        const auto rest = stream.subspan(pos);
        if (std::to_integer<std::uint8_t>(rest[0]) == 0x0B &&
            std::to_integer<std::uint8_t>(rest[1]) == 0x77) {
            const auto frame = iclforge::ac3::io::read_frame_header(rest);
            if (frame && frame->kind == iclforge::ac3::io::StreamKind::kAc3 &&
                frame->bytes <= rest.size()) {
                ++out.frames;
                out.frame_bytes += frame->bytes;
                pos += frame->bytes;
                continue;
            }
        }
        const std::size_t length = th::access_unit_length_bytes(rest);
        const bool major_sync = rest.size() >= 8 && th::starts_with_major_sync(rest.subspan(4));
        if (length >= 4 && length <= rest.size() && (substreams > 0 || major_sync)) {
            if (const auto unit = th::parse_access_unit(rest.first(length), substreams, true)) {
                if (unit->major_sync) {
                    substreams = unit->major_sync->num_substreams;
                }
                out.bytes.insert(out.bytes.end(), rest.begin(),
                                 rest.begin() + static_cast<std::ptrdiff_t>(length));
                pos += length;
                continue;
            }
        }
        if (substreams == 0) {
            ++out.leading_bytes;
            ++pos;
            continue;
        }
        out.bytes.insert(out.bytes.end(), rest.begin(), rest.end());
        pos = stream.size();
    }
    if (out.frames == 0) {
        return std::nullopt;
    }
    return out;
}

bool starts_with_ac3_core_then_truehd(std::span<const std::byte> stream) {
    std::size_t pos = 0;
    while (pos + 2 <= stream.size() && std::to_integer<std::uint8_t>(stream[pos]) == 0x0B &&
           std::to_integer<std::uint8_t>(stream[pos + 1]) == 0x77) {
        const auto frame = iclforge::ac3::io::read_frame_header(stream.subspan(pos));
        if (!frame || frame->kind != iclforge::ac3::io::StreamKind::kAc3 ||
            pos + frame->bytes > stream.size()) {
            return false;
        }
        pos += frame->bytes;
    }
    return pos != 0 && is_truehd_stream(stream.subspan(pos));
}

int run_decode_truehd(std::span<const std::byte> stream, std::string_view in_path,
                      std::string_view out_path, const Options& meta) {
    const auto presentation = parse_presentation(meta.truehd_presentation);
    if (!presentation) {
        fmt::println(stderr, "error: truehd-presentation is best, 2ch, 6ch, 8ch or all (got '{}')",
                     meta.truehd_presentation);
        return kExitUsage;
    }
    const auto status = status_stream(out_path);
    th::DecoderConfig config;
    config.presentation = *presentation;
    config.strictness = meta.truehd_lenient ? th::Strictness::kLenient : th::Strictness::kStrict;
    th::Decoder decoder(config);
    th::ElementaryReader reader(stream);
    Pcm24Wav wav;

    int channels = 0;
    std::uint32_t rate = 0;
    int substream = 0;
    std::uint64_t samples = 0;
    std::uint64_t padding = 0;
    std::uint64_t errors = 0;
    std::size_t index = 0;
    std::string layout_text;
    bool failed = false;
    bool leading_said = false;
    while (true) {
        const std::size_t skipped_before = reader.skipped_bytes() - reader.leading_bytes();
        const auto unit = reader.next();
        if (!leading_said && reader.leading_bytes() != 0) {
            // Bytes ahead of the first access unit are not a loss of the stream.
            fmt::println(stderr,
                         "warning: {}: {} byte(s) ahead of the first access unit are not part of "
                         "the stream and are skipped",
                         in_path, reader.leading_bytes());
            leading_said = true;
        }
        if (reader.skipped_bytes() - reader.leading_bytes() != skipped_before) {
            fmt::println(stderr, "{}: {} bytes before access unit {} do not read as one", in_path,
                         reader.skipped_bytes() - reader.leading_bytes() - skipped_before, index);
            if (!meta.truehd_lenient) {
                failed = true;
                break;
            }
            // The reader dropped a unit the decoder cannot miss: it waits for the next restart.
            decoder.lost_data();
        }
        if (!unit) {
            break;
        }
        const auto result = decoder.decode(*unit);
        if (!result) {
            ++errors;
            fmt::println(stderr, "error: {}: access unit {}: {}", in_path, index,
                         error_text(result.error()));
            if (!meta.truehd_lenient) {
                failed = true;
                break;
            }
        } else if (*result) {
            const th::DecodedAccessUnit& decoded = **result;
            if (channels == 0) {
                const std::uint32_t mask = channel_mask(decoded.layout, decoded.channels);
                if (!wav.open(out_path, decoded.channels, decoded.sample_rate, mask)) {
                    fmt::println(stderr, "error: cannot write {}", out_path);
                    return kExitOutput;
                }
                channels = decoded.channels;
                rate = decoded.sample_rate;
                substream = decoded.substream;
                layout_text = channel_names(decoded.layout);
            } else if (decoded.channels != channels || decoded.sample_rate != rate) {
                fmt::println(
                    stderr,
                    "error: {}: access unit {}: the channel count or the sample rate changes",
                    in_path, index);
                failed = true;
                break;
            }
            if (!wav.write(decoded.pcm)) {
                fmt::println(stderr, "error: cannot write {}", out_path);
                return kExitOutput;
            }
            samples += static_cast<std::uint64_t>(decoded.samples);
            padding += static_cast<std::uint64_t>(decoded.padding_samples);
        }
        ++index;
    }
    if (channels != 0 && !wav.close()) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return kExitOutput;
    }
    if (channels == 0) {
        fmt::println(stderr, "error: {}: no access unit decoded", in_path);
        return kExitInput;
    }

    const auto& counters = decoder.counters();
    const auto& major_sync = decoder.major_sync();
    status_println(status, "{} {} Hz, {} channel(s){}, substream {}",
                   major_sync ? codec_name(*major_sync) : "TrueHD", rate, channels,
                   layout_text.empty() ? "" : fmt::format(" ({})", layout_text), substream);
    status_println(status, "  {} access unit(s) read, {} decoded, {} sample(s) a channel{}", index,
                   counters.units_decoded, samples,
                   padding != 0 ? fmt::format(" ({} of padding at the end removed)", padding) : "");
    status_println(status, "  {} restart header(s); lossless check: {} interval(s) ok, {} failed",
                   counters.restarts, counters.lossless_checks_ok,
                   counters.lossless_check_failures);
    status_println(status, "  other checks (access unit, CRC, parity) failed: {}",
                   counters.check_failures);
    if (counters.units_waiting != 0) {
        status_println(status, "  {} unit(s) waited for a restart header", counters.units_waiting);
    }
    if (failed || (!meta.truehd_lenient && errors != 0)) {
        return kExitInput;
    }
    return kExitOk;
}

int run_probe_truehd(std::string_view in_path, const Options& meta) {
    std::size_t ac3_core_frames = 0;
    const auto stream = read_elementary_stream(in_path, /*reads_truehd=*/true, &ac3_core_frames);
    if (stream.empty()) {
        return kExitUsage;
    }
    return run_probe_truehd_stream(stream, in_path, meta, {}, {}, ac3_core_frames);
}

int run_probe_truehd_stream(std::span<const std::byte> stream, std::string_view in_path,
                            const Options& meta,
                            const std::function<void(iclforge::apps::JsonSink&)>& container_json,
                            const std::function<void()>& container_text,
                            std::size_t ac3_core_frames) {
    // detail=frames and detail=blocks ask for per-unit lines this report does not have, and are
    // refused; detail=evolution, structure and format are its own.
    if (const int refused = check_options_applied(
            meta, {"json=", "detail=evolution", "detail=structure", "detail=format"});
        refused != kExitOk) {
        return refused;
    }
    const auto info = th::probe_stream(stream);
    if (!info) {
        fmt::println(stderr, "error: {}: {}", in_path, error_text(info.error()));
        return 1;
    }

    // Every unit's header, directory and, where the directory says so, the parity and CRC bytes of
    // its segments.
    th::ElementaryReader reader(stream);
    std::uint64_t units = 0;
    std::uint64_t major_syncs = 0;
    std::uint64_t segments = 0;
    std::uint64_t segment_failures = 0;
    int substreams = 0;
    std::uint64_t extra_data_units = 0;
    // The different channel meanings (TH spec §14.1) the major syncs carry; the first is reported.
    constexpr std::size_t kMaxMeanings = 256;
    std::vector<th::ChannelMeaning> meanings;
    // detail=evolution, structure or format: the structure of the evolution frames, the
    // stream-level syntax facts the specification leaves open, or both (the owner's check on a
    // disc's stream).
    const bool want_evolution =
        meta.detail.has_value() && (*meta.detail == "evolution" || *meta.detail == "format");
    const bool want_structure =
        meta.detail.has_value() && (*meta.detail == "structure" || *meta.detail == "format");
    EvolutionSurvey evolution(want_evolution);
    StructureSurvey structure;
    while (const auto unit = reader.next()) {
        const auto parsed = th::parse_access_unit(*unit, substreams, true);
        if (!parsed) {
            continue;
        }
        evolution.add(units, *parsed);
        if (want_structure) {
            structure.add(units, *parsed);
        }
        ++units;
        if (parsed->extra_data) {
            ++extra_data_units;
        }
        if (parsed->major_sync) {
            ++major_syncs;
            substreams = parsed->major_sync->num_substreams;
            if (parsed->major_sync->kind == th::StreamKind::kTrueHd &&
                meanings.size() < kMaxMeanings &&
                std::ranges::find(meanings, parsed->major_sync->channel_meaning) ==
                    meanings.end()) {
                meanings.push_back(parsed->major_sync->channel_meaning);
            }
        }
        for (int i = 0; i < parsed->num_substreams; ++i) {
            const auto& entry = parsed->entries[static_cast<std::size_t>(i)];
            const auto segment = parsed->segments[static_cast<std::size_t>(i)];
            if (!entry.crc_present || segment.size() < 3) {
                continue;
            }
            ++segments;
            const auto payload = segment.first(segment.size() - 2);
            if (th::substream_parity(payload) !=
                    std::to_integer<std::uint8_t>(segment[segment.size() - 2]) ||
                th::substream_crc(payload) !=
                    std::to_integer<std::uint8_t>(segment[segment.size() - 1])) {
                ++segment_failures;
            }
        }
    }
    if (want_structure) {
        structure.decode(stream);
    }
    const std::uint64_t unreadable = info->access_units > units ? info->access_units - units : 0;
    const std::size_t lost_bytes = reader.skipped_bytes() - reader.leading_bytes();
    const bool damaged = segment_failures != 0 || unreadable != 0 || lost_bytes != 0;

    const th::MajorSync& major_sync = info->major_sync;
    std::string channels_text;
    for (int i = 0; i < info->num_substreams; ++i) {
        channels_text +=
            fmt::format("{}{}", i == 0 ? "" : ", ", info->channels[static_cast<std::size_t>(i)]);
    }
    if (meta.json) {
        iclforge::cli::platform::set_stdio_binary();
        JsonWriter json{stdout};
        json.begin_object();
        json.member("schema", "iclforge.probe/1");
        json.member("generator", iclforge::ac3::version_full);
        json.member("file", in_path);
        // container_json writes the member's key and value, as write_container does for AC-3 and AC-4.
        if (container_json) {
            container_json(json);
        } else {
            json.key("container");
            json.value_null();
        }
        json.key("truehd");
        json.begin_object();
        json.member("codec", major_sync.kind == th::StreamKind::kTrueHd ? "truehd" : "mlp");
        json.member("sample_rate", static_cast<std::uint64_t>(info->sample_rate));
        json.member("substreams", static_cast<std::uint64_t>(info->num_substreams));
        json.key("channels");
        json.begin_array();
        for (int i = 0; i < info->num_substreams; ++i) {
            json.value(static_cast<std::uint64_t>(info->channels[static_cast<std::size_t>(i)]));
        }
        json.end_array();
        json.member("access_units", static_cast<std::uint64_t>(info->access_units));
        json.member("samples_per_access_unit",
                    static_cast<std::uint64_t>(info->samples_per_access_unit));
        json.member("variable_rate", major_sync.is_vbr);
        json.member("peak_data_rate", static_cast<std::uint64_t>(major_sync.peak_data_rate));
        json.member("peak_data_rate_bps", major_sync.peak_data_rate_bps());
        json.member("object_substream", info->has_object_substream);
        // TH spec §14.1; an MLP stream's bytes after substream_info are not described.
        json.key("channel_meaning");
        if (major_sync.kind == th::StreamKind::kTrueHd) {
            write_channel_meaning_json(json, major_sync.channel_meaning, info->num_substreams,
                                       meanings.size());
        } else {
            json.value_null();
        }
        json.member("extra_data_units", extra_data_units);
        json.member("ac3_core_frames", static_cast<std::uint64_t>(ac3_core_frames));
        json.member("major_syncs", major_syncs);
        json.member("segments_checked", segments);
        json.member("segment_failures", segment_failures);
        json.member("unreadable_units", unreadable);
        json.member("skipped_bytes", static_cast<std::uint64_t>(lost_bytes));
        json.member("leading_bytes", static_cast<std::uint64_t>(reader.leading_bytes()));
        json.key("evolution");
        evolution.write_json(json);
        if (want_structure) {
            json.key("structure");
            structure.write_json(json);
        }
        json.end_object();
        json.end_object();
        json.finish();
    } else {
        fmt::println("{:<16}{}", "file", in_path);
        if (container_text) {
            container_text();
        }
        fmt::println("{:<16}{}", "codec", codec_name(major_sync));
        fmt::println("{:<16}{} ({} sample(s) each), {} major sync(s)", "access units",
                     info->access_units, info->samples_per_access_unit, major_syncs);
        fmt::println("{:<16}{} Hz", "sample rate", info->sample_rate);
        fmt::println("{:<16}{} ({} channel(s) in substream order)", "substreams",
                     info->num_substreams, channels_text);
        fmt::println("{:<16}{}, peak data rate {} bit/s (field {})", "rate",
                     major_sync.is_vbr ? "variable" : "constant", major_sync.peak_data_rate_bps(),
                     major_sync.peak_data_rate);
        if (major_sync.kind == th::StreamKind::kTrueHd) {
            print_channel_meaning_text(major_sync.channel_meaning, info->num_substreams,
                                       meanings.size());
        }
        if (info->has_object_substream) {
            fmt::println("{:<16}present (read as a 16-channel substream, not rendered)",
                         "object substream");
        }
        if (extra_data_units != 0) {
            fmt::println("{:<16}{} access unit(s) carry extra data (carried, not read)",
                         "extra data", extra_data_units);
        }
        if (ac3_core_frames != 0) {
            fmt::println("{:<16}{} AC-3 frame(s) between the access units, set aside", "ac3 core",
                         ac3_core_frames);
        }
        fmt::println(
            "{:<16}{} of {} segment(s) pass their parity and CRC; {} unreadable unit(s); {} "
            "skipped byte(s)",
            "checks", segments - segment_failures, segments, unreadable, lost_bytes);
        if (reader.leading_bytes() != 0) {
            fmt::println(stderr,
                         "warning: {}: {} byte(s) ahead of the first access unit are not part of "
                         "the stream and are skipped",
                         in_path, reader.leading_bytes());
        }
        evolution.print_text();
        if (want_structure) {
            structure.print_text();
        }
    }
    return damaged ? 1 : 0;
}

}  // namespace forge_cli::commands
