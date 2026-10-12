#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

#include "iclforge/pitch/reference.hpp"

// The reference melody a singer's pitch is scored against, taken from the karaoke stream being
// played (planning/karaoke-microphone.md, D9; findings F4).
//
// An Annex C karaoke stream carries the tune as coded channels: the guide melody M where a centre
// would be, and one or two vocals V1 and V2 where the surrounds would be (Table C.2.1). Decoded as
// coded - the library's defaults, no reproduction - those channels come out of the decoder
// untouched, and a pitch tracker over one of them is the melody. M is the best signal there is (one
// instrument, monophonic); V1 is the lead vocal; V2 is often a harmony, which is not a melody to
// score against, so it is the last resort and says so.
//
// This is a pass over the whole stream (a few seconds for a song), so it is a job for a worker
// thread: `cancel` is looked at every few hundred frames.

namespace iclforge::hearth {

enum class ReferencePart : std::uint8_t {
    kMelodyGuide,  // M
    kVocal1,       // V1
    kVocal2,       // V2
};

struct ReferenceMelody {
    pitch::Contour contour;
    ReferencePart part = ReferencePart::kMelodyGuide;
    // The part in words, for the page: "the melody guide (M)", "vocal 1 (V1)", "vocal 2 (V2)".
    std::string description;
    // How many of the stream's frames the part was found voiced in, 0 to 1: a part that is mostly
    // silent is a poor reference, and the page can say so.
    double voiced_share = 0.0;
};

// The reference of an AC-3 elementary stream. `prefer` names a part the caller wants (the part the
// singer has muted, which is the one they are taking over); where it is absent the first present
// of M, V1 and V2 is used, and `prefer` that is not in the stream is not an error. The error says
// why there is no reference: not a karaoke stream, not AC-3, no frames, no voiced stretch in the
// part.
[[nodiscard]] std::expected<ReferenceMelody, std::string> reference_from_karaoke(
    std::span<const std::byte> ac3_stream, std::optional<ReferencePart> prefer = std::nullopt,
    const std::atomic<bool>* cancel = nullptr);

}  // namespace iclforge::hearth
