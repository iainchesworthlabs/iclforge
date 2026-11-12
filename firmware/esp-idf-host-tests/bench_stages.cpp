// What each stage of the player's path from a decoded block to the sink costs a call, and a sample,
// on the host: the libraries' own stages (the renderer, the routing, the trims and delays, the
// playout's push) and the two loops burst_player.cpp has of its own (the conversion of a unit's
// integers to float planes, and the meter), each run on blocks of 40, 256 and 960 samples. Printed
// as nanoseconds a call at each length, and the straight line through the 40 and the 960 (a fixed
// cost a call, and a cost a sample). The host is a desktop core, not the part: the SHAPE is what it
// shows (which stages are paid a call and which a sample), not the figures.
//
//   bench_stages [layout]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/playout.hpp"
#include "iclforge/render/output_trim.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/routing.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using iclforge::Playout;
using iclforge::render::LayoutRenderer;
using iclforge::render::OutputLayout;
using iclforge::render::OutputTrim;
using iclforge::render::Routing;

constexpr std::size_t kMax = 960;

class NullSink final : public iclforge::PlayoutSink {
   public:
    std::optional<iclforge::PlayoutWrite> write(std::span<const std::span<const float>> /*outputs*/) override {
        play_ += 5333;
        return iclforge::PlayoutWrite{.play_us = play_, .late = false, .gap = false};
    }

   private:
    std::int64_t play_ = 1'000'000;
};

// Nanoseconds a call of `fn`, the least of five repeats of `iterations`.
template <class Fn>
double nanoseconds(Fn&& fn, int iterations) {
    double best = 1e30;
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto begin = Clock::now();
        for (int i = 0; i < iterations; ++i) {
            fn();
        }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / iterations;
        best = std::min(best, ns);
    }
    return best;
}

void report(const char* stage, const std::array<double, 3>& ns) {
    const double per_sample = (ns[2] - ns[0]) / (960.0 - 40.0);
    const double per_call = ns[0] - (40.0 * per_sample);
    std::printf("%-26s %10.0f %10.0f %10.0f   %9.0f ns a call + %6.2f ns a sample\n", stage, ns[0], ns[1], ns[2], per_call,
                per_sample);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string layout_text = argc > 1 ? argv[1] : "5.1";
    const auto layout = OutputLayout::parse(layout_text);
    if (!layout) {
        return 2;
    }
    constexpr std::array<std::size_t, 3> kLengths{40, 256, 960};
    constexpr std::size_t kChannels = 6;
    const std::size_t slots = layout->slots();

    // The input: 6 channels of a unit as the decoder gives them, interleaved.
    std::vector<std::int32_t> pcm(kMax * kChannels);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int32_t>((static_cast<std::int64_t>(i) * 2654435761LL) % 8388608) - 4194304;
    }
    std::vector<std::vector<float>> planes(kChannels, std::vector<float>(kMax));
    std::vector<std::vector<float>> rendered_store(slots, std::vector<float>(kMax));
    std::vector<std::vector<float>> output_store(slots, std::vector<float>(kMax));
    std::vector<std::vector<float>> staging(slots, std::vector<float>(Playout::kBlockFrames));

    iclforge::ac3::eac3::chanmap::Layout bed;
    using Location = iclforge::ac3::eac3::chanmap::Location;
    for (const Location l : {Location::kLeft, Location::kRight, Location::kCentre, Location::kLfe, Location::kLeftSurround,
                             Location::kRightSurround}) {
        bed.items[static_cast<std::size_t>(bed.count++)] = l;
    }
    LayoutRenderer renderer(*layout, 48000);
    renderer.set_bed(bed);
    Routing routing = Routing::identity(slots, slots).value_or(Routing{});
    OutputTrim trim;
    std::vector<float> delay_storage(OutputTrim::kMaxOutputs * 960 + 64);
    trim.setup(delay_storage, slots, 960, 48000);
    trim.set_table({});
    trim.configure(*layout, routing, slots);
    // Distances that put delays on the nearer speakers.
    iclforge::render::speaker_trim::SpeakerTrimTable table;
    (void)table.apply_text("L distance=2.4; R distance=2.4; C distance=2.8; Ls distance=1.9; Rs distance=2.0");
    trim.set_table(table);
    NullSink sink;
    std::vector<float> storage(slots * Playout::kBlockFrames);
    Playout playout(storage, slots);
    std::int64_t target = 2'000'000;
    std::uint64_t frame = 0;

    std::array<std::span<float>, 16> rendered_spans{};
    std::array<std::span<const float>, 16> rendered_views{};
    std::array<std::span<float>, 16> out_spans{};
    std::array<std::span<const float>, 16> out_views{};
    std::array<std::span<const float>, 16> plane_views{};
    std::array<double, 16> window_sum{};
    std::array<float, 16> window_peak{};
    double sink_sum = 0.0;

    std::printf("layout %s, %zu slots, 6 coded channels; ns a call at 40, 256, 960 samples\n", layout_text.c_str(), slots);
    std::printf("%-26s %10s %10s %10s\n", "stage", "n=40", "n=256", "n=960");
    std::array<std::array<double, 3>, 6> all{};
    const char* names[] = {"convert (int to float)", "render_reached", "routing.apply", "trim.process (delays)",
                           "meter (floats, 16-sums)", "playout.push (null sink)"};
    for (std::size_t li = 0; li < kLengths.size(); ++li) {
        const std::size_t n = kLengths[li];
        const int iterations = static_cast<int>(2'000'000 / (n + 200));
        // convert
        all[0][li] = nanoseconds(
            [&] {
                constexpr float kScale = 1.0F / 8388608.0F;
                for (std::size_t c = 0; c < kChannels; ++c) {
                    float* const into = planes[c].data();
                    for (std::size_t s = 0; s < n; ++s) {
                        into[s] = static_cast<float>(pcm[(s * kChannels) + c]) * kScale;
                    }
                }
            },
            iterations);
        for (std::size_t c = 0; c < kChannels; ++c) {
            plane_views[c] = std::span<const float>(planes[c].data(), n);
        }
        for (std::size_t s = 0; s < slots; ++s) {
            rendered_spans[s] = std::span<float>(rendered_store[s].data(), n);
            rendered_views[s] = std::span<const float>(rendered_store[s].data(), n);
            out_spans[s] = std::span<float>(output_store[s].data(), n);
            out_views[s] = std::span<const float>(output_store[s].data(), n);
        }
        const iclforge::render::PcmBlock block{.index = 0,
                                               .blocks = 0,
                                               .channels = std::span<const std::span<const float>>(plane_views.data(), kChannels),
                                               .objects = {},
                                               .object_indices = {},
                                               .object_metadata = nullptr};
        std::uint32_t reached = 0;
        all[1][li] = nanoseconds(
            [&] { reached = renderer.render_reached(block, false, 1.0F, std::span<const std::span<float>>(rendered_spans.data(), slots)); },
            iterations);
        const std::uint32_t live = (std::uint32_t{1} << slots) - 1U;
        all[2][li] = nanoseconds(
            [&] {
                routing.apply(std::span<const std::span<const float>>(rendered_views.data(), slots),
                              std::span<const std::span<float>>(out_spans.data(), slots), live);
            },
            iterations);
        all[3][li] = nanoseconds([&] { trim.process(std::span<const std::span<float>>(out_spans.data(), slots), live); }, iterations);
        all[4][li] = nanoseconds(
            [&] {
                for (std::size_t o = 0; o < slots; ++o) {
                    const std::span<float> samples = out_spans[o];
                    float peak = window_peak[o];
                    double sum = 0.0;
                    std::size_t k = 0;
                    for (; k + 16 <= n; k += 16) {
                        float partial = 0.0F;
                        for (std::size_t j = 0; j < 16; ++j) {
                            const float x = samples[k + j];
                            partial += x * x;
                            peak = std::max(peak, std::fabs(x));
                        }
                        sum += static_cast<double>(partial);
                    }
                    for (; k < n; ++k) {
                        const float x = samples[k];
                        sum += static_cast<double>(x * x);
                        peak = std::max(peak, std::fabs(x));
                    }
                    window_peak[o] = peak;
                    window_sum[o] += sum;
                }
            },
            iterations);
        all[5][li] = nanoseconds(
            [&] {
                playout.push(std::span<const std::span<const float>>(out_views.data(), slots), n, target, frame, 1'000'000, sink, 0);
                target += static_cast<std::int64_t>(n) * 1'000'000 / 48000;
                frame += n;
            },
            iterations);
        sink_sum += static_cast<double>(reached);
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        report(names[i], all[i]);
    }
    return sink_sum < 0.0 ? 1 : 0;
}
