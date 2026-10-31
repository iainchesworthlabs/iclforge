// The Meters view as an ADAPTIVE system (planning/p4x-meters-view.md, round 2): not one design but a rule that picks how the
// channels are shown from the size of the screen and the number of outputs of the room, both of which the board knows from its
// capabilities. Every representation sits under the same 31-band spectrum of the main mix:
//
//   pair    a stereo L/R master (for a room of more, the stereo downmix of the room: the fold of L C R Ls Rs)
//   none    no meters, a line of text
//   sum     the surround sum: six bars L C R LFE Ls Rs, whatever the room (a 7.1.4 room's heights and rears are folded into
//           the side they are on)
//   top4/top8 a fixed number of bars bound to the loudest ACTIVE channels, with labels that change when the binding does
//   mx      a micro matrix: a small square for each output laid out like the room plan, brightness and colour = level
//   mxg     the same squares in a plain grid
//   grid    every output a bar, grouped in space (front, surround, rear, height, LFE)
//   focus   one channel larger: its own meter and its own spectrum, the others small beside it
//
//   meters_adaptive <out> [measure | rooms]
//     stills (and sequences of a few) of every representation that fits each of three sizes in four rooms; with "measure" a
//     minute of each with the pixels LVGL flushed, objects, heap and the binding changes a minute of the top-N views make.

#include <functional>

#include "common.hpp"

namespace {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// --- which master channel and which group an output belongs to ------------------------------------------------------------

enum Master { kML, kMC, kMR, kMLFE, kMLs, kMRs, kMasters };
constexpr const char* kMasterNames[kMasters] = {"L", "C", "R", "LFE", "Ls", "Rs"};

// The six channels of the surround sum: an output is folded into the one it is nearest by azimuth (heights and rears with the side
// they are on); the LFE keeps its own.
int master_of(const rp::PlanSpot& s) {
    if (s.lfe) {
        return kMLFE;
    }
    const double m = std::fabs(s.azimuth_deg);
    if (m < 15.0) {
        return kMC;
    }
    if (m < 75.0) {
        return s.azimuth_deg > 0 ? kML : kMR;
    }
    return s.azimuth_deg > 0 ? kMLs : kMRs;
}

enum Grp { kGFront, kGSide, kGRear, kGHeight, kGLfe, kGroupsN };
constexpr const char* kGroupNames[kGroupsN] = {"Front", "Surround", "Rear", "Height", "LFE"};

int group_of(const rp::PlanSpot& s) {
    if (s.lfe) {
        return kGLfe;
    }
    if (s.elevation_deg > 20.0) {
        return kGHeight;
    }
    const double m = std::fabs(s.azimuth_deg);
    if (m <= 70.0) {
        return kGFront;
    }
    if (m <= 125.0) {
        return kGSide;
    }
    return kGRear;
}

// Levels add as powers when the channels are not known to agree: what the board has, a peak of each output in dB.
float power_sum_db(const Hop& hop, const std::vector<int>& members) {
    double p = 0.0;
    for (const int o : members) {
        const float db = hop.peak_db[static_cast<std::size_t>(o)];
        if (db > meters::kSilentDb) {
            p += std::pow(10.0, static_cast<double>(db) / 10.0);
        }
    }
    return p <= 1e-12 ? meters::kSilentDb : static_cast<float>(10.0 * std::log10(p));
}

struct Folds {
    std::array<std::vector<int>, kMasters> master;
    std::array<std::vector<int>, kGroupsN> group;
    std::vector<int> connected;
};

Folds folds_of(const Room& room) {
    Folds f;
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        const auto& s = room.spots[i];
        if (!s.connected) {
            continue;
        }
        f.master[static_cast<std::size_t>(master_of(s))].push_back(static_cast<int>(i));
        f.group[static_cast<std::size_t>(group_of(s))].push_back(static_cast<int>(i));
        f.connected.push_back(static_cast<int>(i));
    }
    return f;
}

// The stereo downmix of the room, as powers (ITU: the centre and the surrounds at -3 dB into the side they go to).
void downmix_db(const Hop& hop, const Folds& f, float& left, float& right) {
    std::array<double, kMasters> p{};
    for (int m = 0; m < kMasters; ++m) {
        const float db = power_sum_db(hop, f.master[static_cast<std::size_t>(m)]);
        p[static_cast<std::size_t>(m)] = db <= meters::kSilentDb ? 0.0 : std::pow(10.0, static_cast<double>(db) / 10.0);
    }
    const double l = p[kML] + 0.5 * p[kMC] + 0.5 * p[kMLs];
    const double r = p[kMR] + 0.5 * p[kMC] + 0.5 * p[kMRs];
    left = l <= 1e-12 ? meters::kSilentDb : static_cast<float>(10.0 * std::log10(l));
    right = r <= 1e-12 ? meters::kSilentDb : static_cast<float>(10.0 * std::log10(r));
}

// --- colour of a square -----------------------------------------------------------------------------------------------------

std::uint32_t mix_rgb(std::uint32_t a, std::uint32_t b, float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    const auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<std::uint32_t>(x + (y - x) * t + 0.5F) & 0xFFU;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// The level of a channel as a colour: dark when silent, green by -18 dB, amber by -6, red at full scale.
std::uint32_t heat(float db) {
    struct Stop {
        float at;
        std::uint32_t rgb;
    };
    static constexpr Stop kStops[] = {{0.0F, 0x2A2625}, {0.33F, 0x25493B}, {0.70F, 0x4FC28A}, {0.90F, 0xF0B24A}, {1.0F, 0xFF5C40}};
    const float u = unit(db, -60.0F, 0.0F);
    for (std::size_t i = 1; i < std::size(kStops); ++i) {
        if (u <= kStops[i].at) {
            return mix_rgb(kStops[i - 1].rgb, kStops[i].rgb, (u - kStops[i - 1].at) / (kStops[i].at - kStops[i - 1].at));
        }
    }
    return kStops[std::size(kStops) - 1].rgb;
}

// --- the loudest channels, kept ---------------------------------------------------------------------------------------------

// A fixed number of slots, each bound to a channel for as long as that is worth it. A channel takes a slot when it has been
// louder than the weakest bound channel by a margin for a little while, and a slot keeps its channel at least a second; a
// channel that has been silent for two seconds frees its slot. Without it ("raw"), the slots are the top N of every step.
class Binder {
   public:
    Binder(int slots, int outputs, bool hysteresis) : slot_(static_cast<std::size_t>(slots)), env_(static_cast<std::size_t>(outputs), meters::kSilentDb),
                                                     beat_(static_cast<std::size_t>(outputs), 0.0F), quiet_(static_cast<std::size_t>(outputs), 0.0F),
                                                     hysteresis_(hysteresis) {}

    static constexpr float kActiveDb = -50.0F;   // under this a channel is not active
    static constexpr float kMarginDb = 6.0F;     // by how much a challenger must beat the weakest bound channel
    static constexpr float kPersistS = 0.4F;     // for how long
    static constexpr float kMinHoldS = 1.0F;     // how long a slot keeps what it was given
    static constexpr float kFreeAfterS = 2.0F;   // how long a bound channel may be silent before its slot is free
    static constexpr float kFallDbPerS = 24.0F;  // the envelope the choice is made on

    // Returns the slots that changed this step.
    void step(const float* peak_db, float dt, std::vector<int>& changed) {
        changed.clear();
        const int n = static_cast<int>(env_.size());
        for (int o = 0; o < n; ++o) {
            const auto i = static_cast<std::size_t>(o);
            env_[i] = std::max(peak_db[o], env_[i] - kFallDbPerS * dt);
            quiet_[i] = env_[i] < kActiveDb ? quiet_[i] + dt : 0.0F;
        }
        if (!hysteresis_) {
            raw(changed);
            return;
        }
        for (std::size_t k = 0; k < slot_.size(); ++k) {
            Slot& s = slot_[k];
            if (s.out >= 0) {
                s.held += dt;
                if (quiet_[static_cast<std::size_t>(s.out)] >= kFreeAfterS && s.held >= kMinHoldS) {
                    s.out = -1;
                    changed.push_back(static_cast<int>(k));
                    ++rebinds_;
                }
            }
        }
        // Free slots take the loudest active channels that are not bound.
        for (std::size_t k = 0; k < slot_.size(); ++k) {
            if (slot_[k].out >= 0) {
                continue;
            }
            const int best = loudest_unbound();
            if (best >= 0 && env_[static_cast<std::size_t>(best)] >= kActiveDb) {
                slot_[k] = {best, 0.0F};
                changed.push_back(static_cast<int>(k));
                ++rebinds_;
            }
        }
        // A challenger takes the place of the weakest bound channel, if it has beaten it for long enough and that has been held.
        const int challenger = loudest_unbound();
        if (challenger >= 0 && env_[static_cast<std::size_t>(challenger)] >= kActiveDb) {
            int weakest = -1;
            for (std::size_t k = 0; k < slot_.size(); ++k) {
                if (slot_[k].out >= 0 && slot_[k].held >= kMinHoldS &&
                    (weakest < 0 || env_[static_cast<std::size_t>(slot_[k].out)] < env_[static_cast<std::size_t>(slot_[static_cast<std::size_t>(weakest)].out)])) {
                    weakest = static_cast<int>(k);
                }
            }
            const bool beats = weakest >= 0 && env_[static_cast<std::size_t>(challenger)] >
                                                   env_[static_cast<std::size_t>(slot_[static_cast<std::size_t>(weakest)].out)] + kMarginDb;
            for (int o = 0; o < n; ++o) {
                const auto i = static_cast<std::size_t>(o);
                beat_[i] = (o == challenger && beats) ? beat_[i] + dt : 0.0F;
            }
            if (beats && beat_[static_cast<std::size_t>(challenger)] >= kPersistS) {
                slot_[static_cast<std::size_t>(weakest)] = {challenger, 0.0F};
                changed.push_back(weakest);
                ++rebinds_;
                beat_[static_cast<std::size_t>(challenger)] = 0.0F;
            }
        }
    }

    [[nodiscard]] int output_of(std::size_t slot) const { return slot_[slot].out; }
    [[nodiscard]] int rebinds() const { return rebinds_; }

   private:
    struct Slot {
        int out = -1;
        float held = 0.0F;
    };

    [[nodiscard]] bool bound(int o) const {
        for (const Slot& s : slot_) {
            if (s.out == o) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int loudest_unbound() const {
        int best = -1;
        for (int o = 0; o < static_cast<int>(env_.size()); ++o) {
            if (!bound(o) && (best < 0 || env_[static_cast<std::size_t>(o)] > env_[static_cast<std::size_t>(best)])) {
                best = o;
            }
        }
        return best;
    }

    // No hysteresis: whatever is in the top N now has a slot (the ones that stay keep theirs).
    void raw(std::vector<int>& changed) {
        const int n = static_cast<int>(env_.size());
        std::vector<int> order(static_cast<std::size_t>(n));
        for (int o = 0; o < n; ++o) {
            order[static_cast<std::size_t>(o)] = o;
        }
        std::sort(order.begin(), order.end(), [this](int a, int b) { return env_[static_cast<std::size_t>(a)] > env_[static_cast<std::size_t>(b)]; });
        std::vector<int> want;
        for (const int o : order) {
            if (want.size() < slot_.size() && env_[static_cast<std::size_t>(o)] >= kActiveDb) {
                want.push_back(o);
            }
        }
        for (std::size_t k = 0; k < slot_.size(); ++k) {
            if (slot_[k].out >= 0 && std::find(want.begin(), want.end(), slot_[k].out) == want.end()) {
                slot_[k].out = -1;
                changed.push_back(static_cast<int>(k));
                ++rebinds_;
            }
        }
        for (const int o : want) {
            if (!bound(o)) {
                for (std::size_t k = 0; k < slot_.size(); ++k) {
                    if (slot_[k].out < 0) {
                        slot_[k] = {o, 0.0F};
                        changed.push_back(static_cast<int>(k));
                        ++rebinds_;
                        break;
                    }
                }
            }
        }
    }

    std::vector<Slot> slot_;
    std::vector<float> env_;
    std::vector<float> beat_;
    std::vector<float> quiet_;
    bool hysteresis_ = true;
    int rebinds_ = 0;
};

// --- a row of bars -----------------------------------------------------------------------------------------------------------

struct BarRow {
    std::vector<Meter> meter;
    std::vector<lv_obj_t*> name;
    std::vector<lv_obj_t*> value;
    int bar_top = 0;
    int bar_h = 0;
    int decimals = 0;

    // Bars standing on one line: their centres, their width, the rows above and below them.
    void build(lv_obj_t* p, const Dim& d, const std::vector<int>& centres, int pitch, int bar_w, int top, int bottom, bool values,
               const std::vector<std::string>& names) {
        const int vh = values ? d.f_small + 4 : 0;
        const int nh = d.f_small + 6;
        bar_top = top + vh;
        bar_h = std::max(8, bottom - top - vh - nh);
        const auto n = centres.size();
        meter.resize(n);
        name.assign(n, nullptr);
        value.assign(n, nullptr);
        decimals = values && n <= 12 ? 1 : 0;
        for (std::size_t k = 0; k < n; ++k) {
            lv_obj_t* v = nullptr;
            if (values) {
                v = text(p, "", d.f_small, kText, centres[k] - pitch / 2, top, pitch, Align::kCentre);
                value[k] = v;
            }
            meter[k].decimals = decimals;
            meter[k].build(p, centres[k] - bar_w / 2, bar_top, bar_w, bar_h, true, v);
            name[k] = text(p, names[k], d.f_small + 1, kMuted, centres[k] - pitch / 2, bar_top + bar_h + 3, pitch, Align::kCentre);
        }
    }
};

std::vector<int> even_centres(int x, int w, int n) {
    std::vector<int> c;
    for (int k = 0; k < n; ++k) {
        c.push_back(x + static_cast<int>(std::lround((static_cast<float>(k) + 0.5F) * static_cast<float>(w) / static_cast<float>(n))));
    }
    return c;
}

// --- a view with the spectrum on top -------------------------------------------------------------------------------------------

struct SpecView : View {
    Spectrum spectrum;
    Rect bottom;
    const Room* room = nullptr;
    const Scenario* scenario = nullptr;
    Folds folds;
    Dim d;
    lv_obj_t* parent = nullptr;
    int left_x = 0;
    int plot_w = 0;
    bool focus_spectrum = false;

    // The spectrum above a block of `bottom_h` pixels for the meters.
    void frame(const Stage& st, int bottom_h, const std::string& title_right = std::string()) {
        d = st.dim;
        room = st.room;
        scenario = st.scenario;
        parent = st.parent;
        folds = folds_of(*room);
        const bool tall = d.h >= 300;
        const int rows = d.wide ? 2 : 1;
        const int labels_h = rows * (d.f_small + 3) + 3;
        const int top = d.m / 2 + 2;
        left_x = d.m + d.scale_w + 4;
        plot_w = d.w - left_x - d.m;
        int plot_y = top;
        if (tall) {
            text(parent, std::string("Main mix  ") + room->layout, d.f_label, kMuted, left_x, top, plot_w - 300);
            if (!title_right.empty()) {
                text(parent, title_right, d.f_label - 2, kMuted, left_x + plot_w - 460, top + 2, 460, Align::kRight);
            }
            plot_y = top + d.f_label + 8 + 8;
        }
        bottom = {d.m, d.h - d.m / 2 - bottom_h, d.w - 2 * d.m, bottom_h};
        const int plot_h = bottom.y - 6 - labels_h - plot_y;
        spectrum.build(parent, d, left_x, plot_y, plot_w, plot_h, d.merge, true, rows);
    }

    void hop(const Hop& h) override {
        spectrum.update(focus_spectrum ? h.focus : h.main);
        update_meters(h);
    }

    virtual void update_meters(const Hop& h) = 0;
};

// The numbers a status line wants.
std::string peak_text(float db) { return "peak " + db_text(db, 1) + " dB"; }

// pair: the L and R of the room, or its stereo downmix.
struct PairView : SpecView {
    Meter left;
    Meter right;
    lv_obj_t* peak_label = nullptr;
    meters::Ballistic overall;
    bool downmix = false;
    std::string shown;

    void build(const Stage& st) override {
        const Dim& dm = st.dim;
        const bool tall = dm.h >= 300;
        const int meter_h = dm.wide ? 18 : dm.h >= 200 ? 12 : 8;
        const int gap = dm.wide ? 8 : 4;
        const int status_h = tall ? dm.f_label + 6 : dm.h >= 200 ? dm.f_small + 4 : 0;
        const int scale_gap = tall ? 14 : 0;
        const int bottom_h = scale_gap + 2 * meter_h + gap + (status_h > 0 ? status_h + gap : 0);
        frame(st, bottom_h);
        downmix = room->groups.size() > 1 || folds.connected.size() > 2;
        const int value_w = d.wide ? 64 : d.h >= 200 ? 44 : 34;
        int my = bottom.y + scale_gap;
        const int label_w = d.f_small + 6;
        lv_obj_t* const lv = text(parent, "", d.f_label, kText, left_x + plot_w - value_w, my + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
        lv_obj_t* const rv = text(parent, "", d.f_label, kText, left_x + plot_w - value_w, my + meter_h + gap + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
        text(parent, "L", d.f_small + 2, kMuted, d.m, my + (meter_h - d.f_small) / 2 - 3, label_w);
        text(parent, "R", d.f_small + 2, kMuted, d.m, my + meter_h + gap + (meter_h - d.f_small) / 2 - 3, label_w);
        left.build(parent, left_x, my, plot_w - value_w - 8, meter_h, false, lv);
        right.build(parent, left_x, my + meter_h + gap, plot_w - value_w - 8, meter_h, false, rv);
        if (tall) {
            for (const float db : {-60.0F, -40.0F, -20.0F, -10.0F, 0.0F}) {
                const int tx = left_x + static_cast<int>(std::lround(unit(db, -60.0F, 0.0F) * static_cast<float>(plot_w - value_w - 8)));
                text(parent, db_text(db, 0), d.f_small, kFaint, tx - 20, bottom.y, 40, Align::kCentre);
            }
        }
        if (status_h > 0) {
            const int sy = d.h - status_h - d.m / 2;
            rect(parent, d.m, sy - 3, d.w - 2 * d.m, 1, kFaint);
            const std::string line = std::string(scenario->status) + (downmix ? "  stereo downmix" : "");
            text(parent, line, tall ? d.f_label - 2 : d.f_small, kMuted, d.m, sy, d.w - 2 * d.m - 120);
            peak_label = text(parent, "", tall ? d.f_label - 2 : d.f_small, kText, d.w - d.m - 110, sy, 110, Align::kRight);
        }
    }

    void update_meters(const Hop& h) override {
        float l = meters::kSilentDb;
        float r = meters::kSilentDb;
        if (downmix) {
            downmix_db(h, folds, l, r);
        } else {
            l = h.peak_db[static_cast<std::size_t>(std::max(room->left, 0))];
            r = h.peak_db[static_cast<std::size_t>(std::max(room->right, 0))];
        }
        left.update(l, l);
        right.update(r, r);
        if (peak_label != nullptr) {
            const std::string s = peak_text(std::max(left.bal.peak, right.bal.peak));
            if (s != shown) {
                shown = s;
                lv_label_set_text(peak_label, s.c_str());
            }
        }
    }
};

// none: the spectrum, and a line of text.
struct NoneView : SpecView {
    lv_obj_t* peak_label = nullptr;
    meters::Ballistic overall;
    std::string shown;

    void build(const Stage& st) override {
        const Dim& dm = st.dim;
        const int line_h = dm.f_small + 6;
        frame(st, line_h);
        text(parent, room->layout + std::string("  48 kHz"), d.f_small, kMuted, d.m, bottom.y + 2, d.w / 2);
        peak_label = text(parent, "", d.f_small + 1, kText, d.w / 2, bottom.y + 2, d.w / 2 - d.m, Align::kRight);
    }

    void update_meters(const Hop& h) override {
        const float loud = loudest(h, *room);
        overall.update(loud, kHopSeconds, 20.0F, 3.0F, 0.0F);
        const std::string s = peak_text(overall.peak);
        if (s != shown) {
            shown = s;
            lv_label_set_text(peak_label, s.c_str());
        }
    }
};

// A row of vertical bars under the spectrum for the six masters, or every output, or the top N.
struct BarsView : SpecView {
    enum class Kind { kSum, kAll, kGrouped, kTop };
    Kind kind = Kind::kSum;
    int slots = 6;
    BarRow bars;
    std::vector<int> outputs;  // for kAll and kGrouped: the output of each bar
    std::unique_ptr<Binder> binder;
    std::vector<int> changed;
    bool hysteresis = true;
    int total_rebinds = 0;
    Rect highlight;

    BarsView(Kind k, int n = 6, bool hyst = true) : kind(k), slots(n), hysteresis(hyst) {}

    [[nodiscard]] int rebinds() const override { return binder ? binder->rebinds() : 0; }

    void build(const Stage& st) override {
        const Dim& dm = st.dim;
        const bool tall = dm.h >= 300;
        // The room's outputs, and the names under the bars.
        std::vector<std::string> names;
        std::vector<int> centres;
        int bar_w = 0;
        int pitch = 0;
        const int n_bars = kind == Kind::kSum ? kMasters : kind == Kind::kTop ? slots : static_cast<int>(folds_count(st));
        int bottom_h = 0;
        if (tall) {
            bottom_h = kind == Kind::kGrouped ? 196 : 172;
        } else {
            bottom_h = dm.h >= 200 ? 96 : 60;
        }
        frame(st, bottom_h, tall ? scenario_status(st) : std::string());
        const Rect b = bottom;
        const int x0 = left_x;
        const int w = plot_w;
        std::vector<std::string> group_caption;
        std::vector<Rect> group_span;
        if (kind == Kind::kSum) {
            for (const char* nm : kMasterNames) {
                names.emplace_back(nm);
            }
            centres = even_centres(x0, w, kMasters);
            pitch = w / kMasters;
        } else if (kind == Kind::kTop) {
            const int n = std::min(slots, std::max(1, static_cast<int>(folds.connected.size())));
            names.assign(static_cast<std::size_t>(n), "-");
            centres = even_centres(x0, w, n);
            pitch = w / n;
        } else {
            // every output, in space: groups left to right, a gap between them
            outputs.clear();
            std::vector<std::vector<int>> groups;
            for (int g = 0; g < kGroupsN; ++g) {
                if (!folds.group[static_cast<std::size_t>(g)].empty()) {
                    groups.push_back(folds.group[static_cast<std::size_t>(g)]);
                    group_caption.emplace_back(kGroupNames[g]);
                }
            }
            const bool gaps = kind == Kind::kGrouped;
            int count = 0;
            for (const auto& g : groups) {
                count += static_cast<int>(g.size());
            }
            const int gap = gaps ? (dm.wide ? 22 : 8) : 0;
            const int slots_w = w - gap * (static_cast<int>(groups.size()) - 1);
            pitch = slots_w / count;
            int cx = x0;
            for (std::size_t g = 0; g < groups.size(); ++g) {
                const int start = cx;
                for (const int o : groups[g]) {
                    outputs.push_back(o);
                    names.push_back(room->spots[static_cast<std::size_t>(o)].name);
                    centres.push_back(cx + pitch / 2);
                    cx += pitch;
                }
                group_span.push_back({start, 0, cx - start, 0});
                cx += gap;
            }
        }
        bar_w = std::clamp(static_cast<int>(pitch * 0.55F), 6, dm.wide ? 64 : 44);
        const int caption_h = (kind == Kind::kGrouped && tall) ? dm.f_small + 8 : 0;
        const bool values = tall && n_bars <= 16;
        bars.build(parent, dm, centres, pitch, bar_w, b.y, b.y + b.h - caption_h, values, names);
        if (kind == Kind::kGrouped) {
            for (std::size_t g = 0; g < group_span.size() && tall; ++g) {
                const Rect& s = group_span[g];
                rect(parent, s.x + 2, b.y + b.h - caption_h + 2, s.w - 4, 1, kFaint);
                text(parent, group_caption[g], dm.f_small, kMuted, s.x, b.y + b.h - caption_h + 5, s.w, Align::kCentre);
            }
        }
        // Scale at the left.
        if (tall || dm.h >= 200) {
            for (const float db : {0.0F, -18.0F, -40.0F}) {
                const int ly = bars.bar_top + static_cast<int>(std::lround((1.0F - unit(db, -60.0F, 0.0F)) * static_cast<float>(bars.bar_h)));
                text(parent, db_text(db, 0), dm.f_small, kFaint, dm.m, ly - dm.f_small / 2 - 2, dm.scale_w + 2, Align::kRight);
            }
        }
        if (kind == Kind::kTop) {
            binder = std::make_unique<Binder>(static_cast<int>(names.size()), static_cast<int>(room->spots.size()), hysteresis);
        }
        names_ = names;
    }

    void update_meters(const Hop& h) override {
        if (kind == Kind::kSum) {
            for (int m = 0; m < kMasters; ++m) {
                const float db = power_sum_db(h, folds.master[static_cast<std::size_t>(m)]);
                bars.meter[static_cast<std::size_t>(m)].update(db, db);
            }
        } else if (kind == Kind::kTop) {
            binder->step(h.peak_db.data(), kHopSeconds, changed);
            for (const int k : changed) {
                const int o = binder->output_of(static_cast<std::size_t>(k));
                const std::string nm = o >= 0 ? room->spots[static_cast<std::size_t>(o)].name : std::string("-");
                lv_label_set_text(bars.name[static_cast<std::size_t>(k)], nm.c_str());
                lv_obj_set_style_text_color(bars.name[static_cast<std::size_t>(k)], color(o >= 0 ? kText : kFaint), 0);
                // The bar starts from the new channel's level (and its hold is cleared).
                Meter& m = bars.meter[static_cast<std::size_t>(k)];
                const float db = o >= 0 ? h.peak_db[static_cast<std::size_t>(o)] : meters::kSilentDb;
                m.bal.level = db;
                m.bal.peak = db;
                m.bal.hold_left = 0.0F;
            }
            for (std::size_t k = 0; k < bars.meter.size(); ++k) {
                const int o = binder->output_of(k);
                const float db = o >= 0 ? h.peak_db[static_cast<std::size_t>(o)] : meters::kSilentDb;
                bars.meter[k].update(db, db);
            }
        } else {
            for (std::size_t k = 0; k < bars.meter.size(); ++k) {
                const float db = h.peak_db[static_cast<std::size_t>(outputs[k])];
                bars.meter[k].update(db, db);
            }
        }
    }

   private:
    static std::size_t folds_count(const Stage& st) {
        std::size_t n = 0;
        for (const auto& s : st.room->spots) {
            n += s.connected ? 1 : 0;
        }
        return n;
    }
    static std::string scenario_status(const Stage& st) { return st.scenario->status; }
    std::vector<std::string> names_;
};

// mx / mxg: a micro matrix of squares whose colour follows the level.
struct MatrixView : SpecView {
    bool spatial = true;
    std::vector<lv_obj_t*> square;
    std::vector<int> outputs;
    std::vector<int> step;
    std::vector<meters::Ballistic> bal;
    lv_obj_t* peak_label = nullptr;
    std::string shown;
    meters::Ballistic overall;

    explicit MatrixView(bool spatial_) : spatial(spatial_) {}

    void build(const Stage& st) override {
        const Dim& dm = st.dim;
        const bool tall = dm.h >= 300;
        const int bottom_h = tall ? 206 : 112;
        frame(st, bottom_h, tall ? std::string(st.scenario->status) : std::string());
        for (std::size_t i = 0; i < room->spots.size(); ++i) {
            if (room->spots[i].connected) {
                outputs.push_back(static_cast<int>(i));
            }
        }
        const int n = static_cast<int>(outputs.size());
        // The region the squares share; a column at the right for the peak.
        const int side_w = tall ? 190 : 0;
        const Rect region = {bottom.x + (dm.wide ? 0 : 0), bottom.y, bottom.w - side_w, bottom.h};
        std::vector<std::pair<int, int>> centre(static_cast<std::size_t>(n));
        int side = 0;
        if (spatial) {
            // A top-down map of the room: the listener in the middle, the front at the top, ear-level speakers on the
            // ellipse at their azimuth, the heights on an inner one (a ceiling speaker at the middle), the LFE behind.
            const double pad = tall ? 22.0 : 12.0;
            const double rx = region.w / 2.0 - pad;
            const double ry = region.h / 2.0 - pad;
            const double cx = region.x + region.w / 2.0;
            const double cy = region.y + region.h / 2.0;
            for (int k = 0; k < n; ++k) {
                const auto& sp = room->spots[static_cast<std::size_t>(outputs[static_cast<std::size_t>(k)])];
                const double az = sp.azimuth_deg * 3.14159265358979323846 / 180.0;
                const double r = sp.elevation_deg >= 80.0 ? 0.0 : sp.elevation_deg > 20.0 ? 0.5 : 1.0;
                double ux = -std::sin(az) * r;
                double uy = -std::cos(az) * r;
                if (sp.lfe) {
                    ux = 0.0;
                    uy = 1.0;
                }
                centre[static_cast<std::size_t>(k)] = {static_cast<int>(std::lround(cx + ux * rx)), static_cast<int>(std::lround(cy + uy * ry))};
            }
            double nearest = 1e9;
            for (int a = 0; a < n; ++a) {
                for (int b = a + 1; b < n; ++b) {
                    const double dx = centre[static_cast<std::size_t>(a)].first - centre[static_cast<std::size_t>(b)].first;
                    const double dy = centre[static_cast<std::size_t>(a)].second - centre[static_cast<std::size_t>(b)].second;
                    // Squares are on axes, so the nearer axis decides how big they may be.
                    nearest = std::min(nearest, std::max(std::fabs(dx), std::fabs(dy)));
                }
            }
            side = std::clamp(static_cast<int>(nearest * 0.9), 10, tall ? 50 : 34);
        } else {
            const int cols = n <= 8 ? n : (n + 1) / 2;
            const int rows = (n + cols - 1) / cols;
            const int cw = region.w / cols;
            const int ch = region.h / rows;
            side = std::clamp(std::min(cw, ch) - 6, 10, tall ? 56 : 34);
            for (int k = 0; k < n; ++k) {
                centre[static_cast<std::size_t>(k)] = {region.x + (k % cols) * cw + cw / 2, region.y + (k / cols) * ch + ch / 2};
            }
        }
        square.assign(static_cast<std::size_t>(n), nullptr);
        step.assign(static_cast<std::size_t>(n), -1);
        bal.assign(static_cast<std::size_t>(n), {});
        for (int k = 0; k < n; ++k) {
            const auto idx = static_cast<std::size_t>(k);
            square[idx] = rect(parent, centre[idx].first - side / 2, centre[idx].second - side / 2, side, side, 0x2A2625, 255, side >= 20 ? 4 : 2);
            if (side >= 26) {
                lv_obj_t* const label = text(square[idx], room->spots[static_cast<std::size_t>(outputs[idx])].name, dm.f_small, kText, 0, 0, side, Align::kCentre);
                lv_obj_center(label);
                lv_obj_set_style_text_opa(label, LV_OPA_80, 0);
            }
        }
        if (side_w > 0) {
            const int sx = bottom.x + bottom.w - side_w + 20;
            text(parent, "peak", dm.f_small, kMuted, sx, bottom.y + 6, side_w - 20);
            peak_label = text(parent, "", dm.f_label + 8, kText, sx, bottom.y + 6 + dm.f_small + 6, side_w - 20);
            // The colour key.
            const int kw = side_w - 30;
            for (int i = 0; i < 24; ++i) {
                rect(parent, sx + i * kw / 24, bottom.y + bottom.h - 44, kw / 24 + 1, 10, heat(-60.0F + 60.0F * static_cast<float>(i) / 23.0F));
            }
            text(parent, "-60", dm.f_small, kFaint, sx - 8, bottom.y + bottom.h - 30, 40);
            text(parent, "0 dB", dm.f_small, kFaint, sx + kw - 40, bottom.y + bottom.h - 30, 56, Align::kRight);
        }
    }

    void update_meters(const Hop& h) override {
        for (std::size_t k = 0; k < square.size(); ++k) {
            const float db = h.peak_db[static_cast<std::size_t>(outputs[k])];
            bal[k].update(db, kHopSeconds, 24.0F, 0.0F, 24.0F);
            const int q = static_cast<int>(std::lround(unit(bal[k].level, -60.0F, 0.0F) * 20.0F));
            if (q != step[k]) {
                step[k] = q;
                lv_obj_set_style_bg_color(square[k], color(heat(bal[k].level)), 0);
            }
        }
        if (peak_label != nullptr) {
            overall.update(loudest(h, *room), kHopSeconds, 100.0F, 3.0F, 0.0F);
            const std::string s = db_text(overall.peak, 1) + " dB";
            if (s != shown) {
                shown = s;
                lv_label_set_text(peak_label, s.c_str());
            }
        }
    }
};

// focus: one channel larger, with its own spectrum; the others as small bars.
struct FocusView : SpecView {
    int focus = 0;
    Meter big;
    BarRow bars;
    std::vector<int> outputs;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* value_label = nullptr;

    explicit FocusView(int out) : focus(out) { focus_spectrum = true; }

    void build(const Stage& st) override {
        const Dim& dm = st.dim;
        const bool tall = dm.h >= 300;
        const int bottom_h = tall ? 172 : 96;
        frame(st, bottom_h, tall ? std::string("focus: tap a channel") : std::string());
        const Rect b = bottom;
        const std::string nm = room->spots[static_cast<std::size_t>(focus)].name;
        // The big meter: name, a long bar, the peak.
        const int panel_w = tall ? 250 : 130;
        text(parent, nm, dm.f_label + (tall ? 14 : 4), kText, b.x, b.y, panel_w);
        lv_obj_t* const value = text(parent, "", dm.f_label + (tall ? 10 : 2), kText, b.x, b.y + dm.f_label + (tall ? 22 : 10), panel_w, Align::kLeft);
        const int bh = tall ? 30 : 16;
        big.build(parent, b.x, b.y + b.h - bh - 4, panel_w - 16, bh, false, nullptr);
        value_label = value;
        // The others.
        for (std::size_t i = 0; i < room->spots.size(); ++i) {
            if (room->spots[i].connected) {
                outputs.push_back(static_cast<int>(i));
            }
        }
        std::vector<std::string> names;
        for (const int o : outputs) {
            names.push_back(room->spots[static_cast<std::size_t>(o)].name);
        }
        const int x0 = b.x + panel_w + 10;
        const int w = b.x + b.w - x0;
        const int n = static_cast<int>(outputs.size());
        const auto centres = even_centres(x0, w, n);
        const int pitch = w / n;
        // A plate behind the focused bar.
        for (int k = 0; k < n; ++k) {
            if (outputs[static_cast<std::size_t>(k)] == focus) {
                rect(parent, centres[static_cast<std::size_t>(k)] - pitch / 2 + 1, b.y, pitch - 2, b.h, kSurfaceUp, 255, 4);
            }
        }
        bars.build(parent, dm, centres, pitch, std::clamp(static_cast<int>(pitch * 0.5F), 6, 40), b.y + 2, b.y + b.h, false, names);
    }

    void update_meters(const Hop& h) override {
        const float db = h.peak_db[static_cast<std::size_t>(focus)];
        big.update(db, db);
        const std::string s = db_text(big.bal.peak, 1) + " dB";
        if (s != shown_) {
            shown_ = s;
            lv_label_set_text(value_label, s.c_str());
        }
        for (std::size_t k = 0; k < bars.meter.size(); ++k) {
            const float v = h.peak_db[static_cast<std::size_t>(outputs[k])];
            bars.meter[k].update(v, v);
        }
    }

   private:
    std::string shown_;
};

// --- the cases -----------------------------------------------------------------------------------------------------------------

struct Cand {
    const char* key;
    bool zoned;
    std::function<std::unique_ptr<View>(const Room&)> make;
};

int focus_of(const Room& room) {
    // A channel that has something to show: the first height of a room that has them, else a surround, else the right.
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        if (room.spots[i].connected && !room.spots[i].lfe && room.spots[i].elevation_deg > 20.0) {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        if (room.spots[i].name == "Ls") {
            return static_cast<int>(i);
        }
    }
    return std::max(room.right, 0);
}

std::vector<Cand> candidates_for(int width, bool measure) {
    std::vector<Cand> c;
    const auto add = [&](const char* key, std::function<std::unique_ptr<View>(const Room&)> make, bool zoned = false) {
        c.push_back({key, zoned, std::move(make)});
    };
    using K = BarsView::Kind;
    if (width < 400) {
        add("pair", [](const Room&) { return std::unique_ptr<View>(std::make_unique<PairView>()); });
        add("none", [](const Room&) { return std::unique_ptr<View>(std::make_unique<NoneView>()); });
    } else if (width < 800) {
        add("sum", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kSum)); });
        add("top6", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 6)); });
        add("mx", [](const Room&) { return std::unique_ptr<View>(std::make_unique<MatrixView>(true)); });
        add("mxg", [](const Room&) { return std::unique_ptr<View>(std::make_unique<MatrixView>(false)); });
        add("pair", [](const Room&) { return std::unique_ptr<View>(std::make_unique<PairView>()); });
        add("focus", [](const Room& r) { return std::unique_ptr<View>(std::make_unique<FocusView>(focus_of(r))); });
        add("sum-z", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kSum)); }, true);
        if (measure) {
            add("top6-raw", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 6, false)); });
        }
    } else {
        add("grid", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kGrouped)); });
        add("flat", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kAll)); });
        add("top4", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 4)); });
        add("top8", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 8)); });
        add("mx", [](const Room&) { return std::unique_ptr<View>(std::make_unique<MatrixView>(true)); });
        add("mxg", [](const Room&) { return std::unique_ptr<View>(std::make_unique<MatrixView>(false)); });
        add("sum", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kSum)); });
        add("pair", [](const Room&) { return std::unique_ptr<View>(std::make_unique<PairView>()); });
        add("focus", [](const Room& r) { return std::unique_ptr<View>(std::make_unique<FocusView>(focus_of(r))); });
        add("pair-z", [](const Room&) { return std::unique_ptr<View>(std::make_unique<PairView>()); }, true);
        if (measure) {
            add("top4-raw", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 4, false)); });
            add("top8-raw", [](const Room&) { return std::unique_ptr<View>(std::make_unique<BarsView>(K::kTop, 8, false)); });
        }
    }
    return c;
}

bool wants_sequence(const Size& size, const std::string& room, const std::string& cand) {
    if (size.w == 1024) {
        return (room == "7-1-4" && (cand == "grid" || cand == "top8" || cand == "mx")) || (room == "9-1-6" && (cand == "grid" || cand == "top8"));
    }
    if (size.w == 480) {
        return room == "7-1-4" && (cand == "sum" || cand == "top6" || cand == "mx");
    }
    return room == "5-1" && cand == "pair";
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("meters-adaptive");
    const std::string mode = argc > 2 ? argv[2] : "";
    const bool measure = mode == "measure";
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

    if (mode == "rooms") {
        for (const Scenario& def : scenarios) {
            const Room room = make_room(def.layout);
            const Folds f = folds_of(room);
            std::printf("%s:", def.key);
            for (std::size_t i = 0; i < room.spots.size(); ++i) {
                const auto& s = room.spots[i];
                std::printf(" %s(az %.0f el %.0f m%d g%d)", s.name.c_str(), s.azimuth_deg, s.elevation_deg, master_of(s), group_of(s));
            }
            std::printf("\n");
        }
        return 0;
    }

    if (mode == "bench") {
        // What the selection and the folding cost by themselves, a step each, on this computer.
        const Room room = make_room("9.1.6");
        const Folds f = folds_of(room);
        Hop hop;
        for (std::size_t i = 0; i < hop.peak_db.size(); ++i) {
            hop.peak_db[i] = -60.0F + 3.0F * static_cast<float>(i % 17);
        }
        constexpr int kRuns = 200000;
        using Clock = std::chrono::steady_clock;
        float sink = 0.0F;
        auto t0 = Clock::now();
        for (int i = 0; i < kRuns; ++i) {
            hop.peak_db[3] = -20.0F + static_cast<float>(i % 7);
            for (int m = 0; m < kMasters; ++m) {
                sink += power_sum_db(hop, f.master[static_cast<std::size_t>(m)]);
            }
        }
        const double fold_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / kRuns;
        t0 = Clock::now();
        for (int i = 0; i < kRuns; ++i) {
            float l = 0.0F;
            float r = 0.0F;
            hop.peak_db[3] = -20.0F + static_cast<float>(i % 7);
            downmix_db(hop, f, l, r);
            sink += l + r;
        }
        const double dm_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / kRuns;
        for (const int n : {4, 8}) {
            Binder binder(n, static_cast<int>(room.spots.size()), true);
            std::vector<int> changed;
            t0 = Clock::now();
            for (int i = 0; i < kRuns; ++i) {
                hop.peak_db[static_cast<std::size_t>(i % 16)] = -10.0F - static_cast<float>((i / 16) % 40);
                binder.step(hop.peak_db.data(), kHopSeconds, changed);
            }
            std::printf("top-%d selection with hysteresis: %.2f us a step\n", n, std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / kRuns);
        }
        t0 = Clock::now();
        std::uint32_t colours = 0;
        for (int i = 0; i < kRuns; ++i) {
            for (std::size_t o = 0; o < 16; ++o) {
                colours += heat(hop.peak_db[o] + static_cast<float>(i % 3));
            }
        }
        const double heat_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / kRuns;
        std::printf("surround sum (six masters of 16 outputs): %.2f us   stereo downmix: %.2f us   16 square colours: %.2f us   (%.1f %u)\n", fold_us, dm_us, heat_us,
                    static_cast<double>(sink), colours);
        return 0;
    }

    std::ofstream csv;
    if (measure) {
        csv.open(out / "measure.csv");
        csv << "size,scene,view,seconds,pixels_per_s,areas_per_s,render_us_per_tick,update_us_per_hop,objects,heap_bytes,rebinds_per_min\n";
    }
    constexpr int kWarmHops = 60;
    constexpr int kSeqHops = 60;
    for (const Size& size : kSizes) {
        const Screen screen = make_screen(size);
        const Dim dim = dim_of(size.w, size.h - size.header - size.footer);
        const std::vector<Cand> cands = candidates_for(size.w, measure);
        for (const Scenario& def : scenarios) {
            const Room room = make_room(def.layout);
            const int windows = measure ? 600 : def.still + kSeqHops / 2 + 4;
            std::vector<std::array<Hop, 2>> track(static_cast<std::size_t>(windows));
            {
                scene::Scene audio(room.spots, def.content);
                Model model(room, focus_of(room));
                for (int w = 0; w < windows; ++w) {
                    audio.next();
                    model.hops(audio, track[static_cast<std::size_t>(w)]);
                }
            }
            lv_label_set_text(screen.header, nv::heading(def.facts, def.layout, room.spots.size()).c_str());
            for (const Cand& cand : cands) {
                lv_obj_clean(screen.stage);
                g_now_us += 1'000'000;
                g_zoned_spectrum = cand.zoned;
                const std::size_t heap_before = heap_used();
                std::unique_ptr<View> view = cand.make(room);
                view->build(Stage{screen.stage, dim, &room, &def});
                lv_obj_update_layout(screen.stage);
                const std::string stem = std::string(size.key) + "_" + def.key + "_" + cand.key;
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
                    if (wants_sequence(size, def.key, cand.key)) {
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
                    const int rebinds_before = view->rebinds();
                    for (int w = kWarmHops / 2; w < windows; ++w) {
                        for (const Hop& h : track[static_cast<std::size_t>(w)]) {
                            run_hop(*view, h, &counters);
                            ++hops;
                        }
                    }
                    const double seconds = static_cast<double>(hops) * kHopSeconds;
                    char line[512];
                    std::snprintf(line, sizeof line, "%s,%s,%s,%.1f,%.0f,%.1f,%.1f,%.2f,%zu,%zu,%.1f\n", size.key, def.key, cand.key, seconds,
                                  static_cast<double>(g_flushed_pixels) / seconds, static_cast<double>(g_flushed_areas) / seconds,
                                  counters.render_us / std::max<std::uint32_t>(1, counters.ticks), counters.update_us / std::max(1, hops),
                                  count_objects(screen.stage), heap_used() - heap_before, (view->rebinds() - rebinds_before) * 60.0 / seconds);
                    csv << line;
                    std::fputs(line, stdout);
                }
                view.reset();
            }
        }
    }
    g_zoned_spectrum = false;
    return 0;
}
