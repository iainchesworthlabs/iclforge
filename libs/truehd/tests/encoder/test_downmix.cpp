#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// DownmixMode::kStandard: substream 0 carries a stereo downmix, the later substreams' matrices take
// it apart again. The full presentation comes back exactly (FFmpeg's strict decode checks that in
// tools/checks/verify_truehd_encode_interop.py); the lower presentations are checked here against
// the downmix in doubles, within what the 2^-14 coefficients allow. No external decoder checks the
// lower presentations (ERRATA.md entry 38).

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace {

struct DownmixCase {
    int channels;
    bool rear;
};

constexpr int kFrames = 40 * 40;

}  // namespace

TEST_CASE("a standard downmix puts the stereo mix in substream 0 and the source back in the last",
          "[truehd][encoder][downmix]") {
    for (const DownmixCase c : {DownmixCase{3, false}, DownmixCase{6, false}, DownmixCase{7, true},
                                DownmixCase{8, true}}) {
        CAPTURE(c.channels);
        EncoderConfig config;
        config.channels = c.channels;
        config.restart_interval_units = 8;
        config.downmix = DownmixMode::kStandard;
        config.downmix_gains.lfe = 0.5;
        config.verify = true;
        // Room for the sums: a third of full scale in every channel.
        auto pcm = make_pcm(c.channels, kFrames, 24, 31);
        for (auto& v : pcm) {
            v /= 3;
        }
        const Encoded encoded = encode_all(config, pcm);
        REQUIRE(encoded.stats.downmix_reduced_intervals == 0);
        REQUIRE(encoded.stats.matrices_used > 0);
        REQUIRE(decode_all(encoded.units, 24, c.channels) == pcm);

        const double centre = config.downmix_gains.centre;
        const double surround = config.downmix_gains.surround;
        const double lfe = config.downmix_gains.lfe;
        const double rear = config.downmix_gains.rear_to_surround;
        const auto at = [&](std::size_t t, int ch) {
            return static_cast<double>(
                pcm[t * static_cast<std::size_t>(c.channels) + static_cast<std::size_t>(ch)]);
        };
        // The default layouts: 3: L R C; 6: L R C LFE Ls Rs; 7: L R C Ls Rs Lrs Rrs;
        // 8: L R C LFE Ls Rs Lrs Rrs.
        const int lfe_row = (c.channels == 6 || c.channels == 8) ? 3 : -1;
        const int ls_row = c.channels == 3 ? -1 : (c.channels == 7 ? 3 : 4);
        const int rs_row = ls_row < 0 ? -1 : ls_row + 1;
        const int lrs_row = c.rear ? rs_row + 1 : -1;
        const int rrs_row = c.rear ? rs_row + 2 : -1;
        const auto value = [&](std::size_t t, int row) { return row < 0 ? 0.0 : at(t, row); };

        const auto stereo = decode_all(encoded.units, 24, 2, Presentation::k2ch);
        REQUIRE(stereo.size() == 2 * static_cast<std::size_t>(kFrames));
        for (std::size_t t = 0; t < kFrames; ++t) {
            const double ls = value(t, ls_row) + rear * value(t, lrs_row);
            const double rs = value(t, rs_row) + rear * value(t, rrs_row);
            const double left =
                at(t, 0) + centre * at(t, 2) + lfe * value(t, lfe_row) + surround * ls;
            const double right =
                at(t, 1) + centre * at(t, 2) + lfe * value(t, lfe_row) + surround * rs;
            const double tolerance =
                3.1e-5 *
                    (std::abs(at(t, 2)) + std::abs(value(t, lfe_row)) + std::abs(ls) +
                     std::abs(rs) + std::abs(value(t, lrs_row)) + std::abs(value(t, rrs_row))) +
                4.0;
            REQUIRE(std::abs(static_cast<double>(stereo[2 * t]) - left) <= tolerance);
            REQUIRE(std::abs(static_cast<double>(stereo[2 * t + 1]) - right) <= tolerance);
        }

        if (c.rear) {
            // The 6 channel presentation: the source up to the surrounds, and the surrounds with
            // the rear surrounds folded in.
            const int five = c.channels == 8 ? 6 : 5;
            const auto six = decode_all(encoded.units, 24, five, Presentation::k6ch);
            REQUIRE(six.size() ==
                    static_cast<std::size_t>(five) * static_cast<std::size_t>(kFrames));
            for (std::size_t t = 0; t < kFrames; ++t) {
                const auto row = [&](int ch) {
                    return static_cast<double>(
                        six[t * static_cast<std::size_t>(five) + static_cast<std::size_t>(ch)]);
                };
                for (int ch = 0; ch < ls_row; ++ch) {
                    REQUIRE(row(ch) == at(t, ch));
                }
                const double tolerance =
                    3.1e-5 * (std::abs(value(t, lrs_row)) + std::abs(value(t, rrs_row))) + 2.0;
                REQUIRE(std::abs(row(ls_row) - (value(t, ls_row) + rear * value(t, lrs_row))) <=
                        tolerance);
                REQUIRE(std::abs(row(rs_row) - (value(t, rs_row) + rear * value(t, rrs_row))) <=
                        tolerance);
            }
        }
    }
}

TEST_CASE(
    "a downmix that would overflow 24 bits has its gains lowered and the source still comes back",
    "[truehd][encoder][downmix]") {
    EncoderConfig config;
    config.channels = 6;
    config.restart_interval_units = 8;
    config.downmix = DownmixMode::kStandard;
    config.verify = true;
    std::vector<std::int32_t> pcm(6 * kFrames);
    std::uint32_t state = 5;
    for (auto& v : pcm) {
        state = state * 1664525U + 1013904223U;
        v = static_cast<std::int32_t>(state >> 8U) - (1 << 23);
    }
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE(encoded.stats.downmix_reduced_intervals > 0);
    REQUIRE(decode_all(encoded.units, 24, 6) == pcm);
    REQUIRE(decode_all(encoded.units, 24, 2, Presentation::k2ch).size() == 2 * kFrames);
}

TEST_CASE("a downmix of a stereo stream is the stream, and layouts it cannot fold are refused",
          "[truehd][encoder][downmix]") {
    EncoderConfig plain;
    plain.restart_interval_units = 8;
    EncoderConfig folded = plain;
    folded.downmix = DownmixMode::kStandard;
    const auto pcm = make_pcm(2, 40 * 20, 24, 3);
    const Encoded a = encode_all(plain, pcm);
    const Encoded b = encode_all(folded, pcm);
    REQUIRE(a.units.size() == b.units.size());
    for (std::size_t i = 0; i < a.units.size(); ++i) {
        REQUIRE(a.units[i].bytes == b.units[i].bytes);
    }

    EncoderConfig high;
    high.sample_rate = 192000;
    high.channels = 6;
    high.downmix = DownmixMode::kStandard;
    REQUIRE(!Encoder::create(high).has_value());  // one substream: no lower presentation

    EncoderConfig height;
    height.channels = 5;
    height.channel_assignment = 0x1 | 0x4 | 0x10;  // L R, LFE, Lvh Rvh
    height.downmix = DownmixMode::kStandard;
    REQUIRE(!Encoder::create(height).has_value());

    EncoderConfig loud;
    loud.channels = 6;
    loud.downmix = DownmixMode::kStandard;
    loud.downmix_gains.centre = -0.5;
    REQUIRE(!Encoder::create(loud).has_value());
    loud.downmix_gains.centre = 2.5;
    REQUIRE(!Encoder::create(loud).has_value());
}
