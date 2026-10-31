// Where the AUDIO of an ordinary stereo or mono PCM stream (a track a server sends uncoded) comes
// out of a room: the Sendspin player sets the stream's own bed on the renderer (Table E2.5's 2/0,
// or the centre for mono), renders it, and applies the output patch, and a coded channel goes to
// the slot of its LOCATION, not to the slot of its number. In a Dolby-ordered room (L C R Ls Rs
// ...) the stream's R is slot 2 and slot 1, the centre, stays silent.
//
// This holds the samples, which are the audio. The levels the player reports for a stream are
// libs/render/tests/test_pcm_slots.cpp's business.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <numbers>
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
// right one: 440 Hz at a quarter of full scale, then 1 kHz at a half. A mono stream has one, at a
// half.
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
// play_pcm and emit): the stream's bed set on the renderer, or the fold of a room the decoder's
// own stage serves; the slots rendered; the patch applied.
struct Played {
    std::vector<Plane> slots;
    std::vector<Plane> outputs;
    bool folded = false;
};

Played play(const OutputLayout& layout, const Routing& routing, const std::vector<Plane>& source) {
    Played played;
    const std::size_t channels = source.size();
    played.folded = serve(layout, DownmixTarget::kLoRo, ObjectsPolicy::kAuto).fold.has_value();

    LayoutRenderer renderer{layout};
    if (!played.folded) {
        renderer.set_bed(
            iclforge::ac3::eac3::chanmap::expand(iclforge::ac3::eac3::chanmap::acmod_map(
                channels == 1 ? Acmod::k1_0 : Acmod::k2_0, false)));
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
    return played;
}

Routing identity_for(const OutputLayout& layout) {
    return *Routing::identity(layout.slots(), layout.slots());
}

// Slot n to output (slots - 1 - n): every slot on an output other than its own number.
Routing reversed_for(const OutputLayout& layout) {
    std::vector<int> outputs(layout.slots());
    for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
        outputs[slot] = static_cast<int>(outputs.size() - 1 - slot);
    }
    return *Routing::from_outputs(outputs, layout.slots());
}

constexpr std::array<std::string_view, 8> kRooms{
    "2.0", "5.1", "5.1.2", "7.1.4", "9.1.6", "R,L", "L,R,C,LFE,Ls,Rs", "Vhl,Vhr,L,C,R,LFE"};

}  // namespace

TEST_CASE(
    "a stereo PCM stream plays on the front left and front right slots of the room, in its own "
    "order",
    "[render][pcm-audio]") {
    for (const std::string_view name : kRooms) {
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

TEST_CASE(
    "a Dolby-ordered room puts a stereo stream's right channel on slot 2 and leaves the centre "
    "silent",
    "[render][pcm-audio]") {
    for (const std::string_view name : {"5.1", "5.1.2", "7.1.4", "9.1.6"}) {
        CAPTURE(name);
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        REQUIRE(layout->index_of(Location::kLeft) == 0);
        REQUIRE(layout->index_of(Location::kCentre) == 1);
        REQUIRE(layout->index_of(Location::kRight) == 2);
        const std::vector<Plane> source = tones(2);
        const Played played = play(*layout, identity_for(*layout), source);
        CHECK(played.slots[0] == source[0]);
        CHECK(silent(played.slots[1]));
        CHECK(played.slots[2] == source[1]);
    }
}

TEST_CASE(
    "the output patch carries a stereo PCM stream's slots to the outputs it names, and no others",
    "[render][pcm-audio]") {
    for (const std::string_view name : kRooms) {
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        for (const bool reversed : {false, true}) {
            CAPTURE(name, reversed);
            const Routing routing = reversed ? reversed_for(*layout) : identity_for(*layout);
            const std::vector<Plane> source = tones(2);
            const Played played = play(*layout, routing, source);

            const int left_output =
                routing.output_of(static_cast<std::size_t>(layout->index_of(Location::kLeft)));
            const int right_output =
                routing.output_of(static_cast<std::size_t>(layout->index_of(Location::kRight)));
            for (std::size_t output = 0; output < played.outputs.size(); ++output) {
                CAPTURE(output);
                if (static_cast<int>(output) == left_output) {
                    CHECK(played.outputs[output] == source[0]);
                } else if (static_cast<int>(output) == right_output) {
                    CHECK(played.outputs[output] == source[1]);
                } else {
                    CHECK(silent(played.outputs[output]));
                }
            }
        }
    }
}

TEST_CASE("a mono PCM stream plays on the centre of a room that has one", "[render][pcm-audio]") {
    for (const std::string_view name : {"5.1", "5.1.2", "7.1.4", "9.1.6"}) {
        CAPTURE(name);
        const auto layout = OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        const std::vector<Plane> source = tones(1);
        const Played played = play(*layout, identity_for(*layout), source);
        const int centre = layout->index_of(Location::kCentre);
        REQUIRE(centre >= 0);
        for (std::size_t slot = 0; slot < played.slots.size(); ++slot) {
            CAPTURE(slot);
            if (static_cast<int>(slot) == centre) {
                CHECK(played.slots[slot] == source[0]);
            } else {
                CHECK(silent(played.slots[slot]));
            }
        }
    }
}
