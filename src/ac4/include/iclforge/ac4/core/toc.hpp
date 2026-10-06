#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac4/export.hpp"

// AC-4 sync-frame / table-of-contents / presentation / substream-group
// framing. ETSI TS 103 190-1 V1.4.1 (2025-07), "Digital Audio Compression
// (AC-4) Standard; Part 1: Channel based coding", and ETSI TS 103 190-2
// V1.3.1 (2025-07), "... Part 2: Immersive and personalized audio". Section
// numbers on each declaration cite whichever part actually defines that
// element; Part 2 clause 6 supersedes Part 1 clause 4 for bitstream_version
// >= 2 (see Toc::bitstream_version and parse_raw_frame()).
//
// This is a bitstream INSPECTOR, not a decoder: audio_data and metadata()
// payloads are reported as byte ranges (Substream::audio_size, Substream
// itself), never decoded. It is deliberately codec-blind in the same sense
// iclforge::mpegts::/iclforge::mp4::/iclforge::matroska:: are - it depends on nothing under
// iclforge::ac3, and knows nothing about AC-3, E-AC-3 or Atmos.
//
// Scope covers both channel-coded and object/A-JOC-coded substream groups
// (b_channel_coded 1 or 0): TOC/presentation/substream-group/substream-info
// framing for A-JOC-coded (§6.3.2.8), direct-coded-object (§6.3.2.10) and
// OAMD (§6.3.2.12) substreams is parsed the same way the channel-coded path
// is - object position/bed assignment (bed_dyn_obj_assignment(), §6.2.1.10)
// and, at the TOC level, oamd_common_data() (§6.2.8.1, AjocSubstreamInfo::
// oamd_common_data) included: ac4_substream_info_ajoc()'s own
// b_oamd_common_data_present flag embeds it inline, ahead of the fields that
// follow it in the same element, so reading it correctly is what keeps the
// rest of the TOC in step. The OAMD substream DATA payload itself
// (oamd_substream(), §6.2.2.4 - which embeds a second, independent
// oamd_common_data() of its own) was never in scope of this inspector either
// way - like every non-audio substream, it is reported as a byte range only.
// iclforge::ac4::Decoder (src/ac4/src/decoder) reads it.
//
// The bitstream_version >= 2 path (TS 103 190-2 clause 6, presentation_v1
// and substream-group framing) is cross-checked against real Dolby
// Encoding Engine 6.5.4 output - both plain-channel and 5.1.4
// channel-based-immersive encodes - byte for byte against
// tools/references/ac4_parse.py's independent transcription, and
// semantically against MediaInfo's own AC-4 reader. The bitstream_version
// <= 1 path (legacy TS 103 190-1 ac4_toc()/ac4_presentation_info()) has no
// such stream to test against - no encoder available to this project
// writes it - so it is transcribed and page-verified against the published
// spec text only. A bitstream_version 1 presentation can also nest a
// presentation_version 1 description inside presentation_config_ext_info()
// (TS 103 190-2 §6.2.1.5 and Table 4); that nested element is skipped as
// bytes, so such a presentation reads as presentation_config 7 with no
// substreams.
//
// A-JOC/direct-coded-object/OAMD framing has a narrower verification story
// still: the one encoded A-JOC stream this project has is Chromium's
// ac4-ajoc.ac4, kept out of the tree (tests/ac4/core/test_ac4.cpp holds its table
// of contents), and `dee_ac4ajoc_encoder.exe` accepts only an Atmos ADM BWF
// mezzanine, which this project's own tooling cannot produce one DEE accepts
// (the same "gates on content provenance, not syntax" limit
// docs/verification.md already states for the AC-3/E-AC-3 side), and
// `dee_ac4ims_encoder.exe` - the other locally available object-adjacent
// encoder, despite its name - was confirmed to stay channel-coded
// regardless. What stands in for more streams is a set of synthetic,
// hand-built bitstreams cross-checked between this parser and
// tools/references/ac4_parse.py, each built by an independent bit writer
// in neither module - see tests/ac4/core/test_ac4.cpp. See docs/verification.md.

namespace iclforge::ac4 {

enum class Error : std::uint8_t {
    kTruncated,
    kLostSync,
    kUnsupportedBitstreamVersion,  // > 2; TS 103 190-2 §6.3.2.1.1
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(Error error);

// --- §4.2.3.7 content_type --------------------------------------------------

struct ContentType {
    int content_classifier = 0;  // Table 91
    std::optional<std::vector<std::byte>> language_tag;
    // b_serialized_language_tag: the tag comes a chunk a frame
    // (language_tag_chunk), which one table of contents does not hold whole,
    // so language_tag stays unset.
    bool serialized_language_tag = false;
};

// --- §4.2.3.6 ac4_substream_info (presentation_version 0) / §6.2.1.8
// --- ac4_substream_info_chan (presentation_version 1) ----------------------

struct OriginalContent {
    // §6.3.2.7.3-.5: whether channels the coded channel_mode implies exist
    // are actually populated in the source, or carry encoded silence.
    bool b_4_back_channels_present = false;
    bool b_centre_present = false;
    int top_channels_present = 0;  // Table 59
};

struct ChannelSubstreamInfo {
    int channel_mode = 0;           // raw code, Table 88 or Table 56
    std::string channel_mode_name;  // e.g. "Stereo", "7.1.4"
    std::optional<int> ch_mode;     // nullopt for a reserved code
    std::optional<OriginalContent> original_content;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;          // nullopt if unmapped ("unlimited" or reserved)
    std::optional<int> brate_ind;             // Table 90's brate_ind, 0-19, when b_bitrate_info
    std::optional<ContentType> content_type;  // presentation_version 0 only
    std::optional<int> substream_index;       // index into Toc::substream_sizes
    // §4.3.3.7.6: which channel pair the additional channels of a 5/2/0 or
    // 3/2/2 7.X mode are based on. Set only for those four channel modes.
    std::optional<bool> add_ch_base;
    // §4.3.3.7.8 b_iframe (presentation_version 0) or §6.3.2.7.6
    // b_audio_ndot (presentation_version 1): one entry per
    // frame_rate_factor, true where that substream instance depends on no
    // earlier frame. A decoder needs it to know whether I-frame-only
    // configuration is present.
    std::vector<bool> b_iframe;
    // §4.2.3.9 ac4_hsf_ext_substream_info: set only for the legacy
    // (bitstream_version <= 1) path's first role substream when its
    // presentation's b_hsf_ext is set - the v1 path's equivalent is
    // GroupSubstream::hsf_ext_substream_index instead, since that one
    // wrapper covers chan/ajoc/obj alike.
    std::optional<int> hsf_ext_substream_index;
};

// --- §6.2.1.10 bed_dyn_obj_assignment / §6.3.2.10.8 -------------------------

enum class ObjectKind : std::uint8_t { kBed, kDyn, kIsf };

struct ObjectEntry {
    ObjectKind kind = ObjectKind::kDyn;
    bool lfe = false;
    bool ajoc_coded = false;
    // A bed object's loudspeaker (Annex F.3's "channel"), as TS 103 190-2
    // Table A.27 indexes speakers: 0 L, 1 R, 2 C, 3 Ls, 4 Rs, 5 Lb, 6 Rb, 7
    // Tfl, 8 Tfr, 9 Tbl, 10 Tbr, 11 LFE, 12 Tsl, 13 Tsr, 19 LFE2, 26 Lw and 27
    // Rw, the ones Tables 62 to 66 can assign. Unset for other objects.
    std::optional<int> speaker{};
};

// --- Annex F: an object's properties ------------------------------------------

// Annex F.2 to F.10, and add_per_object_md()'s data (TS 103 190-2 clause
// 6.3.9.11): what one block update of an object's metadata sets (clause
// 6.3.9), which iclforge::ac4::Decoder reports and iclforge::ac4::Encoder writes.
struct ObjectProperties {
    // Whether the object's essence carries sound (!b_object_not_active).
    bool active = true;
    // F.5, object_gain in dB; -infinity for silence.
    double gain_db = 0.0;
    // F.7, 0 to 1.
    double priority = 1.0;
    // F.2, for a dynamic object: X from the left wall (0) to the right (1), Y
    // from the front wall (0) to the back (1), Z from the floor (-1) through
    // the height of the screen (0) to the ceiling (1).
    std::array<double, 3> position{0.5, 0.5, 0.0};
    // F.8: zone_mask (Table 104) and b_enable_elevation; F.10: b_object_snap.
    int zone_mask = 0;
    bool enable_elevation = true;
    bool snap = false;
    // F.6, the object's width in X, Y and Z, 0 to 1 (object_width in all
    // three where the stream sends one value).
    std::array<double, 3> width{};
    // F.4: object_screen_factor, and the exponent object_depth_factor gives
    // the Y position (Table 107).
    double screen_factor = 0.0;
    double depth_exponent = 1.0;
    // object_distance_factor (Table 108), infinity for b_obj_at_infinity;
    // unset where the stream sends none.
    std::optional<double> distance;
    // F.9, object_divergence, 0 to 1.
    double divergence = 0.0;
    // b_obj_trim_disable, hp_render_mode_obj (Table 121) and
    // b_head_track_disable_obj.
    bool trim_disabled = false;
    std::optional<int> headphone_render_mode;
    bool head_track_disabled = false;
};

// --- §6.2.1.13 oamd_substream_info ------------------------------------------

struct OamdSubstreamInfo {
    bool b_oamd_ndot = false;
    std::optional<int> substream_index;
};

// --- §6.2.8.13-16 tool_tb_to_f_s[_b] / tool_tf_to_f_s[_b], §6.2.9.9-10 -----
// tool_t2_to_f_s[_b]: eight tables, three call shapes total (t2/tb/tf each
// with and without a "to side" middle branch), differing only in field
// names - one shared struct and reader.

struct GainTool {
    std::optional<int> code_a;
    int code_b = 0;  // read, or the derived value 7 (never transmitted)
    std::optional<int> code_c;
};

// --- §6.2.8.8a stereo_dmx_coeff ----------------------------------------------

struct StereoDmxCoeff {
    int loro_centre_mixgain = 0;
    int loro_surround_mixgain = 0;
    std::optional<int> ltrt_centre_mixgain;
    std::optional<int> ltrt_surround_mixgain;
    std::optional<int> lfe_mixgain;
    int preferred_dmx_method = 0;
};

// --- §6.2.8.8 bed_render_info ------------------------------------------------

struct BedRenderInfo {
    std::optional<StereoDmxCoeff> stereo_dmx_coeff;
    std::optional<int> gain_w_to_f_code;
    std::optional<int> gain_b4_to_b2_code;
    std::optional<GainTool> t2_to_f_s_b;
    std::optional<GainTool> t2_to_f_s;
    std::optional<GainTool> tb_to_f_s_b;
    std::optional<GainTool> tb_to_f_s;
    std::optional<GainTool> tf_to_f_s_b;
    std::optional<GainTool> tf_to_f_s;
    std::optional<int> gain_tfb_to_tm_code;
};

// --- §6.2.8.9 trim / §6.2.8.9a headphone -------------------------------------

// One entry per configuration trim() reads (0 to kNumTrimConfigs):
// nullopt where b_default_trim was set (the default profile applies,
// nothing else to report), disabled where b_disable_trim was set, the
// balance fields trim_balance_presence names otherwise.
struct TrimConfig {
    bool disabled = false;
    int presence = 0;
    std::optional<int> trim_centre;
    std::optional<int> trim_surround;
    std::optional<int> trim_height;
    std::optional<std::pair<int, int>> bal3d_y_tb;   // sign, amount
    std::optional<std::pair<int, int>> bal3d_y_lis;  // sign, amount
};

struct Trim {
    int warp_mode = 0;
    int global_trim_mode = 0;
    std::vector<std::optional<TrimConfig>> configs;  // empty unless global_trim_mode == 0b10
};

struct Headphone {
    int hp_operation_mode = 0;
    std::optional<bool> b_head_track_disable_all;
};

// --- §6.2.8.1 oamd_common_data ------------------------------------------------

struct OamdCommonData {
    bool b_default_screen_size_ratio = false;
    std::optional<int> master_screen_size_ratio_code;
    bool b_bed_object_chan_distribute = false;
    std::optional<Trim> trim;
    std::optional<BedRenderInfo> bed_render_info;
    std::optional<Headphone> headphone;
};

// --- §6.2.1.9 ac4_substream_info_ajoc ---------------------------------------

struct AjocSubstreamInfo {
    bool b_lfe = false;
    bool b_static_dmx = false;
    int n_fullband_dmx_signals = 0;
    std::vector<ObjectEntry> static_objects;   // empty when b_static_dmx
    std::optional<OamdCommonData> oamd_common_data;
    int n_fullband_upmix_signals = 0;
    // The upmix's bed and ISF objects, as bed_dyn_obj_assignment() lists them;
    // the upmix signals it does not list are dynamic objects, all of them
    // where b_dyn_objects_only is set.
    std::vector<ObjectEntry> upmix_objects;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;
    std::optional<int> brate_ind;  // Table 90's brate_ind, 0-19, when b_bitrate_info
    std::optional<int> substream_index;
    // §6.3.2.7.6 b_audio_ndot, one entry per frame_rate_factor, as
    // ChannelSubstreamInfo::b_iframe.
    std::vector<bool> b_iframe;
};

// --- §6.2.1.11 ac4_substream_info_obj ---------------------------------------

struct ObjSubstreamInfo {
    // The objects the element lists: with b_dynamic_objects, an LFE where
    // b_lfe is set and then the dynamic objects (TS 103 190-2 Table 60 counts
    // the LFE on top of n_objects_code's objects - src/ac4/ERRATA.md,
    // "n_objects_code and the LFE"); otherwise the bed or intermediate spatial
    // format objects a substream that starts them assigns, which substreams
    // after it in the group may carry a share of.
    std::vector<ObjectEntry> objects;
    // Whether the substream holds dynamic objects (§6.3.2.10.3); otherwise
    // static_kind below says what it holds.
    bool b_dynamic_objects = false;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;
    std::optional<int> brate_ind;  // Table 90's brate_ind, 0-19, when b_bitrate_info
    std::optional<int> substream_index;
    // Table 60: the objects besides the LFE that the substream's audio codes
    // (0, 1, 2, 3 or 5); unset for a code the table reserves (5 to 7).
    std::optional<int> num_objects;
    bool b_lfe = false;  // b_dynamic_objects' b_lfe
    // Without dynamic objects: what the substream holds (§6.3.2.10.4 to
    // 6.3.2.10.7) - a bed, intermediate spatial format objects, or reserved
    // data - and whether it starts them (b_bed_start, b_isf_start) rather than
    // extending a previous substream's without listing its objects again.
    enum class Static : std::uint8_t { kNone, kBed, kIsf, kReserved };
    Static static_kind = Static::kNone;
    bool static_start = false;
    std::vector<bool> b_iframe;  // b_audio_ndot, as AjocSubstreamInfo::b_iframe
};

// --- §6.2.1.6 ac4_substream_group_info --------------------------------------

// One entry of a substream group's own substream list. Exactly one of
// `chan`/`ajoc`/`obj` is set, selected by `kind` - a tagged union rather
// than std::variant so callers can query without visiting.
struct GroupSubstream {
    enum class Kind : std::uint8_t { kChan, kAjoc, kObj };
    Kind kind = Kind::kChan;
    std::optional<ChannelSubstreamInfo> chan;
    std::optional<AjocSubstreamInfo> ajoc;
    std::optional<ObjSubstreamInfo> obj;
    // §4.2.3.9 ac4_hsf_ext_substream_info, read once per substream when the
    // group's own b_hsf_ext is set - covers chan/ajoc/obj alike, unlike
    // ChannelSubstreamInfo::hsf_ext_substream_index (the legacy path's own
    // field, which this struct does not exist for).
    std::optional<int> hsf_ext_substream_index;
};

struct SubstreamGroupInfo {
    bool b_substreams_present = false;
    // Whether each substream has an HSF extension (§6.3.2.6.2); where
    // b_substreams_present is 0 the indices it would name are not sent.
    bool b_hsf_ext = false;
    bool b_channel_coded = true;
    std::optional<OamdSubstreamInfo> oamd;  // set only when !b_channel_coded and b_oamd_substream
    std::vector<GroupSubstream> substreams;
    std::optional<ContentType> content_type;
};

// --- §4.2.3.2 ac4_presentation_info (bitstream_version <= 1) ---------------

// presentation_config 6 is an EMDF-only presentation: it carries additional
// EMDF substreams and nothing else, so md_compat, presentation_id and
// substreams stay empty.
struct PresentationInfoV0 {
    int presentation_version = 0;
    std::optional<int>
        presentation_config;       // Table 85; nullopt for a single-substream presentation
    std::optional<int> md_compat;  // Table 86
    std::optional<int> presentation_id;
    std::vector<std::pair<std::string, ChannelSubstreamInfo>> substreams;  // role, info
    bool b_pre_virtualized = false;  // §4.3.3.3.5
    // Substreams holding emdf_payloads_substream() (§4.2.4.4), from the
    // presentation's emdf_info() and its additional EMDF substream list.
    std::vector<int> emdf_payloads_substream_indices;
};

// --- §6.2.1.3 ac4_presentation_v1_info (bitstream_version >= 2) ------------

// An emdf_info()'s version and authentication ID (Part 1 4.3.3.6), which
// TS 103 190-2 Annex E.10's DSI repeats.
struct EmdfVersionKey {
    int emdf_version = 0;
    int key_id = 0;
};

// One target of an alternative presentation (§6.3.3.1.5 to 6.3.3.1.8): its
// target_level, which Annex E.12 calls target_md_compat, and Table 67's
// target_device_category[], the first Boolean sent (index 0, stereo speakers)
// its most significant of four bits.
struct AlternativeTarget {
    int md_compat = 0;
    int device_category = 0;
};

// What Annex E.12's alternative_info() says of an alternative presentation:
// its name, as UTF-8 bytes without the terminating 0 the presentation
// substream sends, and its targets.
struct AlternativeInfo {
    std::string name{};
    std::vector<AlternativeTarget> targets{};
};

// presentation_config 6 is an EMDF-only presentation (Table 53): md_compat,
// enable_presentation and group_refs stay empty, and frame_rate_factor stays
// 1 because the presentation does not transmit one.
struct PresentationInfoV1 {
    int presentation_version = 0;
    std::optional<int> presentation_config;  // Table 53
    std::vector<int> group_refs;             // ac4_sgi_specifier() group_index values
    std::optional<int> md_compat;            // Table 55
    std::optional<bool> enable_presentation;
    int frame_rate_factor = 1;  // Table 87; threaded into this frame's substream groups
    // §6.2.1.4 / Table 18: 1, or the 2 or 4 transmission frames one coded
    // frame is spread over in the efficient high frame rate mode. Above 1,
    // this frame's substreams are fragments, not whole substreams.
    int frame_rate_fraction = 1;
    std::optional<int> presentation_id;
    bool b_pre_virtualized = false;  // §4.3.3.3.5
    // b_multi_pid, for a presentation of several substream groups: whether
    // they are split over more than one elementary stream.
    bool b_multi_pid = false;
    // §6.2.1.12 ac4_presentation_substream_info(). Unset for an EMDF-only
    // presentation (presentation_config 6), which carries none.
    std::optional<int> presentation_substream_index;
    bool b_alternative = false;
    bool b_pres_ndot = false;
    // Substreams holding emdf_payloads_substream() (§4.2.4.4), from the
    // presentation's emdf_info() and its additional EMDF substream list.
    std::vector<int> emdf_payloads_substream_indices;
    // The presentation's emdf_info(), and each additional EMDF substream's.
    EmdfVersionKey emdf{};
    bool b_add_emdf_substreams = false;
    std::vector<EmdfVersionKey> add_emdf;
    // Annex E.10's de_indicator and immersive_audio_indicator. They describe
    // the substreams (metadata()'s dialogue enhancement, the presentation
    // substream's immersive_audio_indicator), which the table of contents
    // does not carry, so parse_raw_frame() leaves them unset. A writer that
    // knows them sets them, and build_dac4() then writes them.
    std::optional<bool> de_indicator;
    std::optional<bool> immersive_audio_indicator;
    // An alternative presentation's name and targets (Annex E.12), which its
    // presentation substream carries and the table of contents does not:
    // parse_raw_frame() leaves this unset, and build_dac4() describes an
    // alternative presentation only where a writer that knows them has set it.
    std::optional<AlternativeInfo> alternative_info;
};

// --- §4.2.1 / §6.2.1.1 ac4_toc ---------------------------------------------

struct Toc {
    int bitstream_version = 0;
    int sequence_counter = 0;
    std::optional<int> wait_frames;  // Table 81
    int sample_rate_hz = 48000;      // Table 82
    int frame_rate_index = 0;        // Table 83/84
    bool b_iframe_global = false;
    int n_presentations = 0;
    int payload_base = 0;  // bytes, relative to the end of the byte-aligned ac4_toc()

    // Exactly one of these two is populated, selected by bitstream_version
    // (see parse_raw_frame()): presentations_v0 for <= 1, presentations_v1 and
    // substream_groups for >= 2.
    std::vector<PresentationInfoV0> presentations_v0;
    std::vector<PresentationInfoV1> presentations_v1;
    std::vector<SubstreamGroupInfo> substream_groups;

    int n_substreams = 0;
    std::vector<int> substream_sizes;  // bytes, §4.3.3.12.4

    // §6.2.1.1's program identifier, for bitstream_version 2: short_program_id
    // where b_program_id is set, and program_uuid's 16 bytes where
    // b_program_uuid_present is.
    std::optional<int> short_program_id;
    std::optional<std::array<std::byte, 16>> program_uuid;
};

// --- §4.2.4.2 / §6.2.2.2 ac4_substream: outer envelope only -----------------

struct Substream {
    std::size_t offset = 0;  // byte offset of ac4_substream_data() within the raw frame
    std::size_t size = 0;    // bytes, from Toc::substream_sizes
    // True when this index was referenced by an ac4_substream_info()/
    // ac4_substream_info_chan()/ac4_substream_info_ajoc()/
    // ac4_substream_info_obj() element - i.e. this is an ac4_substream()
    // this parser knows how to read the audio_size header of (Table 50:
    // all four map to the same envelope). False covers
    // ac4_presentation_substream(), oamd_substream() and
    // emdf_payloads_substream() (§6.2.1.12, §6.2.2.4, §4.2.4.4) - different
    // shapes, reported by byte range only.
    bool is_audio = false;
    std::optional<int> audio_size;  // §4.3.4.1, only set when is_audio
};

struct RawFrame {
    Toc toc;
    std::vector<Substream> substreams;
};

// §4.2.1 raw_ac4_frame(): ac4_toc() then n_substreams substream payloads,
// located via payload_base and substream_index_table()'s sizes
// (§4.3.3.12.4's Pseudocode 1) rather than by parsing through audio_data.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::expected<RawFrame, Error> parse_raw_frame(
    std::span<const std::byte> raw_ac4_frame);

}  // namespace iclforge::ac4
