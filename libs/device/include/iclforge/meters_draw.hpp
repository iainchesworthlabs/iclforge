#pragma once

// The Meters view's pixels: bar charts written straight into an RGB565 buffer (planning/p4x-meters-view.md, "Kit results"). The first drawing
// made each bar three LVGL rectangles and a tick, and an update that moved 31 bars and a dozen meters left 100 to 200 small invalid areas, each a
// pass over the object tree and a copy to the frame buffer: 130 to 440 ms an update on the P4, where the speakers view's few circles take 2 to 8.
// Here a whole chart is one buffer, written by the rows that changed, and LVGL is told one area.
//
// Nothing here knows LVGL, allocates or draws a glyph: a buffer, a stride and the chart's numbers go in and pixels come out, so that the
// host tests hold every colour and every row, and the firmware and the preview draw the same thing.
//
// Rows are counted from the top of the chart. A bar is lit from `top` to the floor; a lit pixel is red above `red_row`, amber above
// `amber_row` and green below it (iclforge/meters_view.hpp's zones, as pixels). A tick is two rows (or columns) of its own colour. A line of the
// scale crosses everything but the ticks, as a translucent stroke of the stage's colour does.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace iclforge::meters::draw {

[[nodiscard]] constexpr std::uint16_t pack565(std::uint32_t rgb) noexcept {
    return static_cast<std::uint16_t>(((rgb >> 8) & 0xF800U) | ((rgb >> 5) & 0x07E0U) | ((rgb >> 3) & 0x001FU));
}

// `over` at `opa` (0 to 255) on `under`, both 0xRRGGBB, per channel as LVGL blends.
[[nodiscard]] constexpr std::uint32_t blend(std::uint32_t over, std::uint32_t under, unsigned opa) noexcept {
    const auto channel = [&](unsigned shift) {
        const unsigned a = (over >> shift) & 0xFFU;
        const unsigned b = (under >> shift) & 0xFFU;
        return (a * opa + b * (255U - opa) + 127U) / 255U;
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

struct Colours {
    std::uint16_t stage = 0;   // between the bars
    std::uint16_t trough = 0;  // the unlit part of a meter's bar (a spectrum has none: it is the stage's)
    std::uint16_t green = 0;
    std::uint16_t amber = 0;
    std::uint16_t red = 0;
    std::uint16_t tick = 0;
    // The same, under a line of the scale.
    std::uint16_t stage_line = 0;
    std::uint16_t trough_line = 0;
    std::uint16_t green_line = 0;
    std::uint16_t amber_line = 0;
    std::uint16_t red_line = 0;
};

[[nodiscard]] constexpr Colours make_colours(std::uint32_t stage, std::uint32_t trough, std::uint32_t green, std::uint32_t amber,
                                             std::uint32_t red, std::uint32_t tick, std::uint32_t line, unsigned line_opa) noexcept {
    return {pack565(stage),
            pack565(trough),
            pack565(green),
            pack565(amber),
            pack565(red),
            pack565(tick),
            pack565(blend(line, stage, line_opa)),
            pack565(blend(line, trough, line_opa)),
            pack565(blend(line, green, line_opa)),
            pack565(blend(line, amber, line_opa)),
            pack565(blend(line, red, line_opa))};
}

// --- vertical bars -----------------------------------------------------------------------------------------------------

struct VBar {
    int x = 0;
    int width = 0;
    int top = 0;    // the first lit row; the chart's height or more: nothing lit
    int tick = -1;  // the first of the tick's two rows; negative: no tick
};

struct VChart {
    int width = 0;
    int height = 0;
    int amber_row = 0;  // lit pixels from this row down are green, the ones above it amber
    int red_row = 0;    // and above this one red
};

// Rows [y0, y1) of the chart into `px` (`stride` pixels a row). `line_rows[y]` is non-zero where a line of the scale crosses row y; it
// may be empty. `troughs`: each bar has its trough (a meter), not just the stage under it (a spectrum).
inline void render_vbars(std::uint16_t* px, std::size_t stride, const VChart& chart, std::span<const VBar> bars, std::span<const std::uint8_t> line_rows,
                         const Colours& colours, bool troughs, int y0, int y1) noexcept {
    y0 = std::max(y0, 0);
    y1 = std::min(y1, chart.height);
    for (int y = y0; y < y1; ++y) {
        std::uint16_t* const row = px + static_cast<std::size_t>(y) * stride;
        const bool line = static_cast<std::size_t>(y) < line_rows.size() && line_rows[static_cast<std::size_t>(y)] != 0;
        std::fill_n(row, chart.width, line ? colours.stage_line : colours.stage);
        const std::uint16_t zone = y < chart.red_row ? (line ? colours.red_line : colours.red)
                                   : y < chart.amber_row ? (line ? colours.amber_line : colours.amber)
                                                         : (line ? colours.green_line : colours.green);
        const std::uint16_t trough = line ? colours.trough_line : colours.trough;
        for (const VBar& bar : bars) {
            const int x0 = std::max(bar.x, 0);
            const int x1 = std::min(bar.x + bar.width, chart.width);
            if (x1 <= x0) {
                continue;
            }
            std::uint16_t colour = troughs ? trough : (line ? colours.stage_line : colours.stage);
            if (y >= bar.top) {
                colour = zone;
            }
            if (bar.tick >= 0 && y >= bar.tick && y < bar.tick + 2) {
                colour = colours.tick;
            }
            if (colour != (line ? colours.stage_line : colours.stage)) {
                std::fill(row + x0, row + x1, colour);
            }
        }
    }
}

// The rows a bar's change touches: [first, last) of the rows that differ between its old and its new `top` and `tick`, or an empty range. A
// chart's rows to redraw are the union over its bars.
struct Rows {
    int first = 0;
    int last = 0;  // exclusive; first >= last: none
    [[nodiscard]] constexpr bool empty() const noexcept { return first >= last; }
    constexpr void join(const Rows& other) noexcept {
        if (other.empty()) {
            return;
        }
        if (empty()) {
            *this = other;
            return;
        }
        first = std::min(first, other.first);
        last = std::max(last, other.last);
    }
};

[[nodiscard]] constexpr Rows rows_touched(int top0, int tick0, int top1, int tick1, int height) noexcept {
    Rows out;
    if (top0 != top1) {
        out.join({std::min(top0, top1), std::max(top0, top1)});
    }
    if (tick0 != tick1) {
        if (tick0 >= 0) {
            out.join({tick0, tick0 + 2});
        }
        if (tick1 >= 0) {
            out.join({tick1, tick1 + 2});
        }
    }
    out.first = std::max(out.first, 0);
    out.last = std::min(out.last, height);
    return out;
}

// --- horizontal bars -----------------------------------------------------------------------------------------------------

struct HBar {
    int y = 0;
    int height = 0;
    int lit = 0;    // columns lit from the left
    int tick = -1;  // the first of the tick's two columns; negative: none
};

struct HChart {
    int width = 0;
    int height = 0;
    int amber_col = 0;  // lit pixels left of this column are green, the ones from it amber
    int red_col = 0;    // and from this one red
};

// Rows [y0, y1) of the chart: every bar has its trough across the chart's width.
inline void render_hbars(std::uint16_t* px, std::size_t stride, const HChart& chart, std::span<const HBar> bars, const Colours& colours, int y0,
                         int y1) noexcept {
    y0 = std::max(y0, 0);
    y1 = std::min(y1, chart.height);
    for (int y = y0; y < y1; ++y) {
        std::uint16_t* const row = px + static_cast<std::size_t>(y) * stride;
        std::fill_n(row, chart.width, colours.stage);
        for (const HBar& bar : bars) {
            if (y < bar.y || y >= bar.y + bar.height) {
                continue;
            }
            std::fill_n(row, chart.width, colours.trough);
            const int lit = std::clamp(bar.lit, 0, chart.width);
            const int green_end = std::min(lit, chart.amber_col);
            const int amber_end = std::max(green_end, std::min(lit, chart.red_col));
            if (green_end > 0) {
                std::fill(row, row + green_end, colours.green);
            }
            if (amber_end > green_end) {
                std::fill(row + green_end, row + amber_end, colours.amber);
            }
            if (lit > amber_end) {
                std::fill(row + amber_end, row + lit, colours.red);
            }
            if (bar.tick >= 0) {
                const int t0 = std::clamp(bar.tick, 0, chart.width);
                const int t1 = std::clamp(bar.tick + 2, 0, chart.width);
                if (t1 > t0) {
                    std::fill(row + t0, row + t1, colours.tick);
                }
            }
        }
    }
}

}  // namespace iclforge::meters::draw
