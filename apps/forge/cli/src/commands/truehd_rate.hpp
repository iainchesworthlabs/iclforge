#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"

namespace iclforge::apps {
class JsonSink;
}  // namespace iclforge::apps

// `forge probe detail=rate`: the arrival model of libs/truehd/ratectl.hpp run over a stream's
// access units, as figures for each restart interval: its bitrate, the largest unit's rate and the
// least headroom any unit has before it would arrive later than the decoder's 75 ms of buffer (TH
// spec 3.4, 18; provisional, ERRATA.md entry 71). Counts and rates only, never audio. The output is
// a stable contract like the rest of `probe`: integers, headroom in microseconds in JSON and in
// milliseconds with three decimals in text.

namespace forge_cli::commands {

// The headroom in microseconds as milliseconds with three decimals ("74.583", "-16.000").
[[nodiscard]] std::string format_headroom_ms(std::int64_t microseconds);

class RateSurvey {
   public:
    // `model_bps` 0 runs the model at the rate the stream's major sync declares.
    explicit RateSurvey(bool wanted, std::uint64_t model_bps = 0) noexcept
        : wanted_(wanted), model_bps_(model_bps) {}

    [[nodiscard]] bool wanted() const noexcept { return wanted_; }
    void run(std::span<const std::byte> stream);
    // Writes the member's value (an object); the caller wrote its key.
    void write_json(iclforge::apps::JsonSink& json) const;
    void print_text() const;

   private:
    bool wanted_;
    std::uint64_t model_bps_;
    std::expected<iclforge::truehd::RateReport, iclforge::truehd::Error> report_ =
        std::unexpected(iclforge::truehd::Error{});
};

}  // namespace forge_cli::commands
