// How tied to the music a glow is, and what it costs, for a set of constants: the scene's levels (scene.hpp) run
// through a copy of rp::Glow with the constants as arguments. Not LVGL: the pixels are counted as the core's area for
// a core step and the halo's for a halo step. For choosing the constants of room_plan.hpp.
//
//   tune.exe  -> a table

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "scene.hpp"

struct Params {
    const char* name;
    int core_steps;
    int halo_steps;
    float span_db;
    float top_below_ceiling;
    float rms_fall;
    float peak_fall;
    float margin;
    int every;  // follow every nth window
    bool relative;
    float halo_margin = -1.0F;
};

struct Glow {
    Params p;
    float rms = -120.0F;
    float peak = -120.0F;
    int core = -1;
    int halo = -1;
    static float unit(float db, float lo, float hi) { return std::clamp((db - lo) / (hi - lo), 0.0F, 1.0F); }
    int hold(int current, float position, float margin) const {
        if (current >= 0 && position > static_cast<float>(current) - 0.5F - margin && position < static_cast<float>(current) + 0.5F + margin) {
            return current;
        }
        return static_cast<int>(std::lround(position));
    }
    // true when a step changed
    bool update(float rms_db, float peak_db, float seconds, float ceiling) {
        rms = std::max(rms_db, rms - p.rms_fall * seconds);
        peak = std::max(peak_db, peak - p.peak_fall * seconds);
        float top = (p.relative ? ceiling : -14.0F + 2.0F) - p.top_below_ceiling;
        if (!p.relative) {
            top = -14.0F;
        }
        const float ptop = top + 6.0F;
        const int c = rms > -80.0F ? 1 + hold(core - 1, unit(rms, top - p.span_db, top) * static_cast<float>(p.core_steps - 1), p.margin) : 0;
        const int h = rms > -80.0F ? hold(halo, unit(peak, ptop - p.span_db, ptop) * static_cast<float>(p.halo_steps), p.halo_margin >= 0.0F ? p.halo_margin : p.margin) : 0;
        const bool changed = c != core || h != halo;
        core = c;
        halo = h;
        return changed;
    }
};

int main() {
    const std::vector<Params> sets = {
        {"old (5 Hz, 5 steps, span 48, 14 dB/s)", 5, 3, 48.0F, 0.0F, 14.0F, 30.0F, 0.25F, 2, false},
        {"A 6/24/60/.30 halo m.6 fall25", 6, 3, 24.0F, 2.0F, 60.0F, 25.0F, 0.30F, 1, true, 0.6F},
        {"B 5/24/40/.30 halo m.6 fall25", 5, 3, 24.0F, 2.0F, 40.0F, 25.0F, 0.30F, 1, true, 0.6F},
        {"C 6/30/60/.35 halo2 m.6 fall25", 6, 2, 30.0F, 2.0F, 60.0F, 25.0F, 0.35F, 1, true, 0.6F},
        {"D 5/30/60/.35 halo3 m.6 fall25", 5, 3, 30.0F, 2.0F, 60.0F, 25.0F, 0.35F, 1, true, 0.6F},
        {"E 5/27/60/.40 halo3 m.6 fall25", 5, 3, 27.0F, 2.0F, 60.0F, 25.0F, 0.40F, 1, true, 0.6F},
        {"F 6/27/60/.40 halo3 m.6 fall25", 6, 3, 27.0F, 2.0F, 60.0F, 25.0F, 0.40F, 1, true, 0.6F},
        {"G 5/24/60/.40 halo3 m.6 fall25", 5, 3, 24.0F, 2.0F, 60.0F, 25.0F, 0.40F, 1, true, 0.6F},
        {"H 5/30/40/.45 halo3 m.6 fall25", 5, 3, 30.0F, 2.0F, 40.0F, 25.0F, 0.45F, 1, true, 0.6F},
        {"I 6/30/60/.40 halo2 m.7 fall20", 6, 2, 30.0F, 2.0F, 60.0F, 20.0F, 0.40F, 1, true, 0.7F},
    };
    struct Room {
        const char* layout;
        scene::Content content;
    };
    const Room rooms[] = {{"2.0", scene::Content::kStereo}, {"5.1", scene::Content::kBed}, {"7.1.4", scene::Content::kBedAndObjects}};
    std::printf("%-40s", "constants");
    for (const Room& r : rooms) {
        std::printf(" | %-5s chg/s r kpx/s onset-hit flicker", r.layout);
    }
    std::printf("\n");
    for (const Params& p : sets) {
        std::printf("%-40s", p.name);
        for (const Room& r : rooms) {
            const auto parsed = iclforge::render::OutputLayout::parse(r.layout);
            const auto spots = iclforge::render::plan_spots(*parsed);
            scene::Scene sc(spots, r.content);
            std::vector<scene::Window> track;
            for (int i = 0; i < 600; ++i) {
                track.push_back(sc.next());
            }
            const auto nodes = iclforge::render::plan_nodes(spots, {1024, 472});
            std::vector<Glow> glow(spots.size(), Glow{p});
            float ceiling = -48.0F;
            double changes = 0.0;
            double px = 0.0;
            double sxx = 0.0;
            double syy = 0.0;
            double sxy = 0.0;
            double sx = 0.0;
            double sy = 0.0;
            double n = 0.0;
            double onsets = 0.0;
            double onset_hits = 0.0;
            double quiet_moves = 0.0;
            double quiet_windows = 0.0;
            std::vector<float> last_rms(spots.size(), -120.0F);
            int speakers = 0;
            for (std::size_t o = 0; o < spots.size(); ++o) {
                speakers += nodes[o].shape != iclforge::render::PlanShape::kNone ? 1 : 0;
            }
            for (int w = 0; w < 600; ++w) {
                if (w % p.every != 0) {
                    continue;
                }
                const scene::Window& win = track[static_cast<std::size_t>(w)];
                float loudest = -120.0F;
                for (int o = 0; o < win.outputs; ++o) {
                    loudest = std::max(loudest, win.rms_db[static_cast<std::size_t>(o)]);
                }
                ceiling = std::max({loudest, ceiling - 2.0F * 0.1F * static_cast<float>(p.every), -48.0F});
                for (std::size_t o = 0; o < spots.size(); ++o) {
                    if (nodes[o].shape == iclforge::render::PlanShape::kNone) {
                        continue;
                    }
                    Glow& g = glow[o];
                    const int before_core = g.core;
                    const int before_halo = g.halo;
                    g.update(win.rms_db[o], win.peak_db[o], 0.1F * static_cast<float>(p.every), ceiling);
                    if (w >= 60 && win.rms_db[o] > -60.0F && last_rms[o] > -60.0F) {
                        const float delta = win.rms_db[o] - last_rms[o];
                        if (delta >= 5.0F) {
                            onsets += 1.0;
                            onset_hits += g.core > before_core ? 1.0 : 0.0;
                        }
                        if (std::fabs(delta) < 1.5F) {
                            quiet_windows += 1.0;
                            quiet_moves += g.core != before_core ? 1.0 : 0.0;
                        }
                    }
                    last_rms[o] = win.rms_db[o];
                    if (w >= 60) {
                        const double a = nodes[o].w;
                        const double halo_area = (a + 24) * (a + 24);
                        if (g.halo != before_halo) {
                            changes += 1.0;
                            px += halo_area;
                        } else if (g.core != before_core) {
                            changes += 1.0;
                            px += a * a;
                        }
                        const double x = g.core;
                        const double y = std::max(win.rms_db[o], -80.0F);
                        sx += x;
                        sy += y;
                        sxx += x * x;
                        syy += y * y;
                        sxy += x * y;
                        n += 1.0;
                    }
                }
            }
            const double seconds = (600 - 60) / 10.0;
            const double cov = sxy / n - (sx / n) * (sy / n);
            const double corr = cov / std::sqrt((sxx / n - (sx / n) * (sx / n)) * (syy / n - (sy / n) * (sy / n)) + 1e-9);
            std::printf(" | %4.1f %4.2f %4.0f on%3.0f%% fl%3.0f%%", changes / seconds / speakers, corr, px / seconds / 1000.0,
                        onsets > 0 ? 100.0 * onset_hits / onsets : 0.0, quiet_windows > 0 ? 100.0 * quiet_moves / quiet_windows : 0.0);
        }
        std::printf("\n");
    }
    return 0;
}
