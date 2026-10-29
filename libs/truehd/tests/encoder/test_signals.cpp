#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// Signals chosen to be hard for a lossless coder, held to an exact round trip: the cases
// tools/ci/fuzz_truehd_encoder_space.py found, and their neighbours.

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace {

constexpr std::int32_t kTop = (1 << 23) - 1;
constexpr std::int32_t kBottom = -(1 << 23);

std::vector<std::int32_t> build(int channels, int frames, auto sample) {
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(channels) *
                                  static_cast<std::size_t>(frames));
    for (int t = 0; t < frames; ++t) {
        for (int c = 0; c < channels; ++c) {
            pcm[static_cast<std::size_t>(t) * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(c)] = sample(t, c);
        }
    }
    return pcm;
}

void check_round_trip(const std::vector<std::int32_t>& pcm, int channels, std::uint32_t rate,
                      int interval) {
    EncoderConfig config;
    config.sample_rate = rate;
    config.channels = channels;
    config.restart_interval_units = interval;
    config.allow_rate_violations = true;
    config.verify = true;
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE(decode_all(encoded.units, 24, channels) == pcm);
}

}  // namespace

TEST_CASE("a steady level far from zero comes back, in a pair and on its own",
          "[truehd][encoder][signals]") {
    // The residual of the first block of an interval is the row itself (the filter has no history
    // yet), and a constant row of 1.2 * 2^23 is out of the offset's 15 bits and every width its
    // spread suggests: the coder has to look at every width.
    for (const std::int32_t level : {kTop, kBottom, 8000000, -5000000, 16384, 65536}) {
        CAPTURE(level);
        check_round_trip(
            build(2, 40 * 20, [level](int, int c) { return c == 0 ? level : level / 2; }), 2, 48000,
            9);
        check_round_trip(build(1, 40 * 20, [level](int, int) { return level; }), 1, 48000, 8);
    }
}

TEST_CASE("full-scale squares, alternating extremes and impulses come back",
          "[truehd][encoder][signals]") {
    for (const int interval : {1, 8, 9}) {
        CAPTURE(interval);
        check_round_trip(
            build(2, 4800, [](int t, int c) { return ((t / (20 + c)) % 2 == 0) ? kTop : kBottom; }),
            2, 48000, interval);
        check_round_trip(build(2, 4800, [](int t, int) { return t % 2 == 0 ? kTop : kBottom; }), 2,
                         48000, interval);
        check_round_trip(
            build(5, 4800,
                  [](int t, int c) { return t % 997 == c ? (c % 2 ? kTop : kBottom) : 0; }),
            5, 44100, interval);
    }
}

TEST_CASE("identical, inverted and lone loud channels come back", "[truehd][encoder][signals]") {
    const auto tone = [](int t) {
        return static_cast<std::int32_t>(kTop * 0.4 * ((t % 91) / 45.0 - 1.0));
    };
    check_round_trip(build(2, 4000, [&](int t, int) { return tone(t); }), 2, 48000, 8);
    check_round_trip(build(2, 4000, [&](int t, int c) { return c == 0 ? tone(t) : -tone(t); }), 2,
                     96000, 8);
    check_round_trip(
        build(6, 4000,
              [&](int t, int c) {
                  return c == 3 ? static_cast<std::int32_t>(
                                      ((static_cast<std::uint32_t>(t) * 2654435761U) >> 8) -
                                      (1U << 23))
                                : 0;
              }),
        6, 48000, 9);
}
