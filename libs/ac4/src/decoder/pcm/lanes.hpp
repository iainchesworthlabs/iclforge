#pragma once

#include <cstddef>
#include <type_traits>

#include "iclforge/ac4/decoder/executor.hpp"

// fn(index, lane) for each index below `count`: through the executor
// (iclforge/ac4/decoder/executor.hpp) where it has a second lane to give, and in order on the
// calling thread, as lane 0, where not. The iterations must not touch each other's data: the
// decoded PCM is the same either way, bit for bit.

namespace iclforge::ac4::detail {

template <typename Fn>
void run_lanes(Executor* executor, std::size_t count, Fn&& fn) {
    if (executor == nullptr || count < 2 || executor->lanes() < 2) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i, std::size_t{0});
        }
        return;
    }
    using F = std::remove_reference_t<Fn>;
    executor->run(
        count,
        [](void* context, std::size_t index, std::size_t lane) {
            (*static_cast<F*>(context))(index, lane);
        },
        &fn);
}

}  // namespace iclforge::ac4::detail
