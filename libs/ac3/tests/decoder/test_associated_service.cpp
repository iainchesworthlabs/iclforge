#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/associated_service.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

// §E3.10's receiver mixer: Tables E3.15-E3.17, the scale factors that mean what
// §E3.10.1-E3.10.7 say, and the ones the spec gives nothing to apply.
//
// No external oracle reaches here. FFmpeg decodes one programme and ignores the
// whole mixmdate group; the Dolby Reference Player is not scriptable for a
// two-programme mix. What the tests hold is the spec's own arithmetic, derived
// independently of the implementation from Tables E3.15-E3.17 as printed, and an
// end-to-end run through this project's own encoder and decoder.

namespace {

using iclforge::ac3::Acmod;
using iclforge::ac3::AssociatedServiceMixConfig;
using iclforge::ac3::AssociatedServiceMixer;
using iclforge::ac3::DecodedAccessUnit;
using iclforge::ac3::DownmixTarget;
using iclforge::ac3::MixError;
using iclforge::ac3::eac3::chanmap::Location;
namespace meta = iclforge::ac3::meta;

constexpr double kHalfPi = std::numbers::pi / 2.0;

double amplitude_db(double gain) {
    return 20.0 * std::log10(gain);
}

// A unit whose every channel holds one constant, so a mix is a few
// multiplications a test can write down.
DecodedAccessUnit unit_of(Acmod acmod, bool lfe, std::size_t samples,
                          std::span<const float> values) {
    DecodedAccessUnit unit;
    unit.acmod = acmod;
    unit.layout =
        iclforge::ac3::eac3::chanmap::expand(iclforge::ac3::eac3::chanmap::acmod_map(acmod, lfe));
    REQUIRE(static_cast<std::size_t>(unit.layout.count) == values.size());
    for (const float value : values) {
        unit.channels.emplace_back(samples, value);
    }
    return unit;
}

DecodedAccessUnit stereo(float left, float right, std::size_t samples = 1536) {
    const std::array<float, 2> v{left, right};
    return unit_of(Acmod::k2_0, false, samples, v);
}

DecodedAccessUnit mono(float value, std::size_t samples = 1536) {
    const std::array<float, 1> v{value};
    return unit_of(Acmod::k1_0, false, samples, v);
}

DecodedAccessUnit five_one(float value, std::size_t samples = 1536) {
    const std::array<float, 6> v{value, value, value, value, value, value};
    return unit_of(Acmod::k3_2, true, samples, v);
}

constexpr std::array<Location, 6> k51 = {Location::kLeft,          Location::kCentre,
                                         Location::kRight,         Location::kLeftSurround,
                                         Location::kRightSurround, Location::kLfe};
constexpr std::array<Location, 2> kStereo = {Location::kLeft, Location::kRight};

double power_sum(const std::vector<double>& weights) {
    double sum = 0.0;
    for (const double w : weights) {
        sum += w * w;
    }
    return sum;
}

}  // namespace

TEST_CASE("programme and external scale codes map to the gains Annex E gives",
          "[eac3][mixing][associated]") {
    // §E2.3.1.13: 0 is mute, 1..63 are -50 dB to +12 dB in 1 dB steps.
    CHECK(meta::pgm_scale_gain(0) == 0.0);
    CHECK(amplitude_db(meta::pgm_scale_gain(1)) == Catch::Approx(-50.0).margin(1e-9));
    CHECK(meta::pgm_scale_gain(51) == Catch::Approx(1.0).margin(1e-12));
    CHECK(amplitude_db(meta::pgm_scale_gain(63)) == Catch::Approx(12.0).margin(1e-9));
    // A code that cannot have come off the wire leaves the audio alone.
    CHECK(meta::pgm_scale_gain(64) == 1.0);
    CHECK(meta::pgm_scale_gain(-1) == 1.0);

    // Table E2.8: -1 dB steps to -6, then wider, and code 15 is -infinity.
    CHECK(amplitude_db(meta::external_scale_gain(0)) == Catch::Approx(-1.0).margin(1e-9));
    CHECK(amplitude_db(meta::external_scale_gain(5)) == Catch::Approx(-6.0).margin(1e-9));
    CHECK(amplitude_db(meta::external_scale_gain(6)) == Catch::Approx(-8.0).margin(1e-9));
    CHECK(amplitude_db(meta::external_scale_gain(14)) == Catch::Approx(-28.0).margin(1e-9));
    // kExternalScaleDb holds that row as 0.0 dB; the gain must not.
    CHECK(meta::external_scale_gain(15) == 0.0);
    CHECK(meta::external_scale_gain(16) == 1.0);
}

TEST_CASE("a mono service over a stereo main follows Table E3.15", "[eac3][mixing][associated]") {
    const auto left_of = [](int p) {
        if (p <= 19) {
            return std::cos(kHalfPi * (p + 20) / 40.0);
        }
        if (p <= 99) {
            return 0.0;
        }
        if (p <= 139) {
            return std::sin(kHalfPi * (p - 100) / 40.0);
        }
        if (p <= 219) {
            return 1.0;
        }
        return std::cos(kHalfPi * (p - 220) / 40.0);
    };
    const auto right_of = [](int p) {
        if (p <= 19) {
            return std::sin(kHalfPi * (p + 20) / 40.0);
        }
        if (p <= 99) {
            return 1.0;
        }
        if (p <= 139) {
            return std::cos(kHalfPi * (p - 100) / 40.0);
        }
        if (p <= 219) {
            return 0.0;
        }
        return std::sin(kHalfPi * (p - 220) / 40.0);
    };
    for (int p = 0; p <= iclforge::ac3::meta::kPanMeanMax; ++p) {
        CAPTURE(p);
        const auto w = iclforge::ac3::pan_weights(p, kStereo);
        REQUIRE(w.size() == 2);
        CHECK(w[0] == Catch::Approx(left_of(p)).margin(1e-12));
        CHECK(w[1] == Catch::Approx(right_of(p)).margin(1e-12));
        // Constant power through every crossfade, and at the hard sides.
        CHECK(power_sum(w) == Catch::Approx(1.0).margin(1e-12));
    }
    // The three positions a listener can name.
    const auto centre = iclforge::ac3::pan_weights(0, kStereo);
    CHECK(centre[0] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
    CHECK(centre[1] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
    CHECK(iclforge::ac3::pan_weights(60, kStereo)[1] == 1.0);
    CHECK(iclforge::ac3::pan_weights(180, kStereo)[0] == 1.0);
}

TEST_CASE("a mono service over a 5.1 main follows Tables E3.16 and E3.17",
          "[eac3][mixing][associated]") {
    // Written out row by row from the tables, not from the implementation.
    struct Row {
        int p;
        double l, c, r, ls, rs;
    };
    const auto row = [](int p) -> Row {
        if (p <= 19) {
            return {p, 0, std::cos(kHalfPi * p / 20.0), std::sin(kHalfPi * p / 20.0), 0, 0};
        }
        if (p <= 72) {
            return {p, 0,
                    0, std::cos(kHalfPi * (p - 20) / 53.0),
                    0, std::sin(kHalfPi * (p - 20) / 53.0)};
        }
        if (p <= 166) {
            return {p,
                    0,
                    0,
                    0,
                    std::sin(kHalfPi * (p - 73) / 94.0),
                    std::cos(kHalfPi * (p - 73) / 94.0)};
        }
        if (p <= 219) {
            return {p, std::sin(kHalfPi * (p - 167) / 53.0), 0,
                    0, std::cos(kHalfPi * (p - 167) / 53.0), 0};
        }
        return {p, std::cos(kHalfPi * (p - 220) / 20.0), std::sin(kHalfPi * (p - 220) / 20.0), 0, 0,
                0};
    };
    for (int p = 0; p <= iclforge::ac3::meta::kPanMeanMax; ++p) {
        CAPTURE(p);
        const auto w = iclforge::ac3::pan_weights(p, k51);
        REQUIRE(w.size() == 6);
        const auto want = row(p);
        CHECK(w[0] == Catch::Approx(want.l).margin(1e-12));
        CHECK(w[1] == Catch::Approx(want.c).margin(1e-12));
        CHECK(w[2] == Catch::Approx(want.r).margin(1e-12));
        CHECK(w[3] == Catch::Approx(want.ls).margin(1e-12));
        CHECK(w[4] == Catch::Approx(want.rs).margin(1e-12));
        CHECK(w[5] == 0.0);  // the LFE is not part of the service
        CHECK(power_sum(w) == Catch::Approx(1.0).margin(1e-12));
    }
    // The speakers themselves: C at 0, R at 20 (30 degrees), Rs at 73 (109.5),
    // Ls at 167 (250.5), L at 220 (330).
    CHECK(iclforge::ac3::pan_weights(0, k51)[1] == 1.0);
    CHECK(iclforge::ac3::pan_weights(20, k51)[2] == 1.0);
    CHECK(iclforge::ac3::pan_weights(73, k51)[4] == 1.0);
    CHECK(iclforge::ac3::pan_weights(167, k51)[3] == 1.0);
    CHECK(iclforge::ac3::pan_weights(220, k51)[0] == 1.0);
}

TEST_CASE("a pan lands somewhere sensible for a main the tables do not cover",
          "[eac3][mixing][associated]") {
    SECTION("a reserved or out-of-range index is the centre") {
        for (const int p : {240, 255, -1, 1000}) {
            CAPTURE(p);
            CHECK(iclforge::ac3::pan_weights(p, k51) == iclforge::ac3::pan_weights(0, k51));
        }
    }
    SECTION("a mono main takes the service whole, whatever the pan") {
        const std::array<Location, 1> centre = {Location::kCentre};
        for (const int p : {0, 20, 90, 130, 200, 230}) {
            CAPTURE(p);
            CHECK(iclforge::ac3::pan_weights(p, centre)[0] == Catch::Approx(1.0).margin(1e-12));
        }
    }
    SECTION("seven one splits each surround's power over the two channels in the seat") {
        const std::array<Location, 8> l71 = {
            Location::kLeft,          Location::kCentre, Location::kRight, Location::kLeftSurround,
            Location::kRightSurround, Location::kLrs,    Location::kRrs,   Location::kLfe};
        // Directly behind and to the right: the 5.1 table gives the right
        // surround all of it, and Rs and Rrs share the seat.
        const auto w = iclforge::ac3::pan_weights(73, l71);
        CHECK(w[4] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
        CHECK(w[6] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
        CHECK(w[3] == 0.0);
        CHECK(w[7] == 0.0);
        for (int p = 0; p <= iclforge::ac3::meta::kPanMeanMax; ++p) {
            CHECK(power_sum(iclforge::ac3::pan_weights(p, l71)) ==
                  Catch::Approx(1.0).margin(1e-12));
        }
    }
    SECTION("heights and LFEs receive none of it") {
        const std::array<Location, 4> tops = {Location::kLeft, Location::kRight, Location::kVhl,
                                              Location::kLfe};
        for (int p = 0; p <= iclforge::ac3::meta::kPanMeanMax; ++p) {
            const auto w = iclforge::ac3::pan_weights(p, tops);
            CHECK(w[2] == 0.0);
            CHECK(w[3] == 0.0);
            CHECK(power_sum(w) == Catch::Approx(1.0).margin(1e-12));
        }
    }
    SECTION("a main without surrounds keeps the rear of the pan at its own side") {
        const std::array<Location, 3> lcr = {Location::kLeft, Location::kCentre, Location::kRight};
        // 90 degrees back right is all right surround in the 5.1 tables.
        const auto w = iclforge::ac3::pan_weights(60, lcr);
        CHECK(w[2] == Catch::Approx(1.0).margin(1e-12));
        CHECK(w[0] == 0.0);
        for (int p = 0; p <= iclforge::ac3::meta::kPanMeanMax; ++p) {
            CHECK(power_sum(iclforge::ac3::pan_weights(p, lcr)) ==
                  Catch::Approx(1.0).margin(1e-12));
        }
    }
    SECTION("a quad main with no centre shares the front over its two fronts") {
        const std::array<Location, 4> quad = {Location::kLeft, Location::kRight,
                                              Location::kLeftSurround, Location::kRightSurround};
        const auto w = iclforge::ac3::pan_weights(0, quad);
        CHECK(w[0] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
        CHECK(w[1] == Catch::Approx(std::numbers::sqrt2 / 2.0).margin(1e-12));
        CHECK(w[2] == 0.0);
    }
    SECTION("no horizontal channel at all means nowhere to put it") {
        const std::array<Location, 2> lfes = {Location::kLfe, Location::kLfe2};
        CHECK(power_sum(iclforge::ac3::pan_weights(0, lfes)) == 0.0);
    }
}

TEST_CASE("pgmscl scales its own programme and extpgmscl the other", "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    auto main = stereo(0.5F, 0.5F);
    auto service = mono(0.25F);
    main.mixing.emplace();
    main.mixing->pgmscl = 45;  // -6 dB on the main
    service.mixing.emplace();
    service.mixing->pgmscl = 41;     // -10 dB on the service
    service.mixing->extpgmscl = 48;  // -3 dB on the main, from the service
    // Pan 0: the centre, which a stereo main takes at -3 dB in each side.

    const auto result = mixer.mix(main, service);
    REQUIRE(result.has_value());
    CHECK(result->main_gain_db == Catch::Approx(-9.0).margin(1e-9));
    CHECK(result->associated_gain_db == Catch::Approx(-10.0).margin(1e-9));
    REQUIRE(result->panmean.has_value());
    CHECK(*result->panmean == 0);

    const double main_gain = std::pow(10.0, -9.0 / 20.0);
    const double service_gain = std::pow(10.0, -10.0 / 20.0) * std::numbers::sqrt2 / 2.0;
    const double want = 0.5 * main_gain + 0.25 * service_gain;
    for (const auto& channel : main.channels) {
        for (const float sample : channel) {
            CHECK(sample == Catch::Approx(want).margin(1e-6));
        }
    }
}

TEST_CASE("the main's own extpgmscl scales the associated service", "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    auto main = stereo(0.0F, 0.0F);
    auto service = mono(0.5F);
    main.mixing.emplace();
    main.mixing->extpgmscl = 45;  // -6 dB, on the service
    const auto result = mixer.mix(main, service);
    REQUIRE(result.has_value());
    CHECK(result->associated_gain_db == Catch::Approx(-6.0).margin(1e-9));
    CHECK(result->main_gain_db == Catch::Approx(0.0).margin(1e-9));
    const double want = 0.5 * std::pow(10.0, -6.0 / 20.0) * std::numbers::sqrt2 / 2.0;
    CHECK(main.channels[0][100] == Catch::Approx(want).margin(1e-6));
    CHECK(main.channels[1][100] == Catch::Approx(want).margin(1e-6));
}

TEST_CASE("a programme scale of zero mutes", "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    auto main = stereo(0.5F, 0.5F);
    auto service = mono(0.5F);
    service.mixing.emplace();
    service.mixing->pgmscl = 0;
    const auto result = mixer.mix(main, service);
    REQUIRE(result.has_value());
    CHECK(std::isinf(result->associated_gain_db));
    CHECK(result->associated_gain_db < 0.0);
    CHECK(main.channels[0][0] == 0.5F);
    CHECK(main.channels[1][1535] == 0.5F);

    // And the main muted by the service's extpgmscl.
    AssociatedServiceMixer second;
    auto main2 = stereo(0.5F, 0.5F);
    auto service2 = mono(0.25F);
    service2.mixing.emplace();
    service2.mixing->extpgmscl = 0;
    const auto result2 = second.mix(main2, service2);
    REQUIRE(result2.has_value());
    CHECK(std::isinf(result2->main_gain_db));
    CHECK(main2.channels[0][700] == Catch::Approx(0.25 * std::numbers::sqrt2 / 2.0).margin(1e-6));
}

TEST_CASE("the per-channel scales trim channels of the other programme",
          "[eac3][mixing][associated]") {
    // mixdef 3's mixdata2e: the service trims the main's channels one by one.
    AssociatedServiceMixer mixer;
    auto main = five_one(0.5F);
    auto service = mono(0.0F);
    service.mixing.emplace();
    service.mixing->extpgmscl = 45;  // -6 dB on everything
    service.mixing->mixing.mixdef = meta::MixDefinition::kExtended;
    meta::ExternalScales scales;
    scales.left = 5;            // a further -6 dB
    scales.right_surround = 7;  // -10 dB
    scales.lfe = 15;            // mute
    service.mixing->mixing.external = scales;

    const auto result = mixer.mix(main, service);
    REQUIRE(result.has_value());
    const auto level = [&](std::size_t channel) { return main.channels[channel][500]; };
    CHECK(level(0) == Catch::Approx(0.5 * std::pow(10.0, -12.0 / 20.0)).margin(1e-6));  // L
    CHECK(level(1) == Catch::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));   // C
    CHECK(level(2) == Catch::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));   // R
    CHECK(level(3) == Catch::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));   // Ls
    CHECK(level(4) == Catch::Approx(0.5 * std::pow(10.0, -16.0 / 20.0)).margin(1e-6));  // Rs
    CHECK(level(5) == 0.0F);                                                            // LFE
}

TEST_CASE("dmixscl stands in for the per-channel scales on a folded main",
          "[eac3][mixing][associated]") {
    // The main reached the mixer as Lo/Ro from a 5.1 programme: L and R are
    // already sums, so extpgmlscl means nothing and dmixscl is the trim.
    DecodedAccessUnit main = five_one(0.0F);
    main.channels.resize(2);  // what a fold leaves behind
    for (auto& channel : main.channels) {
        channel.assign(1536, 0.5F);
    }
    auto service = mono(0.0F);
    service.mixing.emplace();
    service.mixing->mixing.mixdef = meta::MixDefinition::kExtended;
    meta::ExternalScales scales;
    scales.left = 5;     // -6 dB: must NOT apply to a fold
    scales.dmixscl = 7;  // -10 dB: must
    service.mixing->mixing.external = scales;

    AssociatedServiceMixer mixer{{.main_fold = DownmixTarget::kLoRo}};
    REQUIRE(mixer.mix(main, service).has_value());
    for (const auto& channel : main.channels) {
        CHECK(channel[800] == Catch::Approx(0.5 * std::pow(10.0, -10.0 / 20.0)).margin(1e-6));
    }

    // The same scales on an unfolded stereo main take the per-channel route.
    auto plain = stereo(0.5F, 0.5F);
    AssociatedServiceMixer second;
    REQUIRE(second.mix(plain, service).has_value());
    CHECK(plain.channels[0][800] == Catch::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));
    CHECK(plain.channels[1][800] == Catch::Approx(0.5).margin(1e-6));
}

TEST_CASE("a stereo or wider service is routed by location and not panned",
          "[eac3][mixing][associated]") {
    SECTION("stereo over 5.1 lands in L and R alone") {
        AssociatedServiceMixer mixer;
        auto main = five_one(0.0F);
        auto service = stereo(0.25F, -0.5F);
        // A pan the stream sent for the service is meaningless to a service
        // with two channels, and must not move them.
        service.mixing.emplace();
        service.mixing->pan = meta::PanInfo{.panmean = 120};
        const auto result = mixer.mix(main, service);
        REQUIRE(result.has_value());
        CHECK_FALSE(result->panmean.has_value());
        CHECK(main.channels[0][10] == Catch::Approx(0.25).margin(1e-9));
        CHECK(main.channels[2][10] == Catch::Approx(-0.5).margin(1e-9));
        for (const std::size_t other : {1U, 3U, 4U, 5U}) {
            CHECK(main.channels[other][10] == 0.0F);
        }
    }
    SECTION("5.1 over a mono main puts the seats in the one speaker") {
        // C at unity, L and R at -3 dB each (the centre seat's fold), the
        // surrounds into the fronts on their side and so on to the centre.
        AssociatedServiceMixer mixer{{.main_fold = DownmixTarget::kMono}};
        DecodedAccessUnit main = mono(0.0F);
        auto service = five_one(0.0F);
        service.channels[1].assign(1536, 0.5F);  // centre only
        REQUIRE(mixer.mix(main, service).has_value());
        CHECK(main.channels[0][3] == Catch::Approx(0.5).margin(1e-9));
    }
    SECTION("a service channel the main has no seat for goes where it would sit") {
        // A centre service into a stereo main: split between the two fronts.
        AssociatedServiceMixer mixer;
        auto main = stereo(0.0F, 0.0F);
        const std::array<float, 3> v{0.0F, 0.5F, 0.0F};
        auto service = unit_of(Acmod::k3_0, false, 1536, v);
        REQUIRE(mixer.mix(main, service).has_value());
        CHECK(main.channels[0][0] == Catch::Approx(0.5 * std::numbers::sqrt2 / 2.0).margin(1e-6));
        CHECK(main.channels[1][0] == Catch::Approx(0.5 * std::numbers::sqrt2 / 2.0).margin(1e-6));
    }
}

TEST_CASE("a dual mono service is two services, each with its own scale and pan",
          "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    auto main = stereo(0.0F, 0.0F);
    const std::array<float, 2> v{0.5F, 0.25F};
    auto service = unit_of(Acmod::kDualMono, false, 1536, v);
    service.mixing.emplace();
    service.mixing->pgmscl = 51;                           // 0 dB
    service.mixing->pgmscl2 = 45;                          // -6 dB
    service.mixing->pan = meta::PanInfo{.panmean = 20};    // hard right
    service.mixing->pan2 = meta::PanInfo{.panmean = 220};  // hard left
    REQUIRE(mixer.mix(main, service).has_value());
    CHECK(main.channels[1][5] == Catch::Approx(0.5).margin(1e-6));
    CHECK(main.channels[0][5] == Catch::Approx(0.25 * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));
}

TEST_CASE("a change of gain ramps over the first block and holds after",
          "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    auto first = stereo(0.0F, 0.0F);
    auto service = mono(0.5F);
    service.mixing.emplace();
    service.mixing->pgmscl = 51;  // unity
    REQUIRE(mixer.mix(first, service).has_value());
    // The first unit has nothing to ramp from.
    const float steady = first.channels[0][0];
    CHECK(steady == Catch::Approx(0.5 * std::numbers::sqrt2 / 2.0).margin(1e-6));
    CHECK(first.channels[0][1535] == steady);

    auto second = stereo(0.0F, 0.0F);
    service.mixing->pgmscl = 39;  // -12 dB
    REQUIRE(mixer.mix(second, service).has_value());
    const float target = static_cast<float>(steady * std::pow(10.0, -12.0 / 20.0));
    const auto& out = second.channels[0];
    // Monotone down across the block, never past the target, then constant.
    for (std::size_t n = 1; n < 256; ++n) {
        CHECK(out[n] <= out[n - 1]);
        CHECK(out[n] >= target - 1e-6F);
    }
    CHECK(out[0] > target);
    CHECK(out[0] < steady);
    CHECK(out[255] == Catch::Approx(target).margin(1e-6));
    CHECK(out[256] == Catch::Approx(target).margin(1e-6));
    CHECK(out[1535] == Catch::Approx(target).margin(1e-6));

    // reset() forgets the previous unit: the next one is applied at once.
    mixer.reset();
    auto third = stereo(0.0F, 0.0F);
    service.mixing->pgmscl = 51;
    REQUIRE(mixer.mix(third, service).has_value());
    CHECK(third.channels[0][0] == steady);
}

TEST_CASE("the fields Annex E gives no processing leave the audio untouched",
          "[eac3][mixing][associated]") {
    const auto mixed = [](const meta::MixMetadata& metadata) {
        AssociatedServiceMixer mixer;
        auto main = five_one(0.3F);
        auto service = mono(0.2F);
        service.mixing = metadata;
        const auto result = mixer.mix(main, service);
        REQUIRE(result.has_value());
        return main.channels;
    };
    meta::MixMetadata plain;
    plain.pgmscl = 45;
    plain.extpgmscl = 48;
    plain.pan = meta::PanInfo{.panmean = 90, .paninfo = 0};
    const auto baseline = mixed(plain);

    meta::MixMetadata decorated = plain;
    // §E2.3.1.19-21: "decoders are not required to use them".
    decorated.mixing.mixdef = meta::MixDefinition::kExtended;
    meta::ExternalScales scales;
    scales.premix = {.premixcmpsel = meta::PremixCompressionSource::kCompr,
                     .drcsrc = meta::DrcSource::kThisSubstream,
                     .premixcmpscl = 7};
    // §E2.3.1.45-51: placeholders for undefined data.
    decorated.mixing.speech = meta::SpeechEnhancement{
        .spchdat = 31,
        .additional = meta::SpeechEnhancement::Additional{
            .spchdat1 = 17,
            .spchan1att = 3,
            .more = meta::SpeechEnhancement::Additional::More{.spchdat2 = 9, .spchan2att = 5}}};
    decorated.mixing.external = scales;
    // §E2.3.1.59-61 and the reserved paninfo bits.
    decorated.blkmixcfginfo = std::array<std::optional<int>, 6>{1, 2, 3, 4, 5, 6};
    decorated.pan = meta::PanInfo{.panmean = 90, .paninfo = 0x3F};
    CHECK(mixed(decorated) == baseline);

    // mixdef 1's bare triple, the same.
    meta::MixMetadata triple = plain;
    triple.mixing.mixdef = meta::MixDefinition::kPremix;
    triple.mixing.premix = {.premixcmpsel = meta::PremixCompressionSource::kCompr,
                            .drcsrc = meta::DrcSource::kThisSubstream,
                            .premixcmpscl = 3};
    CHECK(mixed(triple) == baseline);
}

TEST_CASE("the mixer refuses what it cannot line up", "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer;
    SECTION("different sample rates") {
        auto main = stereo(0.1F, 0.1F);
        auto service = mono(0.1F);
        service.sample_rate = iclforge::ac3::SampleRate::k44100;
        const auto result = mixer.mix(main, service);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MixError::kSampleRateMismatch);
    }
    SECTION("different lengths") {
        auto main = stereo(0.1F, 0.1F, 1536);
        auto service = mono(0.1F, 768);
        const auto result = mixer.mix(main, service);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MixError::kFrameLengthMismatch);
        CHECK(main.channels[0][0] == 0.1F);  // untouched
    }
    SECTION("a dual-mono main") {
        const std::array<float, 2> v{0.1F, 0.1F};
        auto main = unit_of(Acmod::kDualMono, false, 1536, v);
        auto service = mono(0.1F);
        const auto result = mixer.mix(main, service);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MixError::kUnmixableMain);
    }
    SECTION("channels that disagree with the layout") {
        auto main = stereo(0.1F, 0.1F);
        main.channels.pop_back();
        auto service = mono(0.1F);
        const auto result = mixer.mix(main, service);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MixError::kChannelCountMismatch);
    }
    SECTION("an associated service of nothing but an LFE") {
        auto main = stereo(0.1F, 0.1F);
        DecodedAccessUnit service;
        service.acmod = Acmod::k1_0;
        service.layout.items[0] = Location::kLfe;
        service.layout.count = 1;
        service.channels.emplace_back(1536, 0.1F);
        const auto result = mixer.mix(main, service);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MixError::kUnmixableAssociated);
    }
    SECTION("every error has words") {
        for (const auto error : {MixError::kSampleRateMismatch, MixError::kFrameLengthMismatch,
                                 MixError::kChannelCountMismatch, MixError::kUnmixableMain,
                                 MixError::kUnmixableAssociated}) {
            CHECK_FALSE(iclforge::ac3::describe(error).empty());
        }
    }
}

TEST_CASE("the listener's trim moves the associated service alone", "[eac3][mixing][associated]") {
    AssociatedServiceMixer mixer{{.associated_trim_db = -6.0}};
    auto main = stereo(0.5F, 0.5F);
    auto service = mono(0.5F);
    const auto result = mixer.mix(main, service);
    REQUIRE(result.has_value());
    CHECK(result->associated_gain_db == Catch::Approx(-6.0).margin(1e-9));
    CHECK(result->main_gain_db == Catch::Approx(0.0).margin(1e-9));
    const double want = 0.5 + 0.5 * std::pow(10.0, -6.0 / 20.0) * std::numbers::sqrt2 / 2.0;
    CHECK(main.channels[0][0] == Catch::Approx(want).margin(1e-6));
}
