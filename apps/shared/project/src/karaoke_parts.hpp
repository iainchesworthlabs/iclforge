#pragma once

#include <array>
#include <cstddef>
#include <string_view>

// Where a karaoke stream's parts sit, once, for the Forge GUI's Karaoke flow
// (apps/forge/gui/src/karaoke_authoring) and for `forge project`, which lays a project's parts out
// by it when the project does not say. It has no dependency, so that the Qt-free test binaries that
// hold the flow compile it too.
//
// A karaoke stream (ETSI TS 102 366 Annex C, Table C.2.1) is a main service with bsmod 7 and a
// coding mode above 1/0 whose channels are not speakers but parts:
//
//   3/0  L M R          the backing, and a guide melody where a centre would be
//   2/1  L R V1         one vocal where a surround would be
//   3/1  L M R V1
//   2/2  L R V1 V2      two vocals where the surrounds would be
//   3/2  L M R V1 V2
//
// The author's files are the backing (two channels, L and R), an optional melody guide (M, one
// channel) and one or two vocals (V1, V2, one channel each), and the mode follows from which of them
// there are. A project names a source's part in its `part` member.

namespace iclforge::apps::project {

// The `part` values of a project's source: "backing", "melody", "vocal1" and "vocal2".
inline constexpr std::string_view kPartBacking = "backing";
inline constexpr std::string_view kPartMelody = "melody";
inline constexpr std::string_view kPartVocal1 = "vocal1";
inline constexpr std::string_view kPartVocal2 = "vocal2";

struct KaraokeSlot {
    // The bed position the channel takes, as `map=` and the window name it: "L", "C", "R", "Cs"
    // (the one surround of 2/1 and 3/1), "Ls" or "Rs".
    std::string_view position;
    // Table C.2.1's name for what it carries: "L", "M", "R", "V1" or "V2".
    std::string_view part;
};

struct KaraokeMode {
    // "3/0", "2/1", "3/1", "2/2" or "3/2", as the beds are named.
    std::string_view bed;
    // The coded channels, in coded order; the first `count` are used.
    std::array<KaraokeSlot, 5> slots;
    std::size_t count;
};

// The mode a set of parts is, or nullptr where Annex C has none: a backing alone is plain stereo
// with nothing to sing along to, and more than two vocals are not carried. `vocals` is how many
// there are, 0 to 2; a second vocal without a first is the caller's to name, since no mode carries
// it.
[[nodiscard]] const KaraokeMode* karaoke_mode(bool melody, int vocals);

}  // namespace iclforge::apps::project
