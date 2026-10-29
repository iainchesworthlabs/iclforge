#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"

// Rate control: the input timing of each access unit under the decoder's FIFO model (3.4, 18).
//
// Section 18 says the stream is delivered at no more than peak_data_rate into a FIFO, that
// input_timing drives it, and that the decoder's latency stays under 75 ms; 3.4 bounds the step
// between consecutive input timings below by a quarter of an access unit and by the previous
// unit's duration, and above by 75 ms of samples. The model this encoder schedules against (P33):
//
//   - access unit n is decoded from sample time n * T (T samples per access unit) and must have
//     arrived whole by then;
//   - its bits arrive at the peak rate, from its input timing a_n, taking d_n = bits * fs / R;
//   - a_n is at most (n - 1) * T, FFmpeg's timing for a stream that never nears its peak, and no
//     earlier than the end of the unit before it, a quarter unit after that unit's start, or
//     75 ms before its decode time.
//
// Each restart interval is placed as a whole, latest first, so an access unit too large for its
// own slot is moved earlier into the slack of the ones before it. When the interval cannot be
// placed, the stream cannot be delivered at the peak rate: kRateExceeded.

namespace iclforge::mlp::detail {

class Schedule {
   public:
    Schedule() = default;
    Schedule(std::uint64_t peak_bits_per_second, std::uint32_t sample_rate, int samples_per_unit);

    // The 16-bit input timing of each of the next access units, given their sizes in bits.
    [[nodiscard]] std::expected<std::vector<std::uint16_t>, Error> place(
        std::span<const std::size_t> bits);

    // The highest rate an access unit has been delivered at so far: its bits over the time from
    // its input timing to the next unit's.
    [[nodiscard]] std::uint64_t peak_rate() const noexcept { return peak_; }

   private:
    double rate_ = 18e6;
    double fs_ = 48000.0;
    double t_ = 40.0;
    double latency_ = 3600.0;  // 75 ms of samples
    std::int64_t next_ = 0;    // the index of the next access unit
    double prev_start_ = -1e300;
    double prev_end_ = -1e300;
    double prev_bits_ = 0.0;
    std::uint64_t peak_ = 0;
};

}  // namespace iclforge::mlp::detail
