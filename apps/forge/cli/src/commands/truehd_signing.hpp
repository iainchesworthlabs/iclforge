#pragma once

#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "../support.hpp"
#include "iclforge/truehd/signing/evolution_signer.hpp"

// sign-objects, verify-objects and gate-objects for TrueHD: the per-frame evolution tag of
// iclforge::truehd::signing (docs/concepts/object-signing.md, "TrueHD evolution-frame signing").
// ICL Forge's own scheme with the operator's own key (signing-key=, repeatable for the two that
// check, or ICLFORGE_SIGNING_KEY[_FILE]); it is not a Dolby tag and a licensed decoder will not
// take it for one. Off unless asked for, like E-AC-3's.

namespace forge_cli::commands {

// sign-objects: signs every evolution frame of `stream` in place with the one key. Returns the
// number signed, or nullopt (the reason already printed) when a key is missing or unreadable or
// two were given. Not asked for -> 0, nothing touched.
[[nodiscard]] std::optional<int> apply_truehd_object_signing(std::span<std::byte> stream,
                                                             const Options& meta, FILE* status);

// verify-objects: nothing when not asked for; else the counts, or nullopt (printed) when a key is
// missing or a frame's tag does not match.
[[nodiscard]] std::optional<iclforge::truehd::signing::VerifySummary>
apply_truehd_object_verification(std::span<const std::byte> stream, const Options& meta,
                                 FILE* status);

struct TruehdGate {
    std::vector<std::byte> bytes;  // the stream to decode instead
    int gated = 0;                 // units whose object layer was removed
};

// gate-objects: the stream with the object layer removed from every unit that does not verify
// (iclforge::truehd::signing::gate_evolution_stream), or nullopt (printed) when a key is missing or
// a unit could be neither verified nor removed. Call it only when meta.gate_objects.
[[nodiscard]] std::optional<TruehdGate> apply_truehd_object_gate(std::span<const std::byte> stream,
                                                                 const Options& meta, FILE* status);

}  // namespace forge_cli::commands
