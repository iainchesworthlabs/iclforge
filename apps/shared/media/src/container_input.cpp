#include "container_input.hpp"
#include "container_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <vector>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/matroska/reader.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "iclforge/containers/mpegts/reader.hpp"

namespace iclforge::apps {

namespace {

// EBML's own magic: the four bytes of the EBML header id every Matroska and
// WebM file opens with - the same kEbmlHeader constant
// libs/containers/src/matroska/ebml_detail.hpp holds, written out big-endian.
constexpr std::array<std::byte, 4> kEbmlMagic{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF},
                                              std::byte{0xA3}};

// ISOBMFF has no magic at offset 0 - it opens with a box, whose first four
// bytes are a LENGTH. The type is what identifies it, four bytes in, and
// 'ftyp' is what a well-formed file leads with (ISO/IEC 14496-12 4.3 says it
// "should be placed as early as possible"). 'styp' is a bare CMAF media
// segment, and a plain 'moov'/'mdat'/'moof' opener occurs in files written by
// tools that skipped ftyp - all of them are what a reader is handed in
// practice.
constexpr std::array<std::string_view, 5> kIsobmffLeadingTypes{"ftyp", "styp", "moov", "moof",
                                                               "mdat"};

[[nodiscard]] bool has_isobmff_box_at_start(std::span<const std::byte> head) {
    if (head.size() < 8) {
        return false;
    }
    const std::string_view type{reinterpret_cast<const char*>(head.data()) + 4, 4};
    return std::ranges::contains(kIsobmffLeadingTypes, type);
}

// RIFF/WAVE's magic: "RIFF", or RF64/BW64's 64-bit-size ids (EBU Tech 3306,
// ITU-R BS.2088), at offset 0 and "WAVE" at offset 8 - the same test
// libs/base/src/wav_format.cpp's is_riff_wave makes, repeated here because
// that header is private to forge. A WAV is not a container this reads, but
// it must be recognised positively BEFORE the packet grid below: PCM of a
// steady tone repeats bytes at a fixed period, and a 1 kHz sine at 48 kHz
// (48 samples a cycle) put five 0x47 bytes exactly 192 apart in a float WAV
// `forge decode` wrote, which the grid alone read as an M2TS capture.
[[nodiscard]] bool has_riff_wave_magic(std::span<const std::byte> head) {
    if (head.size() < 12) {
        return false;
    }
    const std::string_view id{reinterpret_cast<const char*>(head.data()), 4};
    const std::string_view form{reinterpret_cast<const char*>(head.data()) + 8, 4};
    return (id == "RIFF" || id == "RF64" || id == "BW64") && form == "WAVE";
}

// A transport stream has no header at all - it is a bare repeating grid of
// 188-byte packets, each starting with 0x47, and a capture may begin
// anywhere in it. So the test is the grid itself: a sync byte that recurs at
// one of the three strides in the wild (188, M2TS's 192, or 204 with parity)
// several times over. A lone 0x47 proves nothing; five in a row exactly a
// stride apart is not a coincidence.
//
// "Anywhere" still bounds where the grid can START, though: a capture cut
// mid-packet reaches its first whole packet's sync byte within one stride of
// its first byte, so a grid whose first sync sits a stride or more in has
// something before it that is not transport stream at all. Requiring the
// run to begin inside the first stride keeps a file that merely contains a
// periodic 0x47 pattern somewhere in its first 64 KiB (as steady PCM or a
// repetitive payload can) from being taken for one.
//
// Checked LAST, after the formats that do have magic: an MP4 or Matroska
// file can easily contain a 0x47 pattern by chance somewhere in its audio,
// and the grid test is the loosest of them.
constexpr std::array<std::size_t, 3> kTsStrides{188, 192, 204};
constexpr int kTsSyncRuns = 5;

[[nodiscard]] bool has_ts_packet_grid(std::span<const std::byte> head) {
    const std::size_t search_end = std::min(head.size(), kTsStrides.back());
    for (std::size_t at = 0; at < search_end; ++at) {
        if (std::to_integer<std::uint8_t>(head[at]) != 0x47) {
            continue;
        }
        for (const auto stride : kTsStrides) {
            if (at >= stride) {
                continue;  // not the first packet of this grid - see above
            }
            int seen = 1;
            for (int i = 1; i < kTsSyncRuns; ++i) {
                const std::size_t next = at + (stride * static_cast<std::size_t>(i));
                if (next >= head.size() || std::to_integer<std::uint8_t>(head[next]) != 0x47) {
                    break;
                }
                ++seen;
            }
            if (seen >= kTsSyncRuns) {
                return true;
            }
        }
    }
    return false;
}

// How much of the head of a file sniff_container needs to look at. Every
// magic/grid check above resolves within a few hundred bytes at most; this is
// generous headroom rather than a measured minimum, and small enough that
// sniffing a multi-gigabyte rip costs nothing.
constexpr std::size_t kContainerSniffBytes = 64 * 1024;

// One demuxed track/programme's frames, concatenated into a single owned
// buffer - iclforge::ac3::split_frames/split_access_units need one contiguous stream,
// but a container's frames are views scattered across the source file (or,
// for mpegts, across its own reassembly buffer), never contiguous with each
// other.
[[nodiscard]] std::vector<std::byte> concat_frames(
    std::span<const std::span<const std::byte>> frames) {
    std::size_t total = 0;
    for (const auto& frame : frames) {
        total += frame.size();
    }
    std::vector<std::byte> out;
    out.reserve(total);
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

// `value` counted in 1/`from` seconds, as a count in 1/`to` seconds, to the
// nearest. Whole seconds first and the remainder after, so no step
// overflows for any value a file can hold; a result past 64 bits saturates.
[[nodiscard]] std::uint64_t rescale(std::uint64_t value, std::uint32_t to, std::uint32_t from) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t whole = value / from;
    const std::uint64_t part = ((value % from) * to + (from / 2)) / from;
    if (whole > (kMax - part) / to) {
        return kMax;
    }
    return (whole * to) + part;
}

[[nodiscard]] ContainerFacts mp4_facts(
    const iclforge::containers::mp4::ReadTrack& track,
    const std::vector<iclforge::containers::mp4::SkippedTrack>& skipped, std::size_t samples) {
    ContainerFacts facts;
    facts.kind = ContainerKind::kMp4;
    for (const auto& other : skipped) {
        OtherTrack listed{.id = other.track_id,
                          .kind = std::string{describe(other.kind)},
                          .codec_id = other.codec_id,
                          .language = other.language};
        if (other.protection.has_value()) {
            const iclforge::containers::mp4::Protection& p = *other.protection;
            listed.protection = ProtectionFacts{
                .entry_type = p.entry_type,
                .scheme = p.scheme,
                .original_format = p.original_format,
                .default_kid = p.has_tenc ? iclforge::containers::mp4::format_kid(p.default_kid)
                                          : std::string{},
                .description = iclforge::containers::mp4::describe(p),
                .default_iv_size = p.default_iv_size,
                .crypt_byte_block = p.crypt_byte_block,
                .skip_byte_block = p.skip_byte_block,
                .has_clear_entry = p.track_has_clear_entry};
        }
        facts.other_tracks.push_back(std::move(listed));
    }
    facts.codec_id = track.codec_id;
    facts.track = track.track_id;
    facts.language = track.language;
    facts.samples = samples;
    facts.sample_rate = track.sample_rate;
    facts.channels = track.channels;
    facts.timescale = track.timescale;
    facts.movie_timescale = track.movie_timescale;
    facts.edits = track.edits.size();
    const iclforge::containers::mp4::CodecConfig& config = track.codec_config;
    if (!config.payload.empty()) {
        CodecBox box;
        box.bytes = config.payload.size();
        if (config.ac4) {
            box.type = "dac4";
        } else if (config.mlp) {
            // 'dmlp' (TrueHD): its format_info and peak data rate are in the TrueHD stream's own
            // major sync, which `forge probe` reads; the box is named here and nothing else.
            box.type = "dmlp";
        } else {
            box.type = config.eac3 ? "dec3" : "dac3";
            box.fscod = config.fscod;
            box.bsid = config.bsid;
            box.bsmod = config.bsmod;
            box.acmod = config.acmod;
            box.lfeon = config.lfeon;
            box.bit_rate_code = config.bit_rate_code;
            box.data_rate_kbps = config.data_rate_kbps;
            box.independent_substreams = config.eac3 ? config.num_ind_sub + 1 : 0;
            box.num_dep_sub = config.num_dep_sub;
            box.chan_loc = config.chan_loc;
            box.asvc = config.asvc;
            box.complexity_index = config.oba_complexity_index;
        }
        facts.codec_box = std::move(box);
    }
    return facts;
}

[[nodiscard]] ContainerFacts facts_of(const iclforge::containers::mp4::Demuxed& demuxed) {
    return mp4_facts(demuxed.track, demuxed.skipped, demuxed.samples.size());
}

[[nodiscard]] ContainerFacts matroska_facts(
    const iclforge::containers::matroska::ReadTrack& track,
    const std::vector<iclforge::containers::matroska::SkippedTrack>& skipped,
    std::size_t frames) {
    ContainerFacts facts;
    facts.kind = ContainerKind::kMatroska;
    for (const auto& other : skipped) {
        facts.other_tracks.push_back(OtherTrack{.id = other.track_number,
                                                .kind = std::string{describe(other.kind)},
                                                .codec_id = other.codec_id,
                                                .language = other.language});
    }
    facts.codec_id = track.codec_id;
    facts.track = track.track_number;
    facts.language = track.language;
    facts.samples = frames;
    facts.sample_rate = track.sample_rate;
    facts.channels = track.channels;
    return facts;
}

[[nodiscard]] ContainerFacts facts_of(const iclforge::containers::matroska::Demuxed& demuxed) {
    return matroska_facts(demuxed.track, demuxed.skipped, demuxed.frames.size());
}

[[nodiscard]] std::string_view signalling_token(
    iclforge::containers::mpegts::CodecSignalling signalling) {
    switch (signalling) {
        case iclforge::containers::mpegts::CodecSignalling::kAtscStreamType:
            return "atsc_stream_type";
        case iclforge::containers::mpegts::CodecSignalling::kDvbDescriptor: return "dvb_descriptor";
        case iclforge::containers::mpegts::CodecSignalling::kRegistrationDescriptor:
            return "registration_descriptor";
        case iclforge::containers::mpegts::CodecSignalling::kDvbExtensionDescriptor:
            return "dvb_extension_descriptor";
        case iclforge::containers::mpegts::CodecSignalling::kHdmvStreamType:
            return "hdmv_stream_type";
        case iclforge::containers::mpegts::CodecSignalling::kBareTruehdStreamType:
            return "bare_truehd_stream_type";
    }
    return "";
}

[[nodiscard]] ContainerFacts facts_of(const iclforge::containers::mpegts::Demuxed& demuxed) {
    const iclforge::containers::mpegts::ReadStream& stream = demuxed.stream;
    ContainerFacts facts;
    facts.kind = ContainerKind::kMpegTs;
    facts.track = stream.elementary_pid;
    facts.samples = demuxed.payloads.size();
    facts.program_number = stream.program_number;
    facts.pmt_pid = stream.pmt_pid;
    facts.stream_type = stream.stream_type;
    facts.signalling = std::string{signalling_token(stream.signalling)};
    facts.packet_size = stream.packet_size;
    if (stream.service.has_value()) {
        const iclforge::containers::mpegts::ServiceInfo& service = *stream.service;
        facts.service_present = true;
        facts.service_bsmod = service.bsmod;
        facts.service_bsmod_present = service.bsmod_present;
        facts.service_full_service = service.full_service;
        facts.service_bsid = service.bsid;
        facts.service_mainid = service.mainid;
        facts.service_priority = service.priority;
        facts.service_asvc = service.asvc.has_value() ? std::optional<int>{*service.asvc}
                                                       : std::nullopt;
        facts.service_mix_metadata = service.mix_metadata;
    }
    return facts;
}

// The codec a reader names a track's codec by, in the reader's own words: Matroska's CodecID,
// MP4's sample entry type and MPEG-TS's stream type (reported as the flags ReadStream has).
[[nodiscard]] std::optional<TrackCodec> codec_of_matroska(std::string_view codec_id) {
    namespace mkv = iclforge::containers::matroska;
    if (codec_id == mkv::kCodecAc3) return TrackCodec::kAc3;
    if (codec_id == mkv::kCodecEac3) return TrackCodec::kEac3;
    if (codec_id == mkv::kCodecTruehd) return TrackCodec::kTruehd;
    return std::nullopt;
}

[[nodiscard]] std::optional<TrackCodec> codec_of_mp4(std::string_view codec_id) {
    namespace mp4 = iclforge::containers::mp4;
    if (codec_id == mp4::kCodecAc3) return TrackCodec::kAc3;
    if (codec_id == mp4::kCodecEac3) return TrackCodec::kEac3;
    if (codec_id == mp4::kCodecAc4) return TrackCodec::kAc4;
    if (codec_id == mp4::kCodecTruehd) return TrackCodec::kTruehd;
    return std::nullopt;
}

[[nodiscard]] TrackCodec codec_of_mpegts(const iclforge::containers::mpegts::ReadStream& stream) {
    if (stream.truehd) return TrackCodec::kTruehd;
    if (stream.ac4) return TrackCodec::kAc4;
    return stream.eac3 ? TrackCodec::kEac3 : TrackCodec::kAc3;
}

// Every codec the readers select is allowed, so a track a demux did not find is not one the
// caller's set left out.
[[nodiscard]] bool allows_every_codec(const CodecSet& codecs) {
    return codecs.ac3 && codecs.eac3 && codecs.ac4 && codecs.truehd;
}

// The result for a container whose audio is all in codecs the caller's set leaves out: no bytes,
// and an error that names what the file holds, which is what this file's callers print after "is
// a" ("x.mkv is a Matroska/WebM file whose audio is TrueHD, ...").
[[nodiscard]] ElementaryStreamResult declined(std::string_view container, TrackCodec codec,
                                              ContainerFacts facts) {
    return {.bytes = {},
            .error = std::string{container} + " file whose audio is " +
                     std::string{codec_name(codec)} + ", which this program does not read",
            .declined_codec = codec,
            .container = std::move(facts)};
}

}  // namespace

std::string_view codec_name(TrackCodec codec) {
    switch (codec) {
        case TrackCodec::kAc3: return "AC-3";
        case TrackCodec::kEac3: return "E-AC-3";
        case TrackCodec::kAc4: return "AC-4";
        case TrackCodec::kTruehd: return "TrueHD";
    }
    return "";
}

std::string_view container_token(ContainerKind kind) {
    switch (kind) {
        case ContainerKind::kMatroska: return "matroska";
        case ContainerKind::kMp4: return "mp4";
        case ContainerKind::kMpegTs: return "mpegts";
        case ContainerKind::kUnknown: break;
    }
    return "";
}

StreamTrim trim_from_edit_list(const iclforge::containers::mp4::ReadTrack& track,
                               std::string& note) {
    StreamTrim trim;
    note.clear();
    const iclforge::containers::mp4::EditListEntry* media = nullptr;
    std::size_t with_media = 0;
    for (const iclforge::containers::mp4::EditListEntry& edit : track.edits) {
        if (edit.media_time < 0) {
            continue;  // an empty edit
        }
        ++with_media;
        if (media == nullptr) {
            media = &edit;
        }
    }
    if (media == nullptr) {
        return trim;
    }
    constexpr std::int32_t kNormalSpeed = 0x00010000;
    constexpr std::string_view kUntrimmed =
        ", so it is not applied and every sample of the track plays.";
    if (with_media > 1) {
        note = "The file's edit list has " + std::to_string(with_media) +
               " edits with audio in them" + std::string{kUntrimmed};
        return trim;
    }
    if (media->media_rate != kNormalSpeed) {
        note = "The file's edit list plays the audio at another speed" + std::string{kUntrimmed};
        return trim;
    }
    // The rate the stream's samples are counted at: the sample entry's field,
    // which holds every rate AC-3 and E-AC-3 code, and mdhd's timescale
    // otherwise, which is normally the same number.
    const std::uint32_t rate = track.sample_rate != 0 ? track.sample_rate : track.timescale;
    if (rate == 0 || track.timescale == 0) {
        note = "The file's edit list has no timescale to be read in" + std::string{kUntrimmed};
        return trim;
    }
    trim.start = rescale(static_cast<std::uint64_t>(media->media_time), rate, track.timescale);
    // A zero duration runs to the end of the media, as a fragmented file
    // writes it; without a movie timescale there is nothing to count one in.
    if (media->segment_duration != 0 && track.movie_timescale != 0) {
        trim.length = rescale(media->segment_duration, rate, track.movie_timescale);
    }
    return trim;
}

ContainerKind sniff_container(std::span<const std::byte> head) {
    const auto sniffed = head.first(std::min(head.size(), kContainerSniffBytes));
    if (sniffed.size() >= kEbmlMagic.size() &&
        std::equal(kEbmlMagic.begin(), kEbmlMagic.end(), sniffed.begin())) {
        return ContainerKind::kMatroska;
    }
    if (has_isobmff_box_at_start(sniffed)) {
        return ContainerKind::kMp4;
    }
    // A raw AC-3/E-AC-3 elementary stream is checked for BEFORE the packet
    // grid below, not after: this is what actually reads a well-formed
    // syncframe rather than one coincidental byte, and it settles a real
    // collision the grid alone cannot - AC-3 at 48 kbps/48 kHz codes
    // exactly 192-byte frames, one of the grid's own three strides, and a
    // steady or otherwise low-entropy signal encodes near-identical frames,
    // so "0x47 recurs every 192 bytes" is something a perfectly ordinary
    // elementary stream can produce on its own, not just an MPEG-TS capture.
    // iclforge::ac3::io::read_frame_header validates the sync word and the whole of
    // bsi, which no accidental byte pattern satisfies by chance the way a
    // single recurring byte can.
    if (iclforge::ac3::io::read_frame_header(sniffed).has_value()) {
        return ContainerKind::kUnknown;
    }
    // A WAV likewise, by its magic - see has_riff_wave_magic.
    if (has_riff_wave_magic(sniffed)) {
        return ContainerKind::kUnknown;
    }
    if (looks_like_truehd(sniffed)) {
        return ContainerKind::kUnknown;
    }
    if (has_ts_packet_grid(sniffed)) {
        return ContainerKind::kMpegTs;
    }
    return ContainerKind::kUnknown;
}

ElementaryStreamResult elementary_stream_from_bytes(std::span<const std::byte> file,
                                                    const CodecSet& codecs) {
    switch (sniff_container(file)) {
        case ContainerKind::kUnknown:
            return {.bytes = std::vector<std::byte>(file.begin(), file.end()), .error = {}};
        case ContainerKind::kMatroska: {
            namespace mkv = iclforge::containers::matroska;
            const auto demuxed = mkv::demux(
                file, mkv::ReadOptions{.auto_select = {.ac3 = codecs.ac3,
                                                       .eac3 = codecs.eac3,
                                                       .truehd = codecs.truehd}});
            if (!demuxed && demuxed.error() == mkv::DemuxError::kNoAudioTrack &&
                !allows_every_codec(codecs)) {
                // Nothing in a codec the caller can use: look again with every codec allowed,
                // to say what the file holds.
                const auto every = mkv::demux(file);
                const auto codec = every ? codec_of_matroska(every->track.codec_id) : std::nullopt;
                if (codec) {
                    return declined("Matroska/WebM", *codec, facts_of(*every));
                }
            }
            if (!demuxed) {
                return {.bytes = {},
                        .error =
                            std::string{"Matroska/WebM file this build cannot demux ("} +
                            std::string{iclforge::containers::matroska::describe(demuxed.error())} +
                            ")"};
            }
            return {.bytes = concat_frames(demuxed->frames),
                    .error = {},
                    .container = facts_of(*demuxed),
                    .codec = codec_of_matroska(demuxed->track.codec_id)};
        }
        case ContainerKind::kMp4: {
            namespace mp4 = iclforge::containers::mp4;
            const auto demuxed = mp4::demux(
                file, mp4::ReadOptions{.auto_select = {.ac3 = codecs.ac3,
                                                       .eac3 = codecs.eac3,
                                                       .ac4 = codecs.ac4,
                                                       .truehd = codecs.truehd}});
            if (!demuxed && demuxed.error() == mp4::DemuxError::kNoAudioTrack &&
                !allows_every_codec(codecs)) {
                const auto every = mp4::demux(file);
                const auto codec = every ? codec_of_mp4(every->track.codec_id) : std::nullopt;
                if (codec) {
                    return declined("MP4", *codec, facts_of(*every));
                }
            }
            if (!demuxed) {
                if (demuxed.error() == iclforge::containers::mp4::DemuxError::kProtectedTrack) {
                    // Which track, and with what scheme and KID, is what a person needs to hear
                    // first: the file is not damaged, its audio is encrypted. The generic sentence
                    // stands in if the second walk finds nothing to name.
                    const auto refused = iclforge::containers::mp4::list_refused_tracks(file);
                    std::string named;
                    if (refused.has_value()) {
                        named = iclforge::containers::mp4::describe_refusal(*refused);
                    } else {
                        named = std::string{iclforge::containers::mp4::describe(demuxed.error())};
                    }
                    return {.bytes = {}, .error = "MP4 file this build cannot demux: " + named};
                }
                return {.bytes = {},
                        .error = std::string{"MP4 file this build cannot demux ("} +
                                 std::string{iclforge::containers::mp4::describe(demuxed.error())} +
                                 ")"};
            }
            if (demuxed->track.codec_id == iclforge::containers::mp4::kCodecAc4) {
                // An 'ac-4' sample is the raw_ac4_frame ALONE (TS 103 190-2
                // Annex E.4) - no sync word, no frame_size, no CRC - so a
                // plain concatenation is not an elementary stream anything
                // can re-sync on. Re-wrap each sample in Annex G.3.1's
                // ac4_syncframe: 0xAC40 (the no-CRC sync word), the 16-bit
                // frame_size, and the escape (0xFFFF + 24-bit) for a frame
                // the short field cannot hold.
                std::vector<std::byte> out;
                for (const auto sample : demuxed->samples) {
                    const auto put = [&out](std::uint32_t v, int bytes) {
                        for (int b = bytes - 1; b >= 0; --b) {
                            out.push_back(static_cast<std::byte>((v >> (8 * b)) & 0xFFu));
                        }
                    };
                    put(0xAC40u, 2);
                    if (sample.size() >= 0xFFFF) {
                        put(0xFFFFu, 2);
                        put(static_cast<std::uint32_t>(sample.size()), 3);
                    } else {
                        put(static_cast<std::uint32_t>(sample.size()), 2);
                    }
                    out.insert(out.end(), sample.begin(), sample.end());
                }
                return {.bytes = std::move(out),
                        .error = {},
                        .container = facts_of(*demuxed),
                        .codec = TrackCodec::kAc4};
            }
            std::string note;
            const StreamTrim trim = trim_from_edit_list(demuxed->track, note);
            return {.bytes = concat_frames(demuxed->samples),
                    .error = {},
                    .trim = trim,
                    .trim_note = std::move(note),
                    .container = facts_of(*demuxed),
                    .codec = codec_of_mp4(demuxed->track.codec_id)};
        }
        case ContainerKind::kMpegTs: {
            namespace ts = iclforge::containers::mpegts;
            const auto demuxed = ts::demux(
                file, ts::ReadOptions{.auto_select = {.ac3 = codecs.ac3,
                                                      .eac3 = codecs.eac3,
                                                      .ac4 = codecs.ac4,
                                                      .truehd = codecs.truehd}});
            if (!demuxed && demuxed.error() == ts::DemuxError::kNoAudioStream &&
                !allows_every_codec(codecs)) {
                const auto every = ts::demux(file);
                if (every) {
                    return declined("Transport Stream", codec_of_mpegts(every->stream),
                                    facts_of(*every));
                }
            }
            if (!demuxed) {
                return {
                    .bytes = {},
                    .error = std::string{"Transport Stream this build cannot demux ("} +
                             std::string{iclforge::containers::mpegts::describe(demuxed.error())} +
                             ")"};
            }
            return {.bytes = concat_frames(demuxed->payloads),
                    .error = {},
                    .container = facts_of(*demuxed),
                    .codec = codec_of_mpegts(demuxed->stream)};
        }
    }
    return {.bytes = {}, .error = "unrecognised container"};
}

// ---------------------------------------------------------------------------------------------
// A container's track where it lies (container_stream.hpp)
// ---------------------------------------------------------------------------------------------

void ExtentMap::add(std::uint64_t file_offset, std::size_t length, bool join) {
    if (length == 0) {
        return;
    }
    if (join && !runs_.empty()) {
        Run& last = runs_.back();
        if (last.file_offset + last.length == file_offset &&
            length <= std::numeric_limits<std::uint32_t>::max() - last.length) {
            last.length += static_cast<std::uint32_t>(length);
            size_ += length;
            return;
        }
    }
    // A frame is far below four gibibytes (the readers refuse elements past 16 MiB); split one that
    // is not rather than let its length wrap.
    while (length > std::numeric_limits<std::uint32_t>::max()) {
        constexpr std::uint32_t kPiece = std::numeric_limits<std::uint32_t>::max();
        runs_.push_back({.stream_offset = size_, .file_offset = file_offset, .length = kPiece});
        size_ += kPiece;
        file_offset += kPiece;
        length -= kPiece;
    }
    runs_.push_back({.stream_offset = size_,
                     .file_offset = file_offset,
                     .length = static_cast<std::uint32_t>(length)});
    size_ += length;
}

std::size_t ExtentMap::read(ByteSource& file, std::uint64_t offset, std::span<std::byte> out) const {
    if (offset >= size_ || out.empty()) {
        return 0;
    }
    // The last run that begins at or before `offset`.
    auto it = std::upper_bound(
        runs_.begin(), runs_.end(), offset,
        [](std::uint64_t at, const Run& run) { return at < run.stream_offset; });
    --it;
    std::size_t done = 0;
    for (; it != runs_.end() && done < out.size(); ++it) {
        const std::uint64_t into = offset + done - it->stream_offset;
        const std::size_t want =
            std::min<std::size_t>(out.size() - done, static_cast<std::size_t>(it->length - into));
        const std::size_t got = file.read(it->file_offset + into, out.subspan(done, want));
        done += got;
        if (got < want) {
            break;
        }
    }
    return done;
}

namespace {

// The error the batch path words for a container this build cannot demux.
[[nodiscard]] std::string cannot_demux(std::string_view container, std::string_view why) {
    return std::string{container} + " file this build cannot demux (" + std::string{why} + ")";
}

[[nodiscard]] StreamedTrack failed(std::string error) {
    StreamedTrack out;
    out.error = std::move(error);
    return out;
}

[[nodiscard]] StreamedTrack declined_track(std::string_view container, TrackCodec codec,
                                           ContainerFacts facts) {
    StreamedTrack out;
    out.error = std::string{container} + " file whose audio is " + std::string{codec_name(codec)} +
                ", which this program does not read";
    out.declined_codec = codec;
    out.container = std::move(facts);
    return out;
}

[[nodiscard]] StreamedTrack index_matroska(ByteSource& file, const CodecSet& codecs,
                                           std::size_t window_bytes) {
    namespace mkv = iclforge::containers::matroska;
    std::vector<std::byte> window(window_bytes);
    const std::uint64_t size = file.size();

    // Feeds the file to a reader in windows until the file ends, or `stop` says to; `on_frame`
    // gets each frame's place.
    const auto feed = [&](mkv::Reader& reader, const mkv::Reader::FrameAtFn& on_frame,
                          const auto& stop) -> std::expected<void, mkv::DemuxError> {
        for (std::uint64_t at = 0; at < size && !stop();) {
            const std::size_t got = file.read(at, window);
            if (got == 0) {
                break;
            }
            if (auto pushed =
                    reader.push_at(std::span<const std::byte>{window}.first(got), on_frame);
                !pushed.has_value()) {
                return std::unexpected(pushed.error());
            }
            at += got;
        }
        return reader.finish();
    };

    StreamedTrack out;
    std::size_t frames = 0;
    mkv::Reader reader(mkv::ReadOptions{.auto_select = {.ac3 = codecs.ac3,
                                                        .eac3 = codecs.eac3,
                                                        .truehd = codecs.truehd}});
    const auto verdict = feed(
        reader,
        [&](std::span<const std::byte> frame, std::uint64_t at) {
            out.extents.add(at, frame.size());
            ++frames;
        },
        [] { return false; });
    if (!verdict.has_value()) {
        if (verdict.error() == mkv::DemuxError::kNoAudioTrack && !allows_every_codec(codecs)) {
            // Nothing in a codec the caller can use: look at the Tracks element again with every
            // codec allowed, to say what the file holds. The frames are not wanted.
            mkv::Reader every;
            const auto looked = feed(
                every, [](std::span<const std::byte>, std::uint64_t) {},
                [&every] { return every.track_found(); });
            (void)looked;
            if (every.track_found()) {
                if (const auto codec = codec_of_matroska(every.track().codec_id)) {
                    return declined_track("Matroska/WebM", *codec,
                                          matroska_facts(every.track(), every.skipped(), 0));
                }
            }
        }
        return failed(cannot_demux("Matroska/WebM", mkv::describe(verdict.error())));
    }
    out.container = matroska_facts(reader.track(), reader.skipped(), frames);
    out.codec = codec_of_matroska(reader.track().codec_id);
    return out;
}

[[nodiscard]] StreamedTrack index_mp4(ByteSource& file, const CodecSet& codecs) {
    namespace mp4 = iclforge::containers::mp4;
    const auto read_at = [&file](std::uint64_t offset, std::span<std::byte> out) {
        return file.read(offset, out);
    };
    const std::uint64_t size = file.size();

    StreamedTrack out;
    std::vector<mp4::SkippedTrack> skipped;
    std::size_t samples = 0;
    bool join = true;
    const auto track = mp4::demux_seekable_at(
        read_at, size, [&](const mp4::ReadTrack& t) { join = t.codec_id != mp4::kCodecAc4; },
        [&](std::span<const std::byte> sample, std::uint64_t at) {
            // AC-4 samples are wrapped one by one when they are read back, so each stays a run.
            out.extents.add(at, sample.size(), join);
            ++samples;
        },
        mp4::ReadOptions{.auto_select = {.ac3 = codecs.ac3,
                                         .eac3 = codecs.eac3,
                                         .ac4 = codecs.ac4,
                                         .truehd = codecs.truehd}},
        &skipped);
    if (!track.has_value()) {
        if (track.error() == mp4::DemuxError::kNoAudioTrack && !allows_every_codec(codecs)) {
            // As for Matroska: the track the reader would have taken, found without reading a
            // sample. Once the boxes are read the source answers nothing more.
            bool boxes_done = false;
            std::vector<mp4::SkippedTrack> other;
            mp4::ReadTrack seen;
            const auto every = mp4::demux_seekable_at(
                [&](std::uint64_t offset, std::span<std::byte> bytes) -> std::size_t {
                    return boxes_done ? 0 : file.read(offset, bytes);
                },
                size,
                [&](const mp4::ReadTrack& t) {
                    boxes_done = true;
                    seen = t;
                },
                [](std::span<const std::byte>, std::uint64_t) {}, mp4::ReadOptions{}, &other);
            if (every.has_value()) {
                if (const auto codec = codec_of_mp4(seen.codec_id)) {
                    return declined_track("MP4", *codec, mp4_facts(seen, other, 0));
                }
            }
        }
        if (track.error() == mp4::DemuxError::kProtectedTrack) {
            // Which track, and with what scheme and KID, is what a person needs to hear first: the
            // file is not damaged, its audio is encrypted. The generic sentence stands in if the
            // second walk finds nothing to name.
            const auto refused = mp4::list_refused_tracks(read_at, size);
            const std::string named = refused.has_value()
                                          ? mp4::describe_refusal(*refused)
                                          : std::string{mp4::describe(track.error())};
            return failed("MP4 file this build cannot demux: " + named);
        }
        return failed(cannot_demux("MP4", mp4::describe(track.error())));
    }
    out.container = mp4_facts(*track, skipped, samples);
    out.codec = codec_of_mp4(track->codec_id);
    if (track->codec_id != mp4::kCodecAc4) {
        out.trim = trim_from_edit_list(*track, out.trim_note);
    }
    return out;
}

}  // namespace

StreamedTrack index_container_track(ByteSource& file, const CodecSet& codecs,
                                    const StreamOptions& options) {
    std::vector<std::byte> head(
        static_cast<std::size_t>(std::min<std::uint64_t>(file.size(), kContainerSniffBytes)));
    head.resize(file.read(0, head));
    switch (sniff_container(head)) {
        case ContainerKind::kUnknown: {
            StreamedTrack out;
            out.bare = true;
            return out;
        }
        case ContainerKind::kMatroska:
            return index_matroska(file, codecs, std::max<std::size_t>(options.window_bytes, 4096));
        case ContainerKind::kMp4: return index_mp4(file, codecs);
        case ContainerKind::kMpegTs: {
            StreamedTrack out;
            out.whole_file = true;
            out.container.kind = ContainerKind::kMpegTs;
            return out;
        }
    }
    return failed("unrecognised container");
}

std::vector<std::byte> read_track_bytes(ByteSource& file, const StreamedTrack& track) {
    std::vector<std::byte> out;
    if (track.codec != TrackCodec::kAc4 || track.container.kind != ContainerKind::kMp4) {
        out.resize(static_cast<std::size_t>(track.extents.size()));
        out.resize(track.extents.read(file, 0, out));
        return out;
    }
    // An 'ac-4' sample is the raw_ac4_frame alone (TS 103 190-2 Annex E.4): wrapped in Annex
    // G.3.1's ac4_syncframe, as elementary_stream_from_bytes does for the same samples.
    std::vector<std::byte> sample;
    for (const auto& run : track.extents.all()) {
        sample.resize(run.length);
        if (track.extents.read(file, run.stream_offset, sample) != run.length) {
            break;
        }
        const auto put = [&out](std::uint32_t v, int bytes) {
            for (int b = bytes - 1; b >= 0; --b) {
                out.push_back(static_cast<std::byte>((v >> (8 * b)) & 0xFFu));
            }
        };
        put(0xAC40u, 2);
        if (sample.size() >= 0xFFFF) {
            put(0xFFFFu, 2);
            put(static_cast<std::uint32_t>(sample.size()), 3);
        } else {
            put(static_cast<std::uint32_t>(sample.size()), 2);
        }
        out.insert(out.end(), sample.begin(), sample.end());
    }
    return out;
}

}  // namespace iclforge::apps
