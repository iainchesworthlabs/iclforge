// The downmix (libs/ac4/src/decoder/pcm/downmix.hpp, ETSI TS 103 190-1 V1.4.1 clause
// 6.2.17): Tables 149 and 149a's gains, every matrix of the cascade - 7.X to
// 5.X by Table 219, 5.X and 3.0 to Lo/Ro, Lt/Rt and its Pro Logic II form by
// Tables 217 and 218, with the LFE and the loudness corrections, and to mono -
// against its formula with the stream's gains; the gains persisting between
// the frames that send them; an alternative presentation's target loudness
// correction by the device category (Part 2 clause 4.8.5.4, Tables 17 and 67)
// and the real-time loudness correction (4.8.5.5), which are the frame's own;
// and through iclforge::ac4::Decoder, DEE's 5.1 tones, one per channel,
// measured in each channel of the downmix to 0.01 dB.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "units.hpp"

#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "decoder/pcm/downmix.hpp"

namespace {

namespace detail = iclforge::ac4::detail;
using S = iclforge::ac4::Speaker;
using Row = std::vector<double>;
using QmfMatrix = detail::QmfMatrix;
using QmfValue = detail::QmfValue;
using Real = detail::Real;

double db(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

// A gain in dB (db() above) is rarely an exact binary value, so a QmfValue
// scaled by one holds it within a tolerance scaled to Real's own epsilon,
// not to double's exactness.
const double kTolerance = 1e4 * ac4_units::relative_epsilon();

constexpr std::array<S, 6> kFiveOne = {S::kLeft, S::kRight,        S::kCentre,
                                       S::kLfe,  S::kLeftSurround, S::kRightSurround};

// A stream's stereo downmix coefficients.
detail::StereoDmxCoeff coefficients(int loro_c, int loro_s, int ltrt_c, int ltrt_s,
                                    std::optional<int> lfe, int preferred) {
    detail::StereoDmxCoeff c;
    c.loro_centre_mixgain = loro_c;
    c.loro_surround_mixgain = loro_s;
    c.b_ltrt_mixinfo = true;
    c.ltrt_centre_mixgain = ltrt_c;
    c.ltrt_surround_mixgain = ltrt_s;
    c.lfe_mixgain = lfe;
    c.preferred_dmx_method = preferred;
    return c;
}

// The matrix a stage takes after one frame of `values`.
std::vector<Row> matrix_for(std::span<const S> speakers, bool add_ch_base,
                            iclforge::ac4::DownmixTarget target,
                            const detail::DownmixValues& values, bool mix_lfe = true) {
    detail::DownmixStage stage;
    stage.configure(speakers, add_ch_base, target, mix_lfe);
    std::vector<std::vector<QmfValue>> channels(speakers.size(), std::vector<QmfValue>(64));
    std::vector<QmfMatrix> in;
    for (auto& channel : channels) {
        in.push_back(channel);
    }
    std::vector<std::vector<QmfValue>> out;
    stage.process(values, in, out);
    return stage.matrix();
}

void check_matrix(const std::vector<Row>& got, const std::vector<Row>& expected) {
    REQUIRE(got.size() == expected.size());
    for (std::size_t o = 0; o < got.size(); ++o) {
        REQUIRE(got[o].size() == expected[o].size());
        for (std::size_t c = 0; c < got[o].size(); ++c) {
            CAPTURE(o, c, got[o][c], expected[o][c]);
            CHECK(std::abs(got[o][c] - expected[o][c]) < 1e-12);
        }
    }
}

std::vector<std::byte> read_stream(const std::string& leg) {
    const std::filesystem::path path =
        std::filesystem::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

struct Decoded {
    std::vector<S> speakers;
    std::vector<std::vector<float>> channels;
};

Decoded decode_all(std::span<const std::byte> stream, iclforge::ac4::DownmixTarget target) {
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    iclforge::ac4::DecoderConfig config;
    config.output.downmix = target;
    iclforge::ac4::Decoder decoder(config);
    Decoded out;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        out.speakers = pcm.speakers;
        out.channels.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
    }
    return out;
}

// The amplitude of `hz` in x's middle, by a projection onto it.
double amplitude(const std::vector<float>& x, double hz) {
    const std::size_t from = 24000;
    const std::size_t to = x.size() - 24000;
    std::complex<double> sum{};
    for (std::size_t n = from; n < to; ++n) {
        sum += static_cast<double>(x[n]) *
               std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0);
    }
    return 2.0 * std::abs(sum) / static_cast<double>(to - from);
}

}  // namespace

TEST_CASE("Tables 149 and 149a give the downmix's mix gains", "[ac4][decoder][downmix]") {
    constexpr std::array<double, 7> kCentre = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
    for (int code = 0; code < 7; ++code) {
        CHECK(std::abs(detail::centre_mix_gain(code) -
                       db(kCentre[static_cast<std::size_t>(code)])) < 1e-12);
    }
    CHECK(detail::centre_mix_gain(7) == 0.0);
    constexpr std::array<double, 5> kSurround = {0.0, -1.5, -3.0, -4.5, -6.0};
    for (int code = 2; code < 7; ++code) {
        CHECK(std::abs(detail::surround_mix_gain(code) -
                       db(kSurround[static_cast<std::size_t>(code - 2)])) < 1e-12);
    }
    CHECK(detail::surround_mix_gain(7) == 0.0);
    // The reserved surround codes, and no code at all, are -3 dB.
    CHECK(std::abs(detail::surround_mix_gain(0) - db(-3.0)) < 1e-12);
    CHECK(std::abs(detail::surround_mix_gain(1) - db(-3.0)) < 1e-12);
    CHECK(std::abs(detail::centre_mix_gain(-1) - db(-3.0)) < 1e-12);
}

TEST_CASE("5.1's downmixes are Table 218's with the stream's gains, LFE and loudness corrections",
          "[ac4][decoder][downmix]") {
    // Lo/Ro: C at 0 dB and the surrounds at -6; Lt/Rt: C at -6 and the
    // surrounds at -4.5; the LFE at 5.5 - 10 = -4.5 dB; Lo/Ro's loudness
    // correction (15 - 21) / 2 = -3 dB2 and Lt/Rt's (15 - 13) / 2 = 1 dB2.
    detail::DownmixValues values;
    values.coeff = coefficients(2, 6, 6, 5, 10, 1);
    values.loro_loud_corr = 21;
    values.ltrt_loud_corr = 13;
    const double lfe = db(-4.5);
    const double loro = std::exp2(-3.0 / 6.0);
    const double ltrt = std::exp2(1.0 / 6.0);
    const auto scaled = [](Row r, double k) {
        for (double& v : r) {
            v *= k;
        }
        return r;
    };
    // Columns L R C LFE Ls Rs.
    const Row lo = scaled({1, 0, db(0), lfe, db(-6), 0}, loro);
    const Row ro = scaled({0, 1, db(0), lfe, 0, db(-6)}, loro);
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, values),
                 {lo, ro});
    // The stream prefers Lo/Ro, so stereo is Lo/Ro, and mono their sum.
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kStereo, values),
                 {lo, ro});
    Row sum(6);
    for (std::size_t c = 0; c < 6; ++c) {
        sum[c] = lo[c] + ro[c];
    }
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kMono, values), {sum});
    // Lt/Rt: the surrounds' sum out of phase in Lt.
    const double s = db(-4.5);
    check_matrix(
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLtRt, values),
        {scaled({1, 0, db(-6), lfe, -s, -s}, ltrt), scaled({0, 1, db(-6), lfe, s, s}, ltrt)});
    // Where the stream prefers the Pro Logic II form: +1.8 dB near, -3.2 far.
    values.coeff->preferred_dmx_method = 3;
    const double near = s * db(1.8);
    const double far = s * db(-3.2);
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLtRt, values),
                 {scaled({1, 0, db(-6), lfe, -near, -far}, ltrt),
                  scaled({0, 1, db(-6), lfe, far, near}, ltrt)});
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kStereo, values),
                 {scaled({1, 0, db(-6), lfe, -near, -far}, ltrt),
                  scaled({0, 1, db(-6), lfe, far, near}, ltrt)});
    // Without the LFE, and without b_ltrt_mixinfo, which gives Lt/Rt the Lo/Ro
    // gains.
    values.coeff->preferred_dmx_method = 2;
    values.coeff->b_ltrt_mixinfo = false;
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLtRt, values, false),
                 {scaled({1, 0, db(0), 0, -db(-6), -db(-6)}, ltrt),
                  scaled({0, 1, db(0), 0, db(-6), db(-6)}, ltrt)});
    // Nothing sent: -3 dB mix gains, no LFE, no correction.
    check_matrix(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, {}),
                 {{1, 0, db(-3), 0, db(-3), 0}, {0, 1, db(-3), 0, 0, db(-3)}});
}

TEST_CASE("7.X folds to 5.X by Table 219 for each additional pair and add_ch_base",
          "[ac4][decoder][downmix]") {
    constexpr double k = 0.707;
    detail::DownmixValues values;
    // Columns L R C LFE Ls Rs and the pair.
    const std::array<S, 8> back = {S::kLeft,     S::kRight,        S::kCentre,
                                   S::kLfe,      S::kLeftSurround, S::kRightSurround,
                                   S::kLeftBack, S::kRightBack};
    check_matrix(matrix_for(back, false, iclforge::ac4::DownmixTarget::k5X, values),
                 {{1, 0, 0, 0, 0, 0, 0, 0},
                  {0, 1, 0, 0, 0, 0, 0, 0},
                  {0, 0, 1, 0, 0, 0, 0, 0},
                  {0, 0, 0, 1, 0, 0, 0, 0},
                  {0, 0, 0, 0, k, 0, k, 0},
                  {0, 0, 0, 0, 0, k, 0, k}});
    for (const auto& [left, right] :
         {std::pair{S::kLeftWide, S::kRightWide}, std::pair{S::kTopFrontLeft, S::kTopFrontRight}}) {
        const std::array<S, 8> seven = {S::kLeft,         S::kRight,         S::kCentre, S::kLfe,
                                        S::kLeftSurround, S::kRightSurround, left,       right};
        // add_ch_base 0: the pair into L and R; 1: into the surrounds.
        check_matrix(matrix_for(seven, false, iclforge::ac4::DownmixTarget::k5X, values),
                     {{1, 0, 0, 0, 0, 0, k, 0},
                      {0, 1, 0, 0, 0, 0, 0, k},
                      {0, 0, 1, 0, 0, 0, 0, 0},
                      {0, 0, 0, 1, 0, 0, 0, 0},
                      {0, 0, 0, 0, 1, 0, 0, 0},
                      {0, 0, 0, 0, 0, 1, 0, 0}});
        check_matrix(matrix_for(seven, true, iclforge::ac4::DownmixTarget::k5X, values),
                     {{1, 0, 0, 0, 0, 0, 0, 0},
                      {0, 1, 0, 0, 0, 0, 0, 0},
                      {0, 0, 1, 0, 0, 0, 0, 0},
                      {0, 0, 0, 1, 0, 0, 0, 0},
                      {0, 0, 0, 0, k, 0, k, 0},
                      {0, 0, 0, 0, 0, k, 0, k}});
    }
    // And on to Lo/Ro through 5.X: the back pair at 0.707 x -3 dB.
    const double s = db(-3.0);
    check_matrix(matrix_for(back, false, iclforge::ac4::DownmixTarget::kLoRo, values),
                 {{1, 0, s, 0, k * s, 0, k * s, 0}, {0, 1, s, 0, 0, k * s, 0, k * s}});
    // 5.X is left as it is by a 5.X target.
    CHECK(matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::k5X, values).size() == 6);
}

TEST_CASE("3.0, stereo and mono take Table 217, the sum and the 0.707 upmix",
          "[ac4][decoder][downmix]") {
    detail::DownmixValues values;
    values.coeff = coefficients(5, 4, 3, 4, std::nullopt, 1);
    const std::array<S, 3> three = {S::kLeft, S::kRight, S::kCentre};
    check_matrix(matrix_for(three, false, iclforge::ac4::DownmixTarget::kLoRo, values),
                 {{1, 0, db(-4.5)}, {0, 1, db(-4.5)}});
    check_matrix(matrix_for(three, false, iclforge::ac4::DownmixTarget::kLtRt, values),
                 {{1, 0, db(-1.5)}, {0, 1, db(-1.5)}});
    const std::array<S, 2> stereo = {S::kLeft, S::kRight};
    check_matrix(matrix_for(stereo, false, iclforge::ac4::DownmixTarget::kMono, values), {{1, 1}});
    const std::array<S, 1> mono = {S::kCentre};
    check_matrix(matrix_for(mono, false, iclforge::ac4::DownmixTarget::kStereo, values),
                 {{0.707}, {0.707}});
}

TEST_CASE("the downmix's gains hold from the frame that sends them until another does",
          "[ac4][decoder][downmix]") {
    detail::DownmixStage stage;
    stage.configure(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, true);
    std::vector<std::vector<QmfValue>> channels(6, std::vector<QmfValue>(64, QmfValue{Real{1}, Real{0}}));
    std::vector<QmfMatrix> in;
    for (auto& channel : channels) {
        in.push_back(channel);
    }
    std::vector<std::vector<QmfValue>> out;
    detail::DownmixValues sent;
    sent.coeff = coefficients(6, 7, 6, 7, std::nullopt, 1);  // C at -6 dB, no surrounds
    stage.process(sent, in, out);
    const auto first = static_cast<double>(out[0][0].real());
    CHECK(std::abs(first - (1.0 + db(-6.0))) < kTolerance);
    // A frame that sends nothing keeps them.
    stage.process({}, in, out);
    CHECK(static_cast<double>(out[0][0].real()) == first);
    // A reset goes back to -3 dB.
    stage.reset();
    stage.process({}, in, out);
    CHECK(std::abs(static_cast<double>(out[0][0].real()) - (1.0 + 2.0 * db(-3.0))) < kTolerance);
}

namespace {

constexpr std::array<S, 10> kFiveOneFour = {
    S::kLeft,          S::kRight,        S::kCentre,        S::kLfe,         S::kLeftSurround,
    S::kRightSurround, S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight};
constexpr std::array<S, 2> kStereoOut = {S::kLeft, S::kRight};
constexpr std::array<S, 1> kMonoOut = {S::kCentre};

// Table 67's four bits as target_device_category reads them: index 0 (1D) first, so the high bit.
constexpr int k1D = 0b1000;
constexpr int k2D = 0b0100;
constexpr int k3D = 0b0010;
constexpr int kPortable = 0b0001;

// A target of an alternative presentation: the categories it names and its loud_corr_target.
detail::PresentationTarget target(int categories, std::optional<int> loud_corr_target) {
    detail::PresentationTarget t;
    t.target_device_category = categories;
    t.loud_corr_target = loud_corr_target;
    return t;
}

// The values an alternative presentation's substream gives a frame.
detail::DownmixValues alternative(std::vector<detail::PresentationTarget> targets,
                                  std::optional<int> rtll_comp = std::nullopt) {
    detail::PresentationSubstream presentation;
    presentation.targets = std::move(targets);
    if (rtll_comp) {
        detail::FurtherLoudnessInfo loudness;
        loudness.rtll_comp = rtll_comp;
        presentation.further_loudness_info = loudness;
    }
    return detail::downmix_values(&presentation, detail::Metadata{});
}

// The gain the first channel of a stage that takes `values` gives it, where the channels come out
// as coded: the matrix's first diagonal entry.
double gain_of(std::span<const S> speakers, const detail::DownmixValues& values) {
    const std::vector<Row> matrix =
        matrix_for(speakers, false, iclforge::ac4::DownmixTarget::kAsCoded, values);
    REQUIRE_FALSE(matrix.empty());
    return matrix[0][0];
}

// 2^(x / 6): clause 4.8.5.4's and 4.8.5.3's factor for x in dB2.
double dB2(double value) {
    return std::exp2(value / 6.0);
}

}  // namespace

TEST_CASE("an alternative presentation's target loudness correction follows the device category",
          "[ac4][decoder][downmix]") {
    // loud_corr_target 9 is (15 - 9) / 2 = 3 dB2 (clause 6.3.3.1.12).
    const detail::DownmixValues for_2d = alternative({target(k2D, 9)});
    // 5.1 as coded is 2D, which the target names; every channel takes it, the LFE's included.
    const std::vector<Row> as_coded =
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kAsCoded, for_2d);
    REQUIRE(as_coded.size() == kFiveOne.size());
    for (std::size_t c = 0; c < as_coded.size(); ++c) {
        CAPTURE(c);
        CHECK(std::abs(as_coded[c][c] - dB2(3.0)) < 1e-12);
    }
    // Stereo is 1D, which no target names: Table 17's first fallback is 2D.
    const std::vector<Row> stereo =
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, for_2d);
    const std::vector<Row> plain =
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, {});
    REQUIRE(stereo.size() == 2);
    for (std::size_t o = 0; o < 2; ++o) {
        for (std::size_t c = 0; c < stereo[o].size(); ++c) {
            CHECK(std::abs(stereo[o][c] - dB2(3.0) * plain[o][c]) < 1e-12);
        }
    }
    // 5.1.4 is 3D: 2D is its first fallback.
    CHECK(std::abs(gain_of(kFiveOneFour, for_2d) - dB2(3.0)) < 1e-12);
    // Mono is not in Table 17, and takes none.
    CHECK(gain_of(kMonoOut, for_2d) == 1.0);
}

TEST_CASE("an unspecified category takes Table 17's fallbacks in order, portable none",
          "[ac4][decoder][downmix]") {
    // Only 3D is specified: 1D falls back to 2D (none) then 3D, 2D to 3D; portable has none.
    const detail::DownmixValues only_3d = alternative({target(k3D, 5)});
    CHECK(std::abs(gain_of(kStereoOut, only_3d) - dB2(5.0)) < 1e-12);
    CHECK(std::abs(gain_of(kFiveOne, only_3d) - dB2(5.0)) < 1e-12);
    detail::DownmixValues portable = only_3d;
    portable.device = iclforge::ac4::TargetDevice::kPortable;
    CHECK(gain_of(kStereoOut, portable) == 1.0);
    // Only 1D is specified: 3D falls back through 2D to 1D.
    CHECK(std::abs(gain_of(kFiveOneFour, alternative({target(k1D, 14)})) - dB2(0.5)) < 1e-12);
    // 1D and 2D specified: 3D takes 2D before 1D.
    const detail::DownmixValues ones_and_twos = alternative({target(k1D, 14), target(k2D, 5)});
    CHECK(std::abs(gain_of(kFiveOneFour, ones_and_twos) - dB2(5.0)) < 1e-12);
    // The first target that names a category and sends a correction gives it; one that sends none
    // leaves the category to the next.
    const detail::DownmixValues ordered =
        alternative({target(k1D, std::nullopt), target(k1D | k2D, 9), target(k1D, 14)});
    CHECK(std::abs(gain_of(kStereoOut, ordered) - dB2(3.0)) < 1e-12);
    // loud_corr_target 31 is 0 dB, and a specified one: no fallback, and no scaling.
    const detail::DownmixValues zero = alternative({target(k1D, 31), target(k2D, 9)});
    CHECK(gain_of(kStereoOut, zero) == 1.0);
    // Targets that send no correction, and a presentation with no targets, correct nothing.
    CHECK(gain_of(kFiveOne, alternative({})) == 1.0);
    CHECK(gain_of(kFiveOne, alternative({target(k2D, std::nullopt)})) == 1.0);
}

TEST_CASE("the system's device category stands in for the layout's", "[ac4][decoder][downmix]") {
    detail::DownmixValues values = alternative({target(kPortable, 0), target(k2D, 9)});
    // Stereo is 1D by its layout, which falls back to 2D.
    CHECK(std::abs(gain_of(kStereoOut, values) - dB2(3.0)) < 1e-12);
    // A portable device takes its own: loud_corr_target 0 is 7.5 dB2.
    values.device = iclforge::ac4::TargetDevice::kPortable;
    CHECK(std::abs(gain_of(kStereoOut, values) - dB2(7.5)) < 1e-12);
    // And a category the system gives applies to a layout Table 17 does not list.
    values.device = iclforge::ac4::TargetDevice::k2D;
    CHECK(std::abs(gain_of(kMonoOut, values) - dB2(3.0)) < 1e-12);
}

TEST_CASE("the real-time loudness correction is (rtll_comp - 128) / 4 dB, and the frame's own",
          "[ac4][decoder][downmix]") {
    // +2 dB, -32 dB and 0 dB.
    CHECK(std::abs(gain_of(kFiveOne, alternative({}, 136)) - db(2.0)) < 1e-12);
    CHECK(std::abs(gain_of(kFiveOne, alternative({}, 0)) - db(-32.0)) < 1e-12);
    CHECK(gain_of(kFiveOne, alternative({}, 128)) == 1.0);
    // With the target's: the product.
    CHECK(std::abs(gain_of(kFiveOne, alternative({target(k2D, 9)}, 136)) - dB2(3.0) * db(2.0)) <
          1e-12);
    // With the downmix's own correction: the stereo downmix takes both.
    detail::DownmixValues values = alternative({}, 136);
    values.coeff = coefficients(2, 2, 2, 2, std::nullopt, 1);
    values.loro_loud_corr = 9;  // 3 dB2
    const std::vector<Row> corrected =
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, values);
    detail::DownmixValues without = values;
    without.rtll_comp.reset();
    const std::vector<Row> base =
        matrix_for(kFiveOne, false, iclforge::ac4::DownmixTarget::kLoRo, without);
    REQUIRE(corrected.size() == base.size());
    for (std::size_t o = 0; o < base.size(); ++o) {
        for (std::size_t c = 0; c < base[o].size(); ++c) {
            CHECK(std::abs(corrected[o][c] - db(2.0) * base[o][c]) < 1e-12);
        }
    }

    // Both belong to the frame that sends them: the next frame, which sends none, has none.
    detail::DownmixStage stage;
    stage.configure(kFiveOne, false, iclforge::ac4::DownmixTarget::kAsCoded, true);
    CHECK(stage.passes_through());
    stage.update(alternative({target(k2D, 9)}, 136));
    CHECK_FALSE(stage.passes_through());
    CHECK(std::abs(stage.matrix()[0][0] - dB2(3.0) * db(2.0)) < 1e-12);
    stage.update({});
    CHECK(stage.passes_through());
    CHECK(stage.matrix()[0][0] == 1.0);
}

TEST_CASE("DEE's 5.1 tones come out of each downmix at the stream's gains, to 0.01 dB",
          "[ac4][decoder][downmix]") {
    // L R C LFE Ls Rs at 331, 457, 613, 47, 787 and 953 Hz; DEE's stream sends
    // -3 dB centre and surround gains and no LFE mix gain.
    constexpr std::array<double, 6> kHz = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
    const std::vector<std::byte> stream = read_stream("ac4-51-tones-384");
    const Decoded coded = decode_all(stream, iclforge::ac4::DownmixTarget::kAsCoded);
    REQUIRE(coded.speakers == std::vector<S>(kFiveOne.begin(), kFiveOne.end()));
    std::array<double, 6> own{};
    for (std::size_t c = 0; c < 6; ++c) {
        own[c] = amplitude(coded.channels[c], kHz[c]);
    }
    const double m3 = db(-3.0);
    struct Expected {
        iclforge::ac4::DownmixTarget target;
        std::vector<Row> matrix;
    };
    const std::array<Expected, 3> targets = {{
        {iclforge::ac4::DownmixTarget::kLoRo, {{1, 0, m3, 0, m3, 0}, {0, 1, m3, 0, 0, m3}}},
        {iclforge::ac4::DownmixTarget::kLtRt, {{1, 0, m3, 0, -m3, -m3}, {0, 1, m3, 0, m3, m3}}},
        {iclforge::ac4::DownmixTarget::kMono, {{1, 1, 2 * m3, 0, m3, m3}}},
    }};
    for (const Expected& expected : targets) {
        CAPTURE(iclforge::ac4::describe(expected.target));
        const Decoded mixed = decode_all(stream, expected.target);
        REQUIRE(mixed.channels.size() == expected.matrix.size());
        for (std::size_t o = 0; o < mixed.channels.size(); ++o) {
            for (std::size_t c = 0; c < 6; ++c) {
                CAPTURE(o, c);
                const double got = amplitude(mixed.channels[o], kHz[c]) / own[c];
                const double want = std::abs(expected.matrix[o][c]);
                if (want == 0.0) {
                    CHECK(got < 1e-3);
                } else {
                    CHECK(std::abs(20.0 * std::log10(got / want)) < 0.01);
                }
            }
        }
    }
}
