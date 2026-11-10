// The AC-4 encoder's TrackCoder (libs/ac4/src/encoder/asf/coder.cpp): the bit count the rate loop
// asks for without building a track, and the track it settles on written into storage the caller
// keeps, both held to code_track(). Pseudo-random frames, a fixed seed, long and split layouts,
// with and without noise fill; one coder and one output track serve every frame, so the storage
// is reused across frames of different shapes (and the bit count is asked for between fills).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "encoder/asf/coder.hpp"
#include "encoder/asf/layout.hpp"

namespace {

using iclforge::ac4::detail::CodedTrack;
using iclforge::ac4::detail::FrameLayout;
using iclforge::ac4::detail::Grouped;
using iclforge::ac4::detail::TrackCoder;

class Random {
   public:
    explicit Random(std::uint64_t seed) : engine_(seed) {}
    double unit() { return static_cast<double>(engine_() >> 11U) * (1.0 / 9007199254740992.0); }
    double between(double low, double high) { return low + (high - low) * unit(); }
    int below(int n) { return static_cast<int>(engine_() % static_cast<std::uint64_t>(n)); }

   private:
    std::mt19937_64 engine_;
};

// One frame of lines in window order: bands from silence to loud, a tenth of them silent and a tenth
// sparse, as the spectral coder's own test draws them.
std::vector<double> random_spectrum(Random& rng, const FrameLayout& layout) {
    std::vector<double> lines;
    for (const int length : layout.window_length) {
        const std::span<const std::uint16_t> bands = iclforge::ac4::detail::band_offsets(length);
        const std::size_t start = lines.size();
        lines.resize(start + static_cast<std::size_t>(length), 0.0);
        for (std::size_t b = 0; b + 1 < bands.size(); ++b) {
            const int kind = rng.below(10);
            if (kind == 0) {
                continue;
            }
            const double loudness = std::pow(10.0, rng.between(-1.0, 5.0));
            const bool sparse = kind == 1;
            for (std::size_t l = bands[b]; l < bands[b + 1]; ++l) {
                double v = loudness * (2.0 * rng.unit() - 1.0);
                if (sparse && rng.below(8) != 0) {
                    v = 0.0;
                }
                lines[start + l] = v;
            }
        }
    }
    return lines;
}

void require_same(const CodedTrack& got, const CodedTrack& want) {
    REQUIRE(got.q == want.q);
    REQUIRE(got.offset == want.offset);
    REQUIRE(got.cb == want.cb);
    REQUIRE(got.sf == want.sf);
    REQUIRE(got.max_abs == want.max_abs);
    REQUIRE(got.sections.size() == want.sections.size());
    for (std::size_t g = 0; g < want.sections.size(); ++g) {
        REQUIRE(got.sections[g].size() == want.sections[g].size());
        for (std::size_t i = 0; i < want.sections[g].size(); ++i) {
            CHECK(got.sections[g][i].cb == want.sections[g][i].cb);
            CHECK(got.sections[g][i].start == want.sections[g][i].start);
            CHECK(got.sections[g][i].end == want.sections[g][i].end);
        }
    }
    CHECK(got.noise_fill == want.noise_fill);
    REQUIRE(got.dpcm_snf == want.dpcm_snf);
    CHECK(got.section_bits == want.section_bits);
    CHECK(got.spectral_bits == want.spectral_bits);
    CHECK(got.scalefac_bits == want.scalefac_bits);
    CHECK(got.noise_bits == want.noise_bits);
    CHECK(got.bits() == want.bits());
}

}  // namespace

TEST_CASE("AC-4 track coder bit count equals the full track's over random frames", "[ac4][encoder][track_coder]") {
    Random rng(0x54524b43u);
    const std::array<FrameLayout, 4> layouts{
        iclforge::ac4::detail::long_layout(1920),
        iclforge::ac4::detail::split_layout(1920, {2, 2}, {1, -1}),
        iclforge::ac4::detail::split_layout(1920, {0, 2}, {3, 1}),
        iclforge::ac4::detail::split_layout(1920, {1, 3}, {-1, -1}),
    };
    TrackCoder coder;
    int noisy = 0;
    for (int frame = 0; frame < 320; ++frame) {
        const FrameLayout& layout = layouts[static_cast<std::size_t>(frame % 4)];
        const bool noise_fill = frame % 3 == 0;
        const int max_sfb = 20 + rng.below(30);
        const Grouped grouped =
            iclforge::ac4::detail::regroup(random_spectrum(rng, layout), layout, {max_sfb, max_sfb - rng.below(8)});
        std::vector<std::vector<int>> sf(grouped.offset.size());
        for (std::size_t g = 0; g < sf.size(); ++g) {
            for (int b = 0; b < grouped.max_sfb[g]; ++b) {
                sf[g].push_back(40 + rng.below(100));
            }
        }
        const int offset = rng.below(9) - 4;
        const CodedTrack full = iclforge::ac4::detail::code_track(grouped, sf, offset, layout, noise_fill);
        noisy += full.noise_fill ? 1 : 0;
        CAPTURE(frame, noise_fill);
        REQUIRE(coder.bits(grouped, sf, offset, layout, noise_fill) == full.bits());
    }
    // The comparison means something only if some frames did send noise levels.
    CHECK(noisy > 20);
}

TEST_CASE("AC-4 track coder refills reused storage with the track code_track builds", "[ac4][encoder][track_coder]") {
    Random rng(0x52454655u);
    const std::array<FrameLayout, 4> layouts{
        iclforge::ac4::detail::split_layout(1920, {1, 3}, {-1, -1}),
        iclforge::ac4::detail::long_layout(1920),
        iclforge::ac4::detail::split_layout(1920, {2, 2}, {1, -1}),
        iclforge::ac4::detail::long_layout(1920),
    };
    TrackCoder coder;
    CodedTrack kept;  // one track for every frame: its vectors are refilled, never rebuilt
    for (int frame = 0; frame < 320; ++frame) {
        const FrameLayout& layout = layouts[static_cast<std::size_t>(frame % 4)];
        // Noise fill on and off in runs, so a track that had noise levels is refilled by one that has none.
        const bool noise_fill = (frame / 5) % 2 == 0;
        const int max_sfb = 15 + rng.below(35);
        const Grouped grouped =
            iclforge::ac4::detail::regroup(random_spectrum(rng, layout), layout, {max_sfb, max_sfb});
        std::vector<std::vector<int>> sf(grouped.offset.size());
        for (std::size_t g = 0; g < sf.size(); ++g) {
            for (int b = 0; b < grouped.max_sfb[g]; ++b) {
                sf[g].push_back(40 + rng.below(100));
            }
        }
        const int offset = rng.below(9) - 4;
        CAPTURE(frame, noise_fill);
        // A bit count in between, as the rate loop asks for many before it settles on a step.
        (void)coder.bits(grouped, sf, offset + 1, layout, noise_fill);
        coder.code(kept, grouped, sf, offset, layout, noise_fill);
        require_same(kept, iclforge::ac4::detail::code_track(grouped, sf, offset, layout, noise_fill));
    }
}
