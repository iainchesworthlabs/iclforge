#pragma once

#include <array>
#include <cstdint>

#include "bit_reader.hpp"
#include "huffman.hpp"
#include "syntax/context.hpp"

// Advanced joint channel coding (A-JCC) syntax: ETSI TS 103 190-2 V1.3.1
// clause 6.2.6, semantics 6.3.7, for the immersive channel element of the
// 7.X.4 channel modes, which pass b_5fronts 0 (6.2.3.1), and the 9.X.4 modes,
// which pass 1.
//
// Nothing is dequantised: parameter values stay Huffman codebook indices,
// before cb_off, with the diff_type that says how clause 5.6.3.2 decodes them,
// as A-CPL's do (syntax/acpl.hpp).

namespace iclforge::ac4::detail {

inline constexpr int kAjccMaxParamBands = 15;  // Part 2 Table 83
inline constexpr int kAjccMaxParamSets = 2;    // Part 2 Table 90
// The framings and parameters ajcc_data() sends at most, with b_5fronts: four
// ajcc_framing_data() (lf, rf, lb, rb), eight dry and twelve wet parameters.
inline constexpr std::size_t kAjccMaxFramings = 4;
inline constexpr std::size_t kAjccMaxDry = 8;
inline constexpr std::size_t kAjccMaxWet = 12;

// The arguments of get_ajcc_hcb() (Part 2 Pseudocode 29).
enum class AjccDataType : std::uint8_t { kAlpha, kBeta, kDry, kWet };
enum class AjccHcbType : std::uint8_t { kF0, kDf, kDt };

// ajcc_framing_data(), 6.2.6.2.
struct AjccFraming {
    std::uint8_t interpolation_type = 0;  // Table 89: 0 smooth, 1 steep
    std::uint8_t num_param_sets_code = 0;
    std::uint8_t num_param_sets = 1;      // Table 90: ajcc_num_param_sets_code + 1
    // Sent for steep interpolation only, one per parameter set.
    std::array<std::uint8_t, kAjccMaxParamSets> param_timeslot{};
};

// One ajcc_huff_data(), 6.2.6.4.
struct AjccParamSet {
    // 0 DIFF_FREQ: huff_index[0] from the F0 codebook, the bands above it from
    // DF. 1 DIFF_TIME: every band from DT. Always 0 under b_no_dt.
    std::uint8_t diff_type = 0;
    std::array<std::uint16_t, kAjccMaxParamBands> huff_index{};  // codebook indices, before cb_off
};

// One ajced(), 6.2.6.3: a parameter set per parameter set of its framing.
struct AjccParams {
    AjccDataType data_type = AjccDataType::kDry;
    std::uint8_t quant_mode = 0;  // 0 fine, 1 coarse (Tables 87 and 88)
    std::uint8_t num_param_sets = 1;
    std::array<AjccParamSet, kAjccMaxParamSets> sets{};
};

// ajcc_data(b_5fronts), 6.2.6.1.
//
// Without b_5fronts: ajcc_core_mode, ajcc_qm_ab and ajcc_qm_dw, the framing of
// the left and right modules, and alpha1, alpha2, beta1, beta2, dry1 to dry4
// and wet1 to wet6.
// With it: ajcc_qm_f and ajcc_qm_b, the framing of the left front, right
// front, left back and right back modules, and dry1f to dry4f, dry1b to dry4b,
// wet1f to wet6f and wet1b to wet6b, which are no alpha or beta.
struct AjccData {
    bool b_5fronts = false;
    bool b_no_dt = false;
    std::uint8_t num_param_bands_id = 0;
    std::uint8_t num_bands = 15;  // Table 83
    std::uint8_t core_mode = 0;   // Table 84: 0 the core is L R C Ls Rs, 1 L R C Tfl Tfr
    std::uint8_t qm_ab = 0;       // Table 87
    std::uint8_t qm_dw = 0;       // Table 88
    std::uint8_t qm_f = 0;        // Table 85, with b_5fronts
    std::uint8_t qm_b = 0;        // Table 86, with b_5fronts
    // ajcc_nps_l's and ajcc_nps_r's framing data, in that order; with b_5fronts
    // ajcc_nps_lf's, _rf's, _lb's and _rb's.
    std::array<AjccFraming, kAjccMaxFramings> framing{};
    std::array<AjccParams, 2> alpha{};  // ajcc_alpha1 (L), ajcc_alpha2 (R)
    std::array<AjccParams, 2> beta{};   // ajcc_beta1, ajcc_beta2
    // ajcc_dry1 to ajcc_dry4: 1 and 2 L's, 3 and 4 R's; with b_5fronts dry1f to
    // dry4f (1 and 2 lf's, 3 and 4 rf's), then dry1b to dry4b (lb's, rb's).
    std::array<AjccParams, kAjccMaxDry> dry{};
    // ajcc_wet1 to ajcc_wet6: 1 to 3 L's, 4 to 6 R's; with b_5fronts wet1f to
    // wet6f (1 to 3 lf's, 4 to 6 rf's), then wet1b to wet6b (lb's, rb's).
    std::array<AjccParams, kAjccMaxWet> wet{};
};

[[nodiscard]] ParseResult parse_ajcc_data(BitReader& r, bool b_5fronts, AjccData& out);

// get_ajcc_hcb(), Pseudocode 29: alpha and beta take A-CPL's codebooks (Part 1
// Annex A.3), dry and wet A-JCC's (Part 2 Annex A.1.2). quant_mode 0 is FINE,
// 1 COARSE.
[[nodiscard]] const Codebook& ajcc_codebook(AjccDataType data_type, int quant_mode,
                                            AjccHcbType hcb_type) noexcept;

}  // namespace iclforge::ac4::detail
