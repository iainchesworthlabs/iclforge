#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Matroska, MP4 and MPEG-TS files with more than one audio track, built byte by byte, for the
// tests of which track a reader or a loader takes (a Blu-ray remux has TrueHD first and AC-3 or
// E-AC-3 after it). The project's muxers write one track, and the tests of the readers build their
// files by hand for the reason their headers give: a reader and a writer that share a
// misunderstanding agree with each other. Nothing here links a library, so the layout is written
// out again rather than borrowed, and it holds as much of each container as its reader needs: the
// tracks' codec and format, and their access units in order.
//
// An access unit is handed over whole and comes back whole, so these files carry real streams
// (the access units of an encoder's or FFmpeg's output) without knowing what is in them.

namespace iclforge::test::multi_track {

using Bytes = std::vector<std::byte>;

enum class Codec : std::uint8_t { kAc3, kEac3, kTruehd };

struct Track {
    Codec codec = Codec::kEac3;
    std::uint32_t sample_rate = 48000;
    std::uint16_t channels = 6;
    // One access unit each, in order. The views must outlive the call that builds the file.
    std::vector<std::span<const std::byte>> units{};
};

// A TrueHD stream's access units. Each opens with a length of 12 bits after the check nibble, in
// 16-bit words (TH spec section 3.1); the last may be cut short, and then it is left out.
[[nodiscard]] inline std::vector<std::span<const std::byte>> split_truehd(
    std::span<const std::byte> stream) {
    std::vector<std::span<const std::byte>> units;
    std::size_t at = 0;
    while (at + 2 <= stream.size()) {
        const std::size_t words = ((std::to_integer<std::size_t>(stream[at]) & 0x0FU) << 8U) |
                                  std::to_integer<std::size_t>(stream[at + 1]);
        const std::size_t size = words * 2;
        if (size < 4 || at + size > stream.size()) {
            break;
        }
        units.push_back(stream.subspan(at, size));
        at += size;
    }
    return units;
}

namespace detail {

inline void put_u8(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}
inline void put_u16(Bytes& out, std::uint32_t value) {
    put_u8(out, value >> 8U);
    put_u8(out, value);
}
inline void put_u32(Bytes& out, std::uint32_t value) {
    put_u16(out, value >> 16U);
    put_u16(out, value);
}
inline void put_text(Bytes& out, std::string_view text) {
    for (const char c : text) {
        put_u8(out, static_cast<std::uint8_t>(c));
    }
}
inline void put_bytes(Bytes& out, std::span<const std::byte> in) {
    out.insert(out.end(), in.begin(), in.end());
}

[[nodiscard]] inline std::size_t longest(std::span<const Track> tracks) {
    std::size_t most = 0;
    for (const Track& track : tracks) {
        most = track.units.size() > most ? track.units.size() : most;
    }
    return most;
}

// --- Matroska ---------------------------------------------------------------------------------

// An EBML element with an eight-byte size, which any body fits.
[[nodiscard]] inline Bytes ebml(std::uint32_t id, std::span<const std::byte> body) {
    Bytes out;
    const int width = id <= 0xFFU ? 1 : id <= 0xFFFFU ? 2 : id <= 0xFFFFFFU ? 3 : 4;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(out, id >> (8 * i));
    }
    put_u8(out, 0x01);
    for (int i = 6; i >= 0; --i) {
        put_u8(out, static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(body.size()) >> (8 * i)) & 0xFFU));
    }
    put_bytes(out, body);
    return out;
}

[[nodiscard]] inline Bytes ebml_uint(std::uint32_t id, std::uint32_t value) {
    return ebml(id, Bytes{static_cast<std::byte>(value)});
}

[[nodiscard]] inline Bytes ebml_text(std::uint32_t id, std::string_view text) {
    Bytes body;
    put_text(body, text);
    return ebml(id, body);
}

[[nodiscard]] inline std::string_view matroska_codec_id(Codec codec) {
    switch (codec) {
        case Codec::kAc3: return "A_AC3";
        case Codec::kEac3: return "A_EAC3";
        case Codec::kTruehd: return "A_TRUEHD";
    }
    return "";
}

// --- MP4 --------------------------------------------------------------------------------------

[[nodiscard]] inline Bytes box(std::string_view type, std::span<const std::byte> body) {
    Bytes out;
    put_u32(out, static_cast<std::uint32_t>(8 + body.size()));
    put_text(out, type);
    put_bytes(out, body);
    return out;
}

[[nodiscard]] inline Bytes full_box(std::string_view type, std::uint32_t flags,
                                    std::span<const std::byte> body) {
    Bytes full;
    put_u32(full, flags);  // version 0
    put_bytes(full, body);
    return box(type, full);
}

[[nodiscard]] inline std::string_view mp4_entry(Codec codec) {
    switch (codec) {
        case Codec::kAc3: return "ac-3";
        case Codec::kEac3: return "ec-3";
        case Codec::kTruehd: return "mlpa";
    }
    return "";
}

// One trak: the sample entry (with no configuration box, which is not what a reader picks a track
// by), and a sample table of one chunk holding every unit, which starts at `chunk_offset`.
[[nodiscard]] inline Bytes mp4_trak(const Track& track, std::uint32_t track_id,
                                    std::uint64_t chunk_offset) {
    Bytes entry;
    put_u32(entry, 0);
    put_u16(entry, 0);
    put_u16(entry, 1);  // data_reference_index
    put_u32(entry, 0);
    put_u32(entry, 0);  // AudioSampleEntry reserved
    put_u16(entry, track.channels);
    put_u16(entry, 16);  // samplesize
    put_u32(entry, 0);   // pre_defined, reserved
    put_u32(entry, track.sample_rate << 16U);
    Bytes stsd;
    put_u32(stsd, 1);
    put_bytes(stsd, box(mp4_entry(track.codec), entry));

    Bytes stts;  // every sample the same length
    put_u32(stts, 1);
    put_u32(stts, static_cast<std::uint32_t>(track.units.size()));
    put_u32(stts, 1536);
    Bytes stsc;  // all of them in the first chunk
    put_u32(stsc, 1);
    put_u32(stsc, 1);
    put_u32(stsc, static_cast<std::uint32_t>(track.units.size()));
    put_u32(stsc, 1);
    Bytes stsz;
    put_u32(stsz, 0);
    put_u32(stsz, static_cast<std::uint32_t>(track.units.size()));
    for (const auto& unit : track.units) {
        put_u32(stsz, static_cast<std::uint32_t>(unit.size()));
    }
    Bytes stco;
    put_u32(stco, 1);
    put_u32(stco, static_cast<std::uint32_t>(chunk_offset));

    Bytes stbl;
    put_bytes(stbl, full_box("stsd", 0, stsd));
    put_bytes(stbl, full_box("stts", 0, stts));
    put_bytes(stbl, full_box("stsc", 0, stsc));
    put_bytes(stbl, full_box("stsz", 0, stsz));
    put_bytes(stbl, full_box("stco", 0, stco));
    Bytes minf;
    put_bytes(minf, box("stbl", stbl));

    Bytes mdhd;
    put_u32(mdhd, 0);
    put_u32(mdhd, 0);
    put_u32(mdhd, track.sample_rate);  // timescale
    put_u32(mdhd, 0);                  // duration
    put_u16(mdhd, 0x15C7);             // "eng"
    put_u16(mdhd, 0);
    Bytes hdlr;
    put_u32(hdlr, 0);
    put_text(hdlr, "soun");
    for (int i = 0; i < 3; ++i) {
        put_u32(hdlr, 0);
    }
    put_u8(hdlr, 0);
    Bytes mdia;
    put_bytes(mdia, full_box("mdhd", 0, mdhd));
    put_bytes(mdia, full_box("hdlr", 0, hdlr));
    put_bytes(mdia, box("minf", minf));

    Bytes tkhd;
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    put_u32(tkhd, track_id);
    for (int i = 0; i < 17; ++i) {
        put_u32(tkhd, 0);
    }
    Bytes trak;
    put_bytes(trak, full_box("tkhd", 0x000007, tkhd));
    put_bytes(trak, box("mdia", mdia));
    return box("trak", trak);
}

// --- MPEG-TS ----------------------------------------------------------------------------------

// CRC-32/MPEG-2, from the polynomial.
[[nodiscard]] inline std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : (crc << 1U);
        }
    }
    return crc;
}

[[nodiscard]] inline Bytes with_section_header(std::uint8_t table_id, const Bytes& body) {
    Bytes section;
    put_u8(section, table_id);
    put_u16(section, 0xB000U | static_cast<std::uint32_t>((body.size() + 4) & 0x0FFFU));
    put_bytes(section, body);
    put_u32(section, crc32_mpeg2(section));
    return section;
}

// One 188-byte packet, padded out with an adaptation field when the payload is short.
[[nodiscard]] inline Bytes ts_packet(std::uint16_t pid, bool unit_start, std::uint8_t& counter,
                                     std::span<const std::byte> payload) {
    constexpr std::size_t kPayloadBudget = 184;
    Bytes packet;
    put_u8(packet, 0x47);
    put_u16(packet, (unit_start ? 0x4000U : 0U) | (pid & 0x1FFFU));
    const bool stuffed = payload.size() < kPayloadBudget;
    put_u8(packet, (stuffed ? 0x30U : 0x10U) | (counter & 0x0FU));
    counter = static_cast<std::uint8_t>((counter + 1) & 0x0FU);
    if (stuffed) {
        const std::size_t adaptation = kPayloadBudget - payload.size() - 1;
        put_u8(packet, static_cast<std::uint32_t>(adaptation));
        if (adaptation > 0) {
            put_u8(packet, 0x00);
            for (std::size_t i = 1; i < adaptation; ++i) {
                put_u8(packet, 0xFF);
            }
        }
    }
    put_bytes(packet, payload);
    return packet;
}

[[nodiscard]] inline std::uint8_t mpegts_stream_type(Codec codec) {
    switch (codec) {
        case Codec::kAc3: return 0x81;    // ATSC
        case Codec::kEac3: return 0x87;   // ATSC
        case Codec::kTruehd: return 0x83;
    }
    return 0;
}

}  // namespace detail

// Each track is one stream of the file, in the order given, and the units of all of them are
// interleaved one after another. Track numbers, track_IDs and elementary PIDs count from 1, 1 and
// 0x100.

[[nodiscard]] inline Bytes matroska_file(std::span<const Track> tracks) {
    using namespace detail;
    Bytes entries;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const Track& track = tracks[i];
        // SamplingFrequency is a 32-bit float.
        Bytes rate;
        put_u32(rate, std::bit_cast<std::uint32_t>(static_cast<float>(track.sample_rate)));
        Bytes audio;
        put_bytes(audio, ebml(0xB5, rate));
        put_bytes(audio, ebml_uint(0x9F, track.channels));
        Bytes entry;
        put_bytes(entry, ebml_uint(0xD7, static_cast<std::uint32_t>(i + 1)));  // TrackNumber
        put_bytes(entry, ebml_uint(0x83, 2));                                  // TrackType: audio
        put_bytes(entry, ebml_text(0x86, matroska_codec_id(track.codec)));     // CodecID
        put_bytes(entry, ebml_text(0x22B59C, "und"));                          // Language
        put_bytes(entry, ebml(0xE1, audio));                                   // Audio
        put_bytes(entries, ebml(0xAE, entry));
    }
    Bytes cluster;
    put_bytes(cluster, ebml_uint(0xE7, 0));  // Timestamp
    for (std::size_t unit = 0; unit < longest(tracks); ++unit) {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (unit >= tracks[i].units.size()) {
                continue;
            }
            // A SimpleBlock: the track as a one-byte vint, a relative timestamp, the flags (a
            // keyframe), then the frame.
            Bytes block;
            put_u8(block, 0x80U | static_cast<std::uint32_t>(i + 1));
            put_u16(block, 0);
            put_u8(block, 0x80);
            put_bytes(block, tracks[i].units[unit]);
            put_bytes(cluster, ebml(0xA3, block));
        }
    }
    Bytes segment;
    put_bytes(segment, ebml(0x1654AE6B, entries));  // Tracks
    put_bytes(segment, ebml(0x1F43B675, cluster));  // Cluster

    Bytes file;
    put_bytes(file, ebml(0x1A45DFA3, ebml_text(0x4282, "matroska")));  // EBML header, DocType
    put_bytes(file, ebml(0x18538067, segment));                        // Segment
    return file;
}

[[nodiscard]] inline Bytes mp4_file(std::span<const Track> tracks) {
    using namespace detail;
    Bytes ftyp_body;
    put_text(ftyp_body, "isom");
    put_u32(ftyp_body, 0);
    put_text(ftyp_body, "isom");
    const Bytes ftyp = box("ftyp", ftyp_body);

    // The sample tables hold the offset of each track's chunk, which depends on how long the moov
    // is, which does not depend on the offsets: so it is built twice.
    const auto moov_at = [&tracks](std::uint64_t mdat_data) {
        Bytes moov;
        std::uint64_t chunk = mdat_data;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            put_bytes(moov, mp4_trak(tracks[i], static_cast<std::uint32_t>(i + 1), chunk));
            for (const auto& unit : tracks[i].units) {
                chunk += unit.size();
            }
        }
        return box("moov", moov);
    };
    const std::uint64_t mdat_data = ftyp.size() + moov_at(0).size() + 8;

    Bytes data;
    for (const Track& track : tracks) {
        for (const auto& unit : track.units) {
            put_bytes(data, unit);
        }
    }
    Bytes file = ftyp;
    put_bytes(file, moov_at(mdat_data));
    put_bytes(file, box("mdat", data));
    return file;
}

[[nodiscard]] inline Bytes mpegts_file(std::span<const Track> tracks) {
    using namespace detail;
    constexpr std::uint16_t kPmtPid = 0x1000;
    constexpr std::uint16_t kFirstPid = 0x100;
    constexpr std::uint16_t kProgramNumber = 1;

    Bytes out;
    std::uint8_t pat_counter = 0;
    std::uint8_t pmt_counter = 0;

    Bytes pat_body;
    put_u16(pat_body, 1);     // transport_stream_id
    put_u8(pat_body, 0xC1);   // version 0, current
    put_u8(pat_body, 0x00);   // section_number
    put_u8(pat_body, 0x00);   // last_section_number
    put_u16(pat_body, kProgramNumber);
    put_u16(pat_body, 0xE000U | kPmtPid);
    Bytes pat_payload{std::byte{0x00}};  // pointer_field
    put_bytes(pat_payload, with_section_header(0x00, pat_body));
    put_bytes(out, ts_packet(0x0000, true, pat_counter, pat_payload));

    Bytes pmt_body;
    put_u16(pmt_body, kProgramNumber);
    put_u8(pmt_body, 0xC1);
    put_u8(pmt_body, 0x00);
    put_u8(pmt_body, 0x00);
    put_u16(pmt_body, 0xE000U | kFirstPid);  // PCR_PID
    put_u16(pmt_body, 0xF000);               // program_info_length 0
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        put_u8(pmt_body, mpegts_stream_type(tracks[i].codec));
        put_u16(pmt_body, 0xE000U | (kFirstPid + static_cast<std::uint32_t>(i)));
        put_u16(pmt_body, 0xF000);  // ES_info_length 0
    }
    Bytes pmt_payload{std::byte{0x00}};
    put_bytes(pmt_payload, with_section_header(0x02, pmt_body));
    put_bytes(out, ts_packet(kPmtPid, true, pmt_counter, pmt_payload));

    std::vector<std::uint8_t> counters(tracks.size(), 0);
    for (std::size_t unit = 0; unit < longest(tracks); ++unit) {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (unit >= tracks[i].units.size()) {
                continue;
            }
            // One PES packet to an access unit: private_stream_1, no timestamps.
            Bytes pes;
            put_u32(pes, 0x000001BD);
            put_u16(pes, static_cast<std::uint32_t>(3 + tracks[i].units[unit].size()));
            put_u8(pes, 0x84);
            put_u8(pes, 0x00);
            put_u8(pes, 0x00);  // PES_header_data_length
            put_bytes(pes, tracks[i].units[unit]);
            const auto pid = static_cast<std::uint16_t>(kFirstPid + i);
            for (std::size_t at = 0; at < pes.size(); at += 184) {
                const std::size_t take = pes.size() - at < 184 ? pes.size() - at : 184;
                put_bytes(out,
                          ts_packet(pid, at == 0, counters[i],
                                    std::span<const std::byte>{pes}.subspan(at, take)));
            }
        }
    }
    return out;
}

}  // namespace iclforge::test::multi_track
