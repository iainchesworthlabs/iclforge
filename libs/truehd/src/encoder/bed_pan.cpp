#include "iclforge/truehd/encoder/bed_pan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace iclforge::truehd {

namespace {

constexpr double kHalfPi = std::numbers::pi / 2.0;
constexpr int kSideY = 31;  // the side speakers' row; the rear row is kBedPanMaxY
constexpr int kCentreX = 31;
constexpr double kTrimYScale = 37.2;
constexpr double kTrimZStart = 0.2;
constexpr double kTrimZSpan = 0.8;

// Bed rows.
constexpr int kRowL = 0;
constexpr int kRowR = 1;
constexpr int kRowC = 2;
constexpr int kRowLs = 4;
constexpr int kRowRs = 5;
constexpr int kRowLrs = 6;
constexpr int kRowRrs = 7;

// The shares of the speakers of one row for a place x along it. The front row has the centre
// between its two corners; the side and rear rows have two speakers, at x 0 and x 62.
void front_row(int x, std::array<double, kBedPanRows>& shares) noexcept {
    if (x <= kCentreX) {
        const double u = kHalfPi * static_cast<double>(x) / static_cast<double>(kCentreX);
        shares[kRowL] = std::cos(u);
        shares[kRowC] = std::sin(u);
    } else {
        const double u =
            kHalfPi * static_cast<double>(x - kCentreX) / static_cast<double>(kBedPanMaxX - kCentreX);
        shares[kRowC] = std::cos(u);
        shares[kRowR] = std::sin(u);
    }
}

void two_speaker_row(int x, int left_row, int right_row,
                     std::array<double, kBedPanRows>& shares) noexcept {
    const double u = kHalfPi * static_cast<double>(x) / static_cast<double>(kBedPanMaxX);
    shares[static_cast<std::size_t>(left_row)] = std::cos(u);
    shares[static_cast<std::size_t>(right_row)] = std::sin(u);
}

}  // namespace

double bed_layout_trim_db(BedLayout layout) noexcept {
    switch (layout) {
        case BedLayout::k3_1:
            return -4.5;
        case BedLayout::k5_1:
            return -3.0;
        case BedLayout::k5_1_2:
            return -2.25;
        case BedLayout::k7_1:
        case BedLayout::k9_1:
        case BedLayout::k5_1_4:
            return -1.5;
        case BedLayout::k7_1_2:
        case BedLayout::k9_1_2:
            return -0.75;
        case BedLayout::k7_1_4:
        case BedLayout::k7_1_6:
        case BedLayout::k9_1_4:
        case BedLayout::k9_1_6:
            return 0.0;
    }
    return 0.0;
}

BedPosition bed_position_code(double x, double y, double z) noexcept {
    const auto code = [](double value, int top) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * top));
    };
    return {code(x, kBedPanMaxX), code(y, kBedPanMaxY), code(z, kBedPanMaxZ)};
}

double bed_trim_weight(int y_code, int z_code) noexcept {
    const double y = static_cast<double>(std::clamp(y_code, 0, kBedPanMaxY));
    const double z = static_cast<double>(std::clamp(z_code, 0, kBedPanMaxZ));
    const double height =
        std::max(0.0, (z / static_cast<double>(kBedPanMaxZ) - kTrimZStart) / kTrimZSpan);
    return std::min(1.0, y / kTrimYScale + height);
}

std::array<double, kBedPanRows> bed_pan_gains(BedPosition position, double trim_db) noexcept {
    const int x = std::clamp(position.x, 0, kBedPanMaxX);
    const int y = std::clamp(position.y, 0, kBedPanMaxY);
    std::array<double, kBedPanRows> front{};
    std::array<double, kBedPanRows> side{};
    std::array<double, kBedPanRows> rear{};
    front_row(x, front);
    two_speaker_row(x, kRowLs, kRowRs, side);
    two_speaker_row(x, kRowLrs, kRowRrs, rear);
    // The rows the place lies between and how much of each.
    double from_weight = 1.0;
    double to_weight = 0.0;
    const std::array<double, kBedPanRows>* from = &front;
    const std::array<double, kBedPanRows>* to = &side;
    if (y <= kSideY) {
        const double u = kHalfPi * static_cast<double>(y) / static_cast<double>(kSideY);
        from_weight = std::cos(u);
        to_weight = std::sin(u);
    } else {
        const double u =
            kHalfPi * static_cast<double>(y - kSideY) / static_cast<double>(kBedPanMaxY - kSideY);
        from = &side;
        to = &rear;
        from_weight = std::cos(u);
        to_weight = std::sin(u);
    }
    const double trim =
        std::pow(10.0, trim_db * bed_trim_weight(position.y, position.z) / 20.0);
    std::array<double, kBedPanRows> gains{};
    for (std::size_t row = 0; row < gains.size(); ++row) {
        gains[row] = trim * (from_weight * (*from)[row] + to_weight * (*to)[row]);
    }
    return gains;
}

std::array<double, kBedPanRows> bed_pan_gains(BedPosition position) noexcept {
    return bed_pan_gains(position, bed_layout_trim_db(BedLayout::k7_1));
}

std::size_t limit_bed_peaks(std::span<double> samples, int channels, int look_ahead) noexcept {
    if (channels <= 0 || look_ahead < 0) {
        return 0;
    }
    const auto width = static_cast<std::size_t>(channels);
    const std::size_t frames = samples.size() / width;
    const auto reach = static_cast<std::size_t>(look_ahead);
    // The gain each frame needs for itself, then the least of it over the frame and the look-ahead
    // after it (the gain has to be down by the time the peak arrives).
    std::vector<double> need(frames, 1.0);
    bool over = false;
    for (std::size_t i = 0; i < frames; ++i) {
        double peak = 0.0;
        for (std::size_t c = 0; c < width; ++c) {
            peak = std::max(peak, std::abs(samples[i * width + c]));
        }
        if (peak > 1.0) {
            need[i] = 1.0 / peak;
            over = true;
        }
    }
    if (!over) {
        return 0;
    }
    std::vector<double> least(frames, 1.0);
    for (std::size_t i = 0; i < frames; ++i) {
        double low = 1.0;
        const std::size_t last = std::min(frames - 1, i + reach);
        for (std::size_t j = i; j <= last; ++j) {
            low = std::min(low, need[j]);
        }
        least[i] = low;
    }
    // The gain is the mean of that over the look-ahead behind each frame, so that it falls in a
    // straight run as the peak nears and rises in one after it, and is never above what any frame
    // within reach needs. Before the block it is taken as the block's first.
    std::size_t reduced = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k <= reach; ++k) {
            sum += least[i >= k ? i - k : 0];
        }
        const double gain = sum / static_cast<double>(reach + 1);
        if (gain < 1.0) {
            ++reduced;
            for (std::size_t c = 0; c < width; ++c) {
                double& value = samples[i * width + c];
                value = std::clamp(value * gain, -1.0, 1.0);
            }
        }
    }
    return reduced;
}

}  // namespace iclforge::truehd
