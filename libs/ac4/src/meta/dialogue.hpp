#pragma once

#include <array>

// Dialogue enhancement's tables both directions use (ETSI TS 103 190-1 V1.4.1 clause 5.7.8): the
// parameter bands (Table 173), the mixing coefficients (Table 172) and the parameters' values
// (Tables 209 and 210). The decoder applies them (decoder/pcm/de.hpp); the encoder quantises to
// them (encoder/frame/dialogue.hpp).

namespace iclforge::ac4::detail {

// Table 173: the first QMF subband of each of the eight parameter bands, and one past the last
// band's.
inline constexpr std::array<int, 9> kDeBandStart = {0, 1, 2, 4, 7, 11, 17, 27, 41};

// Table 172.
inline constexpr std::array<double, 32> kDeMixCoefficients = {
    0.0,   6.32e-3, 1e-2,   1.79e-2, 3.16e-2, 5.65e-2, 7.87e-2, 0.111,   0.156,   0.218, 0.303,
    0.37,  0.448,   0.533,  0.577,   0.622,   0.7071,  0.783,   0.846,   0.894,   0.929, 0.953,
    0.976, 0.9877,  0.9938, 0.9969,  0.9984,  0.9995,  0.99984, 0.99995, 0.99998, 1.0,
};

// A parameter's value: Table 209 for the channel-independent method, Table 210 for the
// cross-channel method. An index outside its table is taken to the table's nearest end.
[[nodiscard]] double de_parameter(int index, bool cross_channel) noexcept;

// Table 172's coefficient of an index, taken to the table's nearest end outside it.
[[nodiscard]] double de_mix_coefficient(int index) noexcept;

}  // namespace iclforge::ac4::detail
