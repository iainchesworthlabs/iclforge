#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/export.hpp"

// Rate control for a TrueHD stream: the arrival model the encoder holds every access unit to, the
// figures it keeps per restart interval, and a reader that works the same figures out of a stream
// that is already written (`forge probe detail=rate`).
//
// The model is TH spec §3.4 and §18: a unit may start to arrive when its slot opens (one unit before
// its first sample) and the unit before it has arrived; it arrives at the declared peak data rate;
// and a decoder with 75 ms of buffer has the whole unit in time only if its arrival ends within 75 ms
// of its slot. A unit that arrives in less than its own duration takes back time a longer one lost.
// The distance to that limit is the unit's headroom. A stream whose units all have headroom of 0 or
// more can be sent at
// the declared peak rate. PROVISIONAL: the model is the encoder's reading of the specification and no
// stream of the owner's has been checked against it (libs/truehd/ERRATA.md, entry 37).
//
// A second model of the same shape runs at the bitrate the caller chose, which may be below the
// declared peak rate: a stream within it never needs more than 75 ms of buffering at that rate, so
// its average over any window of that length is at most the chosen bitrate.

namespace iclforge::truehd {

// TH spec §3.4, §18.1.
inline constexpr int kRateLatencyMs = 75;

// One access unit's place in the arrival model. All three are counts of samples of the stream's own
// clock (the sample rate is the model's), so that nothing in them is a floating point figure.
struct RateStep {
    std::int64_t start = 0;     // when the unit begins to arrive
    std::int64_t arrival = 0;   // how long its arrival takes at the model's rate
    std::int64_t headroom = 0;  // the latency limit less its arrival's end, from its slot; below 0:
                                // the unit is late
    [[nodiscard]] bool late() const noexcept { return headroom < 0; }
};

// The arrival model at one rate. A value type: copy it to try a unit without committing it.
class ICLFORGE_TRUEHD_EXPORT RateModel {
   public:
    // `nominal_samples` is the samples in a full access unit (40, 80 or 160), `rate_bps` the rate the
    // units arrive at.
    RateModel(std::uint32_t sample_rate, int nominal_samples, std::uint64_t rate_bps) noexcept;

    // Takes the next unit, whose first sample is `first_sample` on the stream's clock, `unit_bytes`
    // long. The first unit after construction or reset() starts at its slot.
    RateStep admit(std::int64_t first_sample, std::size_t unit_bytes) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::int64_t latency_limit() const noexcept { return latency_limit_; }
    [[nodiscard]] std::uint64_t rate_bps() const noexcept { return rate_bps_; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

   private:
    std::uint32_t sample_rate_;
    std::int64_t nominal_;
    std::uint64_t rate_bps_;
    std::int64_t latency_limit_;
    bool started_ = false;
    std::int64_t last_arrival_end_ = 0;
};

// The rate a major sync's `peak_data_rate` field declares for a requested peak rate (TH spec §4.4,
// ERRATA.md entry 30): the largest field value that does not exceed it, read back the way a reader
// does. The encoder's model runs at this rate, so a reader's model of the stream agrees with it.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint64_t declared_peak_rate_bps(
    std::uint64_t requested_bps, std::uint32_t sample_rate) noexcept;

// What the encoder and the reader keep for each restart interval.
struct IntervalRate {
    std::uint64_t first_unit = 0;
    std::uint64_t units = 0;
    std::uint64_t samples = 0;  // per channel, padding included
    std::uint64_t bytes = 0;
    std::uint32_t bitrate_bps = 0;       // bytes of the interval over its duration, rounded down
    std::uint32_t peak_unit_bps = 0;     // the largest unit's bytes over one unit's duration
    std::uint32_t max_unit_bytes = 0;
    std::int64_t min_headroom_us = 0;    // under the declared peak rate, in microseconds, rounded down
    std::uint32_t late_units = 0;        // units whose headroom is below 0
    friend bool operator==(const IntervalRate&, const IntervalRate&) = default;
};

// Builds the figures of one interval, unit by unit.
class ICLFORGE_TRUEHD_EXPORT IntervalMeter {
   public:
    IntervalMeter(std::uint32_t sample_rate, int nominal_samples) noexcept;
    // Starts an interval whose first unit is number `first_unit`.
    void begin(std::uint64_t first_unit) noexcept;
    void add(std::size_t unit_bytes, int samples, const RateStep& step) noexcept;
    // The figures so far. An interval with no unit has all of them 0.
    [[nodiscard]] IntervalRate finish() const noexcept;

   private:
    std::uint32_t sample_rate_;
    int nominal_;
    IntervalRate now_;
    std::int64_t min_headroom_ = 0;
    bool any_ = false;
};

// Microseconds in `samples` at `sample_rate`, rounded toward minus infinity.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::int64_t samples_to_us(std::int64_t samples,
                                                                std::uint32_t sample_rate) noexcept;

struct RateReport {
    std::uint32_t sample_rate = 0;
    int samples_per_unit = 0;
    std::uint64_t peak_data_rate_bps = 0;  // the rate the model ran at
    std::vector<IntervalRate> intervals;
    std::uint64_t units = 0;
    std::uint64_t bytes = 0;
    std::uint32_t bitrate_bps = 0;       // the whole stream
    std::uint32_t peak_unit_bps = 0;
    std::int64_t min_headroom_us = 0;
    std::uint32_t late_units = 0;
};

// The figures of a stream held in memory: the arrival model at `peak_bps` (0: the rate the first
// major sync declares) run over its access units, which are cut into intervals at the units whose
// first substream carries a restart header. Units that do not read are skipped as the elementary
// reader skips them. kBadSync if the data holds no stream.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<RateReport, Error> analyse_rate(
    std::span<const std::byte> stream, std::uint64_t peak_bps = 0);

}  // namespace iclforge::truehd
