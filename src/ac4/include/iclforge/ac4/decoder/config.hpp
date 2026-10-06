#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/ac4/core/syntax.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/export.hpp"

// What an AC-4 Decoder (iclforge/ac4dec/decoder.hpp) is configured by: the output
// processing a system asks for, which presentation it decodes, what it does with a
// frame that will not decode, and full or core decoding.

namespace iclforge::ac4 {

// --- The syntax trace -------------------------------------------------------
//
// One record per syntax element read, in bitstream order, for tests and for
// diagnosing a stream: iclforge::ac4::SyntaxRecord and iclforge::ac4::SyntaxTrace, in
// ac4/syntax.hpp, whose comment states what a record holds. The encoder writes
// records of the same shape, and so does tools/references/ac4_syntax.py.

// --- Output processing -------------------------------------------------------
//
// What decode() does to the decoded channels as a system configures it:
// dialogue enhancement (Part 1 clause 5.7.8), then the output level and
// dynamic range control (5.7.9), then the downmix (6.2.17), or for the
// immersive element Part 2's channel renderer (Part 2 clause 5.10.2).

// The layout decode() renders the decoded channels to (Part 1 clause 6.2.17;
// Part 2 clause 5.10.2 for the immersive element). The .X is the stream's LFE,
// where it has one.
enum class DownmixTarget : std::uint8_t {
    // The channels as coded: for the immersive element, the layout its source
    // had (b_4_back_channels_present and top_channels_present), and in core
    // decoding its 5.X.2 core, 5.X.0 where the source has no top channels. The
    // only target a 22.2 source has: Part 2 Tables 35 to 43 have no 22.2 input,
    // and every other target is refused for it.
    kAsCoded,
    k5X,  // a 7.X element's channels folded to 5.X (Table 219); 5.X.0 for the immersive element
    // Two channels, Lo/Ro or Lt/Rt as the stream's preferred_dmx_method says,
    // Lo/Ro where it says neither.
    kStereo,
    kLoRo,
    kLtRt,  // in its Pro Logic II form where the stream prefers that
    kMono,  // L + R of the stereo downmix
    // The immersive element's other layouts (Part 2 Tables 38 to 43, with a
    // 9.X.4 source's rows folding the screen pair; core
    // decoding has 5.X.2 and 5.X.0 alone, Table 44, and takes the one of those
    // with the target's top channels or without). The other elements come out
    // as coded.
    k7X4,
    k7X2,
    k7X0,
    k5X4,
    k5X2,
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(DownmixTarget target);

// Part 1 Table 161's DRC decoder modes, and how decode() chooses one.
enum class DrcMode : std::uint8_t {
    kOff,                 // no compression: the output level gain alone
    kDefault,             // the mode clause 5.7.9.2 selects for the output level
    kHomeTheatre,         // decoder mode 0
    kFlatPanelTv,         // 1
    kPortableSpeakers,    // 2
    kPortableHeadphones,  // 3
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(DrcMode mode);

// The controls of planning/ac4.md's "One control for both formats" that act on
// the decoded channels. Decoder::set_output() changes them from the next frame.
// Every field has a default, so a designated initializer names only the fields
// it sets; the same holds for PresentationChoice and DecoderConfig.
struct OutputConfig {
    // Lout of Part 1 clause 5.7.9.3.3, in dBFS: the level the stream's
    // dialnorm is taken to, by 2^((Lout - dialnorm) / 6), which cuts or
    // boosts. Part 1 gives no default, the system supplies it; unset leaves the
    // stream at its coded level and compresses nothing.
    std::optional<double> output_level_dbfs{};
    // With an output level: the mode that compresses. A mode the stream does
    // not configure compresses nothing.
    DrcMode drc = DrcMode::kDefault;
    // Where kDefault's output level falls in the portable modes' range (-16 to
    // 0 dBFS), whether it takes portable headphones or portable speakers.
    bool headphones = false;
    // G_DE of Part 1 clause 5.7.8, in dB: how far the dialogue is raised where
    // the stream sends dialogue enhancement parameters, up to the stream's cap
    // of 3, 6, 9 or 12 dB. 0 leaves the output as the tool bypassed would.
    double dialogue_enhancement_db = 0.0;
    // The layout the channels come out in; a stream narrower than the target
    // comes out as coded, except mono, which a two-channel target takes to
    // both channels. A 22.2 source is refused (kUnsupported) for any target
    // but kAsCoded.
    DownmixTarget downmix = DownmixTarget::kAsCoded;
    // Whether a two-channel or mono downmix takes the LFE, at the stream's
    // lfe_mixgain, as Part 1 does; off drops it, outside the text.
    bool mix_lfe = true;
    // g_dialog of Part 1 clause 6.2.16.1, in dB: the level of a presentation's
    // dialogue substreams against its music and effects, up to the
    // g_dialog_max the stream allows (0 dB where it sends none). Below -120
    // dB the dialogue is silent.
    double dialogue_gain_db = 0.0;
    // g_assoc of Part 1 clause 6.2.16.2, in dB, 0 or less: the level of a
    // presentation's associated audio. Below -120 dB it is silent.
    double associated_gain_db = 0.0;
};

// --- Presentations -----------------------------------------------------------
//
// Which presentation decode() decodes, when a stream carries several (Part 2
// clause 4.8.2): of those it can decode, of a presentation_version it decodes,
// carrying audio, whose md_compat is within the decoder's level and which the
// stream has not disabled, the one a system asks for by presentation_id or by
// position, or else the one that best meets its preferences, in the order the
// clause lists them, the first in the table of contents among equals. Where
// the table of contents changes from one frame to the next, the choice is made
// again. src/ac4/ERRATA.md ("Which presentations can be selected" and "The
// order of the preferences") records the readings.

// Part 1 Table 92's refinements of associated audio, which an associated
// substream's language_tag_bytes carry in place of a language.
enum class AssociatedType : std::uint8_t {
    kAny,                        // whatever the content_classifier says
    kAudioDescription,           // qad, or qax premixed
    kAudioDescriptionSubtitles,  // audio description with spoken subtitles: qas, or qtx premixed
    kSpokenSubtitles,            // qss, or qsx premixed
    kEmergencyInformation,       // qei, or qex premixed
};

struct PresentationChoice {
    // The presentation carrying this presentation_id (Part 2 clause
    // 6.3.2.2.4a); where no presentation that can be selected carries it, the
    // rest decides.
    std::optional<int> presentation_id{};
    // Else the presentation at this position of the table of contents, which
    // the text warns can change over time.
    std::optional<std::size_t> index{};
    // Else the preferences. The language of the main or dialogue audio: an
    // IETF BCP 47 tag, a presentation's tag matching it whole before one whose
    // primary subtag matches; empty for none.
    std::string language{};
    // The associated audio: Part 1 Table 91's content_classifier of the
    // service a presentation should carry (0b010 visually impaired, 0b011
    // hearing impaired, 0b101 commentary, and so on), with Table 92's
    // refinement of it; unset for a presentation without associated audio.
    std::optional<int> associated{};
    AssociatedType associated_type = AssociatedType::kAny;
    // The kind of audio: a presentation rendered for headphones before it was
    // encoded (b_pre_virtualized, Part 1 clause 4.3.3.3.5) before one that was
    // not, or the other way round.
    bool headphones = false;
};

// The presentation decode() selects from `toc` for `choice` at compatibility
// level `level` (md_compat, Part 1 Table 86 and Part 2 Table 55): its index in
// Toc::presentations_v1, or in presentations_v0 below bitstream_version 2;
// nothing when no presentation can be selected.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<std::size_t> select_presentation(const Toc& toc,
                                                                           const PresentationChoice& choice,
                                                                           int level);

// --- Concealment ---------------------------------------------------------------
//
// What decode() does with a frame that will not decode, the policies forge's
// AC-3 and E-AC-3 decoders offer. kNone, the default, returns the error; the
// others return a frame's worth of audio instead, made by the decoder's own
// inverse transform and output stages, so the overlap with the frames either
// side stays continuous. The QMF-domain tools (A-SPX, A-CPL) pass a concealed
// frame through, and the frame after it resumes them. A frame that fails before
// any frame has decoded still returns its error: there is nothing to conceal
// from.
//
// After a change of source, the frames that wait for the new source's first
// I-frame are concealed the same way, from the old source's last frame, where
// without a policy they return nothing.
enum class ConcealmentPolicy : std::uint8_t {
    kNone,
    // The last good frame again, fading at the rate forge's decoders fade a
    // repeat, 20 dB for each 32 ms lost in a row: each concealed frame at the
    // level the fade reaches at its end.
    kRepeatFade,
    // Silence, the last good frame's overlap playing out through it.
    kMute,
};

// --- Decoding modes ----------------------------------------------------------
//
// Part 2 clause 4.7: full decoding, in which A-CPL and A-JCC reconstruct every
// channel an immersive element codes, or core decoding, which gives the
// element's core, 5.X.2, with those tools replaced or reduced, for
// low-complexity platforms, and renders it to 5.X.2 or 5.X.0 alone (Part 2
// Table 44). The Part 1 channel elements have no core (Part 2
// Table 71) and decode alike in both (src/ac4/ERRATA.md, "Core decoding of
// the Part 1 elements").
enum class DecodingMode : std::uint8_t {
    kFull,
    kCore,
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(DecodingMode mode);

// A decoder's configuration. Decoder::set_output() and set_presentation()
// change the two halves a system changes while a stream plays; the rest is
// fixed for the decoder.
struct DecoderConfig {
    // One record per syntax element read. The configuration owns a copy of the
    // callable, and the decoder one of its own (ac4/syntax.hpp); empty, the
    // default, costs one branch per syntax element.
    SyntaxTrace syntax{};
    OutputConfig output{};
    ConcealmentPolicy concealment = ConcealmentPolicy::kNone;
    // Which presentation decode() decodes (select_presentation()).
    PresentationChoice presentation{};
    // The md_compat level the decoder claims: presentations above it are not
    // selected (Part 2 clause 6.3.2.2.3).
    int level = 3;
    // Full or core decoding (Part 2 clause 4.7), after the fields the
    // decoder's API had without it.
    DecodingMode decoding = DecodingMode::kFull;
};

}  // namespace iclforge::ac4
