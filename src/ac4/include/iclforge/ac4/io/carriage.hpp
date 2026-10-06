#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac4/export.hpp"
#include "iclforge/ac4/core/toc.hpp"

// AC-4 in a container or a manifest: what a muxer, a segmenter and a playlist
// writer need of a stream, read off its parsed table of contents
// (iclforge/ac4/toc.hpp), so that each stays codec-blind.

namespace iclforge::ac4 {

// --- Carriage (AC-4 bitstream inspector's separable slice) -------------------------------
//
// Everything below serves putting AC-4 INTO a container, not parsing it:
// the 'dac4' box an ISO-BMFF 'ac-4' sample entry carries (TS 103 190-2
// Annex E.5's ac4_dsi_v1), the per-frame sample count a container's timing
// needs (Table 84), and RFC 6381's codec string for HLS/DASH signalling
// (Annex E.13). All three read the already-parsed Toc rather than raw
// bytes, so a caller pays for exactly one parse however many it needs.

// The 'dac4' box payload - ac4_dsi_v1 (Annex E.6), box header excluded, the
// same contract as iclforge::mp4::AudioTrack::codec_config ("payload only").
//
// TOC-level fields are carried in full: ac4_dsi_version 1, the stream's own
// bitstream_version / fs_index / frame_rate_index, n_presentations, and for
// bitstream_version 2 its program identifier where it sends one. The bit-rate
// DSI (Annex E.7) takes its mode from wait_frames, as Table E.7 asks, with the
// rate unknown: 0, and a precision of 0xFFFFFFFF.
//
// Each presentation gets the whole of Annex E.10's ac4_presentation_v1_dsi(),
// with an ac4_substream_group_dsi() (E.11) for each substream group its
// ac4_sgi_specifier()s name, in their order: a single substream group, the
// configurations of Table 53 (0 to 5, and 6's EMDF payloads alone), channel
// coded, A-JOC or direct coded objects, and its channel mode, core and
// channel groups by Pseudocodes 25, 26 and E.3 over every substream of those
// groups. src/ac4/ERRATA.md records the readings it takes. Its closing
// de_indicator and immersive_audio_indicator are written where the Toc
// carries them (see PresentationInfoV1), and left out otherwise, which the
// syntax allows; an alternative presentation's name and targets, which the
// syntax does not let it leave out, only from
// PresentationInfoV1::alternative_info.
//
// Empty where the Toc holds something this cannot describe whole, which
// dac4_refusal() names: a writer then has no complete box to carry.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::vector<std::byte> build_dac4(const Toc& toc);

// Why build_dac4() writes nothing for `toc`, a string literal naming what it
// cannot describe; empty where it describes every presentation whole.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view dac4_refusal(const Toc& toc);

// Why a CMAF track (TS 103 190-2 Annex H.1.2.1) cannot carry the stream `toc`
// describes, a string literal naming the first rule it breaks; empty where it
// keeps them: bitstream_version 2, presentation_version 1, at most 64
// presentations, and a presentation_id in every presentation, no two the
// same. A presentation of configuration 6, EMDF payloads alone, has no field
// for a presentation_id, so a stream with one is refused; an MP4 that is not
// fragmented carries it (build_dac4()). The rules for a presentation whose
// groups several tracks carry (H.1.2.2 and H.1.2.3), and for the samples'
// equivalent configurations (H.1.2.4), are the muxer's to keep.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view cmaf_refusal(const Toc& toc);

// Samples per AC-4 frame at the stream's own sample rate - what
// iclforge::mp4::AudioTrack::samples_per_frame and an MPEG-TS PTS cadence need.
// Table 84: most frame rates divide the sample rate exactly; the
// 1000/1001-family entries whose frame length alternates between two values
// (29.97/59.94/119.88 fps) have no single answer and return nullopt;
// media_timing() below gives an ISOBMFF track the time scale in which they
// have one. At 44.1 kHz only frame_rate_index 13 (the 2048-sample frame) is
// defined at all (Table 83).
[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<std::uint32_t> samples_per_frame(const Toc& toc);

// TS 103 190-2 Table E.1: the media time scale an ISOBMFF track of the stream
// counts in, and each sample's duration in it (sample_delta). Where a frame
// is a whole number of samples that is the sample rate and
// samples_per_frame(); 29.97, 59.94 and 119.88 fps, whose frame lengths
// alternate at 48 kHz, take the table's other time scale, 240 000, in which a
// frame is 8 008, 4 004 or 2 002. Nothing for a frame rate Table 83 or 84
// does not define.
struct MediaTiming {
    std::uint32_t timescale = 0;
    std::uint32_t sample_delta = 0;
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<MediaTiming> media_timing(const Toc& toc);

// Part 1 Tables 83 and 84 for the stream's frame_rate_index and sample rate:
// frames a second (24 000 / 1 001 at 23.976 fps, 48 000 / 2 048 at index 13),
// the samples a frame codes (frame_len_base) and the internal rate they are
// coded at, their product: 46 033.97 Hz at the 1000/1001 rates, 46 080 at 24,
// 30, 48 and 60 fps, 51 200 at 25, 50 and 100, the sample rate at index 13.
// Nothing for an index the tables reserve, or any but 13 at 44.1 kHz.
struct FrameRate {
    double frames_per_second = 0.0;
    int frame_length = 0;
    double internal_rate_hz = 0.0;
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<FrameRate> frame_rate(const Toc& toc);

// RFC 6381 codec string per Annex E.13: "ac-4.AA.BB.CC" with two lowercase
// hex digits each of bitstream_version, presentation_version and mdcompat,
// taken from signalled_presentation(), which a manifest describes a track by.
// An absent md_compat reads as 0.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::string rfc6381_codec_string(const Toc& toc);

// --- Manifests (TS 103 190-2 Annex G, and HLS) ------------------------------
//
// What an HLS playlist and a DASH MPD say of an AC-4 track, read off its table
// of contents as build_dac4() reads its box: plain values, so that the
// manifest writers (mp4/hls.hpp, mp4/dash.hpp) stay codec-blind.

// The presentation a manifest describes a track by: Annex G.2.3's "AC-4
// presentation with the widest compatibility", read as the lowest md_compat
// among the presentations that carry audio and that the stream does not
// disable, the first of them where several share it (src/ac4/ERRATA.md,
// "Manifests"). The first presentation where none carries audio; nothing for
// a table of contents without presentations.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<std::size_t> signalled_presentation(const Toc& toc);

// A DASH descriptor: the scheme it names, and its value there.
struct ManifestDescriptor {
    std::string scheme_id_uri{};
    std::string value{};
};

// Annex G.3.3's AudioChannelConfiguration for signalled_presentation(): the
// MPEG scheme urn:mpeg:mpegB:cicp:ChannelConfiguration with Table G.1's value
// where the presentation's audio channel groups (Annex E.10.3, Pseudocode E.3
// as build_dac4() writes them) map to one, which G.3.3.1 prefers, and the
// "Dolby:2015" scheme tag:dolby.com,2015:dash:audio_channel_configuration:2015
// otherwise, six hexadecimal digits with group g at bit g and bit 23 set for
// object audio (src/ac4/ERRATA.md, "Manifests", on G.3.3.2's bit order).
// Nothing for a bitstream_version below 2, or a presentation whose substreams
// the table of contents does not describe whole.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<ManifestDescriptor> dash_channel_configuration(
    const Toc& toc);

// The SupplementalProperty descriptors Annex G.3 asks of a Representation for
// signalled_presentation(): G.3.2's frame rate
// (tag:dolby.com,2017:dash:audio_frame_rate:2017, in DASH's FrameRateType:
// "25", "30000/1001", and at frame_rate_index 13 the sample rate over 2 048,
// "375/16" at 48 kHz), and G.3.1's pre-virtualized content
// (tag:dolby.com,2016:dash:virtualized_content:2016, "1") where
// b_pre_virtualized is set. Nothing for a frame rate Tables 83 and 84 do not
// define.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::vector<ManifestDescriptor> dash_supplemental_properties(
    const Toc& toc);

// How many channels signalled_presentation() has: the speakers of its audio
// channel groups (Table A.27), which is what HLS's CHANNELS attribute counts.
// Nothing for object audio, or a presentation whose substreams the table of
// contents does not describe whole.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::optional<int> presentation_channel_count(const Toc& toc);

// Annex H.1.2.4: whether two tables of contents have equivalent
// configurations, which every sample of a CMAF track must: the same
// frame_rate_index, fs_index and n_presentations; each presentation's
// b_single_substream_group and presentation_config; and each substream group's
// content_classifier, b_language_indicator and the language tag's primary
// subtag, and each of its substreams' channel_mode and sf_multiplier. Empty
// where they are, else a string literal naming the first that differs.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view configuration_difference(const Toc& a,
                                                                            const Toc& b);

}  // namespace iclforge::ac4
