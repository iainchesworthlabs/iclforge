// How an ordinary stereo PCM stream (what a server sends for a track that is not coded) lands on a
// room's slots, and the output a meter that read the stream's channels must put each one on
// (LayoutRenderer::direct_outputs, which the ESP32 player's integer meter stands on).
//
// The stream's channels are a bed of their own - L R, or C for mono - and the renderer places a bed
// by LOCATION: the stream's R goes to the slot named R. In a Dolby-ordered room (L C R Ls Rs ...)
// that is slot 2, and slot 1 is the centre. The meter once read channel n as output n, which on
// 9.1.6 lit L and C and left R dark while the audio was right.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/routing.hpp"

namespace {

using iclforge::ac3::Acmod;
using iclforge::ac3::DownmixTarget;
using iclforge::ac3::render::ObjectsPolicy;
using iclforge::ac3::render::serve;
using iclforge::render::LayoutRenderer;
using iclforge::render::OutputLayout;
using iclforge::render::PcmBlock;
using iclforge::render::Routing;
using Location = iclforge::base::Location;

constexpr std::size_t kFrames = 256;

using Plane = std::vector<float>;

// A tone of its own for each source channel, so a channel on the wrong slot cannot pass for the
// right one: the first at 440 Hz and a quarter of full scale, the second at 1 kHz and a half, the
// third (a mono stream has one) at 440 Hz and a half.
std::vector<Plane> tones(std::size_t channels) {
    constexpr std::array<double, 2> hertz{440.0, 1000.0};
    constexpr std::array<float, 2> level{0.25F, 0.5F};
    std::vector<Plane> source(channels, Plane(kFrames));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t k = 0; k < kFrames; ++k) {
            const double phase =
                2.0 * std::numbers::pi * hertz[c] * static_cast<double>(k) / 48000.0;
            source[c][k] = level[channels == 1 ? 1 : c] * static_cast<float>(std::sin(phase));
        }
    }
    return source;
}

bool silent(const Plane& plane) {
    for (const float x : plane) {
        if (x != 0.0F) {
            return false;
        }
    }
    return true;
}

// What the player's PCM path does with a chunk (firmware/esp-idf/iclforge/src/burst_player.cpp,
// play_pcm and emit), in the part that matters here: the stream's bed set on the renderer, or the
// fold of a room the decoder's own stage serves; the slots rendered; the patch applied.
struct Played {
    std::vector<Plane> slots;
    std::vector<Plane> outputs;
    bool folded = false;
    std::vector<int> direct;  // direct_outputs()
    bool is_direct = false;
};

Played play(const OutputLayout& layout, const Routing& routing, const std::vector<Plane>& source) {
    Played played;
    const std::size_t channels = source.size();
    played.folded = serve(layout, DownmixTarget::kLoRo, ObjectsPolicy::kAuto).fold.has_value();

    LayoutRenderer renderer{layout};
    const auto bed = iclforge::ac3::eac3::chanmap::expand(
        iclforge::ac3::eac3::chanmap::acmod_map(channels == 1 ? Acmod::k1_0 : Acmod::k2_0, false));
    if (!played.folded) {
        renderer.set_bed(bed);
    }

    std::vector<std::span<const float>> views;
    for (const Plane& plane : source) {
        views.emplace_back(plane);
    }
    const PcmBlock block{.index = 0,
                         .blocks = 1,
                         .channels = std::span<const std::span<const float>>(views),
                         .objects = {},
                         .object_indices = {},
                         .object_metadata = nullptr};

    played.slots.assign(layout.slots(), Plane(kFrames, 0.0F));
    std::vector<std::span<float>> slot_spans;
    for (Plane& plane : played.slots) {
        slot_spans.emplace_back(plane);
    }
    if (played.folded) {
        renderer.render_folded(block, 1.0F, slot_spans);
    } else {
        renderer.render_reached(block, false, 1.0F, slot_spans);
    }

    std::vector<std::span<const float>> rendered;
    for (const Plane& plane : played.slots) {
        rendered.emplace_back(plane);
    }
    played.outputs.assign(routing.outputs(), Plane(kFrames, 0.0F));
    std::vector<std::span<float>> output_spans;
    for (Plane& plane : played.outputs) {
        output_spans.emplace_back(plane);
    }
    routing.apply(rendered, output_spans);

    played.direct.assign(channels, -1);
    played.is_direct = renderer.direct_outputs(routing, played.folded, channels, played.direct);
    return played;
}

Routing identity_for(const OutputLayout& layout) {
    return *Routing::identity(layout.slots(), layout.slots());
}

// Slot n to output (slots - 1 - n): every channel on an output other than its own number.
Routing reversed_for(const OutputLayout& layout) {
    std::vector<int> outputs(layout.slots());
    for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
        outputs[slot] = static_cast<int>(outputs.size() - 1 - slot);
    }
    return *Routing::from_outputs(outputs, layout.slots());
}

// The outputs of `played` that hold anything, which must be exactly the source's channels, each
// whole and on one output, and which output holds which.
std::vector<int> outputs_carrying(const Played& played, const std::vector<Plane>& source) {
    std::vector<int> carrying(source.size(), -1);
    std::size_t holding = 0;
    for (std::size_t o = 0; o < played.outputs.size(); ++o) {
        if (silent(played.outputs[o])) {
            continue;
        }
        ++holding;
        for (std::size_t c = 0; c < source.size(); ++c) {
            if (played.outputs[o] == source[c]) {
                carrying[c] = static_cast<int>(o);
            }
        }
    }
    CHECK(holding == source.size());
    return carrying;
}

}  // namespace

TEST_CASE(
    "a stereo PCM stream plays on the front left and front right of the room, in its own order",
    "[render][pcm-source]") {
    for (const std::string_view name :
         {"2.0", "5.1", "5.1.2", "7.1.4", "9.1.6", "R,L", "L,R,C,LFE,Ls,Rs", "Vhl,Vhr,L,C,R,LFE"}) {
        CAPTURE(name);
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        const std::vector<Plane> source = tones(2);
        const Played played = play(*layout, identity_for(*layout), source);

        const int left = layout->index_of(Location::kLeft);
        const int right = layout->index_of(Location::kRight);
        REQUIRE(left >= 0);
        REQUIRE(right >= 0);
        for (std::size_t slot = 0; slot < played.slots.size(); ++slot) {
            CAPTURE(slot);
            if (static_cast<int>(slot) == left) {
                CHECK(played.slots[slot] == source[0]);
            } else if (static_cast<int>(slot) == right) {
                CHECK(played.slots[slot] == source[1]);
            } else {
                CHECK(silent(played.slots[slot]));
            }
        }
    }
}

TEST_CASE("the meter puts a stereo PCM stream's channels on the outputs that carry them",
          "[render][pcm-source]") {
    for (const std::string_view name :
         {"2.0", "5.1", "5.1.2", "7.1.4", "9.1.6", "R,L", "L,R,C,LFE,Ls,Rs", "Vhl,Vhr,L,C,R,LFE"}) {
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        for (const bool reversed : {false, true}) {
            CAPTURE(name, reversed);
            const Routing routing = reversed ? reversed_for(*layout) : identity_for(*layout);
            const std::vector<Plane> source = tones(2);
            const Played played = play(*layout, routing, source);

            REQUIRE(played.is_direct);
            // Whatever output holds the stream's first channel is the one the meter must read it
            // on.
            const std::vector<int> carrying = outputs_carrying(played, source);
            CHECK(played.direct == carrying);
        }
    }
}

TEST_CASE(
    "a stereo PCM stream on a Dolby-ordered room reads on the left and right outputs, not the "
    "centre",
    "[render][pcm-source]") {
    // The owner's report: on 9.1.6 a stereo play lit L and C. Slot 1 is C; R is slot 2.
    for (const std::string_view name : {"5.1", "5.1.2", "7.1.4", "9.1.6"}) {
        CAPTURE(name);
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        REQUIRE(layout->index_of(Location::kLeft) == 0);
        REQUIRE(layout->index_of(Location::kCentre) == 1);
        REQUIRE(layout->index_of(Location::kRight) == 2);
        const Played played = play(*layout, identity_for(*layout), tones(2));
        REQUIRE(played.is_direct);
        CHECK(played.direct == std::vector<int>{0, 2});
    }

    // A room that lists its speakers in WAV order, or a two-speaker one wired the other way round.
    const auto wav = *OutputLayout::parse("L,R,C,LFE,Ls,Rs");
    CHECK(play(wav, identity_for(wav), tones(2)).direct == std::vector<int>{0, 1});
    const auto swapped = *OutputLayout::parse("R,L");
    const Played crossed = play(swapped, identity_for(swapped), tones(2));
    REQUIRE(crossed.folded);
    CHECK(crossed.direct == std::vector<int>{1, 0});
    const auto plain = OutputLayout::stereo();
    CHECK(play(plain, identity_for(plain), tones(2)).direct == std::vector<int>{0, 1});
}

TEST_CASE("a mono PCM stream is the centre of a room that has one", "[render][pcm-source]") {
    for (const std::string_view name : {"5.1", "7.1.4", "9.1.6"}) {
        CAPTURE(name);
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        const std::vector<Plane> source = tones(1);
        const Played played = play(*layout, identity_for(*layout), source);
        const int centre = layout->index_of(Location::kCentre);
        REQUIRE(centre >= 0);
        for (std::size_t slot = 0; slot < played.slots.size(); ++slot) {
            CAPTURE(slot);
            CHECK((static_cast<int>(slot) == centre ? played.slots[slot] == source[0]
                                                    : silent(played.slots[slot])));
        }
        REQUIRE(played.is_direct);
        CHECK(played.direct == std::vector<int>{centre});
    }

    // A room with no centre spreads it between the speakers it has: no channel is a copy of itself.
    const auto no_centre = *OutputLayout::parse("L,R,LFE");
    CHECK_FALSE(play(no_centre, identity_for(no_centre), tones(1)).is_direct);
}

TEST_CASE("the meter measures the outputs when a channel is not a plain copy onto a slot",
          "[render][pcm-source]") {
    // A room with no R: the stream's right channel is panned onto the speaker nearest it.
    const auto no_right = *OutputLayout::parse("L,C,LFE");
    CHECK_FALSE(play(no_right, identity_for(no_right), tones(2)).is_direct);

    // Bass management filters what a small speaker plays.
    const auto small = *OutputLayout::parse("L:small,R:small,C,LFE,Ls,Rs");
    CHECK_FALSE(play(small, identity_for(small), tones(2)).is_direct);

    // A channel whose slot the patch sends nowhere is not on any output.
    const auto five_one = *OutputLayout::parse("5.1");
    std::vector<int> patch{0, 1, 2, 3, 4, 5};
    patch[2] = Routing::kUnassigned;  // R
    const Routing without_right = *Routing::from_outputs(patch, 6);
    CHECK_FALSE(play(five_one, without_right, tones(2)).is_direct);

    // A source whose channel count is not the bed's is not this renderer's to place.
    LayoutRenderer renderer{five_one};
    renderer.set_bed(iclforge::ac3::eac3::chanmap::expand(
        iclforge::ac3::eac3::chanmap::acmod_map(Acmod::k2_0, false)));
    std::array<int, 6> outputs{};
    CHECK(renderer.direct_outputs(identity_for(five_one), false, 2, outputs));
    CHECK_FALSE(renderer.direct_outputs(identity_for(five_one), false, 1, outputs));
    CHECK_FALSE(renderer.direct_outputs(identity_for(five_one), false, 6, outputs));
    CHECK_FALSE(renderer.direct_outputs(identity_for(five_one), false, 0, outputs));
}
