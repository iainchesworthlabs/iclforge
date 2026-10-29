#include "truehd_structure.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "json_sink.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

constexpr std::size_t kMaxMajorSyncs = 8;
constexpr std::size_t kMaxListed = 8;

// The entries of a histogram, most frequent first, at most kMaxListed.
template <typename Key>
std::vector<std::pair<Key, std::uint64_t>> top(const std::map<Key, std::uint64_t>& histogram) {
    std::vector<std::pair<Key, std::uint64_t>> out(histogram.begin(), histogram.end());
    std::ranges::stable_sort(out, [](const auto& a, const auto& b) { return a.second > b.second; });
    if (out.size() > kMaxListed) {
        out.resize(kMaxListed);
    }
    return out;
}

void write_range(iclforge::apps::JsonSink& json, std::string_view name, bool seen,
                 std::uint64_t min, std::uint64_t max) {
    json.key(name);
    if (!seen) {
        json.value_null();
        return;
    }
    json.begin_object();
    json.member("min", min);
    json.member("max", max);
    json.end_object();
}

}  // namespace

void StructureSurvey::add(std::uint64_t unit_index, const th::AccessUnit& unit) {
    if (unit.major_sync) {
        const th::MajorSync& sync = *unit.major_sync;
        MajorSyncSummary summary;
        summary.size_bytes = sync.size_bytes;
        summary.substream_info = sync.substream_info;
        summary.extended_substream_info = sync.extended_substream_info;
        summary.substreams = sync.num_substreams;
        summary.sample_rate = sync.sample_rate();
        summary.extra_present = sync.extra.has_value();
        if (sync.extra) {
            summary.extra_length = sync.extra->length;
            summary.channel_count = sync.extra->sixteench_channel_count;
            summary.dyn_object_only = sync.extra->dyn_object_only ? 1U : 0U;
            summary.lfe_present = sync.extra->lfe_present ? 1U : 0U;
            summary.content_description = sync.extra->content_description;
            summary.channel_assignment = sync.extra->channel_assignment;
            summary.isf = sync.extra->isf;
            summary.dynamic_object_count = sync.extra->dynamic_object_count;
        }
        summary.description = fmt::format(
            "{} {} {} {} {} {} {} {} {} {} {} {}", summary.size_bytes, summary.substream_info,
            summary.extended_substream_info, summary.substreams, summary.sample_rate,
            summary.extra_present, summary.extra_length, summary.channel_count,
            summary.dyn_object_only, summary.lfe_present, summary.content_description,
            summary.channel_assignment * 64 + summary.isf * 8 + summary.dynamic_object_count);
        const auto found =
            std::ranges::find(major_syncs_, summary.description, &MajorSyncSummary::description);
        if (found != major_syncs_.end()) {
            ++found->count;
        } else if (major_syncs_.size() < kMaxMajorSyncs) {
            summary.count = 1;
            summary.first_unit = unit_index;
            major_syncs_.push_back(std::move(summary));
        } else {
            ++major_syncs_other_;
        }
    }
    for (int i = 0; i < unit.num_substreams; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const th::SubstreamEntry& entry = unit.entries[index];
        if (entry.restart_nonexistent) {
            continue;
        }
        std::span<const std::byte> payload = unit.segments[index];
        if (entry.crc_present && payload.size() >= 3) {
            payload = payload.first(payload.size() - 2);
        }
        iclforge::BitReader reader(payload);
        if (reader.read(1) == 0 || reader.read(1) == 0) {
            continue;
        }
        RestartTally& tally = restarts_[index];
        ++tally.count;
        const auto header = th::read_restart_header(reader, payload, true);
        if (!header) {
            ++tally.failures[std::string(th::to_string(header.error().kind))];
            continue;
        }
        ++tally.syncs[header->sync];
        ++tally.tails[header->tail];
        tally.noise_shift_nonzero += header->noise_shift != 0 ? 1U : 0U;
        tally.data_check_present += header->data_check_present ? 1U : 0U;
        tally.min_chan.add(header->min_chan);
        tally.max_chan.add(header->max_chan);
        tally.max_matrix_chan.add(header->max_matrix_chan);
        tally.max_shift.add(header->max_shift);
        tally.max_lsbs.add(header->max_lsbs);
        tally.max_bits.add(header->max_bits);
        tally.max_bits_repeat.add(header->max_bits_repeat);
    }
}

void StructureSurvey::decode(std::span<const std::byte> stream) {
    th::DecoderConfig config;
    config.presentation = th::Presentation::kAll;
    config.strictness = th::Strictness::kLenient;
    th::Decoder decoder(config);
    th::ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        if (!decoded) {
            ++decode_errors_[std::string(th::to_string(decoded.error().kind))];
            continue;
        }
        if (*decoded) {
            samples_ += static_cast<std::uint64_t>((*decoded)->samples);
            padding_samples_ += static_cast<std::uint64_t>((*decoded)->padding_samples);
        }
    }
    counters_ = decoder.counters();
    decoded_ = true;
}

void StructureSurvey::write_json(iclforge::apps::JsonSink& json) const {
    json.begin_object();
    json.key("major_syncs");
    json.begin_array();
    for (const MajorSyncSummary& sync : major_syncs_) {
        json.begin_object();
        json.member("units", sync.count);
        json.member("first_unit", sync.first_unit);
        json.member("size_bytes", sync.size_bytes);
        json.member("sample_rate", sync.sample_rate);
        json.member("substreams", sync.substreams);
        json.member("substream_info", sync.substream_info);
        json.member("extended_substream_info", sync.extended_substream_info);
        json.member("extra_channel_meaning", sync.extra_present);
        if (sync.extra_present) {
            json.member("extra_channel_meaning_length", sync.extra_length);
            json.member("sixteench_channel_count", sync.channel_count);
            json.member("dyn_object_only", sync.dyn_object_only != 0);
            json.member("lfe_present", sync.lfe_present != 0);
            json.member("content_description", sync.content_description);
            json.member("channel_assignment", sync.channel_assignment);
            json.member("isf", sync.isf);
            json.member("dynamic_object_count", sync.dynamic_object_count);
        }
        json.end_object();
    }
    json.end_array();
    json.member("major_syncs_in_further_forms", major_syncs_other_);
    json.key("restart_headers");
    json.begin_array();
    for (std::size_t i = 0; i < restarts_.size(); ++i) {
        const RestartTally& tally = restarts_[i];
        if (tally.count == 0) {
            continue;
        }
        json.begin_object();
        json.member("substream", static_cast<std::uint64_t>(i));
        json.member("count", tally.count);
        json.key("failures");
        json.begin_array();
        for (const auto& [kind, count] : tally.failures) {
            json.begin_object();
            json.member("kind", kind);
            json.member("count", count);
            json.end_object();
        }
        json.end_array();
        json.key("syncs");
        json.begin_array();
        for (const auto& [sync, count] : top(tally.syncs)) {
            json.begin_object();
            json.member("sync", fmt::format("0x{:04X}", sync));
            json.member("count", count);
            json.end_object();
        }
        json.end_array();
        json.key("tail_16_bits");
        json.begin_array();
        for (const auto& [value, count] : top(tally.tails)) {
            json.begin_object();
            json.member("value", fmt::format("0x{:04X}", value));
            json.member("count", count);
            json.end_object();
        }
        json.end_array();
        json.member("tail_distinct_values", static_cast<std::uint64_t>(tally.tails.size()));
        json.member("noise_shift_nonzero", tally.noise_shift_nonzero);
        json.member("data_check_present", tally.data_check_present);
        write_range(json, "min_chan", tally.min_chan.seen, tally.min_chan.min, tally.min_chan.max);
        write_range(json, "max_chan", tally.max_chan.seen, tally.max_chan.min, tally.max_chan.max);
        write_range(json, "max_matrix_chan", tally.max_matrix_chan.seen, tally.max_matrix_chan.min,
                    tally.max_matrix_chan.max);
        write_range(json, "max_shift", tally.max_shift.seen, tally.max_shift.min,
                    tally.max_shift.max);
        write_range(json, "max_lsbs", tally.max_lsbs.seen, tally.max_lsbs.min, tally.max_lsbs.max);
        write_range(json, "max_bits", tally.max_bits.seen, tally.max_bits.min, tally.max_bits.max);
        write_range(json, "max_bits_repeat", tally.max_bits_repeat.seen, tally.max_bits_repeat.min,
                    tally.max_bits_repeat.max);
        json.end_object();
    }
    json.end_array();
    json.key("decode");
    if (!decoded_) {
        json.value_null();
    } else {
        json.begin_object();
        json.member("units", counters_.units);
        json.member("units_decoded", counters_.units_decoded);
        json.member("units_waiting", counters_.units_waiting);
        json.member("restarts", counters_.restarts);
        json.member("lossless_checks_ok", counters_.lossless_checks_ok);
        json.member("lossless_check_failures", counters_.lossless_check_failures);
        json.member("check_failures", counters_.check_failures);
        json.member("samples", samples_);
        json.member("padding_samples", padding_samples_);
        json.key("errors");
        json.begin_array();
        for (const auto& [kind, count] : decode_errors_) {
            json.begin_object();
            json.member("kind", kind);
            json.member("count", count);
            json.end_object();
        }
        json.end_array();
        json.key("matrices");
        json.begin_object();
        json.member("block_headers_with_matrices", counters_.matrix_headers);
        json.member("matrices_with_lsb_bypass", counters_.lsb_bypass_matrices);
        json.end_object();
        json.key("segment_tails");
        json.begin_array();
        for (std::size_t i = 0; i < counters_.tails.size(); ++i) {
            const th::SegmentTailCounters& tail = counters_.tails[i];
            if (tail.segments == 0) {
                continue;
            }
            json.begin_object();
            json.member("substream", static_cast<std::uint64_t>(i));
            json.member("segments", tail.segments);
            json.member("zero_bits_only", tail.zero);
            json.member("terminator_a_at_once", tail.terminator_a);
            json.member("terminator_b_at_once", tail.terminator_b);
            json.member("terminator_b_at_the_end", tail.terminator_b_at_end);
            json.member("end_marker_at_the_end", tail.end_marker);
            json.member("other", tail.other);
            json.member("bits_after_last_block_min", tail.bits_min);
            json.member("bits_after_last_block_max", tail.bits_max);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_object();
}

void StructureSurvey::print_text() const {
    fmt::println("{:<16}stream syntax (provisional readings; ERRATA questions 2, 3, 10, 11)",
                 "structure");
    for (const MajorSyncSummary& sync : major_syncs_) {
        fmt::println(
            "{:<16}major sync x{} (from unit {}): {} byte(s), {} Hz, {} substream(s), "
            "substream_info 0x{:02X}, extended_substream_info {}{}",
            "", sync.count, sync.first_unit, sync.size_bytes, sync.sample_rate, sync.substreams,
            sync.substream_info, sync.extended_substream_info,
            sync.extra_present
                ? fmt::format(", extra channel meaning: length field {}, {} channel(s), "
                              "{}, content description {}, assignment 0x{:03X}, ISF {}, {} "
                              "dynamic object(s)",
                              sync.extra_length, sync.channel_count,
                              sync.dyn_object_only != 0 ? "dynamic objects only" : "with a bed",
                              sync.content_description, sync.channel_assignment, sync.isf,
                              sync.dynamic_object_count)
                : std::string());
    }
    for (std::size_t i = 0; i < restarts_.size(); ++i) {
        const RestartTally& tally = restarts_[i];
        if (tally.count == 0) {
            continue;
        }
        std::string syncs;
        for (const auto& [sync, count] : top(tally.syncs)) {
            syncs += fmt::format("{}0x{:04X} x{}", syncs.empty() ? "" : ", ", sync, count);
        }
        std::string tails;
        for (const auto& [value, count] : top(tally.tails)) {
            tails += fmt::format("{}0x{:04X} x{}", tails.empty() ? "" : ", ", value, count);
        }
        std::string failures;
        for (const auto& [kind, count] : tally.failures) {
            failures += fmt::format(", {} x{}", kind, count);
        }
        fmt::println(
            "{:<16}substream {} restart headers: {} (syncs {}; header CRC failures etc.: "
            "{}); the 16 bits that end the fixed part: {}{}; noise shift set in {}, data "
            "check in {}",
            "", i, tally.count, syncs, failures.empty() ? "none" : failures.substr(2), tails,
            tally.tails.size() > kMaxListed ? " and others" : "", tally.noise_shift_nonzero,
            tally.data_check_present);
    }
    if (!decoded_) {
        return;
    }
    fmt::println(
        "{:<16}decode of every substream, lenient: {} unit(s), {} decoded, {} waiting, {} "
        "restart(s); lossless check {} ok, {} failed; other checks failed {}",
        "", counters_.units, counters_.units_decoded, counters_.units_waiting, counters_.restarts,
        counters_.lossless_checks_ok, counters_.lossless_check_failures, counters_.check_failures);
    for (const auto& [kind, count] : decode_errors_) {
        fmt::println("{:<16}  unit(s) that did not decode: {} x{}", "", kind, count);
    }
    fmt::println("{:<16}block headers with matrices: {}; matrices with lsb_bypass: {}", "",
                 counters_.matrix_headers, counters_.lsb_bypass_matrices);
    for (std::size_t i = 0; i < counters_.tails.size(); ++i) {
        const th::SegmentTailCounters& tail = counters_.tails[i];
        if (tail.segments == 0) {
            continue;
        }
        fmt::println(
            "{:<16}substream {} after the last block, {} segment(s): zero bits only {}, "
            "terminatorA at once {}, terminatorB at once {} and at the end {}, end marker at "
            "the end {}, other {}; {} to {} bit(s) left",
            "", i, tail.segments, tail.zero, tail.terminator_a, tail.terminator_b,
            tail.terminator_b_at_end, tail.end_marker, tail.other, tail.bits_min, tail.bits_max);
    }
}

}  // namespace forge_cli::commands
