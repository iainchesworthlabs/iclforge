#pragma once

// What a host test of the component's player code controls in the stand-ins for the part's kernel,
// heap and clock (shim_runtime.cpp).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fwhost {

// One allocation the code under test holds: its size, and the capabilities it asked for
// (MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL).
struct Allocation {
    std::size_t bytes = 0;
    std::uint32_t caps = 0;
};

// Whether the part has PSRAM (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0). On by default.
void set_psram_present(bool present);

[[nodiscard]] std::vector<Allocation> live_allocations();
// The most bytes held at once by allocations with `caps` among theirs, since the last reset.
[[nodiscard]] std::size_t peak_bytes(std::uint32_t caps);
void reset_allocation_peaks();

// esp_timer_get_time() reads the clock a test sets (microseconds), or the host's own.
void use_virtual_clock(std::int64_t now_us);
void set_virtual_now(std::int64_t now_us);
void use_real_clock();

// Returns when the player's decode task has taken every message the ring holds and is waiting for
// the next: the chunk sent last has been decoded and rendered.
void wait_idle();

}  // namespace fwhost
