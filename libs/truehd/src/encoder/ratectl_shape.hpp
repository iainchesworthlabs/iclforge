#pragma once

#include <cstdint>
#include <span>
#include <vector>

// The one lever rate control has on a lossless stream: how many low bits the object channels of
// substream 3 keep. Giving some up leaves them clear in every sample, which the output shift and
// the quantiser step send for nothing (TH spec §8.2), so the substream shrinks by about one bit a
// sample a channel for each bit given up, and the 7.1 bed of substreams 0 to 2 is untouched. Off
// unless RateMode::kShape asks for it (ERRATA.md entry 71).

namespace iclforge::truehd::detail {

// The number of low bits every sample of `planes` leaves clear: 24 for silence, which has nothing
// to give up.
[[nodiscard]] int common_zero_bits(std::span<const std::vector<std::int32_t>> planes) noexcept;

// Rounds every sample of `planes` (24 bit values) to the nearest multiple of 2^(c + drop_bits),
// where c is what common_zero_bits() found (nothing happens for silence), but never to a step above
// `limit_zero_bits`, and keeps the rounded value within 24 bits. Returns the step now left clear: c
// when nothing could be given up.
int shape_planes(std::span<std::vector<std::int32_t>> planes, int drop_bits, int limit_zero_bits);

}  // namespace iclforge::truehd::detail
