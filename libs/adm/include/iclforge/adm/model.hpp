#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "iclforge/adm/export.hpp"

// The Audio Definition Model (ADM) object graph and the container-level data
// that surrounds it, per Recommendation ITU-R BS.2076-2 ("Audio definition
// model", Annex 1) and Recommendation ITU-R BS.2088-1 ("Long-form file format
// for the international exchange of audio programme materials with
// metadata", Annex 1 - the BW64 container).
//
// This header is deliberately just data: every type here is a plain
// aggregate mirroring one ADM element or one BW64 chunk, with no behaviour
// and no opinion about what an "object" or a "bed" means in AC-3/E-AC-3/JOC
// terms. That mapping is roadmap item B1's phase 2, a separate task - this
// module (iclforge::adm) has no dependency on iclforge::ac3 and does not know
// those concepts exist. IDs are carried as ADM's own string form (e.g.
// "AO_1001", "AC_00031001") rather than resolved into pointers/indices: BS.
// 2076-2 clause 6 defines every ID as a formatted string used purely for
// cross-referencing, and resolving those references is exactly the kind of
// judgement call phase 2 should make once it knows what it needs the graph
// shaped like.
//
// NOTE on the "ac3adm" name: this module is implemented internally on top
// of the vendored third-party libraries libbw64 and libadm (see
// libs/adm/CMakeLists.txt) - and libadm's own public C++ namespace is
// `adm`. This project's namespace here is deliberately "ac3adm", not "adm",
// specifically to avoid colliding with that dependency: `::adm::AudioObject`
// (libadm's parsed-XML class) and this header's own AudioObject would
// otherwise be the same fully-qualified name for two different types.
// ac3adm's own types are independent of and not derived from libadm's -
// libs/adm/src/adm_model.cpp is the only place both namespaces meet, and
// it stays entirely inside this module's implementation, never in a public
// header.

namespace iclforge::adm {

// BS.2076-2 §8, Fig. 4/5: an audioBlockFormat's position is either polar
// (azimuth/elevation/distance) or Cartesian (X/Y/Z) - "not both" (§5.8.3
// says the same for audioProgrammeReferenceScreen, and §5.4.3.3's Tables 15/
// 16 make the same split for Objects/DirectSpeakers). The `cartesian` flag
// inside audioBlockFormat (Table 17) selects which one a given block uses;
// this variant carries that choice instead of leaving both populated and
// hoping a caller checks the right one.
struct PolarPosition {
    double azimuth_deg = 0.0;    // BS.2076-2 Table 15: -180 <= azimuth <= 180
    double elevation_deg = 0.0;  // Table 15: -90 <= elevation <= 90
    double distance = 1.0;       // Table 15: normalized, 1.0 = unit sphere surface
};

struct CartesianPosition {
    double x = 0.0;  // BS.2076-2 Table 16: left/right, normalized to the unit cube
    double y = 0.0;  // back/front
    double z = 0.0;  // bottom/top
};

using Position = std::variant<PolarPosition, CartesianPosition>;

// BS.2076-2 Table 7/10/20/53: the five defined typeDefinition values plus
// the 0x1000-0xFFFF "User Custom" range. kUnknown covers a typeLabel this
// parser does not recognize (still surfaced, not dropped) and the case where
// neither typeLabel nor typeDefinition was present at all - Table 6 requires
// at least one, but a malformed file might have neither.
enum class TypeDefinition : std::uint8_t {
    kUnknown = 0,
    kDirectSpeakers = 1,
    kMatrix = 2,
    kObjects = 3,
    kHoa = 4,
    kBinaural = 5,
    kUserCustom = 6,
};

// BS.2076-2 §10.4 zoneExclusion: one `zone` child. The six Cartesian bounds are attributes of
// the element; the text content, when present, is a label (TS 103 420 Table B.18 uses "ZM1",
// "ZM2_Left", "ZU" and so on). A zone given in polar bounds only carries its label and has
// has_bounds = false.
struct ExclusionZone {
    std::string label;
    bool has_bounds = false;
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
    double min_z = 0.0;
    double max_z = 0.0;
};

// BS.2076-2 §10.5 objectDivergence. Divergence is in [0, 1]; the two range values apply to the
// position-based and azimuth-based forms respectively and keep the schema's own defaults when
// absent.
struct ObjectDivergenceInfo {
    double value = 0.0;
    bool has_azimuth_range = false;
    double azimuth_range_deg = 45.0;
    bool has_position_range = false;
    double position_range = 0.0;
};

// BS.2076-3 §5.4.3.2, Table A1-16: one `coefficient` of a Matrix block's `matrix` element - the
// multiplication factor that one other audioChannelFormat contributes to the channel the block
// describes ("Side = 0.5*Left - 0.5*Right" is two of these). The element's text is the other
// channel's ID. Every block's coefficients are summed to give the channel.
//
// `gain`, `phase_deg` and `delay_ms` are the constant forms and keep the schema's defaults when the
// attribute is absent. Each has a `*_var` form that names a variable for the renderer to resolve
// instead (the standard allows one or the other for each, never both); the constant then stays at
// its default and the name is held here, unresolved. `gain` is always linear: a gainUnit of "dB" is
// converted once at parse time, as AudioBlockFormat::gain is.
struct MatrixCoefficient {
    std::string input_channel_format_ref;  // audioChannelFormatID of the channel this one reads
    double gain = 1.0;                     // linear; negative inverts the signal
    double phase_deg = 0.0;                // phase shift, degrees
    double delay_ms = 0.0;                 // time delay, milliseconds
    std::string gain_var;                  // empty: the constant above applies
    std::string phase_var;
    std::string delay_var;
};

// BS.2076-2 §5.4: one audioBlockFormat, the unit that divides an
// audioChannelFormat along the time axis (§5.4.1: a channel with a single
// block is static; more than one means it is dynamic over time and both
// rtime/duration should be present).
//
// Fields cover the common sub-elements (§5.4.3, Table 11) plus the
// typeDefinition-specific ones this phase's downstream consumer (the
// motion-mapping phase 2, see iclforge::objects::oba::motion's KeyframePath) actually
// needs: position/width/height/depth/speakerLabel/diffuse for
// DirectSpeakers and Objects (§§5.4.3.1, 5.4.3.3), plus HOA's order/degree/
// normalization (§5.4.3.4). channelLock, jumpPosition/interpolationLength
// (§10.2, §10.3) are carried too since they directly affect how a phase-2
// mapper should read the position sequence. For Objects blocks it also carries
// the rendering constraints of §10.4-10.6 - zoneExclusion, objectDivergence,
// screenRef and headLocked - since a consumer that drops them changes how the
// object is rendered. libadm does not parse zoneExclusion at all (its parser
// and writer both leave a TODO for it), so that one element is read from the
// axml text by libs/adm/src/adm_xml_extras.cpp instead. So are a Matrix block's
// outputChannelFormatIDRef, `matrix` coefficients and jumpPosition (§5.4.3.2),
// for which libadm's Matrix block has no parameter either. A Binaural block has
// nothing of its own beyond the common fields (§5.4.3.5).
struct AudioBlockFormat {
    std::string id;  // audioBlockFormatID, e.g. "AB_00031001_00000001"

    // §5.4.1: relative to the start of the parent audioObject. Default
    // 00:00:00.00000 (rtime absent) and "unbounded" (duration absent) are
    // both spelled out by the caller checking has_duration/duration_s.
    double rtime_s = 0.0;
    bool has_duration = false;
    double duration_s = 0.0;

    // Table 11: linear gain applied to this block's samples. Always stored
    // as linear here regardless of the source's gainUnit ("linear" or "dB",
    // §12) - converting once at parse time means nothing downstream has to
    // care which the file used.
    double gain = 1.0;
    bool has_importance = false;
    int importance = 10;

    // DirectSpeakers (§5.4.3.1) / Objects (§5.4.3.3): populated whenever the
    // block carries a <position> element at all; empty position sub-elements
    // (a channel that never says where it is) leave this at PolarPosition{}.
    bool cartesian = false;  // Table 17: which alternative `position` holds
    Position position{};

    // Objects only (Table 15/16/17): width/height/depth default to 0 (a
    // point source) per their own Quantity/Default column.
    double width = 0.0;
    double height = 0.0;
    double depth = 0.0;
    double diffuse = 0.0;  // §10.1: 0 = direct, 1 = fully diffuse

    // DirectSpeakers only (Table 12): 0..* speakerLabel children, e.g. "M+030".
    std::vector<std::string> speaker_labels;

    // Objects only (§10.2/§10.3): unconditioned channelLock (max_distance
    // absent) means "snap to the nearest speaker regardless of distance".
    bool has_channel_lock = false;
    bool channel_lock = false;
    bool has_channel_lock_max_distance = false;
    double channel_lock_max_distance = 0.0;
    bool has_jump_position = false;
    bool jump_position = false;
    bool has_interpolation_length = false;
    double interpolation_length_s = 0.0;

    // Objects only (§10.4-10.6). zone_exclusion is empty when the element is absent;
    // object_divergence is only meaningful when has_object_divergence is set; screen_ref and
    // head_locked default to false, as in the schema.
    std::vector<ExclusionZone> zone_exclusion;
    bool has_object_divergence = false;
    ObjectDivergenceInfo object_divergence;
    bool screen_ref = false;
    bool head_locked = false;

    // HOA only (§5.4.3.4, Table 18): order/degree are required by the spec
    // when typeDefinition is HOA, so has_* tracks whether they were actually
    // present rather than trusting the zero default to mean "ACN 0".
    bool has_hoa_order = false;
    int hoa_order = 0;
    bool has_hoa_degree = false;
    int hoa_degree = 0;
    std::string hoa_normalization;  // "N3D", "SN3D" (default) or "FuMa", §11.2
    // §5.4.3.4, Table A1-17: the near-field-compensation reference distance of the loudspeaker
    // setup, in metres. Absent, or 0, means no compensation is needed. `hoa_equation` is the
    // optional free-text equation that describes the component; empty when absent. screen_ref and
    // head_locked above are read for HOA blocks too, where the schema has them.
    bool has_nfc_ref_dist = false;
    double nfc_ref_dist = 0.0;
    std::string hoa_equation;

    // Matrix only (§5.4.3.2, Tables A1-15 and A1-16). `output_channel_format_ref` is the channel a
    // decoding or direct matrix produces (empty for an encoding matrix, and when the file spelled
    // it with the legacy `outputChannelIDRef`, which is read too). `matrix` is the list of
    // coefficients; the channel is their sum. jumpPosition is read into has_jump_position,
    // jump_position and interpolation_length_s above, as for an Objects block.
    std::string output_channel_format_ref;
    std::vector<MatrixCoefficient> matrix;
};

// BS.2076-2 §5.3: a single sequence of audio samples, subdivided in time by
// its audioBlockFormats.
struct AudioChannelFormat {
    std::string id;    // audioChannelFormatID, e.g. "AC_00031001"
    std::string name;  // audioChannelFormatName
    TypeDefinition type = TypeDefinition::kUnknown;
    std::vector<AudioBlockFormat> block_formats;  // §5.3.2: 1..*
};

// BS.2076-2 §5.5: groups audioChannelFormats (or nested audioPackFormats)
// that belong together, e.g. a stereo pair or a 5.1 bed.
struct AudioPackFormat {
    std::string id;    // audioPackFormatID, e.g. "AP_00031001"
    std::string name;  // audioPackFormatName
    TypeDefinition type = TypeDefinition::kUnknown;
    std::vector<std::string> channel_format_refs;  // audioChannelFormatIDRef*
    std::vector<std::string> pack_format_refs;      // audioPackFormatIDRef* (nesting)

    // Matrix only (§5.5.4, Table A1-24). An encoding matrix names the channel-based pack it reads
    // (input) and the decoding matrices that undo it; a decoding matrix names the pack it produces
    // (output) and the encoding matrices it undoes; a direct matrix names both packs. Empty when
    // absent. libadm has no model for any of the four, so they are read from the axml text.
    std::vector<std::string> encode_pack_format_refs;
    std::vector<std::string> decode_pack_format_refs;
    std::string input_pack_format_ref;
    std::string output_pack_format_ref;

    // HOA only (§5.5.5, Table A1-25): the defaults for the pack's HOA blocks, which a block's own
    // values override. `hoa_normalization` is empty when the pack does not say (the schema's
    // default is "SN3D"); nfc_ref_dist is in metres. Also read from the axml text.
    std::string hoa_normalization;
    bool has_nfc_ref_dist = false;
    double nfc_ref_dist = 0.0;
    bool screen_ref = false;
};

// BS.2076-2 §5.2: identifies the combination of audioTrackFormats needed to
// decode a signal, and whether it names a single channel or a whole pack.
struct AudioStreamFormat {
    std::string id;    // audioStreamFormatID, e.g. "AS_00031001"
    std::string name;  // audioStreamFormatName
    std::optional<std::string> channel_format_ref;  // 0 or 1, §5.2.2 Table 5
    std::optional<std::string> pack_format_ref;      // 0 or 1 - only one of the two is set
    std::vector<std::string> track_format_refs;      // 0..*
};

// BS.2076-2 §5.1: describes the format of one physical track's data.
struct AudioTrackFormat {
    std::string id;    // audioTrackFormatID, e.g. "AT_00031001_01"
    std::string name;  // audioTrackFormatName
    std::optional<std::string> stream_format_ref;  // §5.1.2: audioStreamFormatIDRef
};

// BS.2076-2 §5.9: the unique identifier for one actual audio track/asset,
// joined to the BW64 <chna> chunk's own UID column (§7) by matching `uid`.
struct AudioTrackUid {
    std::string uid;  // e.g. "ATU_00000001" ("ATU_00000000" per §5.6.2 means silent/no track)
    bool has_sample_rate = false;
    std::uint32_t sample_rate = 0;
    bool has_bit_depth = false;
    std::uint32_t bit_depth = 0;
    // §5.9.2: exactly one of these is normally present - track_format_ref
    // for coded/explicit-stream audio, or channel_format_ref directly when
    // audioTrackFormat/audioStreamFormat were both omitted for plain PCM
    // (§5.1, "the audioTrackUID has to refer to the corresponding
    // audioChannelFormat").
    std::optional<std::string> track_format_ref;
    std::optional<std::string> channel_format_ref;
    std::optional<std::string> pack_format_ref;
};

// BS.2076-2 §5.6: links content/format together - which tracks (via
// audioTrackUID) carry this object, and which audioPackFormat describes
// their layout. audioObjects can nest (object_refs), and start/duration are
// relative to the containing audioProgramme (§5.6.7).
struct AudioObject {
    std::string id;    // audioObjectID, e.g. "AO_1001"
    std::string name;  // audioObjectName
    double start_s = 0.0;
    bool has_duration = false;
    double duration_s = 0.0;
    std::vector<std::string> pack_format_refs;  // audioPackFormatIDRef*
    std::vector<std::string> track_uid_refs;    // audioTrackUIDRef*
    std::vector<std::string> object_refs;       // audioObjectIDRef* (nesting)
};

// BS.2076-2 §5.7: describes what an audioObject (or set of them) contains -
// language, dialogue/music/effect classification, loudness. Loudness
// metadata sub-elements (§5.7.4) are not carried here: iclforge::ac3::meta::loudness
// already measures loudness independently, and this phase has no consumer
// for a second, file-supplied copy of the same numbers.
struct AudioContent {
    std::string id;    // audioContentID, e.g. "ACO_1001"
    std::string name;  // audioContentName
    std::vector<std::string> object_refs;  // audioObjectIDRef, 1..*
};

// BS.2076-2 §5.8: the top-level "complete mix" grouping of audioContents. A
// file can define more than one (alternate language mixes, personalized
// audio, §5.8's example) - the lowest-ID one is conventionally the default
// when nothing else picks.
struct AudioProgramme {
    std::string id;    // audioProgrammeID, e.g. "APR_1001"
    std::string name;  // audioProgrammeName
    std::vector<std::string> content_refs;  // audioContentIDRef, 1..*
};

// The whole ADM object graph from one <axml> chunk's XML document
// (audioFormatExtended, BS.2076-2 §5.10). Every collection is populated in
// document order; cross-references between elements are left as the ID
// strings above rather than resolved, per this header's own top comment.
//
// pack_formats/channel_formats/stream_formats/track_formats are never just what the file itself
// declared: the underlying parser (libadm) always merges the file's content into a document
// already pre-populated with Annex A's "common definitions" (~940 predefined elements, one set
// per standard loudspeaker layout and HOA component up to third order), so a file that refers to
// a common-definition ID without re-declaring it locally still resolves. programmes/contents/
// objects/track_uids are unaffected - Annex A defines none of those four - so those collections
// really are only what the file itself declared.
struct AdmModel {
    std::vector<AudioProgramme> programmes;
    std::vector<AudioContent> contents;
    std::vector<AudioObject> objects;
    std::vector<AudioPackFormat> pack_formats;
    std::vector<AudioChannelFormat> channel_formats;
    std::vector<AudioStreamFormat> stream_formats;
    std::vector<AudioTrackFormat> track_formats;
    std::vector<AudioTrackUid> track_uids;
};

// BS.2088-1 §8: one row of the <chna> chunk - the join between a physical
// PCM track number in <data> and the ADM's own ID space. §8.2: a single
// physical track can carry more than one entry when its format changes
// partway through the file (different audioTrackUIDs for different time
// spans), so this is deliberately a flat vector rather than one-entry-per-
// track - callers that want "the entries for track N" filter by
// track_index themselves.
struct ChnaEntry {
    std::uint16_t track_index = 0;  // 1-based; 0 marks an unused/padding row (§8.2)
    std::string uid;                // audioTrackUID value, e.g. "ATU_00000001"
    std::string track_ref;          // audioTrackFormatID or audioChannelFormatID, e.g. "AT_00031001_01"
    std::string pack_ref;           // audioPackFormatID, e.g. "AP_00031001" (may be empty, §8.2)
};

// The <data> chunk's decoded PCM, one vector per physical track in file
// order - the same shape iclforge::base::WavData uses (see iclforge/base/wav.hpp), so a
// caller already familiar with that convention needs nothing new here.
// Samples are normalized to [-1, 1). Integer PCM (8/16/24/32-bit) and
// IEEE float (32/64-bit) both read, through the vendored libbw64 directly -
// its own <fmt > parsing accepts WAVE_FORMAT_IEEE_FLOAT the same way it
// accepts WAVE_FORMAT_PCM, and Bw64Reader::read() decodes whichever the file
// declared. `bits_per_sample` is the container width either way, and not, on
// its own, a statement about which of the two it was. Most real ADM BWF
// masters are 16- or 24-bit integer (EBU Tech 3306/BS.2088-1 Annex 2 §2's
// own PCM-only framing); float ones exist too.
struct PcmAudio {
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::vector<float>> channels;

    [[nodiscard]] std::size_t frame_count() const {
        return channels.empty() ? 0 : channels.front().size();
    }
};

// Everything one BW64/RF64 file (or plain RIFF/WAVE carrying the same
// chunks under the 4 GB threshold, §2.5) yields: the ADM graph, the <chna>
// join table, and the decoded audio. `model` is default-constructed (empty)
// when the file has no <axml> chunk at all - BS.2088-1 does not require one
// (§9 only constrains what happens *if* ADM metadata is carried).
struct AdmDocument {
    AdmModel model;
    std::vector<ChnaEntry> chna;
    PcmAudio audio;
};

}  // namespace iclforge::adm
