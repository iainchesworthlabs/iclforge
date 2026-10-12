// A display with no panel behind it: LVGL draws into a frame buffer in PSRAM, which is what a board
// with no screen runs to see the screen (GET /panel.bmp reads it back) and to measure what drawing
// it costs. See display.hpp and planning/karaoke-p4x-panel.md, "Measuring on the board with no
// panel".
//
// What it leaves out is the glass's own cost: the DSI host reading the whole frame out of PSRAM
// sixty times a second (74 MB/s of the bus the decoder fetches its code from), and the 2D engine
// that copies a rendered area into the frame buffer. It leaves those out on purpose and the plan
// says so; what is left is exactly LVGL's work, the part that takes the CPU.

#include "display.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace player::panel {

namespace {

constexpr int kWidth = CONFIG_ICLFORGE_EXAMPLE_PANEL_WIDTH;
constexpr int kHeight = CONFIG_ICLFORGE_EXAMPLE_PANEL_HEIGHT;

std::uint16_t* g_frame = nullptr;

void flush(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    const int width = lv_area_get_width(area);
    const auto* src = reinterpret_cast<const std::uint16_t*>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        std::memcpy(g_frame + static_cast<std::size_t>(y) * kWidth + area->x1, src,
                    static_cast<std::size_t>(width) * sizeof(std::uint16_t));
        src += width;
    }
    lv_display_flush_ready(disp);
}

void read_pointer(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    InjectedPointer& pointer = injected_pointer();
    data->point.x = pointer.x.load();
    data->point.y = pointer.y.load();
    data->state = pointer.down.load() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

bool display_open(Display& out) {
    g_frame = static_cast<std::uint16_t*>(heap_caps_aligned_calloc(
        64, 1, static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_frame == nullptr) {
        std::printf("panel: no PSRAM for the %d by %d frame buffer\n", kWidth, kHeight);
        return false;
    }

    constexpr std::size_t kBufferBytes =
        static_cast<std::size_t>(kWidth) * CONFIG_ICLFORGE_EXAMPLE_PANEL_LINE_BUFFER_ROWS *
        sizeof(std::uint16_t);
    void* const first = heap_caps_aligned_alloc(64, kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* const second = heap_caps_aligned_alloc(64, kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (first == nullptr || second == nullptr) {
        std::printf("panel: no PSRAM for the render buffers\n");
        heap_caps_free(first);
        heap_caps_free(second);
        heap_caps_free(g_frame);
        g_frame = nullptr;
        return false;
    }

    lv_display_t* const disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, first, second, static_cast<std::uint32_t>(kBufferBytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, &flush);

    lv_indev_t* const pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, &read_pointer);
    lv_indev_set_display(pointer, disp);

    out.lv = disp;
    out.width = kWidth;
    out.height = kHeight;
    out.frame = g_frame;
    out.touch = true;  // injected taps
    out.kind = "null";
    std::printf("panel: a %d by %d frame buffer in PSRAM, no panel behind it\n", kWidth, kHeight);
    return true;
}

void display_set_backlight(int /*percent*/) {}

}  // namespace player::panel
