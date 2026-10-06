#include "meta/dialogue.hpp"

#include <algorithm>
#include <cstddef>

namespace iclforge::ac4::detail {

double de_parameter(int index, bool cross_channel) noexcept {
    if (cross_channel) {
        // Table 210: -3.0 to 3.0 in steps of 0.1.
        return 0.1 * static_cast<double>(std::clamp(index, -30, 30));
    }
    // Table 209: 0 to 1.5 in steps of 0.1, then 1.75, 2.0, and 2.5 to 9.0 in
    // steps of 0.5.
    const int i = std::clamp(index, 0, 31);
    if (i <= 15) {
        return 0.1 * static_cast<double>(i);
    }
    if (i == 16) {
        return 1.75;
    }
    if (i == 17) {
        return 2.0;
    }
    return 2.5 + 0.5 * static_cast<double>(i - 18);
}

double de_mix_coefficient(int index) noexcept {
    return kDeMixCoefficients[static_cast<std::size_t>(std::clamp(index, 0, 31))];
}

}  // namespace iclforge::ac4::detail
