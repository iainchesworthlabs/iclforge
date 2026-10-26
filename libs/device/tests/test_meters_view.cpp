#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "iclforge/meters_view.hpp"
#include "iclforge/playout.hpp"

namespace mv = iclforge::meters;

namespace {

mv::Spot spot(float azimuth, float elevation = 0.0F) { return {.azimuth_deg = azimuth, .elevation_deg = elevation, .lfe = false, .connected = true}; }
mv::Spot lfe() { return {.azimuth_deg = 0.0F, .elevation_deg = 0.0F, .lfe = true, .connected = true}; }

// The rooms of planning/p4x-meters-view.md, from the renderer's own layouts (their azimuths and elevations, in their slot order).
std::vector<mv::Spot> room_2_0() { return {spot(30), spot(-30)}; }
std::vector<mv::Spot> room_5_1() { return {spot(30), spot(0), spot(-30), spot(110), spot(-110), lfe()}; }
std::vector<mv::Spot> room_7_1_4() {
    return {spot(30),      spot(0),       spot(-30),       spot(90),        spot(-90),  spot(150),  spot(-150),
            spot(45, 45),  spot(-45, 45), spot(135, 45),   spot(-135, 45),  lfe()};
}
std::vector<mv::Spot> room_9_1_6() {
    return {spot(30),      spot(0),        spot(-30),      spot(90),      spot(-90),      spot(150),      spot(-150),    spot(60),
            spot(-60),     spot(45, 45),   spot(-45, 45),  spot(0, 45),   spot(135, 45),  spot(-135, 45), spot(180, 90), lfe()};
}

std::size_t count(const mv::Members& m) { return m.count; }

}  // namespace

TEST_CASE("the constants agree with the player's", "[meters]") {
    REQUIRE(mv::kMaxOutputs == iclforge::Playout::kMaxOutputs);
    REQUIRE(mv::kBandLabels.size() == static_cast<std::size_t>(mv::kBands));
    REQUIRE(mv::kBandLabels.front() == "20");
    REQUIRE(mv::kBandLabels.back() == "20k");
}

TEST_CASE("the rule: width and outputs choose the representation", "[meters]") {
    using R = mv::Representation;
    struct Row {
        int width;
        int outputs;
        R expect;
    };
    constexpr Row kRows[] = {
        // large
        {1024, 2, R::kPair}, {1024, 6, R::kGrid}, {1024, 12, R::kGrid}, {1024, 16, R::kGrid}, {800, 6, R::kGrid},
        // medium
        {480, 2, R::kPair}, {480, 6, R::kSum}, {480, 12, R::kSum}, {480, 16, R::kSum}, {799, 6, R::kSum}, {400, 6, R::kSum}, {480, 3, R::kSum},
        // small
        {240, 2, R::kPair}, {240, 6, R::kPair}, {240, 12, R::kPair}, {240, 16, R::kPair}, {399, 16, R::kPair}, {320, 6, R::kPair},
        // a board with one output, or none connected
        {1024, 1, R::kPair}, {1024, 0, R::kPair},
    };
    for (const Row& row : kRows) {
        INFO("width " << row.width << " outputs " << row.outputs);
        REQUIRE(mv::representation_for(row.width, row.outputs) == row.expect);
    }
}

TEST_CASE("a bar of the spectrum is one band, two or three, by width, and covers the bands without a gap", "[meters]") {
    REQUIRE(mv::bands_per_bar(1024) == 1);
    REQUIRE(mv::bands_per_bar(800) == 1);
    REQUIRE(mv::bands_per_bar(799) == 2);
    REQUIRE(mv::bands_per_bar(480) == 2);
    REQUIRE(mv::bands_per_bar(400) == 2);
    REQUIRE(mv::bands_per_bar(399) == 3);
    REQUIRE(mv::bands_per_bar(240) == 3);
    REQUIRE(mv::spectrum_bars(1) == 31);
    REQUIRE(mv::spectrum_bars(2) == 15);
    REQUIRE(mv::spectrum_bars(3) == 10);
    for (const int per : {2, 3}) {
        // 20 Hz is dropped; the rest are covered in order, once each.
        int next = 1;
        for (int bar = 0; bar < mv::spectrum_bars(per); ++bar) {
            REQUIRE(mv::first_band_of_bar(bar, per) == next);
            next += per;
        }
        REQUIRE(next == mv::kBands);
    }
}

TEST_CASE("a bar of several bands is their powers added", "[meters]") {
    std::array<float, mv::kBands> bands{};
    bands.fill(-30.0F);
    // Two equal bands are 3 dB over either, three are 4.77.
    REQUIRE(std::fabs(mv::bar_db(bands, 0, 2) - (-30.0F + 3.0103F)) < 1e-3F);
    REQUIRE(std::fabs(mv::bar_db(bands, 4, 3) - (-30.0F + 4.7712F)) < 1e-3F);
    // One band is itself, and a silent one adds nothing.
    REQUIRE(mv::bar_db(bands, 7, 1) == -30.0F);
    bands[1] = mv::kSilentDb;
    REQUIRE(std::fabs(mv::bar_db(bands, 0, 2) - (-30.0F)) < 1e-3F);
    bands[2] = mv::kSilentDb;
    REQUIRE(mv::bar_db(bands, 0, 2) == mv::kSilentDb);
}

TEST_CASE("the surround sum folds a room's outputs into six masters by azimuth", "[meters]") {
    using M = mv::Master;
    {
        const auto spots = room_5_1();
        const std::array<M, 6> expect = {M::kL, M::kC, M::kR, M::kLs, M::kRs, M::kLfe};
        for (std::size_t i = 0; i < spots.size(); ++i) {
            REQUIRE(mv::master_of(spots[i]) == expect[i]);
        }
    }
    {
        // 7.1.4: the rear speakers and the heights go to the side they are on.
        const auto spots = room_7_1_4();
        const std::array<M, 12> expect = {M::kL, M::kC, M::kR, M::kLs, M::kRs, M::kLs, M::kRs, M::kL, M::kR, M::kLs, M::kRs, M::kLfe};
        for (std::size_t i = 0; i < spots.size(); ++i) {
            INFO("output " << i);
            REQUIRE(mv::master_of(spots[i]) == expect[i]);
        }
        const mv::Fold fold = mv::Fold::of(spots);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kL)]) == 2);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kC)]) == 1);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kLs)]) == 3);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kLfe)]) == 1);
        REQUIRE(count(fold.connected) == 12);
    }
    {
        // 9.1.6: the wides go to the front pair, the top-centre height to the centre, the overhead one to a side.
        const auto spots = room_9_1_6();
        const mv::Fold fold = mv::Fold::of(spots);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kL)]) == 3);   // L, Lw, Vhl
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kC)]) == 2);   // C, Vhc
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kR)]) == 3);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kLs)]) == 4);  // Ls, Lrs, Lts, Ts
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kRs)]) == 3);
        REQUIRE(count(fold.master[static_cast<std::size_t>(M::kLfe)]) == 1);
        // Every output is in one master and one group.
        std::size_t masters = 0;
        std::size_t groups = 0;
        for (const mv::Members& m : fold.master) {
            masters += m.count;
        }
        for (const mv::Members& g : fold.group) {
            groups += g.count;
        }
        REQUIRE(masters == 16);
        REQUIRE(groups == 16);
    }
}

TEST_CASE("the grid groups the outputs in space", "[meters]") {
    using G = mv::Group;
    const auto count_of = [](const mv::Fold& f, G g) { return static_cast<std::size_t>(f.group[static_cast<std::size_t>(g)].count); };
    const mv::Fold f51 = mv::Fold::of(room_5_1());
    REQUIRE(count_of(f51, G::kFront) == 3);
    REQUIRE(count_of(f51, G::kSurround) == 2);
    REQUIRE(count_of(f51, G::kRear) == 0);
    REQUIRE(count_of(f51, G::kHeight) == 0);
    REQUIRE(count_of(f51, G::kLfe) == 1);
    const mv::Fold f714 = mv::Fold::of(room_7_1_4());
    REQUIRE(count_of(f714, G::kFront) == 3);
    REQUIRE(count_of(f714, G::kSurround) == 2);
    REQUIRE(count_of(f714, G::kRear) == 2);
    REQUIRE(count_of(f714, G::kHeight) == 4);
    REQUIRE(count_of(f714, G::kLfe) == 1);
    const mv::Fold f916 = mv::Fold::of(room_9_1_6());
    REQUIRE(count_of(f916, G::kFront) == 5);  // with the wides
    REQUIRE(count_of(f916, G::kHeight) == 6);
    // A 2.0 room is a front.
    const mv::Fold f20 = mv::Fold::of(room_2_0());
    REQUIRE(count_of(f20, G::kFront) == 2);
}

TEST_CASE("an output nothing is connected to is in no master and no group", "[meters]") {
    auto spots = room_5_1();
    spots[3].connected = false;
    const mv::Fold fold = mv::Fold::of(spots);
    REQUIRE(count(fold.connected) == 5);
    REQUIRE(count(fold.master[static_cast<std::size_t>(mv::Master::kLs)]) == 0);
    REQUIRE(count(fold.group[static_cast<std::size_t>(mv::Group::kSurround)]) == 1);
}

TEST_CASE("channels add as powers into a master", "[meters]") {
    const mv::Fold fold = mv::Fold::of(room_7_1_4());
    std::array<float, mv::kMaxOutputs> peak{};
    peak.fill(-20.0F);
    // L has two members, Ls three, C and the LFE one.
    REQUIRE(std::fabs(mv::power_sum_db(peak, fold.master[static_cast<std::size_t>(mv::Master::kL)].span()) - (-20.0F + 3.0103F)) < 1e-3F);
    REQUIRE(std::fabs(mv::power_sum_db(peak, fold.master[static_cast<std::size_t>(mv::Master::kLs)].span()) - (-20.0F + 4.7712F)) < 1e-3F);
    REQUIRE(mv::power_sum_db(peak, fold.master[static_cast<std::size_t>(mv::Master::kC)].span()) == -20.0F);
    // Silent members add nothing, and a master with none is silent.
    peak.fill(mv::kSilentDb);
    peak[0] = -10.0F;
    REQUIRE(mv::power_sum_db(peak, fold.master[static_cast<std::size_t>(mv::Master::kL)].span()) == -10.0F);
    REQUIRE(mv::power_sum_db(peak, fold.master[static_cast<std::size_t>(mv::Master::kC)].span()) == mv::kSilentDb);
    const mv::Members none;
    REQUIRE(mv::power_sum_db(peak, none.span()) == mv::kSilentDb);
}

TEST_CASE("the stereo downmix of a room: L and R as they are in a room of two, the folded room in a bigger one", "[meters]") {
    float l = 0.0F;
    float r = 0.0F;
    {
        const mv::Fold fold = mv::Fold::of(room_2_0());
        std::array<float, mv::kMaxOutputs> peak{};
        peak.fill(mv::kSilentDb);
        peak[0] = -10.0F;
        peak[1] = -30.0F;
        mv::downmix_db(peak, fold, l, r);
        REQUIRE(std::fabs(l - (-10.0F)) < 1e-3F);
        REQUIRE(std::fabs(r - (-30.0F)) < 1e-3F);
    }
    {
        // 5.1 with everything at -20: L + 0.5 C + 0.5 Ls is two powers, the LFE not at all.
        const mv::Fold fold = mv::Fold::of(room_5_1());
        std::array<float, mv::kMaxOutputs> peak{};
        peak.fill(-20.0F);
        mv::downmix_db(peak, fold, l, r);
        REQUIRE(std::fabs(l - (-20.0F + 3.0103F)) < 1e-3F);
        REQUIRE(std::fabs(r - (-20.0F + 3.0103F)) < 1e-3F);
        // Only the LFE playing: nothing in the downmix.
        peak.fill(mv::kSilentDb);
        peak[5] = -3.0F;
        mv::downmix_db(peak, fold, l, r);
        REQUIRE(l == mv::kSilentDb);
        REQUIRE(r == mv::kSilentDb);
        // Only the centre: the same in both, 3 dB down.
        peak[5] = mv::kSilentDb;
        peak[1] = -10.0F;
        mv::downmix_db(peak, fold, l, r);
        REQUIRE(std::fabs(l - (-13.0103F)) < 1e-3F);
        REQUIRE(std::fabs(r - (-13.0103F)) < 1e-3F);
    }
}

TEST_CASE("the zones, the range and the ballistics are in order", "[meters]") {
    REQUIRE(mv::kFloorDb < mv::kMeterAmberDb);
    REQUIRE(mv::kMeterAmberDb < mv::kMeterRedDb);
    REQUIRE(mv::kMeterRedDb < mv::kTopDb);
    REQUIRE(mv::kFloorDb < mv::kSpectrumAmberDb);
    REQUIRE(mv::kSpectrumAmberDb < mv::kSpectrumRedDb);
    REQUIRE(mv::kSpectrumRedDb < mv::kTopDb);
    REQUIRE(mv::kSpectrumFallDbPerS > mv::kSpectrumPeakFallDbPerS);
    REQUIRE(mv::kMeterFallDbPerS > mv::kMeterPeakFallDbPerS);
}
