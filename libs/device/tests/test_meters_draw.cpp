#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/meters_draw.hpp"

namespace md = iclforge::meters::draw;

namespace {

constexpr std::uint32_t kStage = 0x151313;
constexpr std::uint32_t kTrough = 0x201D1C;
constexpr std::uint32_t kGreen = 0x4FC28A;
constexpr std::uint32_t kAmber = 0xF0B24A;
constexpr std::uint32_t kRed = 0xFF5C40;
constexpr std::uint32_t kTick = 0xECE7E5;
constexpr std::uint32_t kLine = 0x151313;

constexpr md::Colours kColours = md::make_colours(kStage, kTrough, kGreen, kAmber, kRed, kTick, kLine, 120);

constexpr std::uint16_t kSentinel = 0xABCD;

struct Canvas {
    int w;
    int h;
    std::vector<std::uint16_t> px;
    Canvas(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width * height), kSentinel) {}
    [[nodiscard]] std::uint16_t at(int x, int y) const { return px[static_cast<std::size_t>(y * w + x)]; }
};

}  // namespace

TEST_CASE("RGB565 packs as the panel reads it, and a translucent line blends per channel", "[meters-draw]") {
    CHECK(md::pack565(0x000000) == 0x0000);
    CHECK(md::pack565(0xFFFFFF) == 0xFFFF);
    CHECK(md::pack565(0xFF0000) == 0xF800);
    CHECK(md::pack565(0x00FF00) == 0x07E0);
    CHECK(md::pack565(0x0000FF) == 0x001F);
    CHECK(md::blend(0xFFFFFF, 0x000000, 0) == 0x000000);
    CHECK(md::blend(0xFFFFFF, 0x000000, 255) == 0xFFFFFF);
    CHECK(md::blend(0x000000, 0xFFFFFF, 120) == 0x878787);  // 255 * 135 / 255
    // The colours under a line are the blends, packed.
    CHECK(kColours.green_line == md::pack565(md::blend(kLine, kGreen, 120)));
    CHECK(kColours.stage_line == md::pack565(kStage));  // a line of the stage's own colour on the stage: unchanged
}

TEST_CASE("a vertical bar is lit from its top to the floor, in the zones' colours", "[meters-draw]") {
    Canvas c(12, 10);
    const md::VChart chart{12, 10, /*amber_row=*/6, /*red_row=*/3};
    const std::array<md::VBar, 2> bars = {{{1, 4, 0, -1}, {7, 4, 5, -1}}};  // the first lit to the top, the second from row 5
    md::render_vbars(c.px.data(), 12, chart, bars, {}, kColours, false, 0, 10);
    // The first bar: red rows 0-2, amber rows 3-5, green rows 6-9.
    for (int y = 0; y < 10; ++y) {
        const std::uint16_t want = y < 3 ? kColours.red : y < 6 ? kColours.amber : kColours.green;
        for (int x = 1; x < 5; ++x) {
            CHECK(c.at(x, y) == want);
        }
    }
    // The second: nothing above row 5, amber at 5, green from 6.
    CHECK(c.at(8, 4) == kColours.stage);
    CHECK(c.at(8, 5) == kColours.amber);
    CHECK(c.at(8, 6) == kColours.green);
    // Between the bars and outside them: the stage.
    CHECK(c.at(0, 5) == kColours.stage);
    CHECK(c.at(5, 5) == kColours.stage);
    CHECK(c.at(6, 9) == kColours.stage);
    CHECK(c.at(11, 9) == kColours.stage);
}

TEST_CASE("a meter's bar has its trough, and a bar that is not lit at all is only the trough", "[meters-draw]") {
    Canvas c(8, 6);
    const md::VChart chart{8, 6, 4, 2};
    const std::array<md::VBar, 2> bars = {{{0, 3, 3, -1}, {4, 3, 6, -1}}};
    md::render_vbars(c.px.data(), 8, chart, bars, {}, kColours, true, 0, 6);
    CHECK(c.at(1, 0) == kColours.trough);
    CHECK(c.at(1, 2) == kColours.trough);
    CHECK(c.at(1, 3) == kColours.amber);  // row 3 is above the amber row 4
    CHECK(c.at(1, 4) == kColours.green);
    CHECK(c.at(5, 5) == kColours.trough);  // top 6 is the floor: nothing lit
    CHECK(c.at(3, 0) == kColours.stage);   // the gap between the bars
}

TEST_CASE("a tick is two rows of its own colour over a bar or the stage", "[meters-draw]") {
    Canvas c(6, 12);
    const md::VChart chart{6, 12, 8, 4};
    const std::array<md::VBar, 1> bars = {{{1, 4, 6, 2}}};
    md::render_vbars(c.px.data(), 6, chart, bars, {}, kColours, false, 0, 12);
    CHECK(c.at(2, 1) == kColours.stage);
    CHECK(c.at(2, 2) == kColours.tick);
    CHECK(c.at(2, 3) == kColours.tick);
    CHECK(c.at(2, 4) == kColours.stage);
    CHECK(c.at(2, 6) == kColours.amber);
    // A tick on a lit bar: over the bar.
    const std::array<md::VBar, 1> over = {{{1, 4, 2, 6}}};
    md::render_vbars(c.px.data(), 6, chart, over, {}, kColours, false, 0, 12);
    CHECK(c.at(2, 3) == kColours.red);
    CHECK(c.at(2, 5) == kColours.amber);
    CHECK(c.at(2, 6) == kColours.tick);
    CHECK(c.at(2, 7) == kColours.tick);
    CHECK(c.at(2, 8) == kColours.green);
    // The tick's two rows end with the bar's own columns, not the chart's.
    CHECK(c.at(0, 6) == kColours.stage);
    CHECK(c.at(5, 6) == kColours.stage);
}

TEST_CASE("a line of the scale crosses the stage and the bars but not a tick", "[meters-draw]") {
    Canvas c(6, 8);
    const md::VChart chart{6, 8, 6, 3};
    std::vector<std::uint8_t> lines(8, 0);
    lines[5] = 1;
    lines[1] = 1;
    const std::array<md::VBar, 1> bars = {{{1, 4, 4, 5}}};
    md::render_vbars(c.px.data(), 6, chart, bars, lines, kColours, false, 0, 8);
    CHECK(c.at(0, 5) == kColours.stage_line);  // the stage under the line
    CHECK(c.at(0, 4) == kColours.stage);
    CHECK(c.at(2, 1) == kColours.stage_line);
    CHECK(c.at(2, 4) == kColours.amber);
    CHECK(c.at(2, 5) == kColours.tick);  // the tick's first row is on the line: the tick wins
    CHECK(c.at(2, 6) == kColours.tick);
    CHECK(c.at(2, 7) == kColours.green);
    lines[7] = 1;
    md::render_vbars(c.px.data(), 6, chart, bars, lines, kColours, false, 0, 8);
    CHECK(c.at(2, 7) == kColours.green_line);
}

TEST_CASE("only the rows asked for are written", "[meters-draw]") {
    Canvas c(5, 8);
    const md::VChart chart{5, 8, 6, 3};
    const std::array<md::VBar, 1> bars = {{{0, 5, 0, -1}}};
    md::render_vbars(c.px.data(), 5, chart, bars, {}, kColours, false, 2, 5);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 5; ++x) {
            if (y >= 2 && y < 5) {
                CHECK(c.at(x, y) != kSentinel);
            } else {
                CHECK(c.at(x, y) == kSentinel);
            }
        }
    }
    // A range outside the chart is clamped, not written past it.
    md::render_vbars(c.px.data(), 5, chart, bars, {}, kColours, false, -3, 20);
    CHECK(c.at(4, 7) != kSentinel);
    CHECK(c.px.size() == 40);
}

TEST_CASE("a chart in a wider buffer keeps to its stride", "[meters-draw]") {
    std::vector<std::uint16_t> px(16 * 4, kSentinel);
    const md::VChart chart{10, 4, 3, 1};
    const std::array<md::VBar, 1> bars = {{{2, 3, 0, -1}}};
    md::render_vbars(px.data(), 16, chart, bars, {}, kColours, false, 0, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 10; x < 16; ++x) {
            CHECK(px[static_cast<std::size_t>(y * 16 + x)] == kSentinel);
        }
        CHECK(px[static_cast<std::size_t>(y * 16)] == kColours.stage);
    }
}

TEST_CASE("the rows a change touches are those between the old and the new top, and the old and the new tick", "[meters-draw]") {
    using md::rows_touched;
    CHECK(rows_touched(10, 4, 10, 4, 50).empty());
    const md::Rows grown = rows_touched(10, -1, 6, -1, 50);
    CHECK(grown.first == 6);
    CHECK(grown.last == 10);
    const md::Rows both = rows_touched(10, 3, 12, 20, 50);
    CHECK(both.first == 3);
    CHECK(both.last == 22);
    const md::Rows tick_only = rows_touched(10, -1, 10, 5, 50);
    CHECK(tick_only.first == 5);
    CHECK(tick_only.last == 7);
    // Clamped to the chart.
    const md::Rows clamped = rows_touched(40, -1, 60, -1, 50);
    CHECK(clamped.first == 40);
    CHECK(clamped.last == 50);
    md::Rows join;
    join.join(grown);
    join.join(md::Rows{});
    join.join(tick_only);
    CHECK(join.first == 5);
    CHECK(join.last == 10);
}

TEST_CASE("a horizontal bar has its trough, its zones left to right and a tick", "[meters-draw]") {
    Canvas c(20, 7);
    const md::HChart chart{20, 7, /*amber_col=*/12, /*red_col=*/17};
    const std::array<md::HBar, 2> bars = {{{0, 3, 19, -1}, {4, 3, 5, 9}}};
    md::render_hbars(c.px.data(), 20, chart, bars, kColours, 0, 7);
    // The first, lit nearly to the end: green to 12, amber to 17, red to 19, the trough at 19.
    CHECK(c.at(0, 1) == kColours.green);
    CHECK(c.at(11, 1) == kColours.green);
    CHECK(c.at(12, 1) == kColours.amber);
    CHECK(c.at(16, 1) == kColours.amber);
    CHECK(c.at(17, 1) == kColours.red);
    CHECK(c.at(18, 1) == kColours.red);
    CHECK(c.at(19, 1) == kColours.trough);
    // The row between the bars is the stage.
    CHECK(c.at(5, 3) == kColours.stage);
    // The second: lit to column 5 (green), then its trough, a tick at 9 and 10.
    CHECK(c.at(4, 5) == kColours.green);
    CHECK(c.at(5, 5) == kColours.trough);
    CHECK(c.at(9, 5) == kColours.tick);
    CHECK(c.at(10, 5) == kColours.tick);
    CHECK(c.at(11, 5) == kColours.trough);
}

TEST_CASE("a horizontal bar that stops before the amber column is all green", "[meters-draw]") {
    Canvas c(20, 2);
    const md::HChart chart{20, 2, 12, 17};
    const std::array<md::HBar, 1> bars = {{{0, 2, 8, -1}}};
    md::render_hbars(c.px.data(), 20, chart, bars, kColours, 0, 2);
    CHECK(c.at(7, 0) == kColours.green);
    CHECK(c.at(8, 0) == kColours.trough);
    const std::array<md::HBar, 1> amber = {{{0, 2, 14, -1}}};
    md::render_hbars(c.px.data(), 20, chart, amber, kColours, 0, 2);
    CHECK(c.at(11, 0) == kColours.green);
    CHECK(c.at(12, 0) == kColours.amber);
    CHECK(c.at(13, 0) == kColours.amber);
    CHECK(c.at(14, 0) == kColours.trough);
}

TEST_CASE("redrawing only the rows a change touches gives the picture a full redraw would, over a long random walk", "[meters-draw]") {
    constexpr int kW = 160;
    constexpr int kH = 90;
    constexpr int kBars = 12;
    const md::VChart chart{kW, kH, 60, 25};
    std::vector<std::uint8_t> lines(kH, 0);
    for (int y = 10; y < kH; y += 20) {
        lines[static_cast<std::size_t>(y)] = 1;
    }
    std::array<md::VBar, kBars> bars{};
    for (int k = 0; k < kBars; ++k) {
        bars[static_cast<std::size_t>(k)] = {k * 13, 10, kH, -1};
    }
    Canvas inc(kW, kH);
    md::render_vbars(inc.px.data(), kW, chart, bars, lines, kColours, false, 0, kH);
    std::uint32_t state = 12345;
    const auto next = [&](int n) {
        state = state * 1664525U + 1013904223U;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    };
    for (int step = 0; step < 4000; ++step) {
        md::Rows touched;
        for (int k = 0; k < kBars; ++k) {
            md::VBar& b = bars[static_cast<std::size_t>(k)];
            if (next(3) == 0) {
                continue;  // this bar did not move
            }
            const int top = std::clamp(b.top + next(21) - 10, 0, kH);
            const int tick = next(4) == 0 ? -1 : std::max(0, std::min(top - 2 - next(30), kH - 2));
            touched.join(md::rows_touched(b.top, b.tick, top, tick, kH));
            b.top = top;
            b.tick = tick;
        }
        if (!touched.empty()) {
            md::render_vbars(inc.px.data(), kW, chart, bars, lines, kColours, false, touched.first, touched.last);
        }
        Canvas full(kW, kH);
        md::render_vbars(full.px.data(), kW, chart, bars, lines, kColours, false, 0, kH);
        REQUIRE(inc.px == full.px);
    }
}

TEST_CASE("a cell writes only its columns and rows, and cuts the bars at its edge", "[meters-draw]") {
    const md::VChart chart{40, 20, 15, 5};
    const std::array<md::VBar, 3> bars{{{2, 10, 6, -1}, {14, 10, 4, -1}, {26, 10, 8, -1}}};
    Canvas c(40, 20);
    md::render_vbars(c.px.data(), 40, chart, bars, {}, kColours, false, md::Cell{12, 30, 3, 9});
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 40; ++x) {
            const bool inside = x >= 12 && x < 30 && y >= 3 && y < 9;
            if (!inside) {
                REQUIRE(c.at(x, y) == kSentinel);
            } else {
                REQUIRE(c.at(x, y) != kSentinel);
            }
        }
    }
    // The second bar is whole, lit from row 4 (red: above row 5); the first is outside the cell and the gap before the second is only stage.
    CHECK(c.at(14, 4) == kColours.red);
    CHECK(c.at(23, 4) == kColours.red);
    CHECK(c.at(14, 3) == kColours.stage);
    CHECK(c.at(12, 6) == kColours.stage);
    // The third is cut at column 30, lit from row 8 (amber: above row 15).
    CHECK(c.at(26, 8) == kColours.amber);
    CHECK(c.at(29, 8) == kColours.amber);
    CHECK(c.at(26, 7) == kColours.stage);
    // An empty cell writes nothing.
    Canvas d(40, 20);
    md::render_vbars(d.px.data(), 40, chart, bars, {}, kColours, false, md::Cell{5, 5, 0, 20});
    CHECK(d.px == Canvas(40, 20).px);
}

TEST_CASE("the cells to redraw: none for no change, one for one, one for neighbours that move alike, one each for bars far apart", "[meters-draw]") {
    std::array<md::Cell, 8> out{};
    std::array<md::Change, 4> none{};
    CHECK(md::plan_cells(none, out) == 0);

    const std::array<md::Change, 3> one{{{0, 10, {}}, {12, 22, {30, 40}}, {24, 34, {}}}};
    REQUIRE(md::plan_cells(one, out) == 1);
    CHECK(out[0].x0 == 12);
    CHECK(out[0].x1 == 22);
    CHECK(out[0].y0 == 30);
    CHECK(out[0].y1 == 40);

    // Neighbours that move over the same rows: one cell is cheaper than two (an area costs 8,000 pixels).
    const std::array<md::Change, 3> alike{{{0, 10, {30, 40}}, {12, 22, {32, 40}}, {24, 34, {30, 38}}}};
    REQUIRE(md::plan_cells(alike, out) == 1);
    CHECK(out[0].x0 == 0);
    CHECK(out[0].x1 == 34);
    CHECK(out[0].y0 == 30);
    CHECK(out[0].y1 == 40);

    // Far apart: 900 columns of ten rows cost more than the second area.
    const std::array<md::Change, 2> apart{{{0, 10, {30, 40}}, {900, 910, {30, 40}}}};
    REQUIRE(md::plan_cells(apart, out) == 2);
    CHECK(out[0].x1 == 10);
    CHECK(out[1].x0 == 900);

    // Rows far apart: a tall cell for both costs more than two short ones.
    const std::array<md::Change, 2> tall{{{0, 100, {0, 10}}, {110, 210, {290, 300}}}};
    REQUIRE(md::plan_cells(tall, out) == 2);
    // A free area is one cell for each bar that changed.
    const std::array<md::Change, 3> free_areas{{{0, 10, {1, 3}}, {12, 22, {1, 3}}, {24, 34, {1, 3}}}};
    CHECK(md::plan_cells(free_areas, out, 0) == 3);
    // Too little room: one cell for all of them.
    std::array<md::Cell, 1> room{};
    REQUIRE(md::plan_cells(free_areas, room, 0) == 1);
    CHECK(room[0].x0 == 0);
    CHECK(room[0].x1 == 34);
}

TEST_CASE("the plan covers every change and costs no more than one cell for all of them or one for each, over a long random walk", "[meters-draw]") {
    std::uint32_t state = 777;
    const auto next = [&](int n) {
        state = state * 1664525U + 1013904223U;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    };
    for (int round = 0; round < 3000; ++round) {
        std::array<md::Change, 31> changes{};
        int moved = 0;
        for (int k = 0; k < 31; ++k) {
            changes[static_cast<std::size_t>(k)].x0 = k * 30;
            changes[static_cast<std::size_t>(k)].x1 = k * 30 + 26;
            if (next(3) != 0) {
                const int a = next(240);
                changes[static_cast<std::size_t>(k)].rows = {a, a + 1 + next(1 + (round % 4) * 12)};
                ++moved;
            }
        }
        std::array<md::Cell, 64> cells{};
        const std::int64_t area_cost = next(4) == 0 ? 0 : 8'000;
        const std::size_t n = md::plan_cells(changes, cells, area_cost);
        if (moved == 0) {
            REQUIRE(n == 0);
            continue;
        }
        REQUIRE(n >= 1);
        std::int64_t planned = static_cast<std::int64_t>(n) * area_cost;
        for (std::size_t i = 0; i < n; ++i) {
            planned += cells[i].area();
            if (i > 0) {
                REQUIRE(cells[i].x0 >= cells[i - 1].x1);  // in column order, apart
            }
        }
        std::int64_t each = 0;
        md::Cell all;
        bool first = true;
        for (const md::Change& c : changes) {
            if (c.rows.empty()) {
                continue;
            }
            // covered by a cell
            bool covered = false;
            for (std::size_t i = 0; i < n; ++i) {
                covered = covered || (cells[i].x0 <= c.x0 && cells[i].x1 >= c.x1 && cells[i].y0 <= c.rows.first && cells[i].y1 >= c.rows.last);
            }
            REQUIRE(covered);
            each += area_cost + static_cast<std::int64_t>(c.x1 - c.x0) * (c.rows.last - c.rows.first);
            all = first ? md::Cell{c.x0, c.x1, c.rows.first, c.rows.last}
                        : md::Cell{std::min(all.x0, c.x0), std::max(all.x1, c.x1), std::min(all.y0, c.rows.first), std::max(all.y1, c.rows.last)};
            first = false;
        }
        REQUIRE(planned <= each);
        REQUIRE(planned <= area_cost + all.area());
    }
}

TEST_CASE("redrawing only the planned cells gives the picture a full redraw would, over a long random walk", "[meters-draw]") {
    constexpr int kW = 400;
    constexpr int kH = 90;
    constexpr int kBars = 31;
    const md::VChart chart{kW, kH, 60, 25};
    std::vector<std::uint8_t> lines(kH, 0);
    for (int y = 10; y < kH; y += 20) {
        lines[static_cast<std::size_t>(y)] = 1;
    }
    std::array<md::VBar, kBars> bars{};
    for (int k = 0; k < kBars; ++k) {
        bars[static_cast<std::size_t>(k)] = {k * 13, 10, kH, -1};
    }
    Canvas inc(kW, kH);
    md::render_vbars(inc.px.data(), kW, chart, bars, lines, kColours, false, 0, kH);
    std::uint32_t state = 4242;
    const auto next = [&](int n) {
        state = state * 1664525U + 1013904223U;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    };
    int cells_drawn = 0;
    for (int step = 0; step < 3000; ++step) {
        std::array<md::Change, kBars> changes{};
        for (int k = 0; k < kBars; ++k) {
            md::VBar& b = bars[static_cast<std::size_t>(k)];
            md::Change& c = changes[static_cast<std::size_t>(k)];
            c.x0 = b.x;
            c.x1 = b.x + b.width;
            if (next(3) == 0) {
                continue;
            }
            const int top = std::clamp(b.top + next(9) - 4, 0, kH);
            const int tick = next(4) == 0 ? -1 : std::max(0, std::min(top - 2 - next(30), kH - 2));
            c.rows = md::rows_touched(b.top, b.tick, top, tick, kH);
            b.top = top;
            b.tick = tick;
        }
        std::array<md::Cell, 64> cells{};
        const std::size_t n = md::plan_cells(changes, cells, step % 5 == 0 ? 0 : md::kAreaCostPx);
        for (std::size_t i = 0; i < n; ++i) {
            md::render_vbars(inc.px.data(), kW, chart, bars, lines, kColours, false, cells[i]);
        }
        cells_drawn += static_cast<int>(n);
        Canvas full(kW, kH);
        md::render_vbars(full.px.data(), kW, chart, bars, lines, kColours, false, 0, kH);
        REQUIRE(inc.px == full.px);
    }
    CHECK(cells_drawn > 3000);  // a free area (every fifth step) is a cell for each run of bars
}
