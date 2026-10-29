#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/containers/mpegts/mpegts.hpp"
#include "iclforge/containers/mpegts/reader.hpp"
#include "truehd/truehd_stream.hpp"

// TrueHD in MPEG-TS (libs/containers/ERRATA.md, entry C8): stream_type 0x83 (TH spec §21.2) and, for FFmpeg's
// demuxer to name it, a program-level 'HDMV' registration descriptor. FFmpeg is the oracle in both directions
// (tools/checks/verify_truehd_containers.py); these cases hold the round trip, the refusals, the two signalling
// forms the reader tells apart and the 192-byte grid of a Blu-ray transport stream.

namespace {

namespace ts = iclforge::containers::mpegts;
using iclforge::containers::testing::load_truehd;
using iclforge::containers::testing::TruehdStream;

constexpr std::size_t kPacket = 188;
constexpr std::uint16_t kPmtPid = 0x1000;

TruehdStream load(const std::string& name) {
    auto stream = load_truehd(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name);
    REQUIRE(stream.has_value());
    return std::move(*stream);
}

ts::AudioTrack track_of(const TruehdStream& stream) {
    return ts::AudioTrack{.codec = ts::AudioCodec::kTruehd,
                          .sample_rate = stream.info.sample_rate,
                          .channels = stream.channels,
                          .samples_per_frame = stream.samples_per_unit};
}

ts::MuxOptions bluray() {
    ts::MuxOptions options;
    options.profile = ts::BroadcastProfile::kBluray;
    return options;
}

std::vector<std::byte> concatenated(const ts::Demuxed& demuxed) {
    std::vector<std::byte> out;
    for (const auto& payload : demuxed.payloads) {
        out.insert(out.end(), payload.begin(), payload.end());
    }
    return out;
}

std::vector<std::byte> stream_bytes(const TruehdStream& stream) {
    std::vector<std::byte> out;
    for (const auto& unit : stream.units) {
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24U;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
        }
    }
    return crc;
}

// The same transport stream with the program-level descriptors taken out of every PMT: stream_type 0x83 alone,
// the way FFmpeg's muxer writes it.
std::vector<std::byte> without_program_descriptors(std::vector<std::byte> file) {
    for (std::size_t at = 0; at + kPacket <= file.size(); at += kPacket) {
        const auto pid = static_cast<std::uint16_t>(((std::to_integer<unsigned>(file[at + 1]) & 0x1FU) << 8U) |
                                                    std::to_integer<unsigned>(file[at + 2]));
        if (pid != kPmtPid || (std::to_integer<unsigned>(file[at + 1]) & 0x40U) == 0) {
            continue;
        }
        const std::size_t section = at + 5 + std::to_integer<std::size_t>(file[at + 4]);
        const std::size_t length =
            3 + (((std::to_integer<std::size_t>(file[section + 1]) & 0x0FU) << 8U) |
                 std::to_integer<std::size_t>(file[section + 2]));
        const std::size_t info =
            ((std::to_integer<std::size_t>(file[section + 10]) & 0x0FU) << 8U) |
            std::to_integer<std::size_t>(file[section + 11]);
        REQUIRE(info == 6);  // the registration descriptor: tag, length, 'HDMV'
        std::vector<std::byte> bare(file.begin() + static_cast<std::ptrdiff_t>(section),
                                    file.begin() + static_cast<std::ptrdiff_t>(section + 12));
        bare.insert(bare.end(), file.begin() + static_cast<std::ptrdiff_t>(section + 12 + info),
                    file.begin() + static_cast<std::ptrdiff_t>(section + length - 4));
        bare[2] = static_cast<std::byte>(bare.size() - 3 + 4);
        bare[11] = std::byte{0};
        const std::uint32_t crc = crc32_mpeg2(bare);
        for (int shift = 24; shift >= 0; shift -= 8) {
            bare.push_back(static_cast<std::byte>((crc >> static_cast<unsigned>(shift)) & 0xFFU));
        }
        std::fill(file.begin() + static_cast<std::ptrdiff_t>(section), file.begin() + static_cast<std::ptrdiff_t>(at + kPacket),
                  std::byte{0xFF});
        std::copy(bare.begin(), bare.end(), file.begin() + static_cast<std::ptrdiff_t>(section));
    }
    return file;
}

}  // namespace

TEST_CASE("a Blu-ray transport stream round-trips the access units", "[truehd][mpegts]") {
    for (const char* name : {"truehd_st48_s24.thd", "truehd_m51_s24.thd", "truehd_st96_s24.thd",
                             "truehd_st192_s24.thd", "truehd_st44_s24.thd", "truehd_mono48_s24.thd"}) {
        SECTION(name) {
            const TruehdStream stream = load(name);
            const auto file = ts::mux(track_of(stream), stream.units, bluray());
            REQUIRE(file.has_value());
            REQUIRE(file->size() % kPacket == 0);

            const auto demuxed = ts::demux(*file);
            REQUIRE(demuxed.has_value());
            CHECK(demuxed->stream.stream_type == 0x83);
            CHECK(demuxed->stream.truehd);
            CHECK_FALSE(demuxed->stream.eac3);
            CHECK_FALSE(demuxed->stream.ac4);
            CHECK(demuxed->stream.signalling == ts::CodecSignalling::kHdmvStreamType);
            CHECK(demuxed->stream.packet_size == 188);
            CHECK(concatenated(*demuxed) == stream_bytes(stream));
        }
    }
}

TEST_CASE("stream_type 0x83 without the HDMV registration is still TrueHD, and says how it was found", "[truehd][mpegts]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    const auto file = ts::mux(track_of(stream), stream.units, bluray());
    REQUIRE(file.has_value());

    const auto demuxed = ts::demux(without_program_descriptors(*file));
    REQUIRE(demuxed.has_value());
    CHECK(demuxed->stream.truehd);
    CHECK(demuxed->stream.signalling == ts::CodecSignalling::kBareTruehdStreamType);
    CHECK(concatenated(*demuxed) == stream_bytes(stream));
}

TEST_CASE("a 192-byte grid, the Blu-ray packet with its TP_extra_header, reads the same", "[truehd][mpegts]") {
    const TruehdStream stream = load("truehd_m51_s24.thd");
    const auto file = ts::mux(track_of(stream), stream.units, bluray());
    REQUIRE(file.has_value());
    std::vector<std::byte> grid;
    for (std::size_t at = 0; at + kPacket <= file->size(); at += kPacket) {
        // TP_extra_header: copy_permission_indicator(2) and a 30-bit arrival time stamp.
        grid.insert(grid.end(), {std::byte{0}, std::byte{0}, std::byte{0}, static_cast<std::byte>((at / kPacket) & 0xFF)});
        grid.insert(grid.end(), file->begin() + static_cast<std::ptrdiff_t>(at),
                    file->begin() + static_cast<std::ptrdiff_t>(at + kPacket));
    }
    const auto demuxed = ts::demux(grid);
    REQUIRE(demuxed.has_value());
    CHECK(demuxed->stream.packet_size == 192);
    CHECK(demuxed->stream.truehd);
    CHECK(concatenated(*demuxed) == stream_bytes(stream));
}

TEST_CASE("the incremental writer's bytes are mux()'s for TrueHD too", "[truehd][mpegts]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    const auto batch = ts::mux(track_of(stream), stream.units, bluray());
    REQUIRE(batch.has_value());
    auto writer = ts::Writer::create(track_of(stream), bluray());
    REQUIRE(writer.has_value());
    std::vector<std::byte> streamed;
    for (const auto& unit : stream.units) {
        const auto bytes = writer->push(unit);
        REQUIRE(bytes.has_value());
        streamed.insert(streamed.end(), bytes->begin(), bytes->end());
    }
    CHECK(streamed == *batch);
}

TEST_CASE("TrueHD is Blu-ray's alone, and Blu-ray writes nothing else", "[truehd][mpegts]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    ts::MuxOptions dvb;
    dvb.profile = ts::BroadcastProfile::kDvb;
    CHECK(ts::mux(track_of(stream), stream.units, dvb).error() == ts::MuxError::kInvalidOptions);
    ts::MuxOptions atsc;
    atsc.profile = ts::BroadcastProfile::kAtsc;
    CHECK(ts::mux(track_of(stream), stream.units, atsc).error() == ts::MuxError::kInvalidOptions);

    ts::AudioTrack eac3 = track_of(stream);
    eac3.codec = ts::AudioCodec::kEac3;
    CHECK(ts::mux(eac3, stream.units, bluray()).error() == ts::MuxError::kInvalidOptions);
    CHECK(ts::Writer::create(eac3, bluray()).error() == ts::MuxError::kInvalidOptions);
    CHECK(ts::Writer::create(track_of(stream), dvb).error() == ts::MuxError::kInvalidOptions);
}

TEST_CASE("a descriptor loop that overruns the program info does not read as HDMV", "[truehd][mpegts]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    auto file = ts::mux(track_of(stream), stream.units, bluray());
    REQUIRE(file.has_value());
    // Every PMT: raise the registration descriptor's length past the program info. The reader must stop at the
    // end of the program info, not read on out of it, and so must not take the descriptor for HDMV.
    for (std::size_t at = 0; at + kPacket <= file->size(); at += kPacket) {
        const auto pid = static_cast<std::uint16_t>(((std::to_integer<unsigned>((*file)[at + 1]) & 0x1FU) << 8U) |
                                                    std::to_integer<unsigned>((*file)[at + 2]));
        if (pid == kPmtPid) {
            (*file)[at + 5 + 12 + 1] = std::byte{0x7F};
        }
    }
    const auto demuxed = ts::demux(*file);
    if (demuxed.has_value()) {
        CHECK(demuxed->stream.signalling != ts::CodecSignalling::kHdmvStreamType);
    }
}
