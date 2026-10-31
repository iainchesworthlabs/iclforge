// Which slot a PCM stream's channel is rendered to (LayoutRenderer::unity_slot() and folded_slot()), and so which
// output its measured level belongs to. The Sendspin player measures a PCM stream's levels from the integers it
// unpacks the samples from, and puts each channel's level on the output of the slot the channel is rendered to:
// a stereo stream in a 5.1 room is on slots 0 and 2 (L and R) and not on 0 and 1 (L and C), which is what it did
// until the owner's stereo play test lit the kit's L and C.
//
// What is held here is that the answer is what the renderer does, sample for sample, and that it is nothing at
// all (the player then measures the floats) wherever the renderer does more than copy a channel to one slot.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/routing.hpp"

namespace {

using iclforge::ac3::DownmixTarget;
using iclforge::ac3::render::ObjectsPolicy;
using iclforge::ac3::render::serve;
using iclforge::render::LayoutRenderer;
using iclforge::render::OutputLayout;
using iclforge::render::Routing;
using Location = iclforge::base::Location;

constexpr std::size_t kN = 256;

// What the burst player does with a PCM block (play_pcm()): the bed of a stereo or mono stream, rendered, or the
// fold of a room of one or two speakers. Each channel is a constant, so that the peak of a slot says which channel
// is on it.
struct Played {
    explicit Played(OutputLayout room) : layout(std::move(room)) {}
    OutputLayout layout;
    bool folded = false;
    std::size_t channels = 0;
    // The renderer's own answer for each channel (-1: none), and the peak of every slot it rendered.
    std::array<int, 2> slot_of{-1, -1};
    std::vector<float> peak;
};

Played play(const char* text, std::size_t channels, std::array<float, 2> amplitude = {0.5F, 0.125F}) {
    Played out(*OutputLayout::parse(text));
    out.channels = channels;
    const auto serving = serve(out.layout, DownmixTarget::kLoRo, ObjectsPolicy::kAuto);
    out.folded = serving.fold.has_value();
    LayoutRenderer renderer(out.layout);
    if (!out.folded) {
        renderer.set_bed(iclforge::ac3::eac3::chanmap::expand(iclforge::ac3::eac3::chanmap::acmod_map(
            channels == 1 ? iclforge::ac3::Acmod::k1_0 : iclforge::ac3::Acmod::k2_0, false)));
    }
    std::array<std::array<float, kN>, 2> source{};
    for (std::size_t c = 0; c < channels; ++c) {
        source[c].fill(amplitude[c]);
    }
    const std::array<std::span<const float>, 2> views{std::span<const float>(source[0]), std::span<const float>(source[1])};
    const iclforge::render::PcmBlock block{.index = 0,
                                           .blocks = 1,
                                           .channels = std::span<const std::span<const float>>(views.data(), channels),
                                           .objects = {},
                                           .object_indices = {},
                                           .object_metadata = nullptr};
    std::vector<std::array<float, kN>> rendered(out.layout.slots());
    std::vector<std::span<float>> slots;
    for (auto& slot : rendered) {
        slot.fill(0.0F);
        slots.emplace_back(slot);
    }
    if (out.folded) {
        renderer.render_folded(block, 1.0F, slots);
    } else {
        (void)renderer.render_reached(block, false, 1.0F, slots);
    }
    for (std::size_t c = 0; c < channels; ++c) {
        out.slot_of[c] = out.folded ? renderer.folded_slot(c, channels) : renderer.unity_slot(c);
    }
    for (const auto& slot : rendered) {
        out.peak.push_back(*std::max_element(slot.begin(), slot.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }));
    }
    return out;
}

// What the fast path claims for a stream: each channel's level, on the slot it names, and silence everywhere
// else. True when that is what the renderer rendered.
bool the_claim_is_what_was_rendered(const Played& played, std::array<float, 2> amplitude = {0.5F, 0.125F}) {
    std::vector<float> claimed(played.peak.size(), 0.0F);
    for (std::size_t c = 0; c < played.channels; ++c) {
        if (played.slot_of[c] < 0) {
            return false;
        }
        claimed[static_cast<std::size_t>(played.slot_of[c])] = amplitude[c];
    }
    return claimed == played.peak;
}

}  // namespace

TEST_CASE("a stereo stream is on the slots named L and R, in every room that has them", "[render][pcm_slots]") {
    struct Row {
        const char* layout;
        int left;
        int right;
    };
    // The slot order of each room is the layout's own: 2.0 is L,R; 2.1 is L,R,LFE; 5.1 is L,C,R,...;
    // the 7.1.4 and 9.1.6 rooms put R at slot 2 as well; "R,L" wires the DACs the other way round.
    for (const Row& row : {Row{"2.0", 0, 1}, Row{"2.1", 0, 1}, Row{"5.1", 0, 2}, Row{"7.1.4", 0, 2}, Row{"9.1.6", 0, 2},
                           Row{"R,L", 1, 0}, Row{"L,C,R", 0, 2}, Row{"C,L,R,LFE", 1, 2}}) {
        CAPTURE(row.layout);
        const Played played = play(row.layout, 2);
        REQUIRE(played.layout.index_of(Location::kLeft) == row.left);
        REQUIRE(played.layout.index_of(Location::kRight) == row.right);
        CHECK(played.slot_of[0] == row.left);
        CHECK(played.slot_of[1] == row.right);
        // And that is where the samples are: nothing is on C, the surrounds, the heights or the LFE.
        CHECK(the_claim_is_what_was_rendered(played));
    }
}

TEST_CASE("the slot a stereo channel is on is not its own number in a room whose centre is slot 1", "[render][pcm_slots]") {
    // The case the kit's play test found: the right channel's level on the centre.
    for (const char* layout : {"5.1", "7.1.4", "9.1.6"}) {
        CAPTURE(layout);
        const Played played = play(layout, 2);
        CHECK(played.slot_of[1] != 1);
        CHECK(played.layout.slot(1).location == Location::kCentre);
        CHECK(played.peak[1] == 0.0F);
        CHECK(played.peak[2] == 0.125F);
        // The claim the player used to make, each channel on the slot of its own number, is false there.
        std::vector<float> by_number(played.peak.size(), 0.0F);
        by_number[0] = 0.5F;
        by_number[1] = 0.125F;
        CHECK(by_number != played.peak);
    }
    // And in a room that wires the DACs the other way round.
    const Played swapped = play("R,L", 2);
    CHECK(std::vector<float>{0.5F, 0.125F} != swapped.peak);
}

TEST_CASE("a mono stream is on the centre where the room has one", "[render][pcm_slots]") {
    for (const char* layout : {"5.1", "7.1.4", "9.1.6", "C,L,R,LFE", "L,C,R"}) {
        CAPTURE(layout);
        const Played played = play(layout, 1);
        const int centre = played.layout.index_of(Location::kCentre);
        REQUIRE(centre >= 0);
        CHECK(played.slot_of[0] == centre);
        CHECK(the_claim_is_what_was_rendered(played));
    }
}

TEST_CASE("a folded room puts a channel where render_folded puts it, and the answer is the rendered one", "[render][pcm_slots]") {
    // 2.0 and "R,L" are folded (a room of two speakers); a one-speaker room folds a stereo pair to the one
    // speaker's slot (the player averages them itself and does not meter the integers there).
    for (const char* layout : {"2.0", "R,L", "1.0"}) {
        for (const std::size_t channels : {std::size_t{1}, std::size_t{2}}) {
            CAPTURE(layout, channels);
            const Played played = play(layout, channels);
            REQUIRE(played.folded);
            for (std::size_t c = 0; c < channels; ++c) {
                CAPTURE(c);
                const int slot = played.slot_of[c];
                if (slot < 0) {
                    continue;  // a room with fewer speakers than channels: the second channel has no slot
                }
                // The channel's samples are on exactly that slot (the one-speaker room's second channel finds no
                // slot, and the first is what it is).
                CHECK(played.peak[static_cast<std::size_t>(slot)] == (c == 0 ? 0.5F : 0.125F));
            }
        }
    }
    const Played swapped = play("R,L", 2);
    CHECK(swapped.slot_of[0] == 1);
    CHECK(swapped.slot_of[1] == 0);
    CHECK(the_claim_is_what_was_rendered(swapped));
}

TEST_CASE("no slot is claimed where the renderer does more than copy the channel to one slot", "[render][pcm_slots]") {
    // Small speakers are bass-managed: their low end goes to the LFE feed, so the slots carry other than the channel.
    {
        const Played played = play("L:small,R:small,C,LFE,Ls,Rs", 2);
        CHECK(played.slot_of[0] == -1);
        CHECK(played.slot_of[1] == -1);
    }
    // A mono stream in a room with no centre is panned across the speakers there are.
    for (const char* layout : {"L,R,LFE", "L,R,Ls,Rs"}) {
        CAPTURE(layout);
        const Played played = play(layout, 1);
        REQUIRE_FALSE(played.folded);
        CHECK(played.slot_of[0] == -1);
    }
    // A stereo stream in a room with no R is panned too.
    {
        const Played played = play("L,C,LFE", 2);
        CHECK(played.slot_of[1] == -1);
    }
}

TEST_CASE("the output a slot's level belongs to is the one the patch sends the slot to", "[render][pcm_slots]") {
    // The renderer's slot, then the patch: with the room's identity patch it is the slot's own number, and with a
    // patch that swaps two outputs it is the other one - which the player reads from Routing, not from the slot.
    const Played played = play("5.1", 2);
    const auto identity = Routing::identity(played.layout.slots(), played.layout.slots());
    REQUIRE(identity.has_value());
    CHECK(identity->output_of(static_cast<std::size_t>(played.slot_of[0])) == 0);
    CHECK(identity->output_of(static_cast<std::size_t>(played.slot_of[1])) == 2);
    const std::array<int, 6> swapped{2, 1, 0, 3, 4, 5};  // L on output 2, R on output 0
    const auto patch = Routing::from_outputs(swapped, 6);
    REQUIRE(patch.has_value());
    CHECK(patch->output_of(static_cast<std::size_t>(played.slot_of[0])) == 2);
    CHECK(patch->output_of(static_cast<std::size_t>(played.slot_of[1])) == 0);
    CHECK(patch->channel_of(2) == played.slot_of[0]);
    CHECK(patch->channel_of(0) == played.slot_of[1]);
}
