#include "iclforge/containers/mp4/reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "isobmff_detail.hpp"
#include "iclforge/containers/mp4/mp4.hpp"

namespace iclforge::containers::mp4 {

namespace {

using detail::BoxHeader;
using detail::BoxRead;
using detail::fourcc;
using detail::get_u16;
using detail::get_u32;
using detail::get_u64;
using detail::get_u8;
using detail::read_box_header;

// Every box type this walk recognises. Anything else is skipped by its own
// declared length.
constexpr std::uint32_t kFtyp = fourcc("ftyp");
constexpr std::uint32_t kStyp = fourcc("styp");
constexpr std::uint32_t kMoov = fourcc("moov");
constexpr std::uint32_t kMvhd = fourcc("mvhd");
constexpr std::uint32_t kTrak = fourcc("trak");
constexpr std::uint32_t kTkhd = fourcc("tkhd");
constexpr std::uint32_t kEdts = fourcc("edts");
constexpr std::uint32_t kElst = fourcc("elst");
constexpr std::uint32_t kMdia = fourcc("mdia");
constexpr std::uint32_t kMdhd = fourcc("mdhd");
constexpr std::uint32_t kHdlr = fourcc("hdlr");
constexpr std::uint32_t kMinf = fourcc("minf");
constexpr std::uint32_t kStbl = fourcc("stbl");
constexpr std::uint32_t kStsd = fourcc("stsd");
constexpr std::uint32_t kStsc = fourcc("stsc");
constexpr std::uint32_t kStsz = fourcc("stsz");
constexpr std::uint32_t kStz2 = fourcc("stz2");
constexpr std::uint32_t kStco = fourcc("stco");
constexpr std::uint32_t kCo64 = fourcc("co64");
constexpr std::uint32_t kMvex = fourcc("mvex");
constexpr std::uint32_t kTrex = fourcc("trex");
constexpr std::uint32_t kMoof = fourcc("moof");
constexpr std::uint32_t kTraf = fourcc("traf");
constexpr std::uint32_t kTfhd = fourcc("tfhd");
constexpr std::uint32_t kTrun = fourcc("trun");
constexpr std::uint32_t kMdat = fourcc("mdat");
constexpr std::uint32_t kAc3Entry = fourcc("ac-3");
constexpr std::uint32_t kEc3Entry = fourcc("ec-3");
constexpr std::uint32_t kAc4Entry = fourcc("ac-4");  // TS 103 190-2 Annex E.4
constexpr std::uint32_t kDac3 = fourcc("dac3");
constexpr std::uint32_t kDec3 = fourcc("dec3");
constexpr std::uint32_t kDac4 = fourcc("dac4");
// A protected sample entry (ISO/IEC 23001-7 Common Encryption) replaces the entry type with
// 'enca' (audio) or 'encv' (video) and adds a 'sinf' child; the type it had is in the 'sinf's
// 'frma'. The entry types are as the W3C ISO Common Encryption stream format note gives them.
constexpr std::uint32_t kEnca = fourcc("enca");
constexpr std::uint32_t kEncv = fourcc("encv");
constexpr std::uint32_t kSinf = fourcc("sinf");
constexpr std::uint32_t kFrma = fourcc("frma");
constexpr std::uint32_t kSchm = fourcc("schm");
constexpr std::uint32_t kSchi = fourcc("schi");
constexpr std::uint32_t kTenc = fourcc("tenc");
constexpr std::uint32_t kMlpaEntry = fourcc("mlpa");  // Dolby TrueHD; ERRATA C1
constexpr std::uint32_t kDmlp = fourcc("dmlp");

// Boxes that are pure containers: the walk descends into them rather than
// buffering them whole, which is what keeps a moov of any size from being
// held in one piece.
[[nodiscard]] bool is_container(std::uint32_t type) {
    return type == kMoov || type == kTrak || type == kEdts || type == kMdia || type == kMinf ||
           type == kStbl || type == kMvex || type == kMoof || type == kTraf;
}

// Leaves the walk needs the bytes of. stsd is a FullBox with children rather
// than a plain container, so it is read whole and its one sample entry
// parsed in place - it is a few dozen bytes, not a table.
[[nodiscard]] bool is_wanted_leaf(std::uint32_t type) {
    return type == kMvhd || type == kTkhd || type == kElst || type == kMdhd || type == kHdlr ||
           type == kStsd || type == kStsc || type == kStsz || type == kStz2 || type == kStco || type == kCo64 ||
           type == kTrex || type == kTfhd || type == kTrun;
}

// The sampling frequency a TrueHD ratebits code names (TH spec §1.4), 0 for a code it does not.
[[nodiscard]] std::uint32_t mlp_rate_hz(std::uint32_t ratebits) {
    switch (ratebits) {
        case 0x0:
            return 48000;
        case 0x1:
            return 96000;
        case 0x2:
            return 192000;
        case 0x8:
            return 44100;
        case 0x9:
            return 88200;
        case 0xA:
            return 176400;
        default:
            return 0;
    }
}

// The sampling frequency a 'dmlp' format_info word states, 0 for one it does not. The word is the
// major sync's field of that name, byte for byte (ERRATA C3), so its layout is the stream's. A TrueHD
// word opens with the rate code, [31:28], and keeps [27:24] reserved (TH spec §5.1). An MLP word
// opens with the two groups' bit depths and then their rates: group 1's rate is at [23:20], and a
// stream with one group sends 0xF as group 2's depth at [27:24] (TH spec §5.2). That 0xF, which a
// TrueHD word never has, tells the two apart. Read as TrueHD's, an MLP stream's 24-bit depth code in
// [31:28] is the code for 192 kHz.
[[nodiscard]] std::uint32_t dmlp_rate_hz(std::uint32_t format_info) {
    const bool mlp_layout = ((format_info >> 24U) & 0xFU) == 0xFU;
    return mlp_rate_hz(mlp_layout ? (format_info >> 20U) & 0xFU : format_info >> 28U);
}

// --- the dac3/dec3 configuration box ---------------------------------------
//
// The read twin of iclforge::ac3::io::build_codec_config_box (ac3/io/dec3.hpp), field
// for field: ETSI TS 102 366 Annex F §F.4 (AC3SpecificBox) and §F.6
// (EC3SpecificBox), plus TS 103 420 §8.3.1/§8.3.2.2's Atmos extension, read
// through iclforge::BitReader (libs/base), which zero-extends past the end rather
// than failing: every field below is optional-by-truncation in some real file,
// and a short box means "this muxer stopped here", not "corrupt". The caller
// checks remaining_bits() where the difference matters - see the Atmos
// extension below.
CodecConfig parse_codec_config(std::uint32_t box_type, std::span<const std::byte> payload) {
    CodecConfig out;
    out.eac3 = box_type == kDec3;
    out.payload.assign(payload.begin(), payload.end());
    if (box_type == kDmlp) {
        // 'dmlp' (ERRATA C3): format_info(32) peak_data_rate(15) reserved(1) reserved(32). The
        // fields are read when the box is long enough and left at zero when it is not; the bytes
        // are kept either way.
        out.mlp = true;
        if (payload.size() >= 6) {
            out.mlp_format_info = get_u32(payload, 0);
            out.mlp_peak_data_rate = static_cast<int>(get_u16(payload, 4) >> 1U);
        }
        return out;
    }
    if (box_type == kDac4) {
        // ac4_dsi_v1 (TS 103 190-2 Annex E.5) shares no field with the
        // dac3/dec3 layout below, and this module has no business
        // interpreting it - the bytes are kept verbatim (remux-ready, same
        // contract as `payload` always had) and iclforge::ac4::'s own parser is the
        // one that understands the stream. The dac3/dec3-shaped fields stay
        // at their defaults.
        out.ac4 = true;
        return out;
    }
    BitReader bits{payload};

    if (!out.eac3) {
        // §F.4: fscod(2) bsid(5) bsmod(3) acmod(3) lfeon(1) bit_rate_code(5)
        // reserved(5).
        out.fscod = static_cast<int>(bits.read(2));
        out.bsid = static_cast<int>(bits.read(5));
        out.bsmod = static_cast<int>(bits.read(3));
        out.acmod = static_cast<int>(bits.read(3));
        out.lfeon = bits.read(1) != 0;
        out.bit_rate_code = static_cast<int>(bits.read(5));
        return out;
    }

    // §F.6: data_rate(13) num_ind_sub(3), then one independent-substream
    // record. Only the first is read: iclforge::containers::mp4::AudioTrack describes exactly one
    // track, and iclforge::ac3::io::scan groups an access unit as one independent
    // substream plus its dependents, so a second record has nowhere to go
    // in ReadTrack - and num_ind_sub is reported verbatim so a caller can
    // see that the file claimed more.
    out.data_rate_kbps = static_cast<int>(bits.read(13));
    out.num_ind_sub = static_cast<int>(bits.read(3));
    out.fscod = static_cast<int>(bits.read(2));
    out.bsid = static_cast<int>(bits.read(5));
    (void)bits.read(1);  // reserved
    out.asvc = bits.read(1) != 0;
    out.bsmod = static_cast<int>(bits.read(3));
    out.acmod = static_cast<int>(bits.read(3));
    out.lfeon = bits.read(1) != 0;
    (void)bits.read(3);  // reserved
    out.num_dep_sub = static_cast<int>(bits.read(4));
    if (out.num_dep_sub > 0) {
        out.chan_loc = static_cast<int>(bits.read(9));
    } else {
        (void)bits.read(1);  // reserved
    }

    // The Atmos extension is a TRAILING addition: a box written before TS
    // 103 420 simply ends here, which is not an error and not "no Atmos" -
    // it is "this box has nothing to say". Only a box that actually carries
    // the byte gets read, hence the explicit length check rather than
    // letting the reader zero-extend and reporting a confident false.
    if (bits.remaining_bits() >= 8) {
        (void)bits.read(7);  // reserved
        const bool flag_type_a = bits.read(1) != 0;
        if (flag_type_a && bits.remaining_bits() >= 8) {
            out.oba_complexity_index = static_cast<int>(bits.read(8));
        }
    }
    return out;
}

// --- the sample table -------------------------------------------------------

// One sample's byte range in the file. This is what both the plain
// (stsc/stsz/stco) and the fragmented (tfhd/trun) paths produce, and the
// only thing the emitter downstream understands.
struct SampleRef {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

// A run of chunks that all carry the same number of samples - ISO/IEC
// 14496-12 §8.7.4's stsc entry, which is a run-length encoding: an entry
// applies from its own first_chunk until the next entry's.
struct ChunkRun {
    std::uint32_t first_chunk = 0;  // 1-based, as stored
    std::uint32_t samples_per_chunk = 0;
};

// Everything one trak contributes, gathered as its boxes arrive and only
// acted on when the trak closes: stsd may follow stco, and nothing can be
// decided until the whole table is in.
struct PendingTrack {
    std::uint32_t track_id = 0;
    std::uint32_t timescale = 0;
    std::string language{"und"};
    // hdlr's handler_type, and the first sample entry's type whatever it is: what a track that is
    // not selected is listed by.
    std::uint32_t handler = 0;
    std::uint32_t first_entry = 0;
    bool has_entry = false;
    // The first protected sample entry of the track, if it has one, and whether 'stsd' also
    // holds an entry that is not protected.
    std::optional<Protection> protection;
    bool has_clear_entry = false;
    std::uint32_t entry_type = 0;
    std::uint32_t sample_rate = 0;
    int channels = 0;
    CodecConfig codec_config;
    std::vector<EditListEntry> edits;
    std::vector<std::uint32_t> sizes;
    std::vector<ChunkRun> chunk_runs;
    std::vector<std::uint64_t> chunk_offsets;
};

// ISO/IEC 14496-12 §8.4.2.2: three 5-bit (letter - 0x60) codes. The read
// twin of isobmff_detail.hpp's pack_language.
[[nodiscard]] std::string unpack_language(std::uint16_t packed) {
    std::string out;
    for (int shift : {10, 5, 0}) {
        const auto letter = static_cast<unsigned>((packed >> shift) & 0x1FU);
        // 0 is what pack_language writes for a character it could not map;
        // anything outside a-z is not a language code either.
        if (letter == 0 || letter > 26) {
            return "und";
        }
        out.push_back(static_cast<char>('a' + letter - 1));
    }
    return out;
}

// How many tracks a file may have listed beside the one that is read: a hostile moov declares as
// many as it likes, and each is a small string.
constexpr std::size_t kMaxSkippedTracks = 64;

// A fourcc as four characters, anything that is not printable ASCII shown as '?'.
[[nodiscard]] std::string fourcc_text(std::uint32_t value) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto c = static_cast<unsigned>((value >> shift) & 0xFFU);
        out.push_back(c >= 0x20U && c <= 0x7EU ? static_cast<char>(c) : '?');
    }
    return out;
}

// --- the protection scheme information box ---------------------------------
//
// Walks the boxes that fill body[at, end), calling fn(type, body) for each. A box whose header
// does not parse, or whose size runs past `end`, ends the walk and is not an error: a damaged
// 'sinf' must not turn a protected track into a malformed file or into a clear one, because the
// damage is no reason to read its samples.
template <typename Fn>
void for_each_child(std::span<const std::byte> body, std::size_t at, std::size_t end,
                    const Fn& fn) {
    while (end - at >= 8) {
        BoxHeader child{};
        if (read_box_header(body.first(end), at, child) != BoxRead::kOk || child.to_eof ||
            child.size > end - at) {
            return;
        }
        fn(child.type, body.subspan(at + child.header_bytes,
                                    static_cast<std::size_t>(child.size) - child.header_bytes));
        at += static_cast<std::size_t>(child.size);
    }
}

// 'tenc', the track encryption box of ISO/IEC 23001-7 (DASH-IF IOP v5 part 6 section 6.2 cites its
// clause 8.2). The standard has not been read; this layout is what GPAC, FFmpeg and Shaka Packager
// write (planning/cenc-conformance.md, M3 and M7) and what tools/generators/cenc_make_protected.py
// writes and GPAC and FFmpeg decrypt: version and flags (4), a reserved byte, a byte that is
// reserved in version 0 and in version 1 holds crypt_byte_block (high four bits) and
// skip_byte_block (low four), default_isProtected, default_Per_Sample_IV_Size, default_KID (16),
// and, for a protected track with an IV size of 0, a constant IV that is not read here.
void read_tenc(std::span<const std::byte> body, Protection& out) {
    constexpr std::size_t kTencBytes = 24;
    if (body.size() < kTencBytes) {
        return;
    }
    out.has_tenc = true;
    if (get_u8(body, 0) >= 1) {
        out.crypt_byte_block = static_cast<std::uint8_t>(get_u8(body, 5) >> 4U);
        out.skip_byte_block = static_cast<std::uint8_t>(get_u8(body, 5) & 0x0FU);
    }
    out.default_is_protected = get_u8(body, 6) != 0;
    out.default_iv_size = get_u8(body, 7);
    for (std::size_t i = 0; i < out.default_kid.size(); ++i) {
        out.default_kid[i] = get_u8(body, 8 + i);
    }
}

// 'sinf' holds 'frma' (the original format: four characters), 'schm' (version and flags, the
// scheme_type's four characters, a 32-bit scheme_version) and 'schi', which for Common Encryption
// holds the 'tenc'. The 'schm' scheme_uri that flag 1 adds is not read.
void read_sinf(std::span<const std::byte> body, Protection& out) {
    for_each_child(
        body, 0, body.size(), [&out](std::uint32_t type, std::span<const std::byte> inner) {
            if (type == kFrma && inner.size() >= 4) {
                out.original_format = fourcc_text(get_u32(inner, 0));
            } else if (type == kSchm && inner.size() >= 12) {
                out.scheme = fourcc_text(get_u32(inner, 4));
                out.scheme_version = get_u32(inner, 8);
            } else if (type == kSchi) {
                for_each_child(inner, 0, inner.size(),
                               [&out](std::uint32_t schi_type, std::span<const std::byte> tenc) {
                                   if (schi_type == kTenc) {
                                       read_tenc(tenc, out);
                                   }
                               });
            }
        });
}

// The protection a sample entry [start, end) of `body` declares. The 'sinf' sits among the
// entry's child boxes, after the fixed fields of an AudioSampleEntry (28 bytes, as in
// parse_stsd) or a VisualSampleEntry (78: 8 of SampleEntry and 70 of its own, ISO/IEC 14496-12
// section 12.1.3, from the fields' widths), and Dolby's text puts it after the 'dec3' or 'dac4'
// while parsers must also accept it first (cenc-conformance.md, section 3); the walk takes either.
// An entry whose 'sinf' cannot be read is still protected: the result says so with empty fields.
Protection parse_protection(std::uint32_t entry_type, std::span<const std::byte> body,
                            std::size_t start, std::size_t end, std::uint32_t index) {
    Protection out;
    out.entry_type = fourcc_text(entry_type);
    out.entry_index = index;
    const std::size_t fixed = entry_type == kEnca ? 28 : 78;
    if (end - start < fixed) {
        return out;
    }
    for_each_child(body, start + fixed, end,
                   [&out](std::uint32_t type, std::span<const std::byte> sinf) {
                       if (type == kSinf) {
                           read_sinf(sinf, out);
                       }
                   });
    return out;
}

// Whether a track whose only protected entry has this original format is one the reader would
// otherwise have read: a Dolby audio format, or one the entry does not state. A protected AAC or
// AVC track is an ordinary track the reader does not take, listed with its protection.
[[nodiscard]] bool is_dolby_or_unstated(const Protection& protection) {
    return protection.entry_type == "enca" &&
           (protection.original_format.empty() || protection.original_format == "ac-3" ||
            protection.original_format == "ec-3" || protection.original_format == "ac-4" ||
            protection.original_format == "mlpa");
}

// What a track that was not selected is listed as: its handler decides the kind, and its first
// sample entry (whatever it is) the codec.
[[nodiscard]] SkippedTrack skipped_of(const PendingTrack& track) {
    SkippedTrack out;
    out.track_id = track.track_id;
    if (track.handler == fourcc("soun")) {
        out.kind = TrackKind::kAudio;
    } else if (track.handler == fourcc("vide")) {
        out.kind = TrackKind::kVideo;
    } else if (track.handler == fourcc("text") || track.handler == fourcc("sbtl") ||
               track.handler == fourcc("subt") || track.handler == fourcc("clcp")) {
        out.kind = TrackKind::kText;
    }
    if (track.first_entry != 0) {
        out.codec_id = fourcc_text(track.first_entry);
    }
    out.language = track.language;
    out.protection = track.protection;
    return out;
}

// Turns stsc's run-length chunk map, stsz's sizes and stco/co64's offsets
// into one flat list of byte ranges. This is where a hostile sample table
// gets to lie: an stsc run naming a chunk that does not exist, a
// samples_per_chunk of four billion, sizes that sum past any plausible
// file. Every one of those is a false return rather than an allocation or
// an out-of-range read - the ranges themselves are bounds-checked later,
// against the bytes actually present.
[[nodiscard]] bool build_sample_refs(const PendingTrack& track, const ReadOptions& options,
                                     std::vector<SampleRef>& out) {
    if (track.chunk_offsets.empty() || track.sizes.empty()) {
        // A track with a sample entry but no samples is legal (a fragmented
        // file's init segment is exactly that) and not an error; it simply
        // contributes nothing here.
        return true;
    }
    if (track.chunk_runs.empty()) {
        return false;  // sizes and offsets but no map from one to the other
    }
    // §8.7.4: first_chunk is 1-based and strictly increasing.
    if (track.chunk_runs.front().first_chunk != 1) {
        return false;
    }
    for (std::size_t i = 1; i < track.chunk_runs.size(); ++i) {
        if (track.chunk_runs[i].first_chunk <= track.chunk_runs[i - 1].first_chunk) {
            return false;
        }
    }

    out.reserve(track.sizes.size());
    std::size_t sample = 0;
    std::size_t run = 0;
    for (std::size_t chunk = 0; chunk < track.chunk_offsets.size(); ++chunk) {
        // Advance to the run covering this chunk (chunk numbers are 1-based
        // in the file, 0-based here).
        while (run + 1 < track.chunk_runs.size() &&
               track.chunk_runs[run + 1].first_chunk <= chunk + 1) {
            ++run;
        }
        const std::uint32_t per_chunk = track.chunk_runs[run].samples_per_chunk;
        if (per_chunk > options.max_samples) {
            return false;
        }
        std::uint64_t offset = track.chunk_offsets[chunk];
        for (std::uint32_t i = 0; i < per_chunk; ++i) {
            if (sample >= track.sizes.size()) {
                // The chunk map claims more samples than stsz describes.
                // Stopping here rather than failing matches what a player
                // does with a table whose tail is over-long, and the sizes
                // that DID exist are all real.
                return true;
            }
            const std::uint32_t size = track.sizes[sample];
            out.push_back(SampleRef{offset, size});
            offset += size;
            ++sample;
        }
    }
    return true;
}

}  // namespace

std::string_view describe(DemuxError error) {
    switch (error) {
        case DemuxError::kNotIsobmff:
            return "not an ISO base media file: no box structure where one has to be";
        case DemuxError::kTruncated:
            return "input ends before the track or its samples";
        case DemuxError::kMalformed:
            return "malformed box, sample table or fragment layout";
        case DemuxError::kNoAudioTrack:
            return "no 'ac-3'/'ec-3'/'ac-4'/'mlpa' track (or the requested track id is absent)";
        case DemuxError::kLimitExceeded:
            return "box size, sample count or nesting depth beyond the reader's limits";
        case DemuxError::kMoovAfterMdat:
            return "the sample table (moov) follows the media data it indexes: this layout "
                   "cannot be read as a stream; read it from a source that can be read at an "
                   "offset (demux_seekable()) or hand demux() the whole file";
        case DemuxError::kProtectedTrack:
            return "the audio track is protected (Common Encryption, ISO/IEC 23001-7): its "
                   "samples are encrypted and decryption is not supported";
    }
    return "unknown error";
}

// The 8-4-4-4-12 form of DASH's cenc:default_KID attribute (DASH-IF IOP v5 part 6 section 7.2).
std::string format_kid(const std::array<std::uint8_t, 16>& kid) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < kid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kDigits[kid[i] >> 4U]);
        out.push_back(kDigits[kid[i] & 0x0FU]);
    }
    return out;
}

std::string describe(const Protection& protection) {
    std::string out = protection.scheme.empty() ? "scheme not stated" : protection.scheme;
    out += protection.original_format.empty() ? ", original format not stated"
                                              : ", original format " + protection.original_format;
    out += protection.has_tenc ? ", default KID " + format_kid(protection.default_kid)
                               : ", no default KID stated";
    return out;
}

std::string describe_refusal(std::span<const SkippedTrack> refused) {
    std::string out;
    for (const SkippedTrack& track : refused) {
        if (!track.protection.has_value()) {
            continue;
        }
        const Protection& protection = *track.protection;
        if (!out.empty()) {
            out += "; ";
        }
        const std::string id = std::to_string(track.track_id);
        if (protection.track_has_clear_entry) {
            out += "track " + id + " mixes a clear sample entry with a protected one, entry " +
                   std::to_string(protection.entry_index) + " (" + describe(protection) +
                   "), and is refused whole";
        } else {
            out += "track " + id + " is protected (" + describe(protection) + ")";
        }
    }
    if (out.empty()) {
        return std::string{describe(DemuxError::kProtectedTrack)};
    }
    return out + "; decryption is not supported";
}

namespace detail {

// Everything the walk carries between calls. demux() keeps one on the stack
// and walks once; Reader owns one across push()es - so both drive the
// identical parser, which has no idea which caller it is serving.
struct ReaderState {
    ReadOptions options;

    struct Open {
        std::uint32_t type = 0;
        std::uint64_t end = 0;  // absolute file offset one past this box
        bool to_eof = false;
    };
    std::vector<Open> open;

    // Absolute offset of the next byte the box parser wants. Absolute, not
    // buffer-relative, because a sample table indexes the FILE.
    std::uint64_t parse_pos = 0;
    // Absolute offset of the first byte still held (window_pos for demux is
    // always 0, since it holds everything).
    std::uint64_t window_pos = 0;

    PendingTrack pending;
    bool in_trak = false;
    std::uint32_t traks_seen = 0;
    // mvhd's timescale. Normally read before any trak, but nothing requires
    // that, so a late one is copied into the track as well.
    std::uint32_t movie_timescale = 0;

    ReadTrack track;
    bool track_found = false;
    // The tracks that were not selected, to kMaxSkippedTracks.
    std::vector<SkippedTrack> skipped;
    // Those of them refused for being protected (each is in `skipped` too), to kMaxSkippedTracks:
    // the reason finish_verdict gives kProtectedTrack and not kNoAudioTrack.
    std::vector<SkippedTrack> refused;
    bool saw_box = false;
    bool saw_mdat = false;
    // A moov that arrived after mdat: fatal for the streaming Reader, fine
    // for demux(), so it is recorded rather than rejected here.
    bool moov_after_mdat = false;

    // Fragmented playback: mvex/trex's per-track defaults, and the traf
    // being read.
    std::uint32_t trex_track_id = 0;
    std::uint32_t trex_default_sample_size = 0;
    bool have_trex = false;
    std::uint64_t current_moof_start = 0;
    std::uint32_t traf_track_id = 0;
    std::uint64_t traf_base_offset = 0;
    std::uint32_t traf_default_sample_size = 0;
    bool traf_is_selected = false;

    // Samples found but not yet handed to the caller, in ascending offset
    // order. An MP4's sample table is an O(duration) index by construction;
    // ReadOptions::max_samples bounds it. The AUDIO never accumulates - only
    // these 12-byte records do, and only until their bytes arrive.
    std::vector<SampleRef> samples;
    std::size_t next_sample = 0;
    std::size_t samples_read = 0;

    // Incremental input only.
    std::vector<std::byte> buffer;
};

}  // namespace detail

namespace {

using detail::ReaderState;

// Returns an error rather than void because closing a trak is where the
// sample table is finally assembled, and a table that does not add up is a
// malformed file - discarding that verdict would leave a hostile stsc
// silently producing no samples instead of an explanation.
std::expected<void, DemuxError> close_finished(ReaderState& s, const ReadOptions& options) {
    while (!s.open.empty()) {
        const auto& top = s.open.back();
        if (top.to_eof || s.parse_pos < top.end) {
            return {};
        }
        if (top.type == kTrak) {
            bool selected_now = false;
            // A track with a protected sample entry is never selected, whatever else its 'stsd'
            // holds. The entry a sample belongs to is named per chunk ('stsc'
            // sample_description_index) or per fragment ('tfhd', 'trex'); a packager that writes a
            // clear lead-in puts a clear entry and a protected one in one track, and a reader that
            // took the clear entry would hand back the lead-in, or the lead-in and then ciphertext
            // as audio (planning/cenc-conformance.md, G1 and M7). Refusing the whole track is the
            // strictest rule that keeps every clear track working, and it is decided here, at
            // 'moov', before any sample is delivered, so the streaming Reader and a pipe get the
            // same answer as demux(). The price is that a track declaring a protected entry no
            // sample uses is refused too; none of the files examined writes one. Reading the clear
            // samples of such a track, and decrypting the rest, waits for a key
            // (planning/cenc-read-side.md, 5.4).
            const bool refuse =
                s.in_trak && s.pending.protection.has_value() &&
                (s.pending.has_entry || is_dolby_or_unstated(*s.pending.protection));
            if (s.in_trak && s.pending.protection.has_value()) {
                s.pending.protection->track_has_clear_entry = s.pending.has_clear_entry;
            }
            if (refuse) {
                if (s.refused.size() < kMaxSkippedTracks) {
                    s.refused.push_back(skipped_of(s.pending));
                }
            } else if (s.in_trak && !s.track_found && s.pending.has_entry) {
                const bool wanted =
                    options.track_id != 0
                        ? s.pending.track_id == options.track_id
                        : ((s.pending.entry_type == kAc3Entry && options.auto_select.ac3) ||
                           (s.pending.entry_type == kEc3Entry && options.auto_select.eac3) ||
                           (s.pending.entry_type == kAc4Entry && options.auto_select.ac4) ||
                           (s.pending.entry_type == kMlpaEntry && options.auto_select.truehd));
                if (wanted) {
                    s.track = ReadTrack{
                        .track_id = s.pending.track_id,
                        .codec_id = std::string{s.pending.entry_type == kAc3Entry    ? kCodecAc3
                                                : s.pending.entry_type == kAc4Entry  ? kCodecAc4
                                                : s.pending.entry_type == kMlpaEntry ? kCodecTruehd
                                                                                     : kCodecEac3},
                        .sample_rate = s.pending.sample_rate,
                        .channels = s.pending.channels,
                        .timescale = s.pending.timescale,
                        .movie_timescale = s.movie_timescale,
                        .language = s.pending.language,
                        .codec_config = s.pending.codec_config,
                        .edits = s.pending.edits,
                    };
                    s.track_found = true;
                    selected_now = true;
                    // The plain path's samples exist the moment the table is
                    // whole; the fragmented path's arrive per moof later.
                    if (!build_sample_refs(s.pending, options, s.samples)) {
                        return std::unexpected(DemuxError::kMalformed);
                    }
                }
            }
            if (s.in_trak && !selected_now && s.pending.track_id != 0 &&
                s.skipped.size() < kMaxSkippedTracks) {
                s.skipped.push_back(skipped_of(s.pending));
            }
            s.in_trak = false;
        }
        s.open.pop_back();
    }
    return {};
}

// --- leaf parsers -----------------------------------------------------------
// Each takes the box BODY (everything after the size+type header) and
// returns false only for a layout that cannot be parsed at all; a body too
// short for a field it does not need is not an error.

[[nodiscard]] bool parse_tkhd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 4) {
        return false;
    }
    const std::uint8_t version = get_u8(body, 0);
    // version 0 packs creation/modification as 32-bit, version 1 as 64-bit;
    // track_ID sits after both (§8.3.2.2).
    const std::size_t at = version == 1 ? 20 : 12;
    if (body.size() < at + 4) {
        return false;
    }
    track.track_id = get_u32(body, at);
    return true;
}

// §8.4.3: version and flags (4), pre_defined (4), then the handler_type four characters. A body
// too short for it is left as no handler, which lists as "other" and is not an error.
void parse_hdlr(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() >= 12) {
        track.handler = get_u32(body, 8);
    }
}

[[nodiscard]] bool parse_mdhd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 4) {
        return false;
    }
    const std::uint8_t version = get_u8(body, 0);
    const std::size_t timescale_at = version == 1 ? 20 : 12;
    const std::size_t language_at = version == 1 ? 32 : 20;
    if (body.size() < timescale_at + 4) {
        return false;
    }
    track.timescale = get_u32(body, timescale_at);
    if (body.size() >= language_at + 2) {
        track.language = unpack_language(get_u16(body, language_at));
    }
    return true;
}

// The two boxes below say how to present the samples, not where they are, so
// neither can make a file unreadable: one too short to read, or one that
// declares more than it holds, is left out - a file with a broken edit list
// still plays, as it did before this reader looked at edit lists - and the
// walk carries on.

// §8.2.2: only the timescale matters here, at the same offsets as mdhd's.
void parse_mvhd(std::span<const std::byte> body, ReaderState& s) {
    if (body.size() < 4) {
        return;
    }
    const std::uint8_t version = get_u8(body, 0);
    const std::size_t timescale_at = version == 1 ? 20 : 12;
    if (body.size() < timescale_at + 4) {
        return;
    }
    s.movie_timescale = get_u32(body, timescale_at);
    if (s.track_found) {
        s.track.movie_timescale = s.movie_timescale;
    }
}

// §8.6.6: an entry count, then per entry a segment_duration and a signed
// media_time (32 bits each in version 0, 64 in version 1) and the rate as
// two 16-bit halves. A second elst in one trak replaces the first, as a
// second stsc does; one that cannot be read leaves the track with none.
void parse_elst(std::span<const std::byte> body, const ReadOptions& options,
                PendingTrack& track) {
    track.edits.clear();
    if (body.size() < 8) {
        return;
    }
    const std::uint8_t version = get_u8(body, 0);
    const std::uint32_t count = get_u32(body, 4);
    const std::size_t entry_bytes = version == 1 ? 20 : 12;
    if (count > options.max_edits ||
        static_cast<std::uint64_t>(count) * entry_bytes + 8 > body.size()) {
        return;
    }
    track.edits.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 8 + (static_cast<std::size_t>(i) * entry_bytes);
        EditListEntry edit;
        if (version == 1) {
            edit.segment_duration = get_u64(body, at);
            edit.media_time = static_cast<std::int64_t>(get_u64(body, at + 8));
        } else {
            edit.segment_duration = get_u32(body, at);
            edit.media_time = static_cast<std::int32_t>(get_u32(body, at + 4));
        }
        const std::size_t rate_at = at + entry_bytes - 4;
        edit.media_rate = static_cast<std::int32_t>(get_u32(body, rate_at));
        track.edits.push_back(edit);
    }
}

// §12.2.3's AudioSampleEntry, on top of §8.5.2's SampleEntry: reserved(6)
// data_reference_index(2) reserved(8) channelcount(2) samplesize(2) pre_defined(2) reserved(2)
// samplerate(4, 16.16) = 28 bytes before the child boxes.
constexpr std::size_t kAudioSampleEntryBytes = 28;

// The 'ac-3'/'ec-3'/'ac-4'/'mlpa' entry at `at` in `body`: the track's channel count, rate and
// codec configuration box. False only for a layout that cannot be read.
[[nodiscard]] bool parse_dolby_entry(std::span<const std::byte> body, const BoxHeader& entry,
                                     std::size_t at, PendingTrack& track) {
    const std::size_t fields = at + entry.header_bytes;
    if (fields + kAudioSampleEntryBytes > body.size()) {
        return false;
    }
    track.entry_type = entry.type;
    track.channels = static_cast<int>(get_u16(body, fields + 16));
    const std::uint32_t rate_field = get_u32(body, fields + 24);
    // 16.16 fixed point, except in 'mlpa', where the field holds the rate as a plain integer
    // (ERRATA C2). A plain rate always has a non-zero low half (no rate is a multiple of 65,536),
    // a 16.16 one of the same rate never does, which tells a file written either way apart.
    track.sample_rate = (entry.type == kMlpaEntry && (rate_field & 0xFFFFU) != 0)
                            ? rate_field
                            : rate_field >> 16U;
    track.has_entry = true;

    // The one child that matters: dac3, dec3, dac4 or dmlp.
    std::size_t child = fields + kAudioSampleEntryBytes;
    const std::size_t entry_end = at + static_cast<std::size_t>(entry.size);
    while (child + 8 <= entry_end) {
        BoxHeader config{};
        if (read_box_header(body, child, config) != BoxRead::kOk || config.to_eof) {
            return false;
        }
        if (config.size > entry_end - child) {
            return false;
        }
        if (config.type == kDac3 || config.type == kDec3 || config.type == kDac4 ||
            config.type == kDmlp) {
            track.codec_config = parse_codec_config(
                config.type,
                body.subspan(child + config.header_bytes,
                             static_cast<std::size_t>(config.size) - config.header_bytes));
            if (config.type == kDmlp) {
                // dmlp's ratebits are the rate the decoder uses; they win over the sample entry's
                // field when they name one (ERRATA C2).
                const std::uint32_t hz = dmlp_rate_hz(track.codec_config.mlp_format_info);
                if (hz != 0) {
                    track.sample_rate = hz;
                }
            }
            break;
        }
        child += static_cast<std::size_t>(config.size);
    }
    return true;
}

// stsd is a FullBox whose body is an entry count followed by sample entries. The first
// 'ac-3'/'ec-3'/'ac-4' one gives the track its codec; anything else means this trak is not ours,
// which is not an error. Every entry is looked at for protection (close_finished says why): the
// first 'enca' or 'encv' is read into the track's Protection, and whether an entry that is not
// protected sits beside it is noted. Once the track has its codec entry, an entry that cannot be
// walked ends the scan quietly, as the scan ended there before; before it, it is malformed.
[[nodiscard]] bool parse_stsd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t entry_count = get_u32(body, 4);
    std::size_t at = 8;
    for (std::uint32_t i = 0; i < entry_count && at + 8 <= body.size(); ++i) {
        BoxHeader entry{};
        if (read_box_header(body, at, entry) != BoxRead::kOk || entry.to_eof ||
            entry.size > body.size() - at) {
            return track.has_entry;
        }
        if (track.first_entry == 0) {
            track.first_entry = entry.type;
        }
        const std::size_t entry_end = at + static_cast<std::size_t>(entry.size);
        if (entry.type == kEnca || entry.type == kEncv) {
            if (!track.protection.has_value()) {
                track.protection =
                    parse_protection(entry.type, body, at + entry.header_bytes, entry_end, i + 1);
            }
        } else {
            track.has_clear_entry = true;
            if (!track.has_entry &&
                (entry.type == kAc3Entry || entry.type == kEc3Entry || entry.type == kAc4Entry ||
                 entry.type == kMlpaEntry) &&
                !parse_dolby_entry(body, entry, at, track)) {
                return false;
            }
        }
        at = entry_end;
    }
    return true;
}

[[nodiscard]] bool parse_stsc(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t count = get_u32(body, 4);
    if (count > options.max_chunks) {
        return false;
    }
    if (static_cast<std::uint64_t>(count) * 12 + 8 > body.size()) {
        return false;
    }
    track.chunk_runs.clear();
    track.chunk_runs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 8 + (static_cast<std::size_t>(i) * 12);
        track.chunk_runs.push_back(
            ChunkRun{.first_chunk = get_u32(body, at), .samples_per_chunk = get_u32(body, at + 4)});
    }
    return true;
}

[[nodiscard]] bool parse_stsz(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 12) {
        return false;
    }
    const std::uint32_t uniform_size = get_u32(body, 4);
    const std::uint32_t count = get_u32(body, 8);
    if (count > options.max_samples) {
        return false;
    }
    if (uniform_size != 0) {
        // Every sample the same size - legal, though no AC-3/E-AC-3 muxer
        // writes it (frame sizes vary even in CBR).
        track.sizes.assign(count, uniform_size);
        return true;
    }
    if (static_cast<std::uint64_t>(count) * 4 + 12 > body.size()) {
        return false;
    }
    track.sizes.clear();
    track.sizes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        track.sizes.push_back(get_u32(body, 12 + (static_cast<std::size_t>(i) * 4)));
    }
    return true;
}

// §8.7.3.3's compact sample size box: a 4/8/16-bit field per sample instead
// of stsz's flat 32. Not written by this project, but written by real
// muxers, and a file using it is otherwise unreadable.
[[nodiscard]] bool parse_stz2(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 12) {
        return false;
    }
    const std::uint32_t field_size = get_u8(body, 7);
    const std::uint32_t count = get_u32(body, 8);
    if (count > options.max_samples) {
        return false;
    }
    if (field_size != 4 && field_size != 8 && field_size != 16) {
        return false;
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(count) * field_size;
    if ((bits + 7) / 8 + 12 > body.size()) {
        return false;
    }
    track.sizes.clear();
    track.sizes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (field_size == 16) {
            track.sizes.push_back(get_u16(body, 12 + (static_cast<std::size_t>(i) * 2)));
        } else if (field_size == 8) {
            track.sizes.push_back(get_u8(body, 12 + i));
        } else {
            const std::uint8_t pair = get_u8(body, 12 + (i / 2));
            track.sizes.push_back((i % 2 == 0) ? (pair >> 4U) : (pair & 0x0FU));
        }
    }
    return true;
}

[[nodiscard]] bool parse_chunk_offsets(std::uint32_t type, std::span<const std::byte> body,
                                       const ReadOptions& options, PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t count = get_u32(body, 4);
    if (count > options.max_chunks) {
        return false;
    }
    const std::size_t width = type == kCo64 ? 8 : 4;
    if (static_cast<std::uint64_t>(count) * width + 8 > body.size()) {
        return false;
    }
    track.chunk_offsets.clear();
    track.chunk_offsets.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 8 + (static_cast<std::size_t>(i) * width);
        track.chunk_offsets.push_back(width == 8 ? get_u64(body, at) : get_u32(body, at));
    }
    return true;
}

// §8.8.7's tfhd. Its flags say which optional fields are present, and
// default-base-is-moof (0x020000) says data offsets run from this moof's
// own start rather than the file's - which is what fragment() writes and
// what CMAF requires.
[[nodiscard]] bool parse_tfhd(std::span<const std::byte> body, ReaderState& s) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t flags = get_u32(body, 0) & 0x00FF'FFFFU;
    s.traf_track_id = get_u32(body, 4);
    std::size_t at = 8;
    s.traf_base_offset = (flags & 0x020000U) != 0 ? s.current_moof_start : 0;
    if ((flags & 0x000001U) != 0) {  // base-data-offset-present
        if (body.size() < at + 8) {
            return false;
        }
        s.traf_base_offset = get_u64(body, at);
        at += 8;
    }
    if ((flags & 0x000002U) != 0) {  // sample-description-index-present
        at += 4;
    }
    if ((flags & 0x000008U) != 0) {  // default-sample-duration-present
        at += 4;
    }
    s.traf_default_sample_size = s.have_trex && s.trex_track_id == s.traf_track_id
                                     ? s.trex_default_sample_size
                                     : 0;
    if ((flags & 0x000010U) != 0) {  // default-sample-size-present
        if (body.size() < at + 4) {
            return false;
        }
        s.traf_default_sample_size = get_u32(body, at);
    }
    s.traf_is_selected = s.track_found && s.traf_track_id == s.track.track_id;
    return true;
}

// §8.8.8's trun.
[[nodiscard]] bool parse_trun(std::span<const std::byte> body, ReaderState& s) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t flags = get_u32(body, 0) & 0x00FF'FFFFU;
    const std::uint32_t count = get_u32(body, 4);
    if (count > s.options.max_samples) {
        return false;
    }
    std::size_t at = 8;
    std::int64_t data_offset = 0;
    if ((flags & 0x000001U) != 0) {  // data-offset-present
        if (body.size() < at + 4) {
            return false;
        }
        data_offset = static_cast<std::int32_t>(get_u32(body, at));
        at += 4;
    }
    if ((flags & 0x000004U) != 0) {  // first-sample-flags-present
        at += 4;
    }
    // Per-sample fields, in the fixed order §8.8.8.2 gives them.
    std::size_t stride = 0;
    std::size_t size_at = 0;
    if ((flags & 0x000100U) != 0) {  // sample-duration-present
        stride += 4;
    }
    const bool has_size = (flags & 0x000200U) != 0;
    if (has_size) {
        size_at = stride;
        stride += 4;
    }
    if ((flags & 0x000400U) != 0) {  // sample-flags-present
        stride += 4;
    }
    if ((flags & 0x000800U) != 0) {  // composition-time-offsets-present
        stride += 4;
    }
    if (static_cast<std::uint64_t>(count) * stride + at > body.size()) {
        return false;
    }
    if (!has_size && s.traf_default_sample_size == 0) {
        // No per-sample size and no default to fall back on: nothing in the
        // fragment says where one sample ends and the next begins.
        return false;
    }
    if (!s.traf_is_selected) {
        return true;  // another track's fragment: parsed, nothing to emit
    }

    const std::int64_t base = static_cast<std::int64_t>(s.traf_base_offset) + data_offset;
    if (base < 0) {
        return false;
    }
    auto offset = static_cast<std::uint64_t>(base);
    if (s.samples.size() + count > s.options.max_samples) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t size =
            has_size ? get_u32(body, at + (static_cast<std::size_t>(i) * stride) + size_at)
                     : s.traf_default_sample_size;
        s.samples.push_back(SampleRef{offset, size});
        offset += size;
    }
    return true;
}

// Hands over every pending sample whose bytes are inside `window`, which
// starts at absolute offset `window_pos`. Samples come out in the order the
// table declares them, which for audio is both decode and presentation
// order.
[[nodiscard]] std::expected<void, DemuxError> drain_samples(ReaderState& s,
                                                            std::span<const std::byte> window,
                                                            std::uint64_t window_pos,
                                                            const Reader::SampleFn& on_sample) {
    const std::uint64_t window_end = window_pos + window.size();
    while (s.next_sample < s.samples.size()) {
        const auto& ref = s.samples[s.next_sample];
        if (ref.offset < window_pos) {
            // The bytes this sample names are already behind the window -
            // only possible when the table arrived after the data it
            // indexes, which a stream cannot go back for.
            return std::unexpected(DemuxError::kMoovAfterMdat);
        }
        const std::uint64_t end = ref.offset + ref.size;
        if (end > window_end) {
            break;  // not here yet
        }
        on_sample(window.subspan(static_cast<std::size_t>(ref.offset - window_pos), ref.size));
        ++s.next_sample;
        ++s.samples_read;
    }
    return {};
}

// Parses as much of `window` as it can. `window_pos` is the absolute file
// offset of window[0]; s.parse_pos is where the parser is, in the same
// absolute terms. Returns nothing - progress is recorded in s.parse_pos, and
// the caller decides what it may now discard.
std::expected<void, DemuxError> walk(ReaderState& s, std::span<const std::byte> window,
                                     std::uint64_t window_pos) {
    const std::uint64_t window_end = window_pos + window.size();

    for (;;) {
        if (s.parse_pos >= window_end) {
            return {};  // need more input (or done)
        }
        const auto at = static_cast<std::size_t>(s.parse_pos - window_pos);

        BoxHeader box{};
        const BoxRead got = read_box_header(window, at, box);
        if (got == BoxRead::kNeedMore) {
            return {};
        }
        if (got == BoxRead::kBad) {
            return std::unexpected(s.saw_box ? DemuxError::kMalformed : DemuxError::kNotIsobmff);
        }
        // A file that does not begin with a recognisable top-level box is
        // not ISOBMFF. ftyp/styp is the normal opener; a bare moov or mdat
        // occurs in the wild too, so the test is "is this one of the box
        // types a file can start with", not "is it ftyp".
        if (!s.saw_box) {
            const bool plausible = box.type == kFtyp || box.type == kStyp || box.type == kMoov ||
                                   box.type == kMoof || box.type == kMdat;
            if (!plausible) {
                return std::unexpected(DemuxError::kNotIsobmff);
            }
            s.saw_box = true;
        }

        // §4.2's largesize escape lets a box declare a size up to UINT64_MAX.
        // box_end below is s.parse_pos + box.size - unchecked, that wraps
        // past 2^64 for a size this close to it and lands BEHIND the
        // current position. A container just leaves the box open forever
        // (harmless), but the streaming Reader's sliding window advances
        // window_pos to track parse_pos, and the next iteration's
        // `parse_pos - window_pos` then underflows to a huge std::size_t -
        // which read_box_header's own "in.size() - at < 8" guard fails to
        // catch for the same reason, so it indexes far past the buffer. A
        // size that cannot be added to the current position is not a
        // length, the same reasoning read_box_header already applies to a
        // size32 smaller than its own header.
        if (!box.to_eof && box.size > UINT64_MAX - s.parse_pos) {
            return std::unexpected(DemuxError::kMalformed);
        }

        const std::uint64_t body_bytes = box.to_eof ? 0 : box.size - box.header_bytes;
        const std::uint64_t box_end = box.to_eof ? 0 : s.parse_pos + box.size;

        if (box.type == kMdat) {
            s.saw_mdat = true;
        } else if (box.type == kMoov && s.saw_mdat) {
            s.moov_after_mdat = true;
        }

        if (is_container(box.type)) {
            if (s.open.size() >= s.options.max_depth) {
                return std::unexpected(DemuxError::kLimitExceeded);
            }
            if (box.type == kMoof) {
                s.current_moof_start = s.parse_pos;
            }
            if (box.type == kTrak) {
                ++s.traks_seen;
                if (s.traks_seen > s.options.max_chunks) {
                    return std::unexpected(DemuxError::kLimitExceeded);
                }
                s.pending = PendingTrack{};
                s.in_trak = true;
            }
            s.open.push_back(ReaderState::Open{box.type, box_end, box.to_eof});
            s.parse_pos += box.header_bytes;
            if (const auto closed = close_finished(s, s.options); !closed) {
                return std::unexpected(closed.error());
            }
            continue;
        }

        if (!is_wanted_leaf(box.type)) {
            // Skipped by length alone - never held. mdat lands here, which
            // is exactly right: its bytes reach the caller through
            // drain_samples, not through this parser.
            if (box.to_eof) {
                // Runs to the end of the file: there is no box after it, so
                // parking the parser past every possible offset ends the
                // walk without pretending to know where the file stops.
                s.parse_pos = UINT64_MAX;
                return {};
            }
            s.parse_pos = box_end;
            if (const auto closed = close_finished(s, s.options); !closed) {
                return std::unexpected(closed.error());
            }
            continue;
        }

        // A leaf worth reading has to be here in full.
        if (box.to_eof || body_bytes > s.options.max_box_bytes) {
            return std::unexpected(DemuxError::kLimitExceeded);
        }
        if (box_end > window_end) {
            return {};  // need more input
        }
        const auto body = window.subspan(at + box.header_bytes,
                                         static_cast<std::size_t>(body_bytes));

        bool ok = true;
        switch (box.type) {
            case kMvhd:
                parse_mvhd(body, s);
                break;
            case kTkhd:
                ok = parse_tkhd(body, s.pending);
                break;
            case kElst:
                parse_elst(body, s.options, s.pending);
                break;
            case kMdhd:
                ok = parse_mdhd(body, s.pending);
                break;
            case kHdlr:
                parse_hdlr(body, s.pending);
                break;
            case kStsd:
                ok = parse_stsd(body, s.pending);
                break;
            case kStsc:
                ok = parse_stsc(body, s.options, s.pending);
                break;
            case kStsz:
                ok = parse_stsz(body, s.options, s.pending);
                break;
            case kStz2:
                ok = parse_stz2(body, s.options, s.pending);
                break;
            case kStco:
            case kCo64:
                ok = parse_chunk_offsets(box.type, body, s.options, s.pending);
                break;
            case kTrex:
                // Body layout past the FullBox header (offset 0): track_ID(4),
                // default_sample_description_index(8), default_sample_duration(12),
                // default_sample_size(16), default_sample_flags(20) - ISO/IEC
                // 14496-12 §8.8.3. Confirmed against this project's own writer
                // (fragment.cpp's build_trex) and the reader test helper's
                // independent read_trex().
                if (body.size() >= 20) {
                    s.trex_track_id = get_u32(body, 4);
                    s.trex_default_sample_size = get_u32(body, 16);
                    s.have_trex = true;
                }
                break;
            case kTfhd:
                ok = parse_tfhd(body, s);
                break;
            case kTrun:
                ok = parse_trun(body, s);
                break;
            default:
                break;
        }
        if (!ok) {
            return std::unexpected(DemuxError::kMalformed);
        }

        s.parse_pos = box_end;
        if (const auto closed = close_finished(s, s.options); !closed) {
            return std::unexpected(closed.error());
        }
    }
}

// The verdict both entry points reach once no more bytes are coming.
[[nodiscard]] std::expected<void, DemuxError> finish_verdict(const ReaderState& s) {
    if (!s.saw_box) {
        return std::unexpected(DemuxError::kNotIsobmff);
    }
    if (!s.track_found) {
        // A file that never got as far as a trak was cut short; one that had
        // traks but none we can use is a different answer, and a protected track
        // is the one worth naming: the file has the audio, and it is encrypted. A
        // named track_id means the protected track asked for, not any.
        if (s.traks_seen == 0) {
            return std::unexpected(DemuxError::kTruncated);
        }
        const bool protected_named =
            s.options.track_id == 0
                ? !s.refused.empty()
                : std::ranges::any_of(s.refused, [&s](const SkippedTrack& track) {
                      return track.track_id == s.options.track_id;
                  });
        return std::unexpected(protected_named ? DemuxError::kProtectedTrack
                                               : DemuxError::kNoAudioTrack);
    }
    return {};
}

}  // namespace

std::expected<Demuxed, DemuxError> demux(std::span<const std::byte> file,
                                         const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;

    const auto walked = walk(s, file, 0);
    if (!walked.has_value()) {
        return std::unexpected(walked.error());
    }
    const auto verdict = finish_verdict(s);
    if (!verdict.has_value()) {
        return std::unexpected(verdict.error());
    }

    Demuxed out;
    // Samples are views into `file`, so collecting them costs one pointer
    // pair each and copies no audio - the zero-copy promise in the header.
    // A sample whose declared range runs past the end of the file is
    // dropped, not fabricated: a truncated download is the ordinary case,
    // and the samples before the cut are all real.
    out.samples.reserve(s.samples.size());
    const auto collect = [&out](std::span<const std::byte> sample) {
        out.samples.push_back(sample);
    };
    const auto drained = drain_samples(s, file, 0, collect);
    if (!drained.has_value()) {
        return std::unexpected(drained.error());
    }
    out.track = s.track;
    out.skipped = s.skipped;
    return out;
}

Reader::Reader(const ReadOptions& options) : state_(std::make_unique<detail::ReaderState>()) {
    state_->options = options;
}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;
Reader::~Reader() = default;

const ReadTrack& Reader::track() const { return state_->track; }
bool Reader::track_found() const { return state_->track_found; }
std::size_t Reader::samples_read() const { return state_->samples_read; }
const std::vector<SkippedTrack>& Reader::refused_tracks() const {
    return state_->refused;
}

std::expected<void, DemuxError> Reader::push(std::span<const std::byte> chunk,
                                             const SampleFn& on_sample) {
    auto& s = *state_;
    s.buffer.insert(s.buffer.end(), chunk.begin(), chunk.end());

    const auto walked = walk(s, s.buffer, s.window_pos);
    if (!walked.has_value()) {
        return std::unexpected(walked.error());
    }
    const auto drained = drain_samples(s, s.buffer, s.window_pos, on_sample);
    if (!drained.has_value()) {
        return std::unexpected(drained.error());
    }

    // Discard everything neither the parser nor the next sample still needs.
    // The parser may be far ahead (it skips mdat by arithmetic, never by
    // holding it) while samples are still being read out of those very
    // bytes, so the low-water mark is the LOWER of the two - which is what
    // keeps peak memory at a chunk plus a sample rather than the file.
    const std::uint64_t next_needed =
        s.next_sample < s.samples.size() ? s.samples[s.next_sample].offset : s.parse_pos;
    const std::uint64_t keep_from = std::min(s.parse_pos, next_needed);
    if (keep_from > s.window_pos) {
        const auto drop = std::min<std::uint64_t>(keep_from - s.window_pos, s.buffer.size());
        s.buffer.erase(s.buffer.begin(), s.buffer.begin() + static_cast<std::ptrdiff_t>(drop));
        s.window_pos += drop;
    }
    return {};
}

std::expected<void, DemuxError> Reader::finish() {
    auto& s = *state_;
    if (s.moov_after_mdat && s.next_sample < s.samples.size()) {
        // The table turned up behind the data. demux() reads this file
        // perfectly; a stream cannot, and saying which is more useful than
        // "no samples".
        return std::unexpected(DemuxError::kMoovAfterMdat);
    }
    return finish_verdict(s);
}

namespace {

// Pass one of demux_seekable() and all of list_refused_tracks(): the boxes. walk() steps over
// every box it does not need by arithmetic (mdat above all), so the window is only ever asked
// for at the parser's own position - the audio is not read to find the table that indexes it,
// whichever side of mdat that table is on.
[[nodiscard]] std::expected<void, DemuxError> read_boxes(ReaderState& s, const ReadAtFn& read_at,
                                                         std::uint64_t size) {
    constexpr std::uint64_t kWindow = 64U * 1024U;
    // A leaf box worth reading has to be held whole, and ReadOptions bounds
    // it; the window may grow to that and a header, never past it.
    const std::uint64_t window_cap = s.options.max_box_bytes + kWindow;
    std::uint64_t want = kWindow;
    std::vector<std::byte> window;
    while (s.parse_pos < size) {
        const std::uint64_t at = s.parse_pos;
        const std::uint64_t length = std::min(want, size - at);
        window.resize(static_cast<std::size_t>(length));
        const std::size_t got = read_at(at, window);
        if (got == 0) {
            break;  // the source ends before its own declared size
        }
        window.resize(got);
        const auto walked = walk(s, window, at);
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (s.parse_pos != at) {
            want = kWindow;
            continue;
        }
        // No progress: the box at `at` is not wholly in the window. A window
        // that already reaches the end of the input means the file is cut
        // short - the verdict says what that amounts to; otherwise
        // ask for more, up to the box bound.
        if (length >= size - at || want >= window_cap) {
            break;
        }
        want = std::min(want * 2, window_cap);
    }
    return {};
}

}  // namespace

std::expected<ReadTrack, DemuxError> demux_seekable(const ReadAtFn& read_at, std::uint64_t size,
                                                    const TrackFn& on_track,
                                                    const Reader::SampleFn& on_sample,
                                                    const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;

    if (const auto boxes = read_boxes(s, read_at, size); !boxes.has_value()) {
        return std::unexpected(boxes.error());
    }

    const auto verdict = finish_verdict(s);
    if (!verdict.has_value()) {
        return std::unexpected(verdict.error());
    }
    if (on_track) {
        on_track(s.track);
    }

    // Pass two: each sample where the table says, one at a time. A sample
    // whose range runs past the end of the input stops delivery, as demux()
    // stops at the first one a truncated file does not hold: the samples
    // before it are real, and a later one is not fabricated.
    std::vector<std::byte> sample;
    for (const auto& ref : s.samples) {
        if (ref.offset > size || ref.size > size - ref.offset) {
            break;
        }
        if (ref.size > options.max_box_bytes) {
            return std::unexpected(DemuxError::kLimitExceeded);
        }
        sample.resize(ref.size);
        if (read_at(ref.offset, sample) != ref.size) {
            break;
        }
        on_sample(sample);
        ++s.samples_read;
    }
    return s.track;
}

std::expected<std::vector<SkippedTrack>, DemuxError> list_refused_tracks(
    std::span<const std::byte> file, const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;
    if (const auto walked = walk(s, file, 0); !walked.has_value()) {
        return std::unexpected(walked.error());
    }
    if (!s.saw_box) {
        return std::unexpected(DemuxError::kNotIsobmff);
    }
    return s.refused;
}

std::expected<std::vector<SkippedTrack>, DemuxError> list_refused_tracks(
    const ReadAtFn& read_at, std::uint64_t size, const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;
    if (const auto boxes = read_boxes(s, read_at, size); !boxes.has_value()) {
        return std::unexpected(boxes.error());
    }
    if (!s.saw_box) {
        return std::unexpected(DemuxError::kNotIsobmff);
    }
    return s.refused;
}

}  // namespace iclforge::containers::mp4
