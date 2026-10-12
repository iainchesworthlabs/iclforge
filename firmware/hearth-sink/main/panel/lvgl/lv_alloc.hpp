#pragma once

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "lvgl.h"

// Containers whose memory is LVGL's: on the firmware, PSRAM (lv_memory.cpp gives LVGL a heap of its
// own that asks PSRAM and nothing else); on a computer, LVGL's own allocator.
//
// What the screen copies from the player (the visible rows, a file with no times as plain text, the
// title) lives as long as the item does. On a P4 that decodes AC-4 the internal RAM it would take is
// the little the decoder leaves (1 to 11 KB, planning/karaoke-p4x-panel.md, Q3, Q10), and a plain
// text of 12,000 bytes is more than that, so its bytes are asked of the PSRAM heap instead of
// ESP-IDF's malloc, which tries internal RAM first for anything up to 16 KB.
//
// Only the screen's task allocates or frees through these (the copies are made and read there), as
// LVGL's own objects are.

namespace player::panel {

template <class T>
struct LvAllocator {
    using value_type = T;

    LvAllocator() noexcept = default;
    template <class U>
    LvAllocator(const LvAllocator<U>& /*other*/) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            std::abort();
        }
        void* const memory = lv_malloc(count * sizeof(T));
        if (memory == nullptr) {
            // The memory a std::allocator would have thrown for, on a build with no exceptions: PSRAM
            // is out, which the screen cannot carry on without.
            std::abort();
        }
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, std::size_t /*count*/) noexcept { lv_free(memory); }

    template <class U>
    bool operator==(const LvAllocator<U>& /*other*/) const noexcept {
        return true;
    }
};

using LvString = std::basic_string<char, std::char_traits<char>, LvAllocator<char>>;

template <class T>
using LvVector = std::vector<T, LvAllocator<T>>;

template <class K, class V>
using LvMap = std::map<K, V, std::less<K>, LvAllocator<std::pair<const K, V>>>;

}  // namespace player::panel
