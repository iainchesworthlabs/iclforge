#pragma once

#include <cstddef>
#include <type_traits>

#include "iclforge/base/executor.hpp"

// A decoder's independent per-channel (or per-object) work, handed to the caller's Executor
// (DecoderConfig::executor, iclforge/base/executor.hpp) where it has a second lane to give, and
// run in order on the calling thread where it has not. Each call of `f(index, lane)` must touch
// state no other call touches and take its scratch from the lane it is given, so the result is
// the same bit for bit however the calls were placed (libs/ac3/tests/decoder/test_lanes.cpp).
//
// More lanes than kMaxLanes is not used: the scratch a decoder keeps for its lanes is sized for
// that many, and the parts this exists for have two cores.

namespace iclforge::ac3::internal {

inline constexpr std::size_t kMaxLanes = 2;

template <typename F>
void run_lanes(iclforge::base::Executor* executor, std::size_t count, F&& f) {
    if (executor == nullptr || count < 2 || executor->lanes() < 2 || executor->lanes() > kMaxLanes) {
        for (std::size_t index = 0; index < count; ++index) {
            f(index, std::size_t{0});
        }
        return;
    }
    using Fn = std::remove_reference_t<F>;
    executor->run(
        count,
        [](void* context, std::size_t index, std::size_t lane) {
            (*static_cast<Fn*>(context))(index, lane);
        },
        const_cast<void*>(static_cast<const void*>(&f)));
}

}  // namespace iclforge::ac3::internal
