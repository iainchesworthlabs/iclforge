// What a player does to each output between the patch and the sink (render/output_trim.hpp): the listener's
// table, a server's settings that replace it, a change while a song plays.
//
// The property that matters most is the quiet one: with nothing set the samples out are the samples in, bit for
// bit, which is what keeps every stream a board played yesterday sounding as it did. After that, the delay of each
// speaker to the sample, that the farthest speaker is never touched, and that a change of the table moves only what
// it changes.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/output_trim.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/render/trim_delay.hpp"

namespace {

using iclforge::render::OutputLayout;
using iclforge::render::OutputTrim;
using iclforge::render::Routing;
using iclforge::render::SpeakerTrimTable;
using iclforge::render::TrimDelay;
namespace st = iclforge::render::speaker_trim;
using Catch::Approx;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kBlock = 256;
constexpr std::size_t kMaxDelay = 1440;  // 30 ms

OutputLayout layout_of(const char* text) {
    const auto layout = OutputLayout::parse(text);
    REQUIRE(layout.has_value());
    return *layout;
}

Routing identity_for(const OutputLayout& layout) {
    const auto routing = Routing::identity(layout.slots(), layout.slots());
    REQUIRE(routing.has_value());
    return *routing;
}

SpeakerTrimTable table_of(const char* body) {
    SpeakerTrimTable table;
    const auto result = table.apply_text(body);
    INFO(result.error);
    REQUIRE(result.ok);
    return table;
}

// An OutputTrim over its own delay lines.
struct Stage {
    std::vector<float> storage;
    OutputTrim trim;

    explicit Stage(std::size_t max_delay = kMaxDelay, std::uint32_t rate = kRate)
        : storage(OutputTrim::kMaxOutputs * max_delay) {
        trim.setup(storage, OutputTrim::kMaxOutputs, max_delay, rate);
    }
};

// A ramp 1, 2, 3, ... on every output, in blocks of `block`, through `trim` with every output live, until `total`
// samples have gone through. What came out, per output.
std::vector<std::vector<float>> run(OutputTrim& trim, std::size_t outputs, std::size_t total, std::size_t block = kBlock,
                                    std::size_t first = 1) {
    std::vector<std::vector<float>> result(outputs);
    std::vector<std::vector<float>> planes(outputs, std::vector<float>(block));
    std::vector<std::span<float>> spans(outputs);
    std::size_t next = first;
    for (std::size_t done = 0; done < total; done += block) {
        const std::size_t n = std::min(block, total - done);
        for (std::size_t o = 0; o < outputs; ++o) {
            for (std::size_t k = 0; k < n; ++k) {
                planes[o][k] = static_cast<float>(next + k);
            }
            spans[o] = std::span<float>(planes[o].data(), n);
        }
        next += n;
        trim.process(spans, (std::uint32_t{1} << outputs) - 1U);
        for (std::size_t o = 0; o < outputs; ++o) {
            result[o].insert(result[o].end(), planes[o].begin(), planes[o].begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    return result;
}

}  // namespace

TEST_CASE("with nothing set the samples out are the samples in, bit for bit", "[render][output-trim]") {
    const OutputLayout layout = layout_of("7.1.4");
    Stage stage;
    stage.trim.configure(layout, identity_for(layout), layout.slots());
    REQUIRE_FALSE(stage.trim.active());
    REQUIRE(stage.trim.delayed_outputs() == 0);
    REQUIRE(stage.trim.clamped() == 0);
    REQUIRE_FALSE(stage.trim.server_level());
    REQUIRE_FALSE(stage.trim.server_delay());
    for (std::size_t o = 0; o < layout.slots(); ++o) {
        REQUIRE(stage.trim.level_db(o) == 0.0);
        REQUIRE(stage.trim.delay_samples(o) == 0);
        REQUIRE(stage.trim.gain(o) == 1.0F);
    }
    // Awkward floats, not a ramp: a trim that touched them would show.
    std::vector<std::vector<float>> planes(layout.slots(), std::vector<float>(kBlock));
    for (std::size_t o = 0; o < planes.size(); ++o) {
        for (std::size_t k = 0; k < kBlock; ++k) {
            planes[o][k] = std::sin(static_cast<float>((o * 977) + (k * 13))) * 0.7311F;
        }
    }
    const auto original = planes;
    std::vector<std::span<float>> spans;
    for (auto& plane : planes) {
        spans.emplace_back(plane);
    }
    stage.trim.process(spans, 0xFFFU);
    REQUIRE(planes == original);

    // And a table with defaults written in it is the same as no table.
    SpeakerTrimTable explicit_defaults;
    REQUIRE(explicit_defaults.set_level_db(0, 0.0));
    stage.trim.set_table(explicit_defaults);
    REQUIRE_FALSE(stage.trim.active());
    stage.trim.process(spans, 0xFFFU);
    REQUIRE(planes == original);
}

TEST_CASE("the table puts each speaker its distance behind the farthest, to the sample, whatever the block", "[render][output-trim]") {
    const OutputLayout layout = layout_of("5.1");  // L C R Ls Rs LFE
    for (const std::size_t block : {256U, 100U, 7U, 1U}) {
        INFO(block);
        Stage stage;
        stage.trim.set_table(table_of("L distance=3.2; R distance=3.2; C distance=2.8; Ls distance=1.9; Rs distance=2.1; LFE distance=3.6"));
        stage.trim.configure(layout, identity_for(layout), 6);
        REQUIRE(stage.trim.farthest_cm() == 360);
        REQUIRE(stage.trim.delayed_outputs() == 0b011111U);
        const std::array<std::size_t, 6> expect = {56, 112, 56, 238, 210, 0};
        for (std::size_t o = 0; o < 6; ++o) {
            REQUIRE(stage.trim.delay_samples(o) == expect[o]);
        }
        const auto out = run(stage.trim, 6, 1500, block);
        for (std::size_t o = 0; o < 6; ++o) {
            for (std::size_t k = 0; k < 1500; ++k) {
                const float want = k < expect[o] ? 0.0F : static_cast<float>(k - expect[o] + 1);
                if (out[o][k] != want) {
                    FAIL("output " << o << " sample " << k << " is " << out[o][k] << ", expected " << want);
                }
            }
        }
    }
}

TEST_CASE("levels are exactly the gains they name, and a delayed speaker is trimmed after its delay", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage;
    stage.trim.set_table(table_of("L level=-6 distance=2.0; R level=+3.5 distance=2.5"));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.level_db(0) == Approx(-6.0));
    REQUIRE(stage.trim.level_db(1) == Approx(3.5));
    // L is the nearer: delayed by 50 cm, 70 samples. R is the farther: not delayed.
    REQUIRE(stage.trim.delay_samples(0) == 70);
    REQUIRE(stage.trim.delay_samples(1) == 0);
    const auto out = run(stage.trim, 2, 400);
    const float gain_l = stage.trim.gain(0);
    const float gain_r = stage.trim.gain(1);
    REQUIRE(gain_l == Approx(0.501187).epsilon(1e-6));
    for (std::size_t k = 0; k < 400; ++k) {
        REQUIRE(out[1][k] == static_cast<float>(k + 1) * gain_r);
        const float want_l = k < 70 ? 0.0F : static_cast<float>(k - 70 + 1) * gain_l;
        REQUIRE(out[0][k] == want_l);
    }
}

TEST_CASE("a trim follows its speaker when the layout or the patch changes", "[render][output-trim]") {
    Stage stage;
    stage.trim.set_table(table_of("Ls level=-3; Rs level=+3; L level=-1"));
    const OutputLayout five = layout_of("5.1");
    stage.trim.configure(five, identity_for(five), 6);
    REQUIRE(stage.trim.role(3) == *st::parse_role("Ls"));
    REQUIRE(stage.trim.level_db(3) == Approx(-3.0));
    REQUIRE(stage.trim.level_db(4) == Approx(3.0));

    const OutputLayout seven = layout_of("7.1.4");
    stage.trim.configure(seven, identity_for(seven), 12);
    REQUIRE(stage.trim.level_db(3) == Approx(-3.0));
    REQUIRE(stage.trim.level_db(4) == Approx(3.0));
    REQUIRE(stage.trim.level_db(5) == 0.0);  // Lrs: nothing set for it

    // A patch that swaps the first two outputs.
    const OutputLayout two = layout_of("2.0");
    const std::array<int, 2> swapped = {1, 0};
    const auto routing = Routing::from_outputs(swapped, 2);
    REQUIRE(routing.has_value());
    stage.trim.configure(two, *routing, 2);
    REQUIRE(stage.trim.level_db(1) == Approx(-1.0));  // L is at output 1 now
    REQUIRE(stage.trim.level_db(0) == 0.0);
    REQUIRE(stage.trim.role(1) == *st::parse_role("L"));

    // Outputs the layout does not reach have no speaker.
    stage.trim.configure(five, identity_for(five), 8);
    REQUIRE(stage.trim.role(7) == st::kNoRole);
    REQUIRE(stage.trim.outputs() == 8);
}

TEST_CASE("a server's trims replace the table's levels alone, and its delays the table's delays alone", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage;
    stage.trim.set_table(table_of("L level=-6 distance=2.0; R level=+3 distance=2.5"));

    // Trims only: the levels are the server's, the delays are still the table's.
    const std::array<double, 2> trims = {1.0, -2.0};
    stage.trim.set_server(std::span<const double>(trims), std::nullopt);
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.server_level());
    REQUIRE_FALSE(stage.trim.server_delay());
    REQUIRE(stage.trim.level_db(0) == Approx(1.0));
    REQUIRE(stage.trim.level_db(1) == Approx(-2.0));
    REQUIRE(stage.trim.delay_samples(0) == 70);

    // Delays only: 1 ms and 0.
    const std::array<double, 2> delays = {1.0, 0.0};
    stage.trim.set_server(std::nullopt, std::span<const double>(delays));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE_FALSE(stage.trim.server_level());
    REQUIRE(stage.trim.server_delay());
    REQUIRE(stage.trim.level_db(0) == Approx(-6.0));
    REQUIRE(stage.trim.delay_samples(0) == 48);
    REQUIRE(stage.trim.delay_samples(1) == 0);
    REQUIRE(stage.trim.clamped() == 0);

    // Both, with lists shorter than the outputs: the rest are 0.
    const std::array<double, 1> one = {4.0};
    stage.trim.set_server(std::span<const double>(one), std::span<const double>(one));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.level_db(0) == Approx(4.0));
    REQUIRE(stage.trim.level_db(1) == 0.0);
    REQUIRE(stage.trim.delay_samples(0) == 192);
    REQUIRE(stage.trim.delay_samples(1) == 0);

    // The server's settings go: the table is back at the next configure.
    stage.trim.set_server(std::nullopt, std::nullopt);
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.level_db(0) == Approx(-6.0));
    REQUIRE(stage.trim.level_db(1) == Approx(3.0));
    REQUIRE(stage.trim.delay_samples(0) == 70);

    // A server's delay is cut to what the build holds, and is not the table's doing.
    const std::array<double, 2> too_long = {500.0, 0.0};
    stage.trim.set_server(std::nullopt, std::span<const double>(too_long));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.delay_samples(0) == kMaxDelay);
    REQUIRE(stage.trim.clamped() == 0);
}

TEST_CASE("a change of the table while a stream plays moves only what it changes", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage;
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5"));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.delay_samples(0) == 70);

    // A level changed mid-stream: R's delay is zero and L's does not change, so neither line is touched and both
    // go on sample for sample. R's level ramps over the next block.
    (void)run(stage.trim, 2, 1024);
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5; R level=-6"));
    REQUIRE(stage.trim.delay_samples(0) == 70);
    auto second = run(stage.trim, 2, 1024, kBlock, 1025);
    for (std::size_t k = 0; k < 1024; ++k) {
        // L: the same delayed ramp across the join - 70 samples behind.
        REQUIRE(second[0][k] == static_cast<float>(1025 + k - 70));
    }
    // R: from unity to -6 dB over the first block, then steady.
    REQUIRE(second[1][0] > 1025.0F * 0.9F);
    const float gain = stage.trim.gain(1);
    REQUIRE(gain == Approx(0.501187).epsilon(1e-6));
    REQUIRE(second[1][kBlock - 1] == Approx(static_cast<float>(1025 + kBlock - 1) * gain).epsilon(1e-5));
    for (std::size_t k = kBlock; k < 1024; ++k) {
        REQUIRE(second[1][k] == static_cast<float>(1025 + k) * gain);
    }

    // A distance changed: only the output whose delay changes loses its line; the other plays on.
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5; R level=-6; L distance=2.25"));
    REQUIRE(stage.trim.delay_samples(0) == st::delay_samples_for(25, kRate));
    auto third = run(stage.trim, 2, 1024, kBlock, 2049);
    for (std::size_t k = 0; k < 1024; ++k) {
        REQUIRE(third[1][k] == static_cast<float>(2049 + k) * gain);
    }
    // L's line was emptied: silent for the new delay's length, then the stream again, delayed by it.
    const std::size_t d = st::delay_samples_for(25, kRate);
    for (std::size_t k = 0; k < d; ++k) {
        REQUIRE(third[0][k] == 0.0F);
    }
    for (std::size_t k = d; k < 1024; ++k) {
        REQUIRE(third[0][k] == static_cast<float>(2049 + k - d));
    }
}

TEST_CASE("the farthest speaker is untouched, so the play-time stamp is the farthest speaker's", "[render][output-trim]") {
    const OutputLayout layout = layout_of("5.0");
    Stage stage;
    stage.trim.set_table(table_of("L distance=3.0; R distance=3.0; C distance=2.0; Ls distance=4.5; Rs distance=4.5"));
    stage.trim.configure(layout, identity_for(layout), 5);
    // Ls and Rs are the farthest: no delay, and the samples through them are the samples in.
    REQUIRE(stage.trim.delay_samples(3) == 0);
    REQUIRE(stage.trim.delay_samples(4) == 0);
    const auto out = run(stage.trim, 5, 600);
    for (std::size_t k = 0; k < 600; ++k) {
        REQUIRE(out[3][k] == static_cast<float>(k + 1));
        REQUIRE(out[4][k] == static_cast<float>(k + 1));
    }
    // Every other speaker is later than the stamp, never earlier.
    for (std::size_t o = 0; o < 3; ++o) {
        REQUIRE(stage.trim.delay_samples(o) > 0);
    }
}

TEST_CASE("a build without delay lines still trims levels, and says the delays were not given", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage(0);
    REQUIRE(stage.trim.max_delay_samples() == 0);
    stage.trim.set_table(table_of("L level=-3 distance=2.0; R level=+1 distance=3.0"));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.level_db(0) == Approx(-3.0));
    REQUIRE(stage.trim.delay_samples(0) == 0);
    REQUIRE(stage.trim.delay_samples(1) == 0);
    REQUIRE(stage.trim.delayed_outputs() == 0);
    REQUIRE(stage.trim.clamped() == 1);  // L wanted 100 cm of delay
    const auto out = run(stage.trim, 2, 100);
    REQUIRE(out[0][50] == 51.0F * stage.trim.gain(0));
    REQUIRE(out[1][50] == 51.0F * stage.trim.gain(1));
}

TEST_CASE("a delay past the build's limit is cut to it, and the output is named", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage(960);  // 20 ms
    stage.trim.set_table(table_of("L distance=1.0; R distance=10.0"));
    stage.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(stage.trim.delay_samples(0) == 960);
    REQUIRE(stage.trim.clamped() == 0b01U);
    REQUIRE(stage.trim.farthest_cm() == 1000);

    // A build with more room does not cut it.
    Stage roomy;
    roomy.trim.set_table(table_of("L distance=1.0; R distance=10.0"));
    roomy.trim.configure(layout, identity_for(layout), 2);
    REQUIRE(roomy.trim.delay_samples(0) == st::delay_samples_for(900, kRate));
    REQUIRE(roomy.trim.clamped() == 0);
}

TEST_CASE("distances are samples at the rate the stream plays at", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    for (const std::uint32_t rate : {44100U, 48000U, 96000U}) {
        INFO(rate);
        Stage stage(TrimDelay::samples_for_ms(30.0, rate), rate);
        stage.trim.set_table(table_of("L distance=1.0; R distance=4.43"));
        stage.trim.configure(layout, identity_for(layout), 2);
        // 3.43 m of extra distance is 10 ms.
        REQUIRE(stage.trim.delay_samples(0) == TrimDelay::samples_for_ms(10.0, rate));
        REQUIRE(stage.trim.sample_rate_hz() == rate);
    }
}

TEST_CASE("a new stream empties the lines and ends a ramp", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage;
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5"));
    stage.trim.configure(layout, identity_for(layout), 2);
    (void)run(stage.trim, 2, 512);
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5; R level=-12"));
    stage.trim.reset();
    const auto out = run(stage.trim, 2, 256);
    // L: silent for 70 samples again - the old stream's tail is gone. R: the new level from its first sample.
    for (std::size_t k = 0; k < 70; ++k) {
        REQUIRE(out[0][k] == 0.0F);
    }
    REQUIRE(out[0][70] == 1.0F);
    REQUIRE(out[1][0] == stage.trim.gain(1));
}

TEST_CASE("only the live outputs are processed, and a delayed output is one a caller keeps live", "[render][output-trim]") {
    const OutputLayout layout = layout_of("2.0");
    Stage stage;
    stage.trim.set_table(table_of("L distance=2.0; R distance=2.5; R level=-6"));
    stage.trim.configure(layout, identity_for(layout), 2);
    std::vector<float> a(64, 1.0F);
    std::vector<float> b(64, 1.0F);
    const std::array<std::span<float>, 2> planes = {std::span<float>(a), std::span<float>(b)};
    // L is nearer, so delayed, and the caller must keep it live: it is in the delayed mask. R is not.
    REQUIRE(stage.trim.delayed_outputs() == 0b01U);
    // A block in which only R is live leaves L's span alone.
    stage.trim.process(planes, 0b10U);
    REQUIRE(a == std::vector<float>(64, 1.0F));
    REQUIRE(b[63] == Approx(stage.trim.gain(1)).epsilon(1e-6));
}

TEST_CASE("storage too small for the delay asked for holds what it can", "[render][output-trim]") {
    std::vector<float> small(OutputTrim::kMaxOutputs * 100);
    OutputTrim trim;
    trim.setup(small, OutputTrim::kMaxOutputs, 1440, kRate);
    REQUIRE(trim.max_delay_samples() == 100);
    const OutputLayout layout = layout_of("2.0");
    trim.set_table(table_of("L distance=1.0; R distance=2.0"));
    trim.configure(layout, identity_for(layout), 2);
    REQUIRE(trim.delay_samples(0) == 100);
    REQUIRE(trim.clamped() == 1);
}

TEST_CASE("a player with fewer outputs than the stage's most holds the delay it was given storage for", "[render][output-trim]") {
    // Eight outputs of 1440 samples: the limit is 1440, not 1440 * 8 / 16.
    std::vector<float> storage(8 * kMaxDelay);
    OutputTrim trim;
    trim.setup(storage, 8, kMaxDelay, kRate);
    REQUIRE(trim.max_delay_samples() == kMaxDelay);
    const OutputLayout layout = layout_of("7.1");
    trim.set_table(table_of("L distance=1.0; R distance=10.0"));
    trim.configure(layout, identity_for(layout), 8);
    REQUIRE(trim.delay_samples(0) == st::delay_samples_for(900, kRate));
    REQUIRE(trim.outputs() == 8);
    // More outputs open than storage was given for are not set up.
    trim.configure(layout_of("9.1.6"), identity_for(layout_of("9.1.6")), 16);
    REQUIRE(trim.outputs() == 8);
}

// What the stage costs, per output and block of 256 frames, on the machine the test runs on. Hidden: run it by
// name ("[output-trim-bench]") for the figures planning/hearth-speaker-trim-identify.md quotes. The boards' own
// are the kit's to measure (the burst player's sink_us and decode figures); this gives the proportions between
// the cases, which the boards share.
TEST_CASE("the cost of the stage per output", "[.][output-trim-bench]") {
    struct Case {
        const char* name;
        const char* table;
    };
    const std::array<Case, 4> cases = {Case{"neutral", ""},
                                       Case{"level", "L level=-1.5; R level=-1.5; C level=-1.5"},
                                       Case{"delay", "L distance=2; R distance=2; C distance=3"},
                                       Case{"level and delay", "L level=-1.5 distance=2; R level=-1.5 distance=2; C level=-1.5 distance=3"}};
    for (const std::size_t outputs : {2U, 6U, 16U}) {
        const char* const layout_text = outputs == 2 ? "2.0" : outputs == 6 ? "5.1" : "9.1.6";
        const OutputLayout layout = layout_of(layout_text);
        for (const Case& c : cases) {
            Stage stage;
            if (c.table[0] != '\0') {
                SpeakerTrimTable table = table_of(c.table);
                // Every speaker of the layout, with a distance when the case has any: the cost is per output.
                for (std::size_t o = 0; o < outputs && c.table[0] != '\0'; ++o) {
                    const st::RoleCode role = st::role_of_slot(layout, o);
                    if (std::string(c.name).find("delay") != std::string::npos) {
                        (void)table.set_distance_m(role, 2.0 + (0.5 * static_cast<double>(o % 3)));
                    }
                    if (std::string(c.name).find("level") != std::string::npos) {
                        (void)table.set_level_db(role, -1.5);
                    }
                }
                stage.trim.set_table(table);
            }
            stage.trim.configure(layout, identity_for(layout), outputs);
            std::vector<std::vector<float>> planes(outputs, std::vector<float>(kBlock, 0.25F));
            std::vector<std::span<float>> spans(outputs);
            for (std::size_t o = 0; o < outputs; ++o) {
                spans[o] = planes[o];
            }
            const std::uint32_t live = (std::uint32_t{1} << outputs) - 1U;
            constexpr int kBlocks = 20000;
            const auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < kBlocks; ++b) {
                stage.trim.process(spans, live);
            }
            const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            std::printf("%2zu outputs, %-16s %8.1f ns a block of 256 frames per output, %7.2f us a block in all, delay lines %zu KB\n",
                        outputs, c.name, elapsed / kBlocks / static_cast<double>(outputs), elapsed / kBlocks / 1000.0,
                        (outputs * stage.trim.max_delay_samples() * sizeof(float)) / 1024);
            REQUIRE(planes[0][0] == planes[0][0]);  // the work is not removed
        }
    }
}
