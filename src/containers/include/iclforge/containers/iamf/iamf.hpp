#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/export.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// High level writing and reading of IAMF (Immersive Audio Model and Formats), per the AOM IAMF
// v2.0.0 specification (https://aomediacodec.github.io/iamf/). This header is the convenient end
// of the module; model.hpp, sequence.hpp and container.hpp are the full one, and everything here
// is built on them.
//
// What it writes: LPCM (`ipcm`) substreams, since E-AC-3 is not in IAMF's codec list (Opus,
// AAC-LC, FLAC and LPCM) and never can be. mux() is a decode-then-rewrap bridge to the IAMF
// ecosystem: a caller decoding a natively-7.1.4-coded E-AC-3 stream (iclforge::ac3::plan::
// LayoutId::k714) gets 12 discrete channels from iclforge::ac3::Eac3Decoder and hands them over as
// PCM; see examples/mux_iamf.cpp. mux_objects() writes object-based Audio Elements, the v2.0
// addition, with positions animated by Parameter Block OBUs.
//
// Standalone and codec-blind in the sense iclforge::containers::matroska, iclforge::containers::mp4 and iclforge::iab
// are: it links nothing from iclforge::ac3. Every OBU and box field is transcribed from the
// published specification, with the structure named at each call site, per CONTRIBUTING.md's
// clean-room rule. AOM's `libiamf` and Open Audio Renderer are oracles only.
//
// Each of the build_*() functions returns the Sequence, which can then be written as an ISO-BMFF
// file (write_isobmff()), as a raw OBU stream (write_sequence()), or fragment by fragment
// (FragmentedWriter). mux() and mux_objects() are the ISO-BMFF shortcut.

namespace iclforge::containers::iamf {

enum class MuxError : std::uint8_t {
    kNoFrames,             // frames is empty
    kInvalidTrack,         // sample_rate/bit_depth not one of the LPCM allowed sets, or
                           // samples_per_frame == 0
    kFrameSizeMismatch,    // a Frame's channel did not carry exactly samples_per_frame samples
    kInvalidTrim,          // trimming that the Audio Frame OBU rules do not allow: more samples
                           // trimmed than the audio holds, or more than a frame trimmed at the end
    kNoObjects,            // mux_objects() was given no Audio Elements, or an element with no objects
    kBadObjectElement,     // an Audio Element with more than two objects
    kObjectLengthMismatch, // the objects of one program do not all have the same number of samples
    kTooManyChannels,      // more than the 18 channels the object-based profiles allow
    kBadPositions,         // a position list that is neither empty nor one entry per frame
    kWriteFailed,          // the Sequence built could not be written; see the Error it came from
};

[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::string_view describe(MuxError error);

// One Temporal Unit's worth of PCM: samples_per_frame samples of each of the 12 channels of a
// 7.1.4ch Audio Element (loudspeaker_layout = 7), ordered L, C, R, Lss, Rss, Lrs, Rrs, Ltf, Rtf,
// Ltb, Rtb, LFE. Planar, matching iclforge::ac3::DecodedAccessUnit::channels' own storage; see
// examples/mux_iamf.cpp for how a caller permutes a decoded access unit into this order. Every
// frame carries exactly AudioTrack::samples_per_frame samples per channel.
struct Frame {
    std::array<std::vector<float>, 12> channels;
};

// A Mix Presentation's LoudnessInfo(): integrated loudness in LKFS (ITU-R BS.1770-4) and digital
// peak in dBFS. The specification defines no "unmeasured" value, and this module is DSP-free, so
// the default {0, 0} is a structurally valid placeholder for a caller with no measurement.
struct LoudnessInfo {
    float integrated_loudness_lkfs = 0.0F;
    float digital_peak_dbfs = 0.0F;
};

struct AudioTrack {
    std::uint32_t sample_rate = 48000;       // one of {44100, 16000, 32000, 48000, 96000}
    int bit_depth = 24;                      // one of {16, 24, 32}
    std::uint32_t samples_per_frame = 1024;  // this writer's own choice; IAMF does not mandate one
    // Every sub-mix includes loudness for the Stereo (Sound System A) layout, besides the
    // programme's own layout.
    LoudnessInfo stereo_loudness{};
    LoudnessInfo layout_714_loudness{};
    // Samples of the programme's audio to discard at the start and the end, written as trimming on
    // the Audio Frame OBUs (and, in ISO-BMFF, an edit list). The start trim may cover several
    // frames; the end trim must fit in the last frame.
    std::uint32_t trim_start_samples = 0;
    std::uint32_t trim_end_samples = 0;
    // Written as a Temporal Delimiter OBU at the start of every Temporal Unit when the Sequence is
    // written as a raw OBU stream. An ISO-BMFF IA Sample never holds one.
    bool temporal_delimiters = false;
    // The ISO-BMFF handler box's name.
    std::string writing_app{"iclforge"};
};

// The IA Sequence for a 7.1.4ch LPCM programme: one channel-based Audio Element (one layer, seven
// Audio Substreams: five coupled pairs, the centre and the LFE) and one Mix Presentation with the
// mandatory Stereo loudness layout and the 7.1.4 one.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Sequence, MuxError> build_sequence(
    const AudioTrack& track, std::span<const Frame> frames);

// build_sequence() written as an ISO-BMFF file.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const Frame> frames);

// --- Object-based audio -------------------------------------------------------------------------

enum class PositionCoding : std::uint8_t {
    kPolar,        // azimuth, elevation and distance: PARAMETER_DEFINITION_POLAR
    kCartesian8,   // x, y, z in 8 bits each
    kCartesian16,  // x, y, z in 16 bits each
};

// A position in room coordinates. Polar uses azimuth_deg (0 straight ahead, positive to the left,
// -180 to 180), elevation_deg (-90 to 90) and distance (0 to 1, 1 on the unit sphere). Cartesian
// uses x (left to right), y (back to front) and z (bottom to top), each -1 to 1 with the cube's
// surface at +-1. Which fields apply follows ObjectTrack::position_coding.
struct ObjectPosition {
    double azimuth_deg = 0.0;
    double elevation_deg = 0.0;
    double distance = 1.0;
    double x = 0.0;
    double y = 1.0;
    double z = 0.0;
};

struct ObjectSource {
    std::vector<float> samples;  // mono PCM of the whole programme
    // The position the object has when no Parameter Block says otherwise: the parameter
    // definition's default, and the start of the first block's movement.
    ObjectPosition initial_position;
    // Empty for an object that stays at initial_position, which writes no Parameter Blocks at all.
    // Otherwise one position per frame: where the object is at the end of that frame, reached by
    // linear interpolation (INTER_LINEAR). A polar move of about 180 degrees, which linear
    // interpolation of the direction cannot describe, is written as a jump (STEP).
    std::vector<ObjectPosition> positions;
};

// One object-based Audio Element: one Audio Substream carrying one object (coded mono) or two
// (coded as a stereo pair, with the "dual" position parameters).
struct ObjectElement {
    std::vector<ObjectSource> objects;
};

struct ObjectTrack {
    std::uint32_t sample_rate = 48000;
    int bit_depth = 24;
    std::uint32_t samples_per_frame = 1024;
    PositionCoding position_coding = PositionCoding::kPolar;
    LoudnessInfo stereo_loudness{};
    // A Temporal Delimiter OBU at the start of every Temporal Unit, for a raw OBU stream.
    bool temporal_delimiters = false;
    // The ISO-BMFF handler box's name.
    std::string writing_app{"iclforge"};
};
// Programmes whose length is not a whole number of frames are padded with silence, and the
// padding is trimmed from the last frame.

// The IA Sequence for a program of object-based Audio Elements: one Audio Element per
// ObjectElement, one Mix Presentation referencing all of them, Base-Advanced profile (at most 18
// channels in all).
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Sequence, MuxError> build_object_sequence(
    const ObjectTrack& track, std::span<const ObjectElement> elements);

[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux_objects(
    const ObjectTrack& track, std::span<const ObjectElement> elements);

// --- Reading PCM back ---------------------------------------------------------------------------

// The decoded audio of one Audio Element.
struct DecodedElement {
    std::uint32_t sample_rate = 0;
    // One name per channel, in channel order: the speaker names of a channel-based layout
    // ("L", "R", ...), "Object 1" and "Object 2", or "ACN n" for an Ambisonics channel.
    std::vector<std::string> channel_names;
    std::vector<std::vector<float>> channels;  // planar, with the trimming applied
};

// Decodes the `ipcm` Audio Substreams of an Audio Element to float PCM in [-1, 1). Supports
// channel-based elements with one layer in a layout layout_info() knows, object-based elements, and
// scene-based elements in mono mode; any other element, and any codec but ipcm, is kUnsupported.
// The Sequence is as read_sequence() or read_isobmff() returns it, or as build_sequence() does.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<DecodedElement, Error> decode_pcm(const Sequence& sequence,
                                                                                    std::uint32_t audio_element_id);

}  // namespace iclforge::containers::iamf
