// Prototypes of a Meters view for the no-lyrics screen (planning/p4x-meters-view.md): an instrument display after the
// owner's reference, RME's ADI-2/4 Pro SE and Fireface UCX II: a third-octave spectrum of the main mix with peak holds and a
// dB scale, level meters with peak-hold ticks and the numeric peak, a status line.
//
//   meters_options <out> [measure]
//     stills of three variants in four rooms (2.0, 5.1, 7.1.4, 9.1.6) at three screen sizes, and short sequences, as BMPs
//     in <out>; with "measure", a minute of each, with the pixels LVGL flushed counted (the exact part of what a view costs),
//     the LVGL objects and heap, written as <out>/measure.csv.
//
//   A  the full instrument: spectrum of the main mix, L and R meters, a status line
//   B  the spectrum smaller, and a bar meter for every OUTPUT of the room under it
//   C  a spectrum for each group of speakers of the room (front, surround, height) with its own peak meter
//
// The audio is made up (scene.hpp of the speakers prototype) and fixed, so every variant sees the same music; the
// spectrum is the real analysis of spectrum.hpp over a mono mix of the rendered outputs, as the board would make it.

#include "common.hpp"

namespace {

// A: the full instrument.
struct ViewA : View {
    Spectrum spectrum;
    Meter left;
    Meter right;
    lv_obj_t* peak_label = nullptr;
    const Room* room = nullptr;
    meters::Ballistic overall;

    void build(const Stage& st) override {
        const Dim& d = st.dim;
        room = st.room;
        lv_obj_t* const p = st.parent;
        const bool tall = d.h >= 300;
        const int title_h = tall ? d.f_label + 8 : 0;
        const int rows = d.wide ? 2 : d.merge == 2 ? 1 : 1;
        const int labels_h = rows * (d.f_small + 3) + 3;
        const int meter_h = d.wide ? 18 : d.h >= 200 ? 12 : 8;
        const int status_h = tall ? d.f_label + 6 : d.h >= 200 ? d.f_small + 4 : 0;
        const int gap = d.wide ? 8 : 4;
        const int value_w = d.wide ? 64 : d.h >= 200 ? 44 : 34;
        const int left_x = d.m + d.scale_w + 4;
        const int plot_w = d.w - left_x - d.m;
        const int top = d.m / 2 + 2;
        if (tall) {
            const std::string title = std::string("Main mix  ") + st.room->layout;
            text(p, title, d.f_label, kMuted, left_x, top, plot_w);
        }
        const int plot_y = top + title_h + (tall ? 8 : 0);
        const int scale_gap = tall ? 14 : 0;  // the meters' own scale, above them
        const int meters_h = 2 * meter_h + gap;
        const int plot_h = d.h - plot_y - labels_h - gap - meters_h - (status_h > 0 ? status_h + gap : 0) - d.m / 2 - scale_gap;
        spectrum.build(p, d, left_x, plot_y, plot_w, plot_h, d.merge, true, rows);
        int my = plot_y + plot_h + labels_h + gap + scale_gap;
        const int label_w = d.f_small + 6;
        lv_obj_t* const lv = text(p, "", d.f_label, kText, left_x + plot_w - value_w, my + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
        lv_obj_t* const rv = text(p, "", d.f_label, kText, left_x + plot_w - value_w, my + meter_h + gap + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
        text(p, "L", d.f_small + 2, kMuted, d.m, my + (meter_h - d.f_small) / 2 - 3, label_w);
        text(p, "R", d.f_small + 2, kMuted, d.m, my + meter_h + gap + (meter_h - d.f_small) / 2 - 3, label_w);
        left.build(p, left_x, my, plot_w - value_w - 8, meter_h, false, lv);
        right.build(p, left_x, my + meter_h + gap, plot_w - value_w - 8, meter_h, false, rv);
        if (tall) {
            meter_scale_h(p, d, left_x, my - scale_gap, plot_w - value_w - 8);
        }
        if (status_h > 0) {
            const int sy = d.h - status_h - d.m / 2;
            rect(p, d.m, sy - 3, d.w - 2 * d.m, 1, kFaint);
            std::string line = st.scenario->status;
            text(p, line, tall ? d.f_label - 2 : d.f_small, kMuted, d.m, sy, d.w - 2 * d.m - 120);
            peak_label = text(p, "", tall ? d.f_label - 2 : d.f_small, kText, d.w - d.m - 110, sy, 110, Align::kRight);
        }
    }

    void hop(const Hop& h) override {
        spectrum.update(h.main);
        const auto l = static_cast<std::size_t>(std::max(room->left, 0));
        const auto r = static_cast<std::size_t>(std::max(room->right, 0));
        left.update(h.peak_db[l], h.peak_db[l]);
        right.update(h.peak_db[r], h.peak_db[r]);
        overall.update(std::max(left.bal.peak, right.bal.peak), kHopSeconds, 100.0F, 3.0F, 0.0F);
        if (peak_label != nullptr) {
            const std::string s = "peak " + db_text(std::max(left.bal.peak, right.bal.peak), 1) + " dB";
            if (s != shown_) {
                shown_ = s;
                lv_label_set_text(peak_label, s.c_str());
            }
        }
    }

    std::string shown_;
};

// B: the spectrum smaller, a bar for every output under it.
struct ViewB : View {
    Spectrum spectrum;
    std::vector<Meter> outputs;
    std::vector<int> index;
    const Room* room = nullptr;
    ViewA fallback;
    bool use_fallback = false;

    void build(const Stage& st) override {
        const Dim& d = st.dim;
        if (d.w < 400) {
            // 240 x 240: no room for a row of outputs.
            use_fallback = true;
            fallback.build(st);
            return;
        }
        room = st.room;
        lv_obj_t* const p = st.parent;
        const bool tall = d.h >= 300;
        const int left_x = d.m + d.scale_w + 4;
        const int plot_w = d.w - left_x - d.m;
        const int top = d.m / 2 + 2;
        const int chips_h = tall ? d.f_label + 8 : 0;
        const int labels_h = d.f_small + 6;
        const int plot_y = top + chips_h;
        const int plot_h = static_cast<int>(static_cast<float>(d.h - plot_y) * (tall ? 0.43F : 0.40F)) - labels_h;
        if (tall) {
            text(p, std::string("Main mix  ") + st.room->layout, d.f_label, kMuted, left_x, top, plot_w - 260);
            text(p, st.scenario->status, d.f_label - 2, kMuted, left_x + plot_w - 460, top + 2, 460, Align::kRight);
        }
        spectrum.build(p, d, left_x, plot_y, plot_w, plot_h, d.merge, true, 1);
        for (std::size_t i = 0; i < st.room->spots.size(); ++i) {
            if (st.room->spots[i].connected) {
                index.push_back(static_cast<int>(i));
            }
        }
        const int n = static_cast<int>(index.size());
        const int row_y = plot_y + plot_h + labels_h + (tall ? 18 : 8);
        const int name_h = d.f_small + 6;
        const int value_h = tall && n <= 12 ? d.f_small + 4 : 0;
        const int bar_top = row_y + value_h;
        const int bar_h = d.h - bar_top - name_h - d.m / 2 - 2;
        const float pitch = static_cast<float>(plot_w) / static_cast<float>(n);
        const int bar_w = std::clamp(static_cast<int>(std::lround(pitch * 0.55F)), 6, d.wide ? 110 : 60);
        outputs.resize(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            const int cx = left_x + static_cast<int>(std::lround((static_cast<float>(k) + 0.5F) * pitch));
            lv_obj_t* value = nullptr;
            if (value_h > 0) {
                value = text(p, "", d.f_small, kText, cx - static_cast<int>(pitch / 2), row_y, static_cast<int>(pitch), Align::kCentre);
            }
            outputs[static_cast<std::size_t>(k)].decimals = value_h > 0 ? 1 : 0;
            outputs[static_cast<std::size_t>(k)].build(p, cx - bar_w / 2, bar_top, bar_w, bar_h, true, value);
            text(p, st.room->spots[static_cast<std::size_t>(index[static_cast<std::size_t>(k)])].name, d.f_small + 1, kMuted, cx - static_cast<int>(pitch / 2),
                 bar_top + bar_h + 3, static_cast<int>(pitch), Align::kCentre);
        }
        // The scale of the bars, at the left.
        for (const float db : {0.0F, -18.0F, -40.0F}) {
            const int ly = bar_top + static_cast<int>(std::lround((1.0F - unit(db, -60.0F, 0.0F)) * static_cast<float>(bar_h)));
            text(p, db_text(db, 0), d.f_small, kFaint, d.m, ly - d.f_small / 2 - 2, d.scale_w + 2, Align::kRight);
        }
    }

    void hop(const Hop& h) override {
        if (use_fallback) {
            fallback.hop(h);
            return;
        }
        spectrum.update(h.main);
        for (std::size_t k = 0; k < outputs.size(); ++k) {
            const auto o = static_cast<std::size_t>(index[k]);
            outputs[k].update(h.peak_db[o], h.peak_db[o]);
        }
    }
};

// C: a spectrum for each group of speakers.
struct ViewC : View {
    struct Column {
        Spectrum spectrum;
        Meter meter;
        int group = 0;
    };
    std::vector<Column> columns;
    ViewA single;
    bool use_single = false;

    void build(const Stage& st) override {
        const Dim& d = st.dim;
        const std::size_t groups = st.room->groups.size();
        if (groups < 2 || d.w < 400) {
            use_single = true;
            single.build(st);
            return;
        }
        lv_obj_t* const p = st.parent;
        const bool tall = d.h >= 300;
        columns.resize(groups);
        const int gap = d.wide ? 24 : 10;
        const int scale_w = d.wide ? d.scale_w : 0;
        const int col_w = (d.w - 2 * d.m - static_cast<int>(groups - 1) * gap) / static_cast<int>(groups);
        const int title_h = tall ? d.f_label + 10 : d.f_small + 6;
        const int labels_h = d.f_small + 6;
        const int meter_h = d.wide ? 16 : 10;
        const int top = d.m / 2 + 2;
        const int merge = std::max(d.merge, 2);
        for (std::size_t g = 0; g < groups; ++g) {
            Column& c = columns[g];
            c.group = static_cast<int>(g);
            const int cx = d.m + static_cast<int>(g) * (col_w + gap);
            // The group's name, and which outputs it takes.
            std::string names;
            for (const int o : st.room->groups[g]) {
                names += (names.empty() ? "" : " ") + st.room->spots[static_cast<std::size_t>(o)].name;
            }
            text(p, st.room->group_names[g], d.f_label, kText, cx + scale_w, top, col_w - scale_w);
            if (d.wide && tall) {
                text(p, names, d.f_small, kMuted, cx + scale_w, top + d.f_label + 2, col_w - scale_w);
            }
            const int plot_y = top + title_h + (d.wide && tall ? d.f_small + 4 : 0);
            const int plot_h = d.h - plot_y - labels_h - meter_h - 22 - d.m / 2;
            c.spectrum.build(p, d, cx + scale_w, plot_y, col_w - scale_w, plot_h, merge, d.wide, 1);
            const int my = plot_y + plot_h + labels_h + 12;
            lv_obj_t* const v = text(p, "", d.f_small, kText, cx + col_w - 56, my + meter_h + 1, 56, Align::kRight);
            c.meter.build(p, cx + scale_w, my, col_w - scale_w, meter_h, false, v);
        }
    }

    void hop(const Hop& h) override {
        if (use_single) {
            single.hop(h);
            return;
        }
        for (Column& c : columns) {
            c.spectrum.update(h.groups[static_cast<std::size_t>(c.group)]);
            c.meter.update(h.group_peak_db[static_cast<std::size_t>(c.group)], 0.0F);
        }
    }
};

struct Maker {
    const char* key;
    std::unique_ptr<View> (*make)();
};

}  // namespace

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("meters-options");
    const bool measure = argc > 2 && std::string(argv[2]) == "measure";
    fs::create_directories(out);

    lv_init();
    lv_tick_set_cb([]() -> std::uint32_t { return static_cast<std::uint32_t>(g_now_us / 1000); });
    lv_display_t* const disp = lv_display_create(kMaxW, kMaxH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static std::uint16_t first[kMaxW * kBufferRows];
    static std::uint16_t second[kMaxW * kBufferRows];
    lv_display_set_buffers(disp, first, second, sizeof first, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, &flush);

    const std::vector<Scenario> scenarios = {
        {"2-0", "2.0", scene::Content::kStereo, {.codec = nv::Codec::kPcm}, "PCM  2.0  48.0 kHz  from Music Assistant", 163},
        {"5-1", "5.1", scene::Content::kBed, {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true},
         "Dolby Digital Plus  5.1  48.0 kHz  640 kb/s", 163},
        {"7-1-4", "7.1.4", scene::Content::kBedAndObjects,
         {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 7}, "Dolby Atmos  7.1.4  48.0 kHz  768 kb/s", 292},
        {"9-1-6", "9.1.6", scene::Content::kBed, {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true},
         "Dolby Digital Plus  5.1 on 9.1.6  48.0 kHz  640 kb/s", 163},
    };
    const std::vector<Maker> makers = {
        {"A", [] { return std::unique_ptr<View>(std::make_unique<ViewA>()); }},
        {"B", [] { return std::unique_ptr<View>(std::make_unique<ViewB>()); }},
        {"C", [] { return std::unique_ptr<View>(std::make_unique<ViewC>()); }},
    };

    std::ofstream csv;
    if (measure) {
        csv.open(out / "measure.csv");
        csv << "size,scene,variant,seconds,pixels_per_s,areas_per_s,drawn_ticks_pct,render_us_per_tick,update_us_per_hop,objects,heap_bytes\n";
    }
    constexpr int kWarmHops = 60;
    constexpr int kSeqHops = 60;
    for (const Size& size : kSizes) {
        const Screen screen = make_screen(size);
        const Dim dim = dim_of(size.w, size.h - size.header - size.footer);
        for (const Scenario& def : scenarios) {
            const Room room = make_room(def.layout);
            // The scene is rendered once: every variant sees the same music (two hops a window).
            const int windows = measure ? 600 : def.still + kSeqHops / 2 + 4;
            std::vector<std::array<Hop, 2>> track(static_cast<std::size_t>(windows));
            {
                scene::Scene audio(room.spots, def.content);
                Model model(room);
                for (int w = 0; w < windows; ++w) {
                    audio.next();
                    model.hops(audio, track[static_cast<std::size_t>(w)]);
                }
            }
            const std::string heading = nv::heading(def.facts, def.layout, room.spots.size());
            lv_label_set_text(screen.header, heading.c_str());
            for (const Maker& maker : makers) {
                lv_obj_clean(screen.stage);
                g_now_us += 1'000'000;
                const std::size_t heap_before = heap_used();
                std::unique_ptr<View> view = maker.make();
                view->build(Stage{screen.stage, dim, &room, &def});
                lv_obj_update_layout(screen.stage);
                const std::string stem = std::string(size.key) + "_" + def.key + "_" + maker.key;
                if (!measure) {
                    const int start = std::max(0, def.still - kWarmHops / 2);
                    for (int w = start; w < def.still; ++w) {
                        for (const Hop& h : track[static_cast<std::size_t>(w)]) {
                            run_hop(*view, h, nullptr);
                        }
                    }
                    run_hop(*view, track[static_cast<std::size_t>(def.still)][0], nullptr);
                    run_hop(*view, track[static_cast<std::size_t>(def.still)][1], nullptr);
                    write_bmp(out / ("still_" + stem + ".bmp"), size.w, size.h);
                    // A sequence: of the large size at the half, of the middle size whole; not of the smallest.
                    const bool sequence = (size.w == 1024 && (std::string(def.key) == "5-1" || std::string(def.key) == "7-1-4")) ||
                                          (size.w == 480 && std::string(def.key) == "5-1");
                    if (sequence) {
                        int frame = 0;
                        for (int w = def.still + 1; frame < kSeqHops; ++w) {
                            for (const Hop& h : track[static_cast<std::size_t>(w)]) {
                                run_hop(*view, h, nullptr);
                                char name[96];
                                std::snprintf(name, sizeof name, "seq_%s_%02d.bmp", stem.c_str(), frame++);
                                write_bmp(out / name, size.w, size.h, size.w == 1024);
                            }
                        }
                    }
                } else {
                    for (int w = 0; w < kWarmHops / 2; ++w) {
                        for (const Hop& h : track[static_cast<std::size_t>(w)]) {
                            run_hop(*view, h, nullptr);
                        }
                    }
                    g_flushed_pixels = 0;
                    g_flushed_areas = 0;
                    Counters counters;
                    int hops = 0;
                    for (int w = kWarmHops / 2; w < windows; ++w) {
                        for (const Hop& h : track[static_cast<std::size_t>(w)]) {
                            run_hop(*view, h, &counters);
                            ++hops;
                        }
                    }
                    const double seconds = static_cast<double>(hops) * kHopSeconds;
                    const std::size_t objects = count_objects(screen.stage);
                    const std::size_t heap_after = heap_used();
                    char line[512];
                    std::snprintf(line, sizeof line, "%s,%s,%s,%.1f,%.0f,%.1f,%.1f,%.1f,%.2f,%zu,%zu\n", size.key, def.key, maker.key, seconds,
                                  static_cast<double>(g_flushed_pixels) / seconds, static_cast<double>(g_flushed_areas) / seconds,
                                  100.0 * counters.drawn_ticks / std::max<std::uint32_t>(1, counters.ticks),
                                  counters.render_us / std::max<std::uint32_t>(1, counters.ticks), counters.update_us / std::max(1, hops), objects,
                                  heap_after - heap_before);
                    csv << line;
                    std::fputs(line, stdout);
                }
                view.reset();
            }
        }
    }
    return 0;
}
