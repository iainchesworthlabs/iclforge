// LVGL's heap, in PSRAM (planning/karaoke-p4x-panel.md, Q3).
//
// ESP-IDF's malloc() sends an allocation of up to CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) to
// internal RAM first, and LVGL's objects, styles, glyph caches and draw buffers are almost all
// smaller than that: they would take the internal RAM the AC-4 decoder has 1 to 11 KB of left while
// it plays. So LVGL is given a memory source of its own, which asks PSRAM and nothing else
// (CONFIG_LV_USE_CUSTOM_MALLOC, sdkconfig.panel). The cost is that every LVGL allocation is behind the
// cache; the screen's data is small and its hot loop is the glyph blit, which reads a cached
// bitmap.
//
// These are the nine functions lv_mem.h asks a custom memory source for. LVGL is C and calls them
// by their C names.

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <cstddef>

#include "esp_heap_caps.h"

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* /*mem*/, size_t /*bytes*/) {
    // Not supported: the heap is ESP-IDF's.
    return nullptr;
}

void lv_mem_remove_pool(lv_mem_pool_t /*pool*/) {}

void* lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* p, size_t new_size) {
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    // What PSRAM has left, for the monitor LVGL offers; the rest is not tracked.
    *mon_p = lv_mem_monitor_t{};
    mon_p->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

namespace player::panel {

// Called from panel_start() so that this object is pulled out of the main component's archive, which
// the linker has passed by the time LVGL's archive asks for lv_malloc_core and the rest: nothing in
// the component names them, and a member is taken only for a symbol already wanted.
bool psram_heap_ready() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

}  // namespace player::panel

#else
#error "main/panel/lvgl needs CONFIG_LV_USE_CUSTOM_MALLOC=y (sdkconfig.panel) so LVGL's heap is PSRAM"
#endif
