#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/matroska/reader.hpp"
#include "truehd/truehd_stream.hpp"

// TrueHD in Matroska (libs/containers/ERRATA.md, entry C7): CodecID A_TRUEHD, no CodecPrivate, one whole access
// unit to a SimpleBlock, keyframe set where the unit opens with a major sync. FFmpeg's muxer and demuxer are the
// oracle (tools/checks/verify_truehd_containers.py); these cases hold the round trip and the block flags.

namespace {

namespace mkv = iclforge::containers::matroska;
using iclforge::containers::testing::load_truehd;
using iclforge::containers::testing::TruehdStream;

TruehdStream load(const std::string& name) {
    auto stream = load_truehd(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name);
    REQUIRE(stream.has_value());
    return std::move(*stream);
}

mkv::AudioTrack track_of(const TruehdStream& stream) {
    return mkv::AudioTrack{.codec_id = std::string{mkv::kCodecTruehd},
                           .sample_rate = stream.info.sample_rate,
                           .channels = stream.channels,
                           .samples_per_frame = stream.samples_per_unit,
                           .bit_depth = 24};
}

// The flags byte of every SimpleBlock (element 0xA3) in a file, by walking for the id: the writer's blocks are
// track vint 0x81, two timestamp bytes, the flags byte. The scan is only sound on a file whose frames are known
// not to contain 0xA3 0x?? 0x81, so it checks the structure it finds against the frame sizes.
std::vector<std::uint8_t> block_flags(const std::vector<std::byte>& file, const TruehdStream& stream) {
    std::vector<std::uint8_t> flags;
    std::size_t unit = 0;
    for (std::size_t i = 0; i + 8 < file.size() && unit < stream.units.size(); ++i) {
        if (file[i] != std::byte{0xA3}) {
            continue;
        }
        // The size vint: one to four bytes for these frames; the block is size - 4 bytes of frame.
        std::size_t at = i + 1;
        std::size_t width = 1;
        std::uint8_t first = std::to_integer<std::uint8_t>(file[at]);
        std::uint8_t mask = 0x80;
        while (width < 8 && (first & mask) == 0) {
            ++width;
            mask >>= 1U;
        }
        std::uint64_t size = first & (mask - 1U);
        for (std::size_t k = 1; k < width; ++k) {
            size = (size << 8U) | std::to_integer<std::uint8_t>(file[at + k]);
        }
        at += width;
        if (size != stream.units[unit].size() + 4 || file[at] != std::byte{0x81}) {
            continue;
        }
        flags.push_back(std::to_integer<std::uint8_t>(file[at + 3]));
        i = at + static_cast<std::size_t>(size) - 1;
        ++unit;
    }
    return flags;
}

}  // namespace

TEST_CASE("an A_TRUEHD file round-trips whole access units", "[truehd][matroska]") {
    for (const char* name : {"truehd_st48_s24.thd", "truehd_m51_s24.thd", "truehd_st96_s24.thd",
                             "truehd_st192_s24.thd", "truehd_st44_s24.thd", "truehd_mono48_s24.thd"}) {
        SECTION(name) {
            const TruehdStream stream = load(name);
            mkv::MuxOptions options;
            options.key_frames = stream.major;
            const auto file = mkv::mux(track_of(stream), stream.units, options);
            REQUIRE(file.has_value());

            const auto demuxed = mkv::demux(*file);
            REQUIRE(demuxed.has_value());
            CHECK(demuxed->track.codec_id == "A_TRUEHD");
            CHECK(demuxed->track.sample_rate == stream.info.sample_rate);
            CHECK(demuxed->track.channels == stream.channels);
            REQUIRE(demuxed->frames.size() == stream.units.size());
            for (std::size_t i = 0; i < stream.units.size(); ++i) {
                REQUIRE(std::equal(stream.units[i].begin(), stream.units[i].end(), demuxed->frames[i].begin(),
                                   demuxed->frames[i].end()));
            }
        }
    }
}

TEST_CASE("only the access units with a major sync are keyframes", "[truehd][matroska]") {
    const TruehdStream stream = load("truehd_m51_s24.thd");
    REQUIRE(std::count(stream.major.begin(), stream.major.end(), true) < static_cast<std::ptrdiff_t>(stream.units.size()));

    mkv::MuxOptions options;
    options.key_frames = stream.major;
    const auto file = mkv::mux(track_of(stream), stream.units, options);
    REQUIRE(file.has_value());
    const std::vector<std::uint8_t> flags = block_flags(*file, stream);
    REQUIRE(flags.size() == stream.units.size());
    for (std::size_t i = 0; i < flags.size(); ++i) {
        INFO("unit " << i);
        CHECK(flags[i] == (stream.major[i] ? 0x80 : 0x00));
    }

    // The default is what the other codecs get: every frame a keyframe.
    const auto plain = mkv::mux(track_of(stream), stream.units);
    REQUIRE(plain.has_value());
    const std::vector<std::uint8_t> all = block_flags(*plain, stream);
    REQUIRE(all.size() == stream.units.size());
    CHECK(std::all_of(all.begin(), all.end(), [](std::uint8_t f) { return f == 0x80; }));
}

TEST_CASE("the incremental writer's flag per frame is the batch writer's", "[truehd][matroska]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    mkv::MuxOptions options;
    options.key_frames = stream.major;
    const auto batch = mkv::mux(track_of(stream), stream.units, options);
    REQUIRE(batch.has_value());

    auto writer = mkv::Writer::create(track_of(stream));
    REQUIRE(writer.has_value());
    std::vector<std::byte> streamed;
    for (std::size_t i = 0; i < stream.units.size(); ++i) {
        const auto closed = writer->push(stream.units[i], stream.major[i]);
        REQUIRE(closed.has_value());
        streamed.insert(streamed.end(), closed->begin(), closed->end());
    }
    const std::vector<std::byte> tail = writer->finalize();
    streamed.insert(streamed.end(), tail.begin(), tail.end());

    const std::vector<std::uint8_t> flags = block_flags(streamed, stream);
    REQUIRE(flags.size() == stream.units.size());
    for (std::size_t i = 0; i < flags.size(); ++i) {
        CHECK(flags[i] == (stream.major[i] ? 0x80 : 0x00));
    }
}

TEST_CASE("key_frames must be one flag a frame", "[truehd][matroska]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    mkv::MuxOptions options;
    options.key_frames = {true, false};
    CHECK(mkv::mux(track_of(stream), stream.units, options).error() == mkv::MuxError::kInvalidOptions);
}

TEST_CASE("the TrueHD bit depth is written when asked for and left out otherwise", "[truehd][matroska]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    const auto with = mkv::mux(track_of(stream), stream.units);
    mkv::AudioTrack bare = track_of(stream);
    bare.bit_depth = 0;
    const auto without = mkv::mux(bare, stream.units);
    REQUIRE(with.has_value());
    REQUIRE(without.has_value());
    // BitDepth is EBML id 0x6264, one byte of size and the value, in the Audio element.
    const auto has_depth = [](const std::vector<std::byte>& file) {
        for (std::size_t i = 0; i + 4 < file.size(); ++i) {
            if (file[i] == std::byte{0x62} && file[i + 1] == std::byte{0x64} && file[i + 2] == std::byte{0x81} &&
                file[i + 3] == std::byte{24}) {
                return true;
            }
        }
        return false;
    };
    CHECK(has_depth(*with));
    CHECK_FALSE(has_depth(*without));
    CHECK(with->size() == without->size() + 4);
}
