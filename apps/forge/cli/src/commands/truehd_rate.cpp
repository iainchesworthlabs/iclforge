#include "truehd_rate.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <cstdlib>

#include "json_sink.hpp"

namespace forge_cli::commands {

namespace th = iclforge::truehd;

std::string format_headroom_ms(std::int64_t microseconds) {
    const std::uint64_t magnitude = static_cast<std::uint64_t>(std::llabs(microseconds));
    return fmt::format("{}{}.{:03}", microseconds < 0 ? "-" : "", magnitude / 1000,
                       magnitude % 1000);
}

void RateSurvey::run(std::span<const std::byte> stream) {
    if (wanted_) {
        report_ = th::analyse_rate(stream, model_bps_);
    }
}

void RateSurvey::write_json(iclforge::apps::JsonSink& json) const {
    if (!wanted_ || !report_) {
        json.value_null();
        return;
    }
    const th::RateReport& r = *report_;
    json.begin_object();
    json.member("model_rate_bps", r.peak_data_rate_bps);
    json.member("latency_ms", static_cast<std::uint64_t>(th::kRateLatencyMs));
    json.member("access_units", r.units);
    json.member("bytes", r.bytes);
    json.member("bitrate_bps", static_cast<std::uint64_t>(r.bitrate_bps));
    json.member("peak_unit_bps", static_cast<std::uint64_t>(r.peak_unit_bps));
    json.member("min_headroom_us", r.min_headroom_us);
    json.member("late_units", static_cast<std::uint64_t>(r.late_units));
    json.key("intervals");
    json.begin_array();
    for (const th::IntervalRate& interval : r.intervals) {
        json.begin_object();
        json.member("first_unit", interval.first_unit);
        json.member("units", interval.units);
        json.member("bytes", interval.bytes);
        json.member("bitrate_bps", static_cast<std::uint64_t>(interval.bitrate_bps));
        json.member("peak_unit_bps", static_cast<std::uint64_t>(interval.peak_unit_bps));
        json.member("min_headroom_us", interval.min_headroom_us);
        json.member("late_units", static_cast<std::uint64_t>(interval.late_units));
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void RateSurvey::print_text() const {
    if (!wanted_) {
        return;
    }
    if (!report_) {
        fmt::println("{:<16}unavailable: {}", "rate model", report_.error().detail);
        return;
    }
    const th::RateReport& r = *report_;
    fmt::println(
        "{:<16}arrival model at {} kbit/s over {} ms: {} interval(s), mean {} kbit/s, largest "
        "unit {} kbit/s, least headroom {} ms, {} late unit(s)",
        "rate model", r.peak_data_rate_bps / 1000, th::kRateLatencyMs, r.intervals.size(),
        r.bitrate_bps / 1000, r.peak_unit_bps / 1000, format_headroom_ms(r.min_headroom_us),
        r.late_units);
    fmt::println("  {:>8} {:>10} {:>6} {:>12} {:>12} {:>12} {:>5}", "interval", "first unit",
                 "units", "kbit/s", "peak kbit/s", "headroom ms", "late");
    std::size_t number = 0;
    for (const th::IntervalRate& interval : r.intervals) {
        fmt::println("  {:>8} {:>10} {:>6} {:>12} {:>12} {:>12} {:>5}", number++,
                     interval.first_unit, interval.units, interval.bitrate_bps / 1000,
                     interval.peak_unit_bps / 1000, format_headroom_ms(interval.min_headroom_us),
                     interval.late_units);
    }
}

}  // namespace forge_cli::commands
