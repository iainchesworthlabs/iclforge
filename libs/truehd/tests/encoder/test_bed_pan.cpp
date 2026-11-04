#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include "encoder/render_bed.hpp"
#include "iclforge/truehd/encoder/bed_pan.hpp"

// The measured render law of the 7.1 bed (iclforge/truehd/encoder/bed_pan.hpp, libs/truehd/notes/
// feature-truehd-measured-bed-render.md). [no-external-oracle]: the law is read off the owner's DEE
// controls; the tests hold the implementation to the law as written (the hand checks of the
// owner's report, the shapes it states, the three places measured by name) and to its own
// invariants.

namespace {

using namespace iclforge::truehd;
using Catch::Approx;

constexpr std::size_t kL = 0;
constexpr std::size_t kR = 1;
constexpr std::size_t kC = 2;
constexpr std::size_t kLfe = 3;
constexpr std::size_t kLs = 4;
constexpr std::size_t kRs = 5;
constexpr std::size_t kLrs = 6;
constexpr std::size_t kRrs = 7;
constexpr double kHalfPi = std::numbers::pi / 2.0;

double power_of(const std::array<double, kBedPanRows>& gains) {
    double power = 0.0;
    for (const double g : gains) {
        power += g * g;
    }
    return power;
}

}  // namespace

TEST_CASE("a metadata place becomes the 6 bit code, rounded and held in its range",
          "[truehd][encoder][atmos][render]") {
    CHECK(bed_position_code(0.0, 0.0, 0.0) == BedPosition{0, 0, 0});
    CHECK(bed_position_code(1.0, 1.0, 1.0) == BedPosition{62, 62, 15});
    CHECK(bed_position_code(0.5, 0.5, 0.5) == BedPosition{31, 31, 8});
    // to nearest: 0.2 * 62 = 12.4 and 0.21 * 62 = 13.02
    CHECK(bed_position_code(0.2, 0.21, 0.0).x == 12);
    CHECK(bed_position_code(0.2, 0.21, 0.0).y == 13);
    // 0.5 * 15 = 7.5 rounds away from zero, as std::lround does
    CHECK(bed_position_code(0.0, 0.0, 0.5).z == 8);
    // outside the room is held at its wall, and below the floor is the floor
    CHECK(bed_position_code(-0.3, 1.7, -1.0) == BedPosition{0, 62, 0});
    CHECK(bed_position_code(2.0, -2.0, 3.0) == BedPosition{62, 0, 15});
}

TEST_CASE("each speaker of the 7.1 bed has all of the signal at its own place",
          "[truehd][encoder][atmos][render]") {
    struct Speaker {
        int x;
        int y;
        std::size_t row;
    };
    constexpr std::array<Speaker, 7> speakers{{{0, 0, kL},
                                               {62, 0, kR},
                                               {31, 0, kC},
                                               {0, 31, kLs},
                                               {62, 31, kRs},
                                               {0, 62, kLrs},
                                               {62, 62, kRrs}}};
    for (const Speaker& s : speakers) {
        CAPTURE(s.x, s.y);
        const auto gains = bed_pan_gains({s.x, s.y, 0}, 0.0);
        for (std::size_t row = 0; row < gains.size(); ++row) {
            CHECK(gains[row] == Approx(row == s.row ? 1.0 : 0.0).margin(1e-12));
        }
    }
}

TEST_CASE("between two speakers of a row the shares are the cosine and sine of the way along",
          "[truehd][encoder][atmos][render]") {
    // the front row: L at 0, C at 31, R at 62
    const double u = kHalfPi * 15.0 / 31.0;
    auto gains = bed_pan_gains({15, 0, 0}, 0.0);
    CHECK(gains[kL] == Approx(std::cos(u)).margin(1e-12));
    CHECK(gains[kC] == Approx(std::sin(u)).margin(1e-12));
    CHECK(gains[kR] == 0.0);
    gains = bed_pan_gains({46, 0, 0}, 0.0);
    CHECK(gains[kL] == 0.0);
    CHECK(gains[kC] == Approx(std::cos(u)).margin(1e-12));
    CHECK(gains[kR] == Approx(std::sin(u)).margin(1e-12));
    // the side and rear rows have only their two corners, 62 apart: halfway is 31
    const double v = kHalfPi * 20.0 / 62.0;
    gains = bed_pan_gains({20, 31, 0}, 0.0);
    CHECK(gains[kLs] == Approx(std::cos(v)).margin(1e-12));
    CHECK(gains[kRs] == Approx(std::sin(v)).margin(1e-12));
    gains = bed_pan_gains({20, 62, 0}, 0.0);
    CHECK(gains[kLrs] == Approx(std::cos(v)).margin(1e-12));
    CHECK(gains[kRrs] == Approx(std::sin(v)).margin(1e-12));
    // the owner's hand check: x 31, y 31 is 0.7071 on each side speaker
    gains = bed_pan_gains({31, 31, 0}, 0.0);
    CHECK(gains[kLs] == Approx(0.70711).margin(1e-5));
    CHECK(gains[kRs] == Approx(0.70711).margin(1e-5));
    gains = bed_pan_gains({31, 62, 0}, 0.0);
    CHECK(gains[kLrs] == Approx(0.70711).margin(1e-5));
    CHECK(gains[kRrs] == Approx(0.70711).margin(1e-5));
}

TEST_CASE("a place between two rows is the outer product of the two splits",
          "[truehd][encoder][atmos][render]") {
    // the owner's hand check: x 0, y 16 is 0.689 on L and 0.725 on Ls
    auto gains = bed_pan_gains({0, 16, 0}, 0.0);
    CHECK(gains[kL] == Approx(0.689).margin(1e-3));
    CHECK(gains[kLs] == Approx(0.725).margin(1e-3));
    const double rows = kHalfPi * 16.0 / 31.0;
    // x 31: the front row is all C, the side row halves between Ls and Rs
    gains = bed_pan_gains({31, 16, 0}, 0.0);
    CHECK(gains[kC] == Approx(std::cos(rows)).margin(1e-12));
    CHECK(gains[kLs] == Approx(std::sin(rows) * std::numbers::sqrt2 / 2.0).margin(1e-12));
    CHECK(gains[kRs] == Approx(std::sin(rows) * std::numbers::sqrt2 / 2.0).margin(1e-12));
    CHECK(gains[kL] == Approx(0.0).margin(1e-12));
    CHECK(gains[kLrs] == 0.0);
    // between the side and rear rows
    const double back = kHalfPi * 10.0 / 31.0;
    gains = bed_pan_gains({0, 41, 0}, 0.0);
    CHECK(gains[kLs] == Approx(std::cos(back)).margin(1e-12));
    CHECK(gains[kLrs] == Approx(std::sin(back)).margin(1e-12));
    CHECK(gains[kL] == 0.0);
}

TEST_CASE("without a trim the squares of the shares add up to one everywhere and the LFE has none",
          "[truehd][encoder][atmos][render]") {
    int places = 0;
    for (int x = 0; x <= kBedPanMaxX; ++x) {
        for (int y = 0; y <= kBedPanMaxY; ++y) {
            const auto gains = bed_pan_gains({x, y, 0}, 0.0);
            CHECK(power_of(gains) == Approx(1.0).margin(1e-12));
            CHECK(gains[kLfe] == 0.0);
            for (const double g : gains) {
                CHECK(g >= 0.0);
            }
            ++places;
        }
    }
    CHECK(places == 63 * 63);
}

TEST_CASE("the trim is T times w, w the distance from the front wall and the height",
          "[truehd][encoder][atmos][render]") {
    CHECK(bed_trim_weight(0, 0) == 0.0);
    CHECK(bed_trim_weight(18, 0) == Approx(18.0 / 37.2).margin(1e-12));
    CHECK(bed_trim_weight(37, 0) == Approx(37.0 / 37.2).margin(1e-12));
    CHECK(bed_trim_weight(38, 0) == 1.0);  // held at 1 from y 37.2 on
    CHECK(bed_trim_weight(62, 0) == 1.0);
    // height: nothing up to a fifth of the way, then to 1 at the ceiling
    CHECK(bed_trim_weight(0, 3) == 0.0);
    CHECK(bed_trim_weight(0, 9) == Approx(0.5).margin(1e-12));
    CHECK(bed_trim_weight(0, 15) == Approx(1.0).margin(1e-12));
    CHECK(bed_trim_weight(5, 9) == Approx(5.0 / 37.2 + 0.5).margin(1e-12));
    CHECK(bed_trim_weight(10, 12) == 1.0);  // 10 / 37.2 + 0.75 is held at 1
    CHECK(bed_trim_weight(30, 15) == 1.0);
    // a code outside its range is held in it
    CHECK(bed_trim_weight(-5, -5) == 0.0);
    CHECK(bed_trim_weight(100, 100) == 1.0);
}

TEST_CASE("the trim of each layout is the measured one", "[truehd][encoder][atmos][render]") {
    CHECK(bed_layout_trim_db(BedLayout::k3_1) == -4.5);
    CHECK(bed_layout_trim_db(BedLayout::k5_1) == -3.0);
    CHECK(bed_layout_trim_db(BedLayout::k5_1_2) == -2.25);
    CHECK(bed_layout_trim_db(BedLayout::k7_1) == -1.5);
    CHECK(bed_layout_trim_db(BedLayout::k9_1) == -1.5);
    CHECK(bed_layout_trim_db(BedLayout::k5_1_4) == -1.5);
    CHECK(bed_layout_trim_db(BedLayout::k7_1_2) == -0.75);
    CHECK(bed_layout_trim_db(BedLayout::k9_1_2) == -0.75);
    CHECK(bed_layout_trim_db(BedLayout::k7_1_4) == 0.0);
    CHECK(bed_layout_trim_db(BedLayout::k7_1_6) == 0.0);
    CHECK(bed_layout_trim_db(BedLayout::k9_1_4) == 0.0);
    CHECK(bed_layout_trim_db(BedLayout::k9_1_6) == 0.0);
}

TEST_CASE("the squares of the trimmed shares add up to 10^(T w / 10), and height moves only the trim",
          "[truehd][encoder][atmos][render]") {
    for (const BedPosition place : {BedPosition{0, 0, 0}, BedPosition{62, 5, 0}, BedPosition{20, 17, 4},
                                    BedPosition{40, 50, 9}, BedPosition{31, 31, 15},
                                    BedPosition{5, 62, 15}}) {
        CAPTURE(place.x, place.y, place.z);
        const double trim_db = -1.5;
        const double w = bed_trim_weight(place.y, place.z);
        const auto gains = bed_pan_gains(place, trim_db);
        CHECK(power_of(gains) == Approx(std::pow(10.0, trim_db * w / 10.0)).margin(1e-12));
        // the default is the 7.1 trim
        CHECK(bed_pan_gains(place) == gains);
        // the same place on the floor has the same shares, less the trim
        const auto floor_gains = bed_pan_gains({place.x, place.y, 0}, 0.0);
        const auto raised_untrimmed = bed_pan_gains(place, 0.0);
        CHECK(raised_untrimmed == floor_gains);
    }
    // a trim of 0 dB leaves the place alone, whatever w is
    CHECK(power_of(bed_pan_gains({10, 60, 15}, 0.0)) == Approx(1.0).margin(1e-12));
    // a place on the left wall at y 62 is 1.5 dB down (w is 1), one at y 31 is 1.5 * 31 / 37.2 down
    CHECK(bed_pan_gains({0, 62, 0})[kLrs] == Approx(std::pow(10.0, -1.5 / 20.0)).margin(1e-12));
    CHECK(bed_pan_gains({0, 31, 0})[kLs] ==
          Approx(std::pow(10.0, -1.5 * (31.0 / 37.2) / 20.0)).margin(1e-12));
}

TEST_CASE("the law gives the shares the owner's DEE controls hold", "[truehd][encoder][atmos][render]") {
    // ctl_b, a lone object held at x 62, y 5: the bed's R row held 0.946 of it and Rs 0.245 (the
    // measured gains of the 7.1 presentation, to 0.002)
    const auto gains = bed_pan_gains(bed_position_code(1.0, 5.0 / 62.0, 0.0));
    CHECK(gains[kR] == Approx(0.946).margin(2e-3));
    CHECK(gains[kRs] == Approx(0.245).margin(2e-3));
    for (std::size_t row : {kL, kC, kLfe, kLs, kLrs, kRrs}) {
        CHECK(gains[row] == Approx(0.0).margin(1e-12));
    }
}

TEST_CASE("the limiter leaves a signal under the ceiling exactly as it was",
          "[truehd][encoder][atmos][render]") {
    std::vector<double> signal(8 * 200);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = 0.99 * std::sin(0.01 * static_cast<double>(i));
    }
    signal[301] = 1.0;  // at the ceiling is not over it
    const std::vector<double> before = signal;
    CHECK(limit_bed_peaks(signal, 8) == 0);
    CHECK(signal == before);
}

TEST_CASE("the limiter brings every sample under the ceiling, ahead of the peak, with one gain for all channels",
          "[truehd][encoder][atmos][render]") {
    constexpr int kChannels = 8;
    constexpr std::size_t kFrames = 400;
    constexpr std::size_t kPeak = 200;
    std::vector<double> signal(kFrames * kChannels, 0.0);
    for (std::size_t i = 0; i < kFrames; ++i) {
        signal[i * kChannels + 0] = 0.5;  // a steady channel that is not itself loud
        signal[i * kChannels + 4] = -0.25;
    }
    signal[kPeak * kChannels + 2] = 2.0;  // one sample at twice the ceiling, on one channel
    const std::vector<double> before = signal;
    const std::size_t reduced = limit_bed_peaks(signal, kChannels);
    CHECK(reduced > 0);
    for (const double value : signal) {
        CHECK(std::abs(value) <= 1.0);
    }
    // the peak itself comes out at the ceiling: its gain is the one it needs, 1 / 2
    CHECK(signal[kPeak * kChannels + 2] == Approx(1.0).margin(1e-12));
    // one gain for the frame: the steady channels move together
    std::size_t reduced_frames = 0;
    double previous_gain = 1.0;
    for (std::size_t i = 0; i < kFrames; ++i) {
        const double gain = signal[i * kChannels + 0] / 0.5;
        CHECK(signal[i * kChannels + 4] / -0.25 == Approx(gain).margin(1e-12));
        CHECK(gain <= 1.0 + 1e-12);
        CHECK(gain > 0.0);
        if (gain < 1.0) {
            ++reduced_frames;
        }
        if (i <= kPeak) {
            // falling in a straight run as the peak nears, never rising before it
            CHECK(gain <= previous_gain + 1e-12);
        }
        previous_gain = gain;
    }
    CHECK(reduced_frames == reduced);
    // the gain comes down in a straight run over the 32 samples before the peak, 1 less
    // 0.5 * (k + 1) / 33 at the k-th of them, and goes back up in a straight run after it
    for (std::size_t k = 0; k <= 32; ++k) {
        CAPTURE(k);
        CHECK(signal[(kPeak - 32 + k) * kChannels + 0] / 0.5 ==
              Approx(1.0 - 0.5 * static_cast<double>(k + 1) / 33.0).margin(1e-12));
    }
    for (std::size_t m = 1; m <= 32; ++m) {
        CAPTURE(m);
        CHECK(signal[(kPeak + m) * kChannels + 0] / 0.5 ==
              Approx(0.5 + 0.5 * static_cast<double>(m) / 33.0).margin(1e-12));
    }
    // it starts 32 samples before the peak (the look-ahead) and not earlier, and is back at 1 once
    // the peak is a look-ahead behind
    CHECK(signal[(kPeak - 33) * kChannels + 0] == 0.5);
    CHECK(signal[(kPeak - 31) * kChannels + 0] < 0.5);
    CHECK(signal[(kPeak + 40) * kChannels + 0] == 0.5);
    // not delayed: where nothing is limited a sample is the input's
    CHECK(signal[10 * kChannels + 4] == before[10 * kChannels + 4]);
}

TEST_CASE("the limiter holds a peak in the first frames and ignores a block it cannot read",
          "[truehd][encoder][atmos][render]") {
    std::vector<double> signal(2 * 40, 0.1);
    signal[0] = 3.0;
    CHECK(limit_bed_peaks(signal, 2) > 0);
    for (const double value : signal) {
        CHECK(std::abs(value) <= 1.0);
    }
    std::vector<double> loud(10, 5.0);
    CHECK(limit_bed_peaks(loud, 0) == 0);
    CHECK(loud == std::vector<double>(10, 5.0));
    CHECK(limit_bed_peaks(loud, 2, -1) == 0);
    std::vector<double> none;
    CHECK(limit_bed_peaks(none, 8) == 0);
}
