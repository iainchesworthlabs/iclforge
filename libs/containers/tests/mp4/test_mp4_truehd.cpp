#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "truehd/truehd_stream.hpp"

// TrueHD in MP4: the 'mlpa' sample entry and its 'dmlp' box (libs/containers/ERRATA.md, entries C1
// to C6). The layout is FFmpeg's, a single oracle; tools/checks/verify_truehd_containers.py has
// FFmpeg read and write these files. What these cases hold is the part the library can state by
// itself: the bytes of the box against what FFmpeg wrote, a round trip through the writer and the
// reader, and the ways the reader meets a file that differs.

namespace {

namespace mp4 = iclforge::containers::mp4;
using iclforge::containers::testing::load_truehd;
using iclforge::containers::testing::TruehdStream;

std::string fixture(const std::string& name) {
    return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name;
}

TruehdStream load(const std::string& name) {
    auto stream = load_truehd(fixture(name));
    REQUIRE(stream.has_value());
    return std::move(*stream);
}

mp4::AudioTrack track_of(const TruehdStream& stream) {
    return mp4::AudioTrack{
        .codec_id = std::string{mp4::kCodecTruehd},
        .sample_rate = stream.info.sample_rate,
        .channels = stream.channels,
        .samples_per_frame = stream.samples_per_unit,
        .codec_config =
            mp4::build_dmlp_payload({.format_info = stream.info.major_sync.format_info,
                                     .peak_data_rate = stream.info.major_sync.peak_data_rate}),
    };
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Where a four-character code first occurs, or npos.
std::size_t find_code(const std::vector<std::byte>& file, std::string_view code,
                      std::size_t from = 0) {
    for (std::size_t i = from; i + code.size() <= file.size(); ++i) {
        if (std::equal(code.begin(), code.end(), file.begin() + static_cast<std::ptrdiff_t>(i),
                       [](char c, std::byte b) { return static_cast<std::byte>(c) == b; })) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::uint32_t be32(const std::vector<std::byte>& file, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8U) | std::to_integer<std::uint32_t>(file[at + i]);
    }
    return v;
}

}  // namespace

TEST_CASE("dmlp is the stream's format_info and peak data rate, as FFmpeg writes it",
          "[truehd][mp4]") {
    // FFmpeg 6.1.1's 'dmlp' for the stereo 48 kHz fixture: format_info 0x0050A001, peak_data_rate
    // 3,199 in the top 15 bits of the next word (0x18FE), then 32 reserved bits.
    CHECK(mp4::build_dmlp_payload({.format_info = 0x0050A001, .peak_data_rate = 0x0C7F}) ==
          bytes_of({0x00, 0x50, 0xA0, 0x01, 0x18, 0xFE, 0x00, 0x00, 0x00, 0x00}));
    // ... and for the 5.1 one: 0x0007800F with the same peak rate.
    CHECK(mp4::build_dmlp_payload({.format_info = 0x0007800F, .peak_data_rate = 0x0C7F}) ==
          bytes_of({0x00, 0x07, 0x80, 0x0F, 0x18, 0xFE, 0x00, 0x00, 0x00, 0x00}));
    // The reserved bit is not peak_data_rate's.
    CHECK(mp4::build_dmlp_payload({.format_info = 0, .peak_data_rate = 0xFFFF}) ==
          bytes_of({0x00, 0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00}));
}

TEST_CASE("an 'mlpa' file round-trips whole access units, with the track described",
          "[truehd][mp4]") {
    for (const char* name :
         {"truehd_st48_s24.thd", "truehd_m51_s24.thd", "truehd_st96_s24.thd",
          "truehd_st192_s24.thd", "truehd_st44_s24.thd", "truehd_mono48_s24.thd"}) {
        SECTION(name) {
            const TruehdStream stream = load(name);
            mp4::MuxOptions options;
            options.sync_samples = stream.major;
            const auto file = mp4::mux(track_of(stream), stream.units, options);
            REQUIRE(file.has_value());

            const auto demuxed = mp4::demux(*file);
            REQUIRE(demuxed.has_value());
            REQUIRE(demuxed->samples.size() == stream.units.size());
            for (std::size_t i = 0; i < stream.units.size(); ++i) {
                REQUIRE(std::equal(stream.units[i].begin(), stream.units[i].end(),
                                   demuxed->samples[i].begin(), demuxed->samples[i].end()));
            }
            const mp4::ReadTrack& track = demuxed->track;
            CHECK(track.codec_id == "mlpa");
            CHECK(track.sample_rate == stream.info.sample_rate);
            CHECK(track.channels == stream.channels);
            CHECK(track.timescale == stream.info.sample_rate);
            CHECK(track.codec_config.mlp);
            CHECK(track.codec_config.mlp_format_info == stream.info.major_sync.format_info);
            CHECK(track.codec_config.mlp_peak_data_rate == stream.info.major_sync.peak_data_rate);
            CHECK(track.codec_config.payload == track_of(stream).codec_config);
            // The stss lists exactly the access units that open with a major sync.
            const std::size_t stss = find_code(*file, "stss");
            REQUIRE(stss != std::string_view::npos);
            const auto expected = static_cast<std::uint32_t>(
                std::count(stream.major.begin(), stream.major.end(), true));
            CHECK(be32(*file, stss + 8) == expected);
        }
    }
}

TEST_CASE("the sample rate of an 'mlpa' entry is a plain integer, whatever the rate",
          "[truehd][mp4]") {
    // ERRATA C2: 16.16 fixed point cannot hold 96,000 or 192,000, and FFmpeg writes the integer in
    // every case.
    for (const char* name :
         {"truehd_st48_s24.thd", "truehd_st96_s24.thd", "truehd_st192_s24.thd"}) {
        const TruehdStream stream = load(name);
        const auto file = mp4::mux(track_of(stream), stream.units);
        REQUIRE(file.has_value());
        const std::size_t entry = find_code(*file, "mlpa");
        REQUIRE(entry != std::string_view::npos);
        // 4 (type) + 6 reserved + 2 data_reference_index + 8 reserved + 2 channelcount + 2
        // samplesize + 4 = 28 .
        CHECK(be32(*file, entry + 4 + 24) == stream.info.sample_rate);
    }
}

TEST_CASE("the reader takes an 'mlpa' sample rate written either way", "[truehd][mp4]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    auto file = mp4::mux(track_of(stream), stream.units);
    REQUIRE(file.has_value());
    const std::size_t entry = find_code(*file, "mlpa");
    REQUIRE(entry != std::string_view::npos);
    const std::size_t rate_at = entry + 4 + 24;

    SECTION("16.16, the way the other codecs write it") {
        (*file)[rate_at] = std::byte{0xBB};
        (*file)[rate_at + 1] = std::byte{0x80};
        (*file)[rate_at + 2] = std::byte{0};
        (*file)[rate_at + 3] = std::byte{0};
        const auto demuxed = mp4::demux(*file);
        REQUIRE(demuxed.has_value());
        CHECK(demuxed->track.sample_rate == 48000);
    }
    SECTION("dmlp's ratebits win over a sample entry that disagrees") {
        (*file)[rate_at + 2] = std::byte{0x12};
        (*file)[rate_at + 3] = std::byte{0x34};
        const auto demuxed = mp4::demux(*file);
        REQUIRE(demuxed.has_value());
        CHECK(demuxed->track.sample_rate == 48000);
    }
}

TEST_CASE("without a usable rate code in dmlp the sample entry's field decides", "[truehd][mp4]") {
    // ratebits 0xF names no rate (TH spec §1.4), so the field is all the reader has, and it has to
    // read it as the integer FFmpeg writes (a low half of 0x7700 here) or as 16.16 (a low half of
    // zero).
    const TruehdStream stream = load("truehd_st96_s24.thd");
    mp4::AudioTrack track = track_of(stream);
    track.codec_config = mp4::build_dmlp_payload({.format_info = 0xF0000000U, .peak_data_rate = 1});
    const auto file = mp4::mux(track, stream.units);
    REQUIRE(file.has_value());
    const auto plain = mp4::demux(*file);
    REQUIRE(plain.has_value());
    CHECK(plain->track.sample_rate == 96000);

    std::vector<std::byte> fixed = *file;
    const std::size_t at = find_code(fixed, "mlpa") + 4 + 24;
    fixed[at] = std::byte{0xBB};
    fixed[at + 1] = std::byte{0x80};
    fixed[at + 2] = std::byte{0};
    fixed[at + 3] = std::byte{0};
    const auto sixteen_sixteen = mp4::demux(fixed);
    REQUIRE(sixteen_sixteen.has_value());
    CHECK(sixteen_sixteen->track.sample_rate == 48000);
}

TEST_CASE("an edit drops the padding samples of the last access unit", "[truehd][mp4]") {
    const TruehdStream stream =
        load("truehd_st44_s24.thd");  // 276 units of 40 samples, 15 of them padding
    REQUIRE(stream.padding_samples == 15);
    mp4::MuxOptions options;
    options.edit = mp4::MuxOptions::Edit{
        .start_samples = 0,
        .duration_samples =
            static_cast<std::uint64_t>(stream.units.size()) * stream.samples_per_unit -
            static_cast<std::uint64_t>(stream.padding_samples)};
    const auto file = mp4::mux(track_of(stream), stream.units, options);
    REQUIRE(file.has_value());
    const auto demuxed = mp4::demux(*file);
    REQUIRE(demuxed.has_value());
    REQUIRE(demuxed->track.edits.size() == 1);
    CHECK(demuxed->track.edits[0].segment_duration == 276U * 40U - 15U);
    CHECK(demuxed->track.edits[0].media_time == 0);
}

TEST_CASE("a fragmented 'mlpa' file round-trips the same access units", "[truehd][mp4]") {
    const TruehdStream stream = load("truehd_m51_s24.thd");
    mp4::FragmentOptions options;
    options.frames_per_fragment = 48;
    options.sync_samples = stream.major;
    const auto fragmented = mp4::fragment(track_of(stream), stream.units, options);
    REQUIRE(fragmented.has_value());
    std::vector<std::byte> file = fragmented->init_segment;
    for (const auto& segment : fragmented->media_segments) {
        file.insert(file.end(), segment.bytes.begin(), segment.bytes.end());
    }
    const auto demuxed = mp4::demux(file);
    REQUIRE(demuxed.has_value());
    REQUIRE(demuxed->samples.size() == stream.units.size());
    for (std::size_t i = 0; i < stream.units.size(); ++i) {
        REQUIRE(std::equal(stream.units[i].begin(), stream.units[i].end(),
                           demuxed->samples[i].begin(), demuxed->samples[i].end()));
    }
    CHECK(demuxed->track.codec_id == "mlpa");
    CHECK(demuxed->track.codec_config.mlp_format_info == stream.info.major_sync.format_info);
}

TEST_CASE("an 'mlpa' track needs its dmlp payload", "[truehd][mp4]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    mp4::AudioTrack track = track_of(stream);
    track.codec_config.clear();
    CHECK(mp4::mux(track, stream.units).error() == mp4::MuxError::kInvalidTrack);
    CHECK(mp4::mux(track_of(stream), stream.units).has_value());
}

TEST_CASE("a damaged 'mlpa' file is refused or read short, never misread", "[truehd][mp4]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    const auto file = mp4::mux(track_of(stream), stream.units);
    REQUIRE(file.has_value());

    SECTION("a dmlp too short for its fields keeps the bytes and zeroes the fields") {
        std::vector<std::byte> bad = *file;
        const std::size_t dmlp = find_code(bad, "dmlp");
        REQUIRE(dmlp != std::string_view::npos);
        // Shrink the box to its header plus two bytes: size 10, the rest of the old box becomes
        // junk the entry walk then reads as a box of its own, so patch the entry's size to match.
        const auto set32 = [&](std::size_t at, std::uint32_t v) {
            for (std::size_t i = 0; i < 4; ++i) {
                bad[at + i] = static_cast<std::byte>((v >> (24U - 8U * i)) & 0xFFU);
            }
        };
        const std::uint32_t old_box = be32(bad, dmlp - 4);
        REQUIRE(old_box == 18);
        set32(dmlp - 4, 10);
        const std::size_t entry = find_code(bad, "mlpa") - 4;
        set32(entry, be32(bad, entry) - 8);
        // The sample tables follow the entry in the file; the size changes shift nothing the reader
        // reads from here on because the bytes are only reinterpreted, not moved. A truncated stsd
        // must not crash it.
        const auto demuxed = mp4::demux(bad);
        if (demuxed.has_value()) {
            CHECK(demuxed->track.codec_config.mlp_format_info == 0);
        }
    }
    SECTION("a file cut in the media data gives the samples that are whole, or an error") {
        const std::vector<std::byte> cut(
            file->begin(), file->begin() + static_cast<std::ptrdiff_t>(file->size() / 2));
        const auto demuxed = mp4::demux(cut);
        if (demuxed.has_value()) {
            CHECK(demuxed->samples.size() < stream.units.size());
            for (std::size_t i = 0; i < demuxed->samples.size(); ++i) {
                CHECK(std::equal(stream.units[i].begin(), stream.units[i].end(),
                                 demuxed->samples[i].begin(), demuxed->samples[i].end()));
            }
        }
    }
    SECTION("a file cut before the sample table ends is not read") {
        const std::vector<std::byte> cut(file->begin(), file->begin() + 100);
        CHECK_FALSE(mp4::demux(cut).has_value());
    }
}
