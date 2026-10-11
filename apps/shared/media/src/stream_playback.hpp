#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"

// Three things a player that decodes a whole elementary stream has to get right
// besides calling the decoders: which decoder reads the stream, which
// programme of it plays, and what becomes of the audio the E-AC-3 decoder is
// still holding when the stream ends. forge's 'monitor' and 'spatial' use all
// three, and 'play' the second. Compiled straight into forge
// and iclforge-app-media-tests, the way container_input.cpp beside it is (see
// recording_sink.hpp for why apps/shared/media/src has no library target), and kept
// out of apps/forge/cli/src so a test can hold them without a render device - none
// of those commands gets past opening one on a headless CI leg.

namespace iclforge::apps {

// One programme of an E-AC-3 stream, picked out for a player: the independent
// substream's id (§E2.3.1.2's substreamid), every id the stream carries, and
// the programme's own access units in order. Consecutive units here ARE
// consecutive frame periods, which iclforge::ac3::split_access_units without a
// programme is not: a stream with a second independent substream comes back
// from that interleaved, one frame period of each programme in turn, and a
// decoder fed it in that order plays them one after the other - a 5.1 main
// and then, at the next unit, a mono audio description.
struct ProgrammeUnits {
    int programme = 0;
    std::vector<int> ids;
    std::vector<std::span<const std::byte>> units;
};

// Why select_programme chose nothing.
struct ProgrammeError {
    // False: the stream does not frame, or holds no programme at all. True:
    // `wanted` named a programme the stream does not carry, and `carried`
    // lists the ids it does.
    bool not_carried = false;
    std::vector<int> carried;
};

// The programme a player plays and its units. §E2.3.1.2's independent
// substreams are alternatives - a second language, an audio description - not
// layers, so exactly one of them is played, never a fold of several.
// `wanted` is the listener's choice; omitted takes the first programme the
// stream carries rather than a hard-coded 0, as forge's decode, qc and levels
// do (a stream someone has cut a programme out of need not still start at
// zero). The span points into `stream`.
[[nodiscard]] std::expected<ProgrammeUnits, ProgrammeError> select_programme(
    std::span<const std::byte> stream, std::optional<int> wanted);

// One programme of `stream` as a stream of its own, for a receiver rather than
// a decoder: the independent substream `programme` with the dependents behind
// it, every frame period, renumbered as independent substream 0 with its CRC
// re-stamped (iclforge::ac3::io::extract_programme). A receiver takes
// substream 0 and ignores the rest, so another programme's frames left as
// they stand would be a stream with nothing it will play. std::nullopt when
// the stream does not scan or has no such programme.
[[nodiscard]] std::optional<std::vector<std::byte>> cut_programme(
    std::span<const std::byte> stream, int programme);


// True when `stream` is read an access unit at a time (split_access_units and
// Eac3Decoder); false when FrameDecoder reads it a frame at a time, which is
// plain AC-3, and for a stream too short to hold a syncframe (the caller's own
// stream_bsid() check reports that). bsid alone does not decide it: A/52
// §E2.3.1.2's legacy-core delivery opens with an AC-3 syncframe and carries
// Annex E dependents behind it, and FrameDecoder refuses the first dependent
// it reaches. 'forge decode' makes the same test; see
// iclforge::ac3::has_eac3_extension_substreams.
[[nodiscard]] bool reads_as_access_units(std::span<const std::byte> stream);

// Eac3Decoder::flush()'s substreams as one access unit, laid out the way
// decode_access_unit lays one out, or std::nullopt when nothing was held
// back. flush() releases what §3.7's transient pre-noise processing still
// holds when a stream ends - its last unit, whenever the stream's last frames
// used the tool - and releases it as raw substreams, because that unit never
// assembled. A player that stops at its last decode_access_unit call never
// plays it.
//
// `programme` is the layout the units before this one rendered, so this
// unit's slots line up with theirs even where it lacks a substream they had
// (that slot is silent). std::nullopt means no unit came out at all; the
// layout is then the union of the flushed substreams' own, as §E3.8.2 builds
// it. `folded` is whether the decoder's DecoderConfig::output folds, which
// flush() has already applied to every substream it returns.
//
// The assembly is decode_access_unit's, with three cases that flush()'s
// output needs spelled out:
// - flush() returns held frames before released ones, so a legacy core,
//   which is never held itself, comes out after the dependent that held its
//   unit back. §E3.8.2 is order-sensitive - a dependent's channel replaces
//   the bed's at the same location - so the bed is laid down first and every
//   dependent over it, in whatever order flush() returned them.
// - Under a fold, flush() has folded each substream on its own, so a
//   dependent's two channels are a fold of its own channels alone. The bed's
//   fold is the programme's compatible rendering (what a decoder that reads
//   no dependents plays), so the unit carries that, and no dependent.
// - Dual mono is one substream whose two channels have no location, so they
//   stay in coded order and `layout` stays empty, as decode_access_unit
//   leaves it.
//
// A flushed dependent with no bed beside it has nothing to extend, and gives
// std::nullopt.
[[nodiscard]] std::optional<iclforge::ac3::DecodedAccessUnit> held_back_unit(
    std::vector<iclforge::ac3::DecodedSubstream> flushed,
    const std::optional<iclforge::ac3::eac3::chanmap::Layout>& programme, bool folded);

}  // namespace iclforge::apps
