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
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// A rectangle of a chart: columns [x0, x1) of rows [y0, y1).
struct Cell {
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return x0 >= x1 || y0 >= y1; }
    [[nodiscard]] constexpr std::int64_t area() const noexcept { return empty() ? 0 : static_cast<std::int64_t>(x1 - x0) * (y1 - y0); }
};

// The cell of the chart into `px` (`stride` pixels a row). `line_rows[y]` is non-zero where a line of the scale crosses row y; it
// may be empty. `troughs`: each bar has its trough (a meter), not just the stage under it (a spectrum). A bar the cell cuts is cut.
inline void render_vbars(std::uint16_t* px, std::size_t stride, const VChart& chart, std::span<const VBar> bars, std::span<const std::uint8_t> line_rows,
                         const Colours& colours, bool troughs, Cell cell) noexcept {
    const int y0 = std::max(cell.y0, 0);
    const int y1 = std::min(cell.y1, chart.height);
    const int cx0 = std::max(cell.x0, 0);
    const int cx1 = std::min(cell.x1, chart.width);
    if (cx0 >= cx1) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        std::uint16_t* const row = px + static_cast<std::size_t>(y) * stride;
        const bool line = static_cast<std::size_t>(y) < line_rows.size() && line_rows[static_cast<std::size_t>(y)] != 0;
        std::fill(row + cx0, row + cx1, line ? colours.stage_line : colours.stage);
        const std::uint16_t zone = y < chart.red_row ? (line ? colours.red_line : colours.red)
                                   : y < chart.amber_row ? (line ? colours.amber_line : colours.amber)
                                                         : (line ? colours.green_line : colours.green);
        const std::uint16_t trough = line ? colours.trough_line : colours.trough;
        for (const VBar& bar : bars) {
            const int x0 = std::max(bar.x, cx0);
            const int x1 = std::min(bar.x + bar.width, cx1);
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

// Rows [y0, y1) of the whole width.
inline void render_vbars(std::uint16_t* px, std::size_t stride, const VChart& chart, std::span<const VBar> bars, std::span<const std::uint8_t> line_rows,
                         const Colours& colours, bool troughs, int y0, int y1) noexcept {
    render_vbars(px, stride, chart, bars, line_rows, colours, troughs, Cell{0, chart.width, y0, y1});
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

// --- what to redraw ----------------------------------------------------------------------------------------------------

// What a bar's change touches: the columns it covers and the rows between its old and new top and tick (empty: it did not move).
struct Change {
    int x0 = 0;
    int x1 = 0;
    Rows rows;
};

// What one more rectangle for LVGL to draw and copy costs, in the pixels of one. The kit's 5.1 room measured about 135 ns a pixel and
// about a millisecond for the first pixel of an area (planning/p4x-meters-view.md, "Kit results"), so 8,000: a spectrum that moves every
// bar a few rows is a handful of rectangles, not one of the whole plot's width and not one for each of its 31 bars.
inline constexpr std::int64_t kAreaCostPx = 8'000;
inline constexpr std::size_t kMaxChanges = 64;

// The rectangles to redraw for the changes of bars side by side, in order of their columns: runs of neighbouring changed bars share a
// rectangle (their columns, the union of their rows) when that costs less than one each, by the least total of `area_cost_px` an
// area and a pixel each (a partition of the changed bars into runs, by dynamic programming: at most 64 of them; more is one rectangle for all).
// Writes at most out.size() cells, in column order, and returns how many; none if nothing changed.
[[nodiscard]] inline std::size_t plan_cells(std::span<const Change> changes, std::span<Cell> out, std::int64_t area_cost_px = kAreaCostPx) noexcept {
    std::array<std::uint8_t, kMaxChanges> index{};
    std::size_t m = 0;
    bool too_many = false;
    Cell all;
    bool any = false;
    for (std::size_t k = 0; k < changes.size(); ++k) {
        const Change& c = changes[k];
        if (c.rows.empty() || c.x0 >= c.x1) {
            continue;
        }
        if (!any) {
            all = {c.x0, c.x1, c.rows.first, c.rows.last};
            any = true;
        } else {
            all = {std::min(all.x0, c.x0), std::max(all.x1, c.x1), std::min(all.y0, c.rows.first), std::max(all.y1, c.rows.last)};
        }
        if (m < kMaxChanges) {
            index[m] = static_cast<std::uint8_t>(k);
        } else {
            too_many = true;
        }
        ++m;
    }
    if (!any || out.empty()) {
        return 0;
    }
    if (too_many || m == 1) {
        out[0] = all;
        return 1;
    }
    constexpr std::int64_t kInfinite = std::numeric_limits<std::int64_t>::max();
    std::array<std::int64_t, kMaxChanges + 1> best{};
    std::array<std::uint8_t, kMaxChanges + 1> from{};
    for (std::size_t j = 1; j <= m; ++j) {
        best[j] = kInfinite;
        int x_min = std::numeric_limits<int>::max();
        int x_max = std::numeric_limits<int>::min();
        int y_min = std::numeric_limits<int>::max();
        int y_max = std::numeric_limits<int>::min();
        for (std::size_t i = j; i-- > 0;) {
            const Change& c = changes[index[i]];
            x_min = std::min(x_min, c.x0);
            x_max = std::max(x_max, c.x1);
            y_min = std::min(y_min, c.rows.first);
            y_max = std::max(y_max, c.rows.last);
            const std::int64_t cost = best[i] + area_cost_px + static_cast<std::int64_t>(x_max - x_min) * (y_max - y_min);
            if (cost < best[j]) {
                best[j] = cost;
                from[j] = static_cast<std::uint8_t>(i);
            }
        }
    }
    std::size_t groups = 0;
    for (std::size_t j = m; j > 0; j = from[j]) {
        ++groups;
    }
    if (groups > out.size()) {
        out[0] = all;
        return 1;
    }
    std::size_t at = groups;
    for (std::size_t j = m; j > 0; j = from[j]) {
        Cell cell;
        bool first = true;
        for (std::size_t i = from[j]; i < j; ++i) {
            const Change& c = changes[index[i]];
            cell = first ? Cell{c.x0, c.x1, c.rows.first, c.rows.last}
                         : Cell{std::min(cell.x0, c.x0), std::max(cell.x1, c.x1), std::min(cell.y0, c.rows.first), std::max(cell.y1, c.rows.last)};
            first = false;
        }
        out[--at] = cell;
    }
    return groups;
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
