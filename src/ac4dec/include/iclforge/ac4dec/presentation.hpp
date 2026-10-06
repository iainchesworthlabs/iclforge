#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac4/toc.hpp"
#include "iclforge/ac4dec/export.hpp"
#include "iclforge/ac4dec/frame.hpp"

// What an AC-4 Decoder (iclforge/ac4dec/decoder.hpp) reports of the presentation it
// decodes: its members, and the loudness, DRC, dialogue enhancement and downmix
// metadata the stream sends for it.

namespace iclforge::ac4 {

// --- What the decoder reports of a stream ---------------------------------------
//
// planning/ac4.md's "Media information": each presentation of the table of
// contents, and the metadata of the one decode() selects, as the frames read
// so far have sent it. Values a stream sends only in I-frames are kept until a
// change of source.

// What one substream is to a presentation (Part 2 clause 4.8.3.2, and the
// dialogue enhancement substream of Part 2 Table 53 and Part 1 Table 85).
enum class SubstreamRole : std::uint8_t {
    kMain,
    kMusicAndEffects,
    kDialogue,
    kDialogueEnhancement,
    kAssociated,
};

[[nodiscard]] ICLFORGE_AC4DEC_EXPORT std::string_view describe(SubstreamRole role);

struct PresentationMember {
    int substream = 0;  // substream_index: the first of a frame-rate-multiplied series
    SubstreamRole role = SubstreamRole::kMain;
    int group = -1;  // its substream group (version 1), -1 for a version 0 presentation
    std::optional<int> content_classifier;  // Part 1 Table 91
    // language_tag_bytes: a BCP 47 tag, or for associated audio a Part 1
    // Table 92 code; empty without one.
    std::string language;
    // Its channel mode's channels; empty for a substream this decoder does
    // not turn into PCM.
    std::vector<Speaker> speakers;
    // The sampling frequency of the substream (Part 1 Table 89): the stream's base rate, or two or
    // four times it where the substream carries sf_multiplier, in which case decode() puts the
    // presentation out at that rate.
    int sample_rate_hz = 0;
};

struct PresentationInfo {
    // In Toc::presentations_v1, or presentations_v0 below bitstream_version 2.
    std::size_t index = 0;
    std::optional<int> presentation_id;
    int presentation_version = 0;
    // Part 2 Table 53 (Part 1 Table 85); unset for a single substream group.
    std::optional<int> presentation_config;
    // The level it needs: Part 2 Table 55, Part 1 Table 86.
    std::optional<int> md_compat;
    bool enabled = true;           // enable_presentation, version 1
    bool alternative = false;      // b_alternative, version 1
    bool pre_virtualized = false;  // b_pre_virtualized
    // presentation_name, for an alternative presentation, once the decoder has
    // it whole (Part 2 clause 6.3.3.1.4, a name in chunks over several frames
    // included); empty until then, and without one.
    std::string name;
    // An alternative presentation's targets as its presentation substream
    // last sent them (Part 2 clauses 6.3.3.1.5 to 6.3.3.1.8): each
    // target_level and target_device_category, as iclforge::ac4::AlternativeTarget
    // holds them, which is what Annex E.12's alternative_info() repeats;
    // empty for a presentation that is not an alternative one.
    std::vector<AlternativeTarget> targets;
    // Its dialogue substream's language, else its main or music and effects
    // substream's (Part 1 clause 4.3.3.8.8), as selection compares it.
    std::string language;
    // The channels decode() puts out as coded: its main or music and effects
    // substream's, which the others are mixed into.
    std::vector<Speaker> speakers;
    // The rate decode() puts the presentation out at, DecodedFrame::sample_rate_hz: that of the
    // same substream, 48000 or 44100 or, with an HSF extension, 96000 or 192000 (Part 1 clause
    // 5.4); 0 for a presentation with no channel-coded substream.
    int sample_rate_hz = 0;
    std::vector<int> substream_groups;  // ac4_sgi_specifier()'s group_index values, version 1
    std::vector<PresentationMember> members;
    // Whether this decoder turns every substream of it into PCM, and whether
    // select_presentation() may choose it at the decoder's level.
    bool decodable = false;
    bool selectable = false;
};

// Part 1 clause 4.3.12's loudness values as a stream sends them, in dB (LKFS,
// LUFS, dBTP) and LU. Each is set where the stream sends it.
struct LoudnessInfo {
    // dialnorm (Part 1 clause 4.3.12.2.1): the dialogue level the output
    // level is taken from, 0 to -31.75 dBFS.
    std::optional<double> dialnorm_dbfs;
    // further_loudness_info() (Part 1 clause 4.3.12.3, Part 2 clause 6.3.8.2):
    // loud_prac_type (Table 156), the dialogue gating a correction used
    // (dialgate_prac_type, Table 157) and whether it ran in real time
    // (b_loudcorr_type).
    std::optional<int> practice;
    std::optional<int> correction_gating;
    bool corrected_in_real_time = false;
    std::optional<double> integrated_lkfs;       // loudrelgat
    std::optional<double> speech_gated_lkfs;     // loudspchgat
    std::optional<int> speech_gating;            // its dialgate_prac_type
    std::optional<double> short_term_lufs;       // loudstrm3s
    std::optional<double> max_short_term_lufs;   // max_loudstrm3s
    std::optional<double> true_peak_dbtp;        // truepk
    std::optional<double> max_true_peak_dbtp;    // max_truepk
    std::optional<double> loudness_range_lu;     // lra
    std::optional<int> loudness_range_practice;  // lra_prac_type
    std::optional<double> momentary_lufs;        // loudmntry
    std::optional<double> max_momentary_lufs;    // max_loudmntry
};

// One DRC decoder mode a stream carries (Part 1 clause 4.3.13.3).
struct DrcModeInfo {
    // Table 161's drc_decoder_mode_id: 0 home theatre, 1 flat panel TV, 2
    // portable speakers, 3 portable headphones; 4 to 7 the output levels from
    // `output_level_from_db` down to `output_level_to_db`.
    int id = 0;
    std::optional<int> output_level_from_db;
    std::optional<int> output_level_to_db;
    enum class Compression : std::uint8_t {
        kDefaultProfile,  // drc_default_profile_flag: the stream's drc_eac3_profile
        kCurve,           // a compression curve of its own (Table 166)
        kGains,           // gains the stream transmits (drc_compression_curve_flag 0)
    };
    Compression compression = Compression::kDefaultProfile;
    std::optional<int> repeat_of;     // drc_repeat_profile_flag's drc_repeat_id
    std::optional<int> gains_config;  // Table 163, with transmitted gains
};

struct DrcInfo {
    int eac3_profile = 0;            // drc_eac3_profile, Table 160
    std::vector<DrcModeInfo> modes;  // in the order the stream sends them
    // The mode decode() compresses with at the output level and DrcMode set
    // (clause 5.7.9.2); nothing where it compresses nothing.
    std::optional<int> applied_mode;
};

// Dialogue enhancement's configuration (Part 1 clause 4.3.14).
struct DialogueEnhancementInfo {
    // de_method, Table 170: 0 channel independent, 1 cross-channel, 2 and 3 the
    // hybrid methods, whose waveform a dialogue enhancement substream carries.
    int method = 0;
    // Which of L, R and C the parameters are for (de_channel_config, Table 171).
    bool left = false;
    bool right = false;
    bool centre = false;
    double max_gain_db = 0.0;  // de_max_gain: the cap on G_DE, 3, 6, 9 or 12 dB
};

// The stereo downmix's values (Part 1 clauses 4.3.12.2.8 to 4.3.12.2.19, Part 2
// clauses 6.2.9.1 and 6.2.9.2), gains in dB; -infinity for a gain of 0.
struct DownmixInfo {
    double loro_centre_db = -3.0;
    double loro_surround_db = -3.0;
    // Lt/Rt's, which are the Lo/Ro values where b_ltrt_mixinfo is 0.
    double ltrt_centre_db = -3.0;
    double ltrt_surround_db = -3.0;
    std::optional<double> lfe_db;  // lfe_mg, 5.5 - lfe_mixgain dB
    // preferred_dmx_method, Table 150.
    enum class Preferred : std::uint8_t { kNotIndicated, kLoRo, kLtRt, kLtRtProLogicII };
    Preferred preferred = Preferred::kNotIndicated;
    // The loudness corrections, in dB2 (6 dB2 a factor of 2).
    std::optional<double> loro_correction_db2;
    std::optional<double> ltrt_correction_db2;
};

// The metadata of the presentation decode() selected, for display: what the
// frames read so far have sent, each part unset until one has.
struct PresentationMetadata {
    // The presentation this describes; unset before one is selected.
    std::optional<std::size_t> presentation;
    LoudnessInfo loudness;
    std::optional<DrcInfo> drc;
    std::optional<DialogueEnhancementInfo> dialogue_enhancement;
    std::optional<DownmixInfo> downmix;
};

}  // namespace iclforge::ac4
