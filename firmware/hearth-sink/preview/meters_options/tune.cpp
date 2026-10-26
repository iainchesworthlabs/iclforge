// The analysis of spectrum.hpp held to what it claims, and priced: no LVGL.
//
//   meters_tune
//     1. calibration: a full-scale sine reads 0 dB in its band, a -20 dBFS sine -20 dB, white noise rises 1 dB a band
//     2. the two-rate analysis against one 8192-point FFT over the synthetic scene (scene.hpp): the difference per band
//     3. what an analysis costs on this computer, and the operations it is on the board

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"

#include "../speakers_options/scene.hpp"
#include "spectrum.hpp"

namespace rp = iclforge::render;

namespace {

constexpr double kPi = 3.14159265358979323846;

int band_of(float hz) {
    int best = 0;
    for (int b = 0; b < meters::kBands; ++b) {
        if (std::fabs(std::log2(hz / meters::kCentre[static_cast<std::size_t>(b)])) < std::fabs(std::log2(hz / meters::kCentre[static_cast<std::size_t>(best)]))) {
            best = b;
        }
    }
    return best;
}

void feed_blocks(meters::Analyzer& a, meters::Reference& r, const std::vector<float>& x) {
    a.push(x);
    r.push(x);
}

void calibration() {
    std::printf("--- calibration (two-rate analysis / 8192-point reference), dB\n");
    std::printf("%8s %8s | %9s %6s | %9s %6s\n", "tone Hz", "level", "band read", "at", "ref read", "at");
    for (const float hz : {25.0F, 63.0F, 100.0F, 250.0F, 500.0F, 800.0F, 1000.0F, 2000.0F, 4000.0F, 10000.0F, 16000.0F}) {
        for (const float amp : {1.0F, 0.1F}) {
            meters::Analyzer a;
            meters::Reference r;
            std::vector<float> x(meters::kRingLength);
            for (std::size_t n = 0; n < x.size(); ++n) {
                x[n] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(n) / meters::kRate));
            }
            feed_blocks(a, r, x);
            std::array<float, meters::kBands> da{};
            std::array<float, meters::kBands> dr{};
            a.analyze(da);
            r.analyze(dr);
            const auto top = [](const std::array<float, meters::kBands>& d) { return static_cast<int>(std::max_element(d.begin(), d.end()) - d.begin()); };
            const int ta = top(da);
            const int tr = top(dr);
            std::printf("%8.0f %8.1f | %9.1f %6.0f | %9.1f %6.0f\n", hz, 20.0 * std::log10(amp), da[static_cast<std::size_t>(ta)], meters::kCentre[static_cast<std::size_t>(ta)],
                        dr[static_cast<std::size_t>(tr)], meters::kCentre[static_cast<std::size_t>(tr)]);
        }
    }
    // White noise: the same power per Hz, so a third-octave band holds more of it the higher it is: +1.0 dB a band (a band
    // is 2^(1/3) wider than the one below). One frame of noise is a few degrees of freedom a band, so the power of many
    // independent frames is averaged before the level is read.
    meters::Analyzer a;
    meters::Reference r;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
    std::array<double, meters::kBands> pa{};
    std::array<double, meters::kBands> pr{};
    constexpr int kFrames = 400;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> noise(meters::kRingLength);
        for (float& v : noise) {
            v = 0.5F * uni(rng);
        }
        feed_blocks(a, r, noise);
        std::array<float, meters::kBands> da{};
        std::array<float, meters::kBands> dr{};
        a.analyze(da);
        r.analyze(dr);
        for (std::size_t b = 0; b < meters::kBands; ++b) {
            pa[b] += 0.5 * std::pow(10.0, da[b] / 10.0) / kFrames;
            pr[b] += 0.5 * std::pow(10.0, dr[b] / 10.0) / kFrames;
        }
    }
    std::printf("white noise (mean square %.4f), the power of %d independent frames averaged: level and step from the band below\n", 0.25 / 3.0, kFrames);
    const auto db = [](double p) { return 10.0 * std::log10(p / 0.5); };
    for (const int b : {2, 5, 10, 14, 15, 16, 17, 18, 19, 20, 25, 29}) {
        const auto i = static_cast<std::size_t>(b);
        std::printf("  %7.1f Hz  two-rate %6.1f (%+.2f)   reference %6.1f (%+.2f)\n", meters::kCentre[i], db(pa[i]), db(pa[i]) - db(pa[i - 1]), db(pr[i]),
                    db(pr[i]) - db(pr[i - 1]));
    }
}

std::vector<float> mono_mix(const scene::Scene& s, std::size_t outputs) {
    // The tap: a sum of the outputs shifted down by half of log2 of their number, rounded up (the board's integers).
    int shift = 0;
    while ((1ULL << (2 * static_cast<unsigned>(shift))) < outputs) {
        ++shift;
    }
    std::vector<float> mix(scene::kWindow, 0.0F);
    const auto& out = s.outputs();
    for (std::size_t o = 0; o < outputs && o < out.size(); ++o) {
        for (int k = 0; k < scene::kWindow; ++k) {
            mix[static_cast<std::size_t>(k)] += out[o][static_cast<std::size_t>(k)];
        }
    }
    const float scale = 1.0F / static_cast<float>(1ULL << shift);
    for (float& v : mix) {
        v *= scale;
    }
    return mix;
}

void against_reference_with(const char* label, float tau) {
    struct Room {
        const char* layout;
        scene::Content content;
    };
    std::printf("%s, averaged over %.0f ms: mean |difference| from the 8192-point reference (averaged the same), dB\n", label, tau * 1000.0F);
    std::printf("  %-7s %9s %9s %9s %9s\n", "room", "20-800 Hz", "1-2.5 kHz", "3.1-20k", "worst");
    for (const Room room : {Room{"2.0", scene::Content::kStereo}, Room{"5.1", scene::Content::kBed}, Room{"7.1.4", scene::Content::kBedAndObjects}}) {
        const auto parsed = rp::OutputLayout::parse(room.layout);
        const auto spots = rp::plan_spots(*parsed);
        scene::Scene s(spots, room.content);
        meters::Analyzer a;
        meters::Reference r;
        meters::PowerAverage avg_a;
        meters::PowerAverage avg_r;
        double sum[3] = {0, 0, 0};
        long count[3] = {0, 0, 0};
        float worst = 0.0F;
        const std::size_t outputs = spots.size();
        for (int w = 0; w < 600; ++w) {
            s.next();
            const std::vector<float> mix = mono_mix(s, outputs);
            for (int h = 0; h < 2; ++h) {
                const std::span<const float> hop(mix.data() + h * 2400, 2400);
                a.push(hop);
                r.push(hop);
                std::array<float, meters::kBands> da{};
                std::array<float, meters::kBands> dr{};
                a.analyze(da);
                r.analyze(dr);
                std::array<float, meters::kBands> sa{};
                std::array<float, meters::kBands> sr{};
                avg_a.feed(da, 0.05F, tau, sa);
                avg_r.feed(dr, 0.05F, tau, sr);
                if (w < 30) {
                    continue;
                }
                for (std::size_t b = 0; b < meters::kBands; ++b) {
                    if (sr[b] > -60.0F) {
                        const int g = b < static_cast<std::size_t>(meters::kLowBands) ? 0 : b < 22 ? 1 : 2;
                        const float d = std::fabs(sa[b] - sr[b]);
                        sum[g] += d;
                        ++count[g];
                        worst = std::max(worst, d);
                    }
                }
            }
        }
        std::printf("  %-7s %9.2f %9.2f %9.2f %9.1f\n", room.layout, sum[0] / static_cast<double>(std::max(1L, count[0])), sum[1] / static_cast<double>(std::max(1L, count[1])),
                    sum[2] / static_cast<double>(std::max(1L, count[2])), worst);
    }
}

void against_reference() {
    std::printf("--- the two-rate analysis against the 8192-point reference over the scene\n");
    against_reference_with("high frame 1024 (21 ms)", 0.001F);
    against_reference_with("high frame 1024 (21 ms)", 0.1F);
    against_reference_with("high frame 1024 (21 ms)", 0.2F);
}

void timing() {
    std::printf("--- cost of one analysis on this computer (MSVC, /O2, one core; the board is another machine)\n");
    // The shipped classes on a ready history of 16-bit samples, as the board calls them (the prototype's wrappers also convert a float ring).
    const auto a = std::make_unique<iclforge::dsp::spectrum::Analyzer>();
    const auto r = std::make_unique<iclforge::dsp::spectrum::Reference>();
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> uni(-0.5F, 0.5F);
    std::array<std::int16_t, meters::kRingLength> x{};
    for (std::int16_t& v : x) {
        v = static_cast<std::int16_t>(std::lround(uni(rng) * 32767.0F));
    }
    std::array<float, meters::kBands> d{};
    constexpr int kRuns = 2000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        a->analyze(x, d);
    }
    const double two_rate = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kRuns;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        r->analyze(x, d);
    }
    const double single = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kRuns;
    std::printf("two-rate (2048 at 12 kHz + 1024 at 48 kHz): %.1f us    one 8192-point: %.1f us    ratio %.2f\n", two_rate, single, single / two_rate);
    // Operations, by the passes: a radix-4 butterfly is 3 complex products (18 flops) and 8 complex sums (16): 34; a radix-2 one 10.
    const auto complex_fft = [](std::size_t n) {
        double flops = 0.0;
        for (std::size_t m = n; m > 1; m /= (m % 4 == 0 ? 4 : 2)) {
            flops += (m % 4 == 0) ? static_cast<double>(n) / 4.0 * 34.0 : static_cast<double>(n) / 2.0 * 10.0;
        }
        return flops;
    };
    // The unpacking of a real frame is about 12 flops a bin, the window 2 a sample, the power 3 a bin; four samples summed to one
    // is 1 an input sample.
    const auto frame_flops = [&](std::size_t frame, std::size_t decimate) {
        const auto half = static_cast<double>(frame / 2);
        return complex_fft(frame / 2) + 12.0 * half + 2.0 * static_cast<double>(frame) + 3.0 * half +
               (decimate > 1 ? static_cast<double>(frame * decimate) : 0.0);
    };
    std::printf("operations a pass: low 2048 at 12 kHz %.0f k, high 1024 %.0f k, 8192 single %.0f k flops\n", frame_flops(2048, 4) / 1e3,
                frame_flops(1024, 1) / 1e3, frame_flops(8192, 1) / 1e3);
    std::printf("two-rate total %.0f k flops\n", (frame_flops(2048, 4) + frame_flops(1024, 1)) / 1e3);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argv;
    // With any argument: the calibration only (tones and noise); with none: the comparison and the cost.
    if (argc > 1) {
        calibration();
        return 0;
    }
    against_reference();
    timing();
    return 0;
}
