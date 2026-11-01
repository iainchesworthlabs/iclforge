#include "iclforge/truehd/encoder/ratectl.hpp"

#include <algorithm>
#include <limits>

#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace iclforge::truehd {

namespace {

// Division rounding toward minus infinity, for a numerator that can be negative.
[[nodiscard]] std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
    std::int64_t quotient = numerator / denominator;
    if ((numerator % denominator != 0) && ((numerator < 0) != (denominator < 0))) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] std::uint32_t clamp_u32(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

}  // namespace

RateModel::RateModel(std::uint32_t sample_rate, int nominal_samples,
                     std::uint64_t rate_bps) noexcept
    : sample_rate_(sample_rate),
      nominal_(nominal_samples),
      rate_bps_(std::max<std::uint64_t>(rate_bps, 1)),
      latency_limit_(static_cast<std::int64_t>(sample_rate) * kRateLatencyMs / 1000) {}

RateStep RateModel::admit(std::int64_t first_sample, std::size_t unit_bytes) noexcept {
    // The unit may start to arrive when its slot opens and the previous unit has arrived (TH spec
    // §3.4). The encoder used to add "and no sooner than one unit duration after the previous
    // start", which keeps the lag of a late unit for the rest of the stream, since the slots
    // advance by exactly that much: a unit could never make up the time and every burst was
    // permanent. A backlog drains here whenever units arrive in less than their own duration
    // (ERRATA.md question 72).
    const std::int64_t slot = first_sample - nominal_;
    std::int64_t start = slot;
    if (started_) {
        start = std::max(slot, last_arrival_end_);
    }
    RateStep step;
    step.start = start;
    step.arrival = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(unit_bytes) * 8U * sample_rate_ + rate_bps_ - 1U) / rate_bps_);
    step.headroom = latency_limit_ - (start + step.arrival - slot);
    started_ = true;
    last_arrival_end_ = start + step.arrival;
    return step;
}

void RateModel::reset() noexcept {
    started_ = false;
    last_arrival_end_ = 0;
}

std::uint64_t declared_peak_rate_bps(std::uint64_t requested_bps,
                                     std::uint32_t sample_rate) noexcept {
    // The field is raw = rate * 16 / sample rate, at most 15 bits; a reader gets raw * sample rate /
    // 16 back, rounded to nearest (MajorSync::peak_data_rate_bps).
    const std::uint64_t raw = std::min<std::uint64_t>(0x7FFF, (requested_bps * 16U) / sample_rate);
    return (raw * sample_rate + 8U) >> 4U;
}

std::int64_t samples_to_us(std::int64_t samples, std::uint32_t sample_rate) noexcept {
    return floor_div(samples * 1'000'000, static_cast<std::int64_t>(sample_rate));
}

IntervalMeter::IntervalMeter(std::uint32_t sample_rate, int nominal_samples) noexcept
    : sample_rate_(sample_rate), nominal_(nominal_samples) {}

void IntervalMeter::begin(std::uint64_t first_unit) noexcept {
    now_ = IntervalRate{};
    now_.first_unit = first_unit;
    min_headroom_ = 0;
    any_ = false;
}

void IntervalMeter::add(std::size_t unit_bytes, int samples, const RateStep& step) noexcept {
    ++now_.units;
    now_.samples += static_cast<std::uint64_t>(samples);
    now_.bytes += unit_bytes;
    now_.max_unit_bytes = std::max(now_.max_unit_bytes, clamp_u32(unit_bytes));
    if (step.late()) {
        ++now_.late_units;
    }
    min_headroom_ = any_ ? std::min(min_headroom_, step.headroom) : step.headroom;
    any_ = true;
}

IntervalRate IntervalMeter::finish() const noexcept {
    IntervalRate out = now_;
    if (now_.samples != 0) {
        out.bitrate_bps = clamp_u32(now_.bytes * 8U * sample_rate_ / now_.samples);
    }
    out.peak_unit_bps = clamp_u32(std::uint64_t{now_.max_unit_bytes} * 8U * sample_rate_ /
                                  static_cast<std::uint64_t>(nominal_));
    out.min_headroom_us = any_ ? samples_to_us(min_headroom_, sample_rate_) : 0;
    return out;
}

std::expected<RateReport, Error> analyse_rate(std::span<const std::byte> stream,
                                              std::uint64_t peak_bps) {
    ElementaryReader reader(stream);
    RateReport report;
    int substreams = 0;
    std::uint64_t unit_number = 0;
    std::int64_t min_headroom = 0;
    bool any_unit = false;
    std::optional<RateModel> model;
    std::optional<IntervalMeter> meter;
    bool open = false;
    std::uint32_t peak_unit_bytes = 0;

    const auto close_interval = [&] {
        if (open) {
            report.intervals.push_back(meter->finish());
            open = false;
        }
    };
    while (const auto unit = reader.next()) {
        const auto parsed = parse_access_unit(*unit, substreams, true);
        if (!parsed) {
            continue;
        }
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
            if (!model) {
                const MajorSync& sync = *parsed->major_sync;
                report.sample_rate = sync.sample_rate();
                report.samples_per_unit = nominal_samples_per_access_unit(sync.ratebits());
                report.peak_data_rate_bps = peak_bps != 0 ? peak_bps : sync.peak_data_rate_bps();
                model.emplace(report.sample_rate, report.samples_per_unit,
                              report.peak_data_rate_bps);
                meter.emplace(report.sample_rate, report.samples_per_unit);
            }
        }
        if (!model) {
            continue;
        }
        // An interval is the units from one restart header of the first substream to the next.
        const bool restart = parsed->num_substreams > 0 && !parsed->entries[0].restart_nonexistent;
        if (restart || !open) {
            close_interval();
            meter->begin(unit_number);
            open = true;
        }
        const RateStep step =
            model->admit(static_cast<std::int64_t>(unit_number) * report.samples_per_unit,
                         unit->size());
        meter->add(unit->size(), report.samples_per_unit, step);
        ++report.units;
        report.bytes += unit->size();
        peak_unit_bytes = std::max(peak_unit_bytes, static_cast<std::uint32_t>(unit->size()));
        report.late_units += step.late() ? 1U : 0U;
        min_headroom = any_unit ? std::min(min_headroom, step.headroom) : step.headroom;
        any_unit = true;
        ++unit_number;
    }
    close_interval();
    if (!model) {
        return std::unexpected(Error{ErrorKind::kBadSync, "no access unit with a major sync"});
    }
    const std::uint64_t samples = report.units * static_cast<std::uint64_t>(report.samples_per_unit);
    if (samples != 0) {
        report.bitrate_bps = clamp_u32(report.bytes * 8U * report.sample_rate / samples);
    }
    report.peak_unit_bps = clamp_u32(std::uint64_t{peak_unit_bytes} * 8U * report.sample_rate /
                                     static_cast<std::uint64_t>(report.samples_per_unit));
    report.min_headroom_us = any_unit ? samples_to_us(min_headroom, report.sample_rate) : 0;
    return report;
}

}  // namespace iclforge::truehd
