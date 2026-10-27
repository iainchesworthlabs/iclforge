// A MIPI DSI panel and its touch controller, on the ESP32-P4 (planning/karaoke-p4x-panel.md, "The
// kit"): the 7-inch 1024 by 600 panel of Espressif's ESP32-P4X-Function-EV-Board, an EK79007 on two
// lanes, with a GT911 touch controller on the I2C bus the board shares with its codec.
//
// RUN on the kit, 2026-10-20 (chip v3.2, planning/karaoke-p4x-panel.md, "First run on the kit"):
// the bus opens, the EK79007 initialises, the GT911 answers (at 0x14: the vendor's own BSP tries
// only 0x5D, which is why the factory demo aborts), the screen is as designed on the glass, and a
// tap lands on every button of the controls sheet once the touch layer's axes are mirrored. Not
// checked: tearing, and what the scan-out costs the decoder. Everything below was written against
// the headers of ESP-IDF v6.1 and the two managed components (espressif/esp_lcd_ek79007 2.0,
// espressif/esp_lcd_touch_gt911 1.2), and is built by CI as the `panel-dsi` image. The values that
// come from a document and not from a measurement are marked.
//
// The picture is a frame buffer the DPI engine reads out of PSRAM, RGB565 (the driver allocates it:
// num_fbs = 1). LVGL renders dirty areas into two small buffers and esp_lcd_panel_draw_bitmap copies
// each into the frame buffer; the engine keeps scanning it out, so what is on the glass is what was
// last copied there. One frame buffer can tear where an area is copied while it is read out; the
// drawing is small and sits still, and two buffers are one number to change if it shows (Q4).

#include "display.hpp"

#include <algorithm>
#include <cstdio>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "sdkconfig.h"

namespace player::panel {

namespace {

constexpr int kWidth = CONFIG_ICLFORGE_EXAMPLE_PANEL_WIDTH;
constexpr int kHeight = CONFIG_ICLFORGE_EXAMPLE_PANEL_HEIGHT;

static_assert(kWidth == 1024 && kHeight == 600,
              "the DSI timings below are the EK79007's for a 1024 by 600 panel; another panel needs its own");

esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_touch_handle_t g_touch = nullptr;
lv_display_t* g_disp = nullptr;
void* g_frame = nullptr;
bool g_backlight_ready = false;

// The copy into the frame buffer is done: the render buffer can be drawn into again. Called from
// the 2D-DMA's interrupt, or from flush() itself when the CPU does the copy;
// lv_display_flush_ready() is the call LVGL makes safe for either.
bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_handle_t /*panel*/,
                                   esp_lcd_dpi_panel_event_data_t* /*edata*/, void* /*ctx*/) {
    if (g_disp != nullptr) {
        lv_display_flush_ready(g_disp);
    }
    return false;
}

void flush(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    // x2 and y2 are inclusive in LVGL and exclusive in esp_lcd.
    if (esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map) !=
        ESP_OK) {
        lv_display_flush_ready(disp);
    }
}

void read_pointer(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    InjectedPointer& injected = injected_pointer();
    if (injected.down.load()) {
        data->point.x = injected.x.load();
        data->point.y = injected.y.load();
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
    if (g_touch == nullptr) {
        return;
    }
    // Polled: the board has no interrupt line to the controller (the BSP header lists it as not
    // connected). A read that fails is no touch.
    TouchCounters& counters = touch_counters();
    const esp_err_t read_result = esp_lcd_touch_read_data(g_touch);
    if (read_result != ESP_OK) {
        counters.failures.fetch_add(1);
        counters.last_error.store(static_cast<int>(read_result));
        return;
    }
    counters.reads.fetch_add(1);
    esp_lcd_touch_point_data_t point{};
    std::uint8_t count = 0;
    static bool was_down = false;
    const bool down = esp_lcd_touch_get_data(g_touch, &point, &count, 1) == ESP_OK && count > 0;
    if (down) {
        counters.touches.fetch_add(1);
        counters.x.store(point.x);
        counters.y.store(point.y);
        if (!was_down) {
            counters.record_press(point.x, point.y);
        }
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    was_down = down;
}

[[nodiscard]] bool open_touch() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = static_cast<gpio_num_t>(CONFIG_ICLFORGE_EXAMPLE_PANEL_I2C_SDA_GPIO);
    bus_config.scl_io_num = static_cast<gpio_num_t>(CONFIG_ICLFORGE_EXAMPLE_PANEL_I2C_SCL_GPIO);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) {
        std::printf("panel: no I2C bus for the touch controller\n");
        return false;
    }

    // The GT911 answers at 0x5D or 0x14 by the level its interrupt pin had as it came out of reset,
    // which is not wired here: try the one and then the other.
    for (const std::uint32_t address :
         {static_cast<std::uint32_t>(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS),
          static_cast<std::uint32_t>(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)}) {
        esp_lcd_panel_io_i2c_config_t io_config{};
        io_config.dev_addr = address;
        io_config.scl_speed_hz = 400000;  // the GT911's ceiling, and the vendor BSP's own speed
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 0;
        io_config.lcd_cmd_bits = 16;
        io_config.flags.disable_control_phase = 1;
        esp_lcd_panel_io_handle_t io = nullptr;
        if (esp_lcd_new_panel_io_i2c(bus, &io_config, &io) != ESP_OK) {
            continue;
        }
        esp_lcd_touch_config_t config{};
        config.x_max = kWidth;
        config.y_max = kHeight;
        config.rst_gpio_num = static_cast<gpio_num_t>(CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_RST_GPIO);
        config.int_gpio_num = static_cast<gpio_num_t>(CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_INT_GPIO);
        // The layer's axes against the display's, which esp_lcd_touch applies to every point.
        config.flags.swap_xy = CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_SWAP_XY;
        config.flags.mirror_x = CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_MIRROR_X;
        config.flags.mirror_y = CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_MIRROR_Y;
        if (esp_lcd_touch_new_i2c_gt911(io, &config, &g_touch) == ESP_OK) {
            std::printf("panel: GT911 touch controller at 0x%02X\n", static_cast<unsigned>(address));
            return true;
        }
        esp_lcd_panel_io_del(io);
    }
    std::printf("panel: no touch controller answered (GT911 at 0x5D or 0x14)\n");
    return false;
}

void open_backlight() {
    constexpr int kGpio = CONFIG_ICLFORGE_EXAMPLE_PANEL_BACKLIGHT_GPIO;
    if (kGpio < 0) {
        return;
    }
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_channel_config_t channel{};
    channel.gpio_num = kGpio;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    if (ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK) {
        g_backlight_ready = true;
    } else {
        std::printf("panel: no PWM for the backlight on GPIO%d\n", kGpio);
    }
}

}  // namespace

bool display_open(Display& out) {
    // The PHY's supply, before the bus: an LDO channel at 2.5 V on the P4X-Function-EV board.
    if (CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI_LDO_CHANNEL >= 0) {
        esp_ldo_channel_config_t ldo_config{};
        ldo_config.chan_id = CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI_LDO_CHANNEL;
        ldo_config.voltage_mv = CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI_LDO_MV;
        esp_ldo_channel_handle_t ldo = nullptr;
        if (esp_ldo_acquire_channel(&ldo_config, &ldo) != ESP_OK) {
            std::printf("panel: could not power the DSI PHY from LDO channel %d\n",
                        CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI_LDO_CHANNEL);
            return false;
        }
    }

    // The bus: two lanes at 900 Mbit/s each, the EK79007's (EK79007_PANEL_BUS_DSI_2CH_CONFIG).
    esp_lcd_dsi_bus_config_t bus_config{};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    // phy_clk_src stays 0, which ESP-IDF reads as "the default for this silicon": PLL_F20M below
    // v3.0 and the crystal from v3.0 on. MIPI_DSI_PHY_CLK_SRC_DEFAULT is the v1.x alias for
    // PLL_F20M, which a v3.x build's clock-source switch does not list: it aborts in
    // esp_lcd_new_dsi_bus (first run on the v3.2 kit, 2026-10-20).
    bus_config.lane_bit_rate_mbps = 900;
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    if (esp_lcd_new_dsi_bus(&bus_config, &bus) != ESP_OK) {
        std::printf("panel: could not open the MIPI DSI bus\n");
        return false;
    }
    esp_lcd_dbi_io_config_t io_config{};
    io_config.virtual_channel = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_dbi(bus, &io_config, &io) != ESP_OK) {
        std::printf("panel: could not open the panel's command channel\n");
        return false;
    }

    // The EK79007's video timing for 1024 by 600 at 60 Hz, a 52 MHz pixel clock
    // (EK79007_1024_600_PANEL_60HZ_CONFIG_CF): from the driver, to be checked against the kit's
    // panel datasheet.
    esp_lcd_dpi_panel_config_t dpi{};
    dpi.virtual_channel = 0;
    dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = 52;
    dpi.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi.num_fbs = 1;
    dpi.video_timing.h_size = kWidth;
    dpi.video_timing.v_size = kHeight;
    dpi.video_timing.hsync_pulse_width = 10;
    dpi.video_timing.hsync_back_porch = 160;
    dpi.video_timing.hsync_front_porch = 160;
    dpi.video_timing.vsync_pulse_width = 1;
    dpi.video_timing.vsync_back_porch = 23;
    dpi.video_timing.vsync_front_porch = 12;

    ek79007_vendor_config_t vendor{};
    vendor.mipi_config.dsi_bus = bus;
    vendor.mipi_config.dpi_config = &dpi;
    vendor.mipi_config.lane_num = 2;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_ICLFORGE_EXAMPLE_PANEL_RESET_GPIO);
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor;
    if (esp_lcd_new_panel_ek79007(io, &panel_config, &g_panel) != ESP_OK ||
        esp_lcd_panel_reset(g_panel) != ESP_OK || esp_lcd_panel_init(g_panel) != ESP_OK) {
        std::printf("panel: the EK79007 did not initialise\n");
        return false;
    }
    if (esp_lcd_dpi_panel_get_frame_buffer(g_panel, 1, &g_frame) != ESP_OK) {
        g_frame = nullptr;
    }

    // Two render buffers in PSRAM, as for the frame buffer with no panel (display_null.cpp).
    constexpr std::size_t kBufferBytes =
        static_cast<std::size_t>(kWidth) * CONFIG_ICLFORGE_EXAMPLE_PANEL_LINE_BUFFER_ROWS *
        sizeof(std::uint16_t);
    void* const first = heap_caps_aligned_alloc(64, kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* const second = heap_caps_aligned_alloc(64, kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (first == nullptr || second == nullptr) {
        std::printf("panel: no PSRAM for the render buffers\n");
        return false;
    }

    g_disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, first, second, static_cast<std::uint32_t>(kBufferBytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, &flush);

    esp_lcd_dpi_panel_event_callbacks_t callbacks{};
    callbacks.on_color_trans_done = &on_color_trans_done;
    if (esp_lcd_dpi_panel_register_event_callbacks(g_panel, &callbacks, nullptr) != ESP_OK) {
        std::printf("panel: could not hear when a copy into the frame buffer is done\n");
        return false;
    }
    if constexpr (CONFIG_ICLFORGE_EXAMPLE_PANEL_DMA2D_FLUSH != 0) {
        // The copy by the 2D-DMA, not the CPU (esp_lcd_panel_dpi.c's draw bitmap hook): without it
        // the copy runs inside flush(), PSRAM to PSRAM, and the callback above is called before it
        // returns.
        const esp_err_t dma = esp_lcd_dpi_panel_enable_dma2d(g_panel);
        std::printf("panel: frame buffer copy by %s\n",
                    dma == ESP_OK ? "2D-DMA" : "the CPU (the 2D-DMA would not start)");
    }

    const bool touch = open_touch();
    lv_indev_t* const pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, &read_pointer);
    lv_indev_set_display(pointer, g_disp);

    open_backlight();
    display_set_backlight(backlight_lit_percent());

    out.lv = g_disp;
    out.width = kWidth;
    out.height = kHeight;
    out.frame = static_cast<const std::uint16_t*>(g_frame);
    out.touch = touch;
    out.backlight = g_backlight_ready;
    out.kind = "dsi";
    std::printf("panel: EK79007 %d by %d on MIPI DSI, touch %s\n", kWidth, kHeight,
                touch ? "on" : "off");
    return true;
}

void display_set_backlight(int percent) {
    if (!g_backlight_ready) {
        return;
    }
    const int clamped = std::clamp(percent, 0, 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, static_cast<std::uint32_t>(clamped) * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

}  // namespace player::panel
