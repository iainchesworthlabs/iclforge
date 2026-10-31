#include "probe_truehd.hpp"

#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace iclforge::apps::truehd_probe {

namespace th = iclforge::truehd;

void RateModel::add(std::size_t unit_bytes, const std::optional<th::MajorSync>& first_sync) {
    if (!started_) {
        if (!first_sync) {
            return;
        }
        started_ = true;
        rate_ = first_sync->sample_rate();
        nominal_ = th::nominal_samples_per_access_unit(first_sync->ratebits());
        // TH spec v3.9 section 4.4 (ERRATA.md question 12): bits per second.
        report_.declared_bps = first_sync->peak_data_rate_bps();
        limit_ = static_cast<std::int64_t>(rate_) * 75 / 1000;
    }
    if (report_.declared_bps == 0 || nominal_ <= 0) {
        return;
    }
    const std::int64_t slot = position_ - nominal_;
    std::int64_t start = slot;
    if (units_ > 0) {
        start = std::max({slot, last_arrival_end_, last_start_ + nominal_});
    }
    const auto bits_at_rate = static_cast<std::uint64_t>(unit_bytes) * 8U * rate_;
    const auto arrival = static_cast<std::int64_t>((bits_at_rate + report_.declared_bps - 1U) /
                                                   report_.declared_bps);
    const std::int64_t latency = start + arrival - slot;
    if (latency > limit_) {
        ++report_.violations;
    }
    report_.worst_latency_ms = std::max(
        report_.worst_latency_ms, 1000.0 * static_cast<double>(latency) / static_cast<double>(rate_));
    report_.max_unit_rate_bps = std::max<std::uint64_t>(
        report_.max_unit_rate_bps, bits_at_rate / static_cast<std::uint64_t>(nominal_));
    last_start_ = start;
    last_arrival_end_ = start + arrival;
    position_ += nominal_;
    ++units_;
}

const char* codec_label(const th::MajorSync& major_sync) noexcept {
    return major_sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP";
}

namespace {

constexpr std::size_t kMaxMeanings = 256;
constexpr std::array<const char*, 3> kPresentationNames{"2ch", "6ch", "8ch"};

// The names a decode of `channels` channels gives them, as the decoder chooses them: the 8 channel
// assignment's list if it has that many, else the 6 channel one's, else L and R for two (TrueHD).
std::vector<std::string> layout_of(const th::MajorSync& major_sync, int channels) {
    if (major_sync.kind != th::StreamKind::kTrueHd || channels <= 0) {
        return {};
    }
    const auto size = static_cast<std::size_t>(channels);
    auto names = [](const std::vector<th::Channel>& list) {
        std::vector<std::string> out;
        for (const th::Channel channel : list) {
            out.emplace_back(th::channel_name(channel));
        }
        return out;
    };
    if (const auto eight = th::channels_of_assignment(major_sync.eightch_decoder_channel_assignment());
        eight.size() == size) {
        return names(eight);
    }
    if (const auto six = th::channels_of_assignment(major_sync.sixch_decoder_channel_assignment());
        six.size() == size) {
        return names(six);
    }
    if (channels == 2) {
        return {"L", "R"};
    }
    return {};
}

LosslessResult lossless_check(std::span<const std::byte> stream) {
    th::DecoderConfig config;
    config.presentation = th::Presentation::kAll;
    config.strictness = th::Strictness::kLenient;
    th::Decoder decoder(config);
    th::ElementaryReader reader(stream);
    LosslessResult result;
    while (const auto unit = reader.next()) {
        if (!decoder.decode(*unit)) {
            ++result.decode_errors;
        }
    }
    const auto& counters = decoder.counters();
    result.intervals_ok = counters.lossless_checks_ok;
    result.intervals_failed = counters.lossless_check_failures;
    result.other_check_failures = counters.check_failures;
    result.units_waiting = counters.units_waiting;
    return result;
}

}  // namespace

std::expected<Report, th::Error> build_report(std::span<const std::byte> stream,
                                              const ReportOptions& options) {
    auto info = th::probe_stream(stream);
    if (!info) {
        return std::unexpected(info.error());
    }
    Report report;
    report.info = *info;

    th::ElementaryReader reader(stream);
    RateModel rate;
    int substreams = 0;
    std::optional<std::uint64_t> last_sync_unit;
    std::uint64_t index = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = th::parse_access_unit(*unit, substreams, true);
        if (!parsed) {
            continue;
        }
        if (options.on_unit) {
            options.on_unit(index, *parsed);
        }
        rate.add(unit->size(), parsed->major_sync);
        report.bytes += unit->size();
        if (parsed->extra_data) {
            ++report.extra_data_units;
        }
        if (parsed->major_sync) {
            ++report.major_syncs;
            substreams = parsed->major_sync->num_substreams;
            if (last_sync_unit) {
                const std::uint64_t gap = index - *last_sync_unit;
                report.restart_min_units =
                    report.restart_min_units == 0 ? gap : std::min(report.restart_min_units, gap);
                report.restart_max_units = std::max(report.restart_max_units, gap);
            }
            last_sync_unit = index;
            if (parsed->major_sync->kind == th::StreamKind::kTrueHd &&
                report.meanings.size() < kMaxMeanings &&
                std::ranges::find(report.meanings, parsed->major_sync->channel_meaning) ==
                    report.meanings.end()) {
                report.meanings.push_back(parsed->major_sync->channel_meaning);
            }
        }
        bool unit_ok = true;
        for (int i = 0; i < parsed->num_substreams; ++i) {
            const auto& entry = parsed->entries[static_cast<std::size_t>(i)];
            const auto segment = parsed->segments[static_cast<std::size_t>(i)];
            if (!entry.crc_present || segment.size() < 3) {
                continue;
            }
            ++report.segments;
            const auto payload = segment.first(segment.size() - 2);
            if (th::substream_parity(payload) !=
                    std::to_integer<std::uint8_t>(segment[segment.size() - 2]) ||
                th::substream_crc(payload) !=
                    std::to_integer<std::uint8_t>(segment[segment.size() - 1])) {
                ++report.segment_failures;
                unit_ok = false;
            }
        }
        if (options.frames) {
            FrameInfo frame;
            frame.index = index;
            frame.offset = static_cast<std::size_t>(unit->data() - stream.data());
            frame.bytes = unit->size();
            frame.time_seconds = info->sample_rate == 0 ? 0.0 : static_cast<double>(index) * info->samples_per_access_unit / static_cast<double>(info->sample_rate);
            frame.major_sync = parsed->major_sync.has_value();
            frame.input_timing = parsed->input_timing;
            frame.substreams = parsed->num_substreams;
            for (int i = 0; i < parsed->num_substreams && i < th::kMaxSubstreams; ++i) {
                frame.substream_bytes[static_cast<std::size_t>(i)] = parsed->segments[static_cast<std::size_t>(i)].size();
            }
            frame.checks_ok = unit_ok;
            frame.extra_data = parsed->extra_data.has_value();
            report.frames.push_back(frame);
        }
        ++index;
    }
    report.units = index;
    report.unreadable_units = info->access_units > index ? info->access_units - index : 0;
    report.skipped_bytes = reader.skipped_bytes() - reader.leading_bytes();
    report.leading_bytes = reader.leading_bytes();
    report.rate = rate.report();

    if (info->sample_rate != 0) {
        report.duration_seconds = static_cast<double>(report.units) *
                                  static_cast<double>(info->samples_per_access_unit) /
                                  static_cast<double>(info->sample_rate);
    }
    if (report.duration_seconds > 0.0) {
        report.average_bps = static_cast<double>(report.bytes) * 8.0 / report.duration_seconds;
    }

    const int presentations = std::min(info->num_substreams, 3);
    for (int i = 0; i < presentations; ++i) {
        const int channels = info->channels[static_cast<std::size_t>(i)];
        report.presentations.push_back(Presentation{.name = kPresentationNames[static_cast<std::size_t>(i)],
                                                    .substream = i,
                                                    .channels = channels,
                                                    .layout = layout_of(info->major_sync, channels)});
    }
    if (info->major_sync.kind == th::StreamKind::kMlp) {
        report.group1_bits = th::mlp_word_bits(info->major_sync.group1_bits_code());
        report.group2_bits = th::mlp_word_bits(info->major_sync.group2_bits_code());
    }
    if (options.lossless) {
        report.lossless = lossless_check(stream);
    }
    return report;
}

namespace {

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += out.empty() ? "" : " ";
        out += name;
    }
    return out;
}

}  // namespace

void write_json(JsonSink& json, const Report& report, std::size_t ac3_core_frames,
                const JsonExtras& extras) {
    const th::StreamInfo& info = report.info;
    const th::MajorSync& major_sync = info.major_sync;
    json.begin_object();
    json.member("codec", major_sync.kind == th::StreamKind::kTrueHd ? "truehd" : "mlp");
    json.member("sample_rate", static_cast<std::uint64_t>(info.sample_rate));
    json.member("substreams", static_cast<std::uint64_t>(info.num_substreams));
    json.key("channels");
    json.begin_array();
    for (int i = 0; i < info.num_substreams; ++i) {
        json.value(static_cast<std::uint64_t>(info.channels[static_cast<std::size_t>(i)]));
    }
    json.end_array();
    json.member("access_units", static_cast<std::uint64_t>(info.access_units));
    json.member("samples_per_access_unit", static_cast<std::uint64_t>(info.samples_per_access_unit));
    json.key("duration_seconds");
    json.value(report.duration_seconds, 6);
    json.member("bytes", report.bytes);
    json.key("average_bit_rate_bps");
    json.value(report.average_bps, 0);
    json.member("variable_rate", major_sync.is_vbr);
    json.member("peak_data_rate", static_cast<std::uint64_t>(major_sync.peak_data_rate));
    json.member("peak_data_rate_bps", major_sync.peak_data_rate_bps());
    json.key("rate_model");
    json.begin_object();
    json.member("declared_bps", report.rate.declared_bps);
    json.member("max_unit_rate_bps", report.rate.max_unit_rate_bps);
    json.key("worst_latency_ms");
    json.value(report.rate.worst_latency_ms, 3);
    json.key("latency_limit_ms");
    json.value(report.rate.latency_limit_ms, 1);
    json.member("violations", report.rate.violations);
    json.end_object();
    json.member("object_substream", info.has_object_substream);
    json.key("presentations");
    json.begin_array();
    for (const Presentation& presentation : report.presentations) {
        json.begin_object();
        json.member("name", presentation.name);
        json.member("substream", static_cast<std::uint64_t>(presentation.substream));
        json.member("channels", static_cast<std::uint64_t>(presentation.channels));
        json.key("layout");
        json.begin_array();
        for (const std::string& name : presentation.layout) {
            json.value(name);
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.key("word_length");
    json.begin_object();
    if (report.group1_bits) {
        json.member("group1_bits", static_cast<std::uint64_t>(*report.group1_bits));
        json.member("group2_bits", static_cast<std::uint64_t>(report.group2_bits.value_or(0)));
    } else {
        json.member("decoded_bits", std::uint64_t{24});
    }
    json.end_object();
    // TH spec §14.1; an MLP stream's bytes after substream_info are not described.
    json.key("channel_meaning");
    if (major_sync.kind == th::StreamKind::kTrueHd && extras.channel_meaning) {
        extras.channel_meaning(json);
    } else {
        json.value_null();
    }
    json.key("restart_spacing");
    json.begin_object();
    json.member("major_syncs", report.major_syncs);
    json.member("min_units", report.restart_min_units);
    json.member("max_units", report.restart_max_units);
    json.end_object();
    if (!report.frames.empty()) {
        json.key("frames");
        json.begin_array();
        for (const FrameInfo& frame : report.frames) {
            json.begin_object();
            json.member("index", frame.index);
            json.member("offset", static_cast<std::uint64_t>(frame.offset));
            json.member("bytes", static_cast<std::uint64_t>(frame.bytes));
            json.key("time_seconds");
            json.value(frame.time_seconds, 6);
            json.member("major_sync", frame.major_sync);
            json.member("input_timing", static_cast<std::uint64_t>(frame.input_timing));
            json.key("substream_bytes");
            json.begin_array();
            for (int i = 0; i < frame.substreams && i < th::kMaxSubstreams; ++i) {
                json.value(
                    static_cast<std::uint64_t>(frame.substream_bytes[static_cast<std::size_t>(i)]));
            }
            json.end_array();
            json.member("checks_ok", frame.checks_ok);
            json.member("extra_data", frame.extra_data);
            json.end_object();
        }
        json.end_array();
    }
    json.member("extra_data_units", report.extra_data_units);
    json.member("ac3_core_frames", static_cast<std::uint64_t>(ac3_core_frames));
    json.member("major_syncs", report.major_syncs);
    json.member("segments_checked", report.segments);
    json.member("segment_failures", report.segment_failures);
    json.member("unreadable_units", report.unreadable_units);
    json.member("skipped_bytes", static_cast<std::uint64_t>(report.skipped_bytes));
    json.member("leading_bytes", static_cast<std::uint64_t>(report.leading_bytes));
    json.key("lossless");
    if (report.lossless) {
        json.begin_object();
        json.member("intervals_ok", report.lossless->intervals_ok);
        json.member("intervals_failed", report.lossless->intervals_failed);
        json.member("other_check_failures", report.lossless->other_check_failures);
        json.member("units_waiting", report.lossless->units_waiting);
        json.member("decode_errors", report.lossless->decode_errors);
        json.end_object();
    } else {
        json.value_null();
    }
    if (extras.tail) {
        extras.tail(json);
    }
    json.end_object();
}

std::vector<TextLine> text_head(const Report& report) {
    const th::StreamInfo& info = report.info;
    const th::MajorSync& major_sync = info.major_sync;
    std::string channels_text;
    for (int i = 0; i < info.num_substreams; ++i) {
        channels_text +=
            fmt::format("{}{}", i == 0 ? "" : ", ", info.channels[static_cast<std::size_t>(i)]);
    }
    std::vector<TextLine> lines;
    lines.push_back({"codec", codec_label(major_sync)});
    lines.push_back({"access units", fmt::format("{} ({} sample(s) each), {} major sync(s)",
                                                 info.access_units, info.samples_per_access_unit,
                                                 report.major_syncs)});
    lines.push_back({"duration", fmt::format("{:.3f} s", report.duration_seconds)});
    lines.push_back({"sample rate", fmt::format("{} Hz", info.sample_rate)});
    lines.push_back({"substreams", fmt::format("{} ({} channel(s) in substream order)",
                                               info.num_substreams, channels_text)});
    for (const Presentation& presentation : report.presentations) {
        if (!presentation.layout.empty()) {
            lines.push_back({fmt::format("  {}", presentation.name), join(presentation.layout)});
        }
    }
    if (report.group1_bits) {
        lines.push_back({"word length", fmt::format("{} bits, then {} bits (MLP groups 1 and 2)",
                                                    *report.group1_bits, report.group2_bits.value_or(0))});
    } else {
        lines.push_back({"word length", "24-bit words decoded (the source's depth is not coded)"});
    }
    lines.push_back({"rate", fmt::format("{}, peak data rate {} bit/s (field {})",
                                         major_sync.is_vbr ? "variable" : "constant",
                                         major_sync.peak_data_rate_bps(), major_sync.peak_data_rate)});
    lines.push_back(
        {"measured", fmt::format("average {:.0f} bit/s, largest unit {} bit/s; {} unit(s) over 75 ms of "
                                 "the declared rate (TH spec section 18, worst {:.2f} ms)",
                                 report.average_bps, report.rate.max_unit_rate_bps,
                                 report.rate.violations, report.rate.worst_latency_ms)});
    if (report.major_syncs > 1) {
        lines.push_back({"restarts", fmt::format("{} major sync(s), {} to {} unit(s) apart",
                                                 report.major_syncs, report.restart_min_units,
                                                 report.restart_max_units)});
    } else {
        lines.push_back({"restarts", fmt::format("{} major sync(s)", report.major_syncs)});
    }
    return lines;
}

std::vector<TextLine> text_tail(const Report& report, std::size_t ac3_core_frames) {
    std::vector<TextLine> lines;
    if (report.info.has_object_substream) {
        lines.push_back({"object substream", "present (read as a 16-channel substream, not rendered)"});
    }
    if (report.extra_data_units != 0) {
        lines.push_back({"extra data", fmt::format("{} access unit(s) carry extra data (carried, not read)",
                                                   report.extra_data_units)});
    }
    if (ac3_core_frames != 0) {
        lines.push_back({"ac3 core", fmt::format("{} AC-3 frame(s) between the access units, set aside",
                                                 ac3_core_frames)});
    }
    lines.push_back({"checks", fmt::format("{} of {} segment(s) pass their parity and CRC; {} unreadable "
                                           "unit(s); {} skipped byte(s)",
                                           report.segments - report.segment_failures, report.segments,
                                           report.unreadable_units, report.skipped_bytes)});
    if (report.lossless) {
        lines.push_back({"lossless check", fmt::format("{} interval(s) ok, {} failed; other checks failed {}; "
                                                       "{} unit(s) waited for a restart header",
                                                       report.lossless->intervals_ok,
                                                       report.lossless->intervals_failed,
                                                       report.lossless->other_check_failures,
                                                       report.lossless->units_waiting)});
    }
    return lines;
}

std::vector<std::string> frame_lines(const Report& report) {
    std::vector<std::string> lines;
    lines.reserve(report.frames.size());
    for (const FrameInfo& frame : report.frames) {
        std::string sizes;
        for (int i = 0; i < frame.substreams && i < th::kMaxSubstreams; ++i) {
            sizes += fmt::format("{}{}", i == 0 ? "" : ", ", frame.substream_bytes[static_cast<std::size_t>(i)]);
        }
        lines.push_back(fmt::format(
            "access unit {} @ {} ({} bytes, t={:.4f}s){}, input timing {}, {} substream(s) [{} bytes]{}{}",
            frame.index, frame.offset, frame.bytes, frame.time_seconds,
            frame.major_sync ? ", major sync" : "", frame.input_timing, frame.substreams, sizes,
            frame.extra_data ? ", extra data" : "", frame.checks_ok ? "" : ", CHECK FAILED"));
    }
    return lines;
}

}  // namespace iclforge::apps::truehd_probe
