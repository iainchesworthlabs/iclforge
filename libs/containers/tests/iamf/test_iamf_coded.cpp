#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// Opus, AAC-LC and FLAC carried in IAMF (the codecs' packets are the caller's), and the ISO-BMFF
// side of it: the `roll` sample group, a 64-bit `mdat`, every IA track of a file, and a protected
// track told apart from one that is not IAMF. The decoder_config bytes are checked against
// hand-assembled expectations from 3.13; the packets are synthetic, since the module carries them
// without decoding.

namespace iamf = iclforge::containers::iamf;

namespace {

using iamf::Bytes;

[[nodiscard]] Bytes bytes_of(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

[[nodiscard]] std::vector<unsigned> ints(const Bytes& bytes) {
    std::vector<unsigned> out;
    for (const std::byte b : bytes) {
        out.push_back(std::to_integer<unsigned>(b));
    }
    return out;
}

// An Opus packet: the TOC byte of CELT full band, `ms` = 20, one frame (code 0), then padding
// bytes standing for the frame.
[[nodiscard]] Bytes opus_packet(unsigned config = 31, bool stereo = false,
                                std::size_t payload = 40) {
    Bytes packet;
    packet.push_back(static_cast<std::byte>((config << 3) | (stereo ? 4U : 0U)));
    packet.resize(1 + payload, std::byte{0x5A});
    return packet;
}

// A FLAC frame header (RFC 9639, 9.1) for the block size, rate and depth of the track, then bytes
// standing for the subframes. Block size 960 is coded as 16 bits (code 0b0111).
[[nodiscard]] Bytes flac_packet(unsigned rate_code, unsigned depth_code, bool stereo,
                                unsigned block = 960, std::size_t payload = 60) {
    Bytes packet = bytes_of({0xFF, 0xF8, (0x7U << 4) | rate_code,
                             ((stereo ? 1U : 0U) << 4) | (depth_code << 1), 0x00 /* frame number */,
                             (block - 1) >> 8, (block - 1) & 0xFFU, 0x00 /* CRC-8 */});
    packet.resize(packet.size() + payload, std::byte{0x33});
    return packet;
}

[[nodiscard]] std::vector<iamf::CodedFrame> opus_frames(std::size_t count, std::size_t substreams) {
    std::vector<iamf::CodedFrame> frames(count);
    for (auto& frame : frames) {
        // Substreams are stereo pairs first, then mono: only the stereo bit differs.
        for (std::size_t s = 0; s < substreams; ++s) {
            frame.substreams.push_back(opus_packet(31, false, 30 + s));
        }
    }
    return frames;
}

[[nodiscard]] std::size_t find_fourcc(const Bytes& data, std::string_view type,
                                      std::size_t from = 0) {
    for (std::size_t i = from; i + 4 <= data.size(); ++i) {
        if (std::equal(type.begin(), type.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
                       [](char c, std::byte b) {
                           return static_cast<unsigned char>(c) == std::to_integer<unsigned>(b);
                       })) {
            return i;
        }
    }
    return data.size();
}

[[nodiscard]] std::uint32_t be32(const Bytes& data, std::size_t at) {
    return (std::to_integer<std::uint32_t>(data[at]) << 24) |
           (std::to_integer<std::uint32_t>(data[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(data[at + 2]) << 8) |
           std::to_integer<std::uint32_t>(data[at + 3]);
}

[[nodiscard]] Bytes make_box(std::string_view type, const Bytes& payload) {
    Bytes out;
    const auto size = static_cast<std::uint32_t>(8 + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((size >> shift) & 0xFFU));
    }
    for (const char c : type) {
        out.push_back(static_cast<std::byte>(c));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

TEST_CASE("IAMF decoder_config bytes follow 3.13", "[iamf][coded]") {
    SECTION("Opus: the ID Header without its signature, big-endian") {
        // version 1, 2 channels, pre-skip 312, 48000 Hz, gain 0, mapping family 0
        CHECK(ints(iamf::opus_decoder_config(312)) ==
              std::vector<unsigned>{1, 2, 0x01, 0x38, 0x00, 0x00, 0xBB, 0x80, 0x00, 0x00, 0x00});
        CHECK(ints(iamf::opus_decoder_config(0, 44100)) ==
              std::vector<unsigned>{1, 2, 0, 0, 0x00, 0x00, 0xAC, 0x44, 0, 0, 0});
    }
    SECTION("AAC-LC: DecoderConfigDescriptor with AudioSpecificConfig 0x1190 at 48 kHz") {
        auto config = iamf::aac_lc_decoder_config(48000);
        REQUIRE(config.has_value());
        CHECK(ints(*config) == std::vector<unsigned>{0x04, 0x11, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0x05, 0x02, 0x11, 0x90});
        auto cd = iamf::aac_lc_decoder_config(44100, 640000, 256000);
        REQUIRE(cd.has_value());
        // maxBitrate 640000 = 0x0009C400, avgBitrate 256000 = 0x0003E800; sampling index 4: 0x1210
        CHECK(ints(*cd) == std::vector<unsigned>{0x04, 0x11, 0x40, 0x15, 0, 0, 0, 0x00, 0x09, 0xC4,
                                                 0x00, 0x00, 0x03, 0xE8, 0x00, 0x05, 0x02, 0x12,
                                                 0x10});
        CHECK_FALSE(iamf::aac_lc_decoder_config(12345).has_value());
    }
    SECTION("FLAC: one last STREAMINFO block") {
        auto config = iamf::flac_decoder_config(48000, 16, 960);
        REQUIRE(config.has_value());
        std::vector<unsigned> expected{0x80, 0x00, 0x00, 0x22,  // last block, STREAMINFO, 34 bytes
                                       0x03, 0xC0, 0x03, 0xC0,  // block sizes 960
                                       0,    0,    0,    0,    0, 0,  // frame sizes unknown
                                       0x0B, 0xB8, 0x02, 0xF0, 0, 0,
                                       0,    0};   // 48000 Hz, 2 channels, 16 bits
        expected.resize(expected.size() + 16, 0);  // MD5
        CHECK(ints(*config) == expected);
        CHECK_FALSE(iamf::flac_decoder_config(48001, 16, 960).has_value());
        CHECK_FALSE(iamf::flac_decoder_config(48000, 20, 960).has_value());
        CHECK_FALSE(iamf::flac_decoder_config(48000, 16, 8).has_value());
    }
}

TEST_CASE("IAMF mux_coded writes an Opus programme with its roll group and pre-skip",
          "[iamf][coded]") {
    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kOpus;
    track.loudspeaker_layout = 1;  // Stereo: one coupled substream
    track.trim_start_samples = 312;
    track.trim_end_samples = 100;
    const auto frames = opus_frames(6, 1);

    auto sequence = iamf::build_coded_sequence(track, frames);
    REQUIRE(sequence.has_value());
    REQUIRE(sequence->codec_configs.size() == 1);
    const iamf::CodecConfig& config = sequence->codec_configs[0];
    CHECK(config.codec_id == "Opus");
    CHECK(config.num_samples_per_frame == 960);
    CHECK(config.audio_roll_distance == -4);  // -ceil(3840 / 960)
    CHECK(config.decoder_config == iamf::opus_decoder_config(312));
    CHECK(iamf::codecs_string(*sequence) == "iamf.000.000.Opus");
    // 312 samples of pre-skip fit the first frame; 100 are trimmed from the last.
    CHECK(sequence->temporal_units.front().audio_frames[0].num_samples_to_trim_at_start == 312);
    CHECK(sequence->temporal_units.back().audio_frames[0].num_samples_to_trim_at_end == 100);

    auto file = iamf::mux_coded(track, frames);
    REQUIRE(file.has_value());
    // 6.2.2: the roll sample group, with roll_distance -4 as a signed 16 bit field.
    const std::size_t sgpd = find_fourcc(*file, "sgpd");
    REQUIRE(sgpd < file->size());
    CHECK(find_fourcc(*file, "roll", sgpd) < file->size());
    CHECK(be32(*file, sgpd + 4 + 4 + 4 + 4) ==
          1);  // version/flags, grouping_type, default_length: entry_count at +16
    CHECK(find_fourcc(*file, "sbgp") < file->size());

    auto read = iamf::read_isobmff(*file);
    REQUIRE(read.has_value());
    CHECK(read->info.timescale == 48000);
    CHECK(read->info.track_id == 1);
    CHECK(read->sequence.codec_configs[0].audio_roll_distance == -4);
    CHECK(read->sequence.codec_configs[0].decoder_config == config.decoder_config);
    REQUIRE(read->sequence.temporal_units.size() == 6);
    CHECK(read->sequence.temporal_units[2].audio_frames[0].data == frames[2].substreams[0]);
    CHECK(read->info.edit.has_value());
}

TEST_CASE("IAMF mux_coded writes AAC-LC and FLAC with the right timescale and no roll for FLAC",
          "[iamf][coded]") {
    SECTION("AAC-LC at 44.1 kHz, 5.1: roll -1, timescale 44100") {
        iamf::CodedTrack track;
        track.codec = iamf::CodedCodec::kAacLc;
        track.sample_rate = 44100;
        track.samples_per_frame = 1024;
        track.loudspeaker_layout = 2;  // 5.1: (L,R) (Ls,Rs) C LFE
        std::vector<iamf::CodedFrame> frames(4);
        for (auto& frame : frames) {
            for (int s = 0; s < 4; ++s) {
                frame.substreams.push_back(bytes_of({0x21, 0x11, 0x45, 0x00, 0x01, 0x02}));
            }
        }
        auto file = iamf::mux_coded(track, frames);
        REQUIRE(file.has_value());
        CHECK(find_fourcc(*file, "sgpd") < file->size());
        auto read = iamf::read_isobmff(*file);
        REQUIRE(read.has_value());
        CHECK(read->info.timescale == 44100);
        CHECK(read->sequence.codec_configs[0].codec_id == "mp4a");
        CHECK(read->sequence.codec_configs[0].audio_roll_distance == -1);
        CHECK(iamf::codecs_string(read->sequence) == "iamf.000.000.mp4a.40.2");
        // The 5.1 element's Mix Presentation lists Stereo and 5.1 loudness.
        REQUIRE(read->sequence.mix_presentations.size() == 1);
        CHECK(read->sequence.mix_presentations[0].sub_mixes[0].layouts.size() == 2);
    }
    SECTION("FLAC at 48 kHz, 24 bit, stereo and mono substreams: no roll group") {
        iamf::CodedTrack track;
        track.codec = iamf::CodedCodec::kFlac;
        track.sample_rate = 48000;
        track.bit_depth = 24;
        track.loudspeaker_layout = 8;  // 3.1.2: (L,R) (Ltf,Rtf) C LFE
        std::vector<iamf::CodedFrame> frames(3);
        for (auto& frame : frames) {
            frame.substreams.push_back(flac_packet(0b1010, 0b110, true));
            frame.substreams.push_back(flac_packet(0b1010, 0b110, true));
            frame.substreams.push_back(flac_packet(0b1010, 0b110, false));
            frame.substreams.push_back(flac_packet(0b1010, 0b110, false));
        }
        auto file = iamf::mux_coded(track, frames);
        REQUIRE(file.has_value());
        CHECK(find_fourcc(*file, "sgpd") == file->size());
        auto read = iamf::read_isobmff(*file);
        REQUIRE(read.has_value());
        CHECK(read->info.timescale == 48000);
        CHECK(read->sequence.codec_configs[0].codec_id == "fLaC");
        CHECK(read->sequence.codec_configs[0].audio_roll_distance == 0);
        CHECK(iamf::codecs_string(read->sequence) == "iamf.000.000.fLaC");
    }
}

TEST_CASE("IAMF coded builders refuse what 3.13 does not allow", "[iamf][coded]") {
    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kOpus;
    track.loudspeaker_layout = 1;
    const auto good = opus_frames(2, 1);

    SECTION("no frames") {
        auto result = iamf::build_coded_sequence(track, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kNoFrames);
    }
    SECTION("a layout that is not a single layer of 0 to 8") {
        track.loudspeaker_layout = 9;
        auto result = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidLayout);
    }
    SECTION("the wrong number of packets") {
        track.loudspeaker_layout = 7;  // 7 substreams
        auto result = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kSubstreamCountMismatch);
    }
    SECTION("an Opus packet with more than one frame") {
        auto frames = good;
        frames[1].substreams[0][0] = static_cast<std::byte>((31U << 3) | 1U);  // code 1: two frames
        auto result = iamf::build_coded_sequence(track, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kBadCodedPacket);
    }
    SECTION("an Opus packet whose duration is not samples_per_frame") {
        auto frames = good;
        frames[0].substreams[0] = opus_packet(30);  // CELT FB 10 ms
        auto result = iamf::build_coded_sequence(track, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kBadCodedPacket);
    }
    SECTION("an empty packet") {
        auto frames = good;
        frames[0].substreams[0].clear();
        auto result = iamf::build_coded_sequence(track, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kBadCodedPacket);
    }
    SECTION("Opus off 48 kHz, or an odd frame length") {
        track.sample_rate = 44100;
        auto at_rate = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(at_rate.has_value());
        CHECK(at_rate.error() == iamf::MuxError::kInvalidTrack);
        track.sample_rate = 48000;
        track.samples_per_frame = 1000;
        auto at_length = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(at_length.has_value());
        CHECK(at_length.error() == iamf::MuxError::kInvalidTrack);
    }
    SECTION("a pre-skip that does not fit 16 bits") {
        track.trim_start_samples = 70000;
        auto result = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrim);
    }
    SECTION("AAC-LC off 1024 samples") {
        track.codec = iamf::CodedCodec::kAacLc;
        track.sample_rate = 48000;
        track.samples_per_frame = 960;
        auto result = iamf::build_coded_sequence(track, good);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrack);
    }
    SECTION("FLAC frames that disagree with the track") {
        track.codec = iamf::CodedCodec::kFlac;
        track.sample_rate = 48000;
        track.bit_depth = 16;
        std::vector<iamf::CodedFrame> frames(1);
        frames[0].substreams.push_back(flac_packet(0b1010, 0b100, true));
        REQUIRE(iamf::build_coded_sequence(track, frames).has_value());

        frames[0].substreams[0] =
            flac_packet(0b1010, 0b100, false);  // mono coding in a stereo substream
        CHECK(iamf::build_coded_sequence(track, frames).error() == iamf::MuxError::kBadCodedPacket);
        frames[0].substreams[0] = flac_packet(0b1001, 0b100, true);  // 44.1 kHz in a 48 kHz track
        CHECK(iamf::build_coded_sequence(track, frames).error() == iamf::MuxError::kBadCodedPacket);
        frames[0].substreams[0] = flac_packet(0b1010, 0b110, true);  // 24 bit in a 16 bit track
        CHECK(iamf::build_coded_sequence(track, frames).error() == iamf::MuxError::kBadCodedPacket);
        frames[0].substreams[0] = flac_packet(0b1010, 0b100, true, 1152);  // another block size
        CHECK(iamf::build_coded_sequence(track, frames).error() == iamf::MuxError::kBadCodedPacket);
    }
}

TEST_CASE("IAMF fragments of an Opus track carry the roll group too", "[iamf][coded]") {
    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kOpus;
    track.loudspeaker_layout = 1;
    const auto frames = opus_frames(4, 1);
    auto sequence = iamf::build_coded_sequence(track, frames);
    REQUIRE(sequence.has_value());

    auto writer = iamf::FragmentedWriter::create(*sequence, {});
    REQUIRE(writer.has_value());
    // The description is in the initialization segment, the sample mapping in each fragment.
    CHECK(find_fourcc(writer->initialization_segment(), "sgpd") <
          writer->initialization_segment().size());
    auto first = writer->fragment(std::span(sequence->temporal_units).first(2));
    auto second = writer->fragment(std::span(sequence->temporal_units).last(2));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(find_fourcc(*first, "sbgp") < first->size());
    CHECK(find_fourcc(*second, "sbgp") < second->size());

    Bytes whole = writer->initialization_segment();
    whole.insert(whole.end(), first->begin(), first->end());
    whole.insert(whole.end(), second->begin(), second->end());
    auto read = iamf::read_isobmff(whole);
    REQUIRE(read.has_value());
    CHECK(read->info.fragmented);
    REQUIRE(read->sequence.temporal_units.size() == 4);
    CHECK(read->sequence.temporal_units[3].audio_frames[0].data == frames[3].substreams[0]);
}

TEST_CASE("IAMF writes a 64-bit mdat size when asked and reads it back", "[iamf][container]") {
    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kOpus;
    track.loudspeaker_layout = 1;
    const auto frames = opus_frames(3, 1);
    auto sequence = iamf::build_coded_sequence(track, frames);
    REQUIRE(sequence.has_value());

    auto normal = iamf::write_isobmff(*sequence, {});
    auto large = iamf::write_isobmff(*sequence, {.large_mdat = true});
    REQUIRE(normal.has_value());
    REQUIRE(large.has_value());
    CHECK(large->size() == normal->size() + 8);
    const std::size_t at = find_fourcc(*large, "mdat");
    REQUIRE(at >= 4);
    CHECK(be32(*large, at - 4) == 1);  // size 1: a largesize follows the type
    CHECK(be32(*large, at + 4) == 0);
    CHECK(be32(*large, at + 8) == 16 + (large->size() - (at - 4 + 16)));

    auto read = iamf::read_isobmff(*large);
    REQUIRE(read.has_value());
    REQUIRE(read->sequence.temporal_units.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(read->sequence.temporal_units[i].audio_frames[0].data == frames[i].substreams[0]);
    }

    // A fragment too.
    auto writer = iamf::FragmentedWriter::create(*sequence, {.large_mdat = true});
    REQUIRE(writer.has_value());
    auto fragment = writer->fragment(sequence->temporal_units);
    REQUIRE(fragment.has_value());
    Bytes whole = writer->initialization_segment();
    whole.insert(whole.end(), fragment->begin(), fragment->end());
    auto fragmented = iamf::read_isobmff(whole);
    REQUIRE(fragmented.has_value());
    CHECK(fragmented->sequence.temporal_units.size() == 3);
    CHECK(fragmented->sequence.temporal_units[1].audio_frames[0].data == frames[1].substreams[0]);
}

namespace {

// A movie file with two IA tracks, track_IDs 1 and 2, built from two fragmented writers' output:
// the first's ftyp, a moov with both traks and both trex entries, then each track's fragment with
// its tfhd track_ID.
struct TwoTracks {
    Bytes file;
    std::vector<iamf::CodedFrame> first;
    std::vector<iamf::CodedFrame> second;
};

[[nodiscard]] Bytes slice(const Bytes& data, std::size_t begin, std::size_t end) {
    return Bytes(data.begin() + static_cast<std::ptrdiff_t>(begin),
                 data.begin() + static_cast<std::ptrdiff_t>(end));
}

void set_be32(Bytes& data, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[at + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFFU);
    }
}

[[nodiscard]] TwoTracks two_tracks() {
    TwoTracks result;
    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kOpus;
    track.loudspeaker_layout = 1;
    result.first = opus_frames(3, 1);
    result.second = opus_frames(5, 1);  // another length, and different packet sizes
    for (auto& frame : result.second) {
        frame.substreams[0] = opus_packet(31, false, 77);
    }
    auto a = *iamf::build_coded_sequence(track, result.first);
    auto b = *iamf::build_coded_sequence(track, result.second);
    auto wa = *iamf::FragmentedWriter::create(a, {});
    auto wb = *iamf::FragmentedWriter::create(b, {});
    const Bytes fa = *wa.fragment(a.temporal_units);
    Bytes fb = *wb.fragment(b.temporal_units);
    const Bytes& ia = wa.initialization_segment();
    const Bytes& ib = wb.initialization_segment();

    // Each initialization segment is ftyp, moov{mvhd, trak, mvex{trex}}: take the trak and the
    // trex out of each by their box headers.
    const auto child = [](const Bytes& data, std::string_view type, std::size_t from) {
        const std::size_t at = find_fourcc(data, type, from);
        REQUIRE(at < data.size());
        return std::pair{at - 4, at - 4 + be32(data, at - 4)};
    };
    const auto ftyp_end = be32(ia, 0);
    const auto moov_a = child(ia, "moov", ftyp_end);
    const auto mvhd_a = child(ia, "mvhd", moov_a.first);
    const auto trak_a = child(ia, "trak", moov_a.first);
    const auto mvex_a = child(ia, "mvex", moov_a.first);
    const auto moov_b = child(ib, "moov", be32(ib, 0));
    Bytes trak_b =
        slice(ib, child(ib, "trak", moov_b.first).first, child(ib, "trak", moov_b.first).second);
    const auto trex_a = child(ia, "trex", mvex_a.first);
    const auto trex_b = child(ib, "trex", child(ib, "mvex", moov_b.first).first);
    Bytes trex_b_box = slice(ib, trex_b.first, trex_b.second);

    // tkhd (a version 0 FullBox): size, type, version/flags, 2 times, then track_ID.
    const std::size_t tkhd_b = find_fourcc(trak_b, "tkhd") - 4;
    set_be32(trak_b, tkhd_b + 20, 2);
    set_be32(trex_b_box, 8 + 4, 2);  // trex: header, version/flags, then track_ID
    // tfhd of the second fragment: header, version/flags, then track_ID.
    const std::size_t tfhd_b = find_fourcc(fb, "tfhd") - 4;
    set_be32(fb, tfhd_b + 12, 2);

    Bytes mvex_body = slice(ia, trex_a.first, trex_a.second);
    mvex_body.insert(mvex_body.end(), trex_b_box.begin(), trex_b_box.end());
    Bytes moov_body = slice(ia, mvhd_a.first, mvhd_a.second);
    const Bytes trak_a_box = slice(ia, trak_a.first, trak_a.second);
    moov_body.insert(moov_body.end(), trak_a_box.begin(), trak_a_box.end());
    moov_body.insert(moov_body.end(), trak_b.begin(), trak_b.end());
    const Bytes mvex_box = make_box("mvex", mvex_body);
    moov_body.insert(moov_body.end(), mvex_box.begin(), mvex_box.end());

    result.file = slice(ia, 0, ftyp_end);
    const Bytes moov = make_box("moov", moov_body);
    result.file.insert(result.file.end(), moov.begin(), moov.end());
    result.file.insert(result.file.end(), fa.begin(), fa.end());
    result.file.insert(result.file.end(), fb.begin(), fb.end());
    return result;
}

}  // namespace

TEST_CASE("IAMF read_isobmff_tracks reads every IA track and read_isobmff the first",
          "[iamf][container]") {
    const TwoTracks input = two_tracks();

    auto first = iamf::read_isobmff(input.file);
    REQUIRE(first.has_value());
    CHECK(first->info.track_id == 1);
    // The second track's fragment is not folded into the first's samples.
    REQUIRE(first->sequence.temporal_units.size() == 3);
    CHECK(first->sequence.temporal_units[2].audio_frames[0].data == input.first[2].substreams[0]);

    auto all = iamf::read_isobmff_tracks(input.file);
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 2);
    CHECK((*all)[0].info.track_id == 1);
    CHECK((*all)[1].info.track_id == 2);
    REQUIRE((*all)[1].sequence.temporal_units.size() == 5);
    CHECK((*all)[1].sequence.temporal_units[4].audio_frames[0].data ==
          input.second[4].substreams[0]);
    CHECK((*all)[0].info.fragmented);
}

TEST_CASE("IAMF tells a protected IA track from a file that is not IAMF", "[iamf][container]") {
    // ftyp(iamf) moov{trak{mdia{minf{stbl{stsd{<entry>}}}}}}: the entry is what the test varies.
    const auto file_with_entry = [](const Bytes& entry) {
        Bytes stsd_body = bytes_of({0, 0, 0, 0, 0, 0, 0, 1});  // version/flags, entry_count 1
        stsd_body.insert(stsd_body.end(), entry.begin(), entry.end());
        const Bytes stsd = make_box("stsd", stsd_body);
        const Bytes stbl = make_box("stbl", stsd);
        const Bytes minf = make_box("minf", stbl);
        const Bytes mdia = make_box("mdia", minf);
        const Bytes trak = make_box("trak", mdia);
        const Bytes moov = make_box("moov", trak);
        Bytes ftyp_body;
        for (const char c : std::string_view("iamf")) {
            ftyp_body.push_back(static_cast<std::byte>(c));
        }
        ftyp_body.insert(ftyp_body.end(), 4, std::byte{0});
        for (const char c : std::string_view("iamf")) {
            ftyp_body.push_back(static_cast<std::byte>(c));
        }
        Bytes file = make_box("ftyp", ftyp_body);
        file.insert(file.end(), moov.begin(), moov.end());
        return file;
    };
    const auto audio_entry = [](std::string_view type, std::string_view original) {
        Bytes body(28, std::byte{0});  // an AudioSampleEntry's fields
        Bytes frma_body;
        for (const char c : original) {
            frma_body.push_back(static_cast<std::byte>(c));
        }
        const Bytes sinf = make_box("sinf", make_box("frma", frma_body));
        body.insert(body.end(), sinf.begin(), sinf.end());
        return make_box(type, body);
    };

    SECTION("enca with the original format iamf: needs a key, so unsupported") {
        auto result = iamf::read_isobmff(file_with_entry(audio_entry("enca", "iamf")));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::Error::kUnsupported);
        auto tracks = iamf::read_isobmff_tracks(file_with_entry(audio_entry("enca", "iamf")));
        REQUIRE_FALSE(tracks.has_value());
        CHECK(tracks.error() == iamf::Error::kUnsupported);
    }
    SECTION("enca of another codec is just not IAMF") {
        auto result = iamf::read_isobmff(file_with_entry(audio_entry("enca", "mp4a")));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::Error::kNotIamf);
    }
}

TEST_CASE("IAMF codecs_string names the profiles and the codec", "[iamf][coded]") {
    iamf::Sequence sequence;
    CHECK_FALSE(iamf::codecs_string(sequence).has_value());  // no Audio Element

    iamf::CodecConfig config;
    config.codec_id = "ipcm";
    sequence.codec_configs.push_back(config);
    iamf::AudioElement element;
    sequence.audio_elements.push_back(element);
    sequence.header = {static_cast<std::uint8_t>(iamf::Profile::kBaseAdvanced), 255};
    CHECK(iamf::codecs_string(sequence) == "iamf.003.255.ipcm");

    sequence.codec_configs[0].codec_id = "xxxx";
    CHECK_FALSE(iamf::codecs_string(sequence).has_value());
    sequence.audio_elements[0].codec_config_id = 7;  // no such Codec Config
    CHECK_FALSE(iamf::codecs_string(sequence).has_value());
}
