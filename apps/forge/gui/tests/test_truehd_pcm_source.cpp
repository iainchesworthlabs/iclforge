#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <system_error>
#include <string>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "container_input.hpp"
#include "truehd_pcm_source.hpp"

// The Forge window's TrueHD and MLP player reads the file a window at a time
// (apps/forge/gui/src/truehd_pcm_source.hpp). What is held here is that the windows add up to the
// decode FFmpeg's strict decode gives (the SHA-256 of testdata/truehd/manifest.json, which the
// library's own tests hold forge's decoder to), that a seek lands on the sample asked for, and that
// the WAV the window exports is the 24-bit WAV `forge decode` writes.
//
// ICLFORGE_GUI_FIXTURE_DIR comes from apps/forge/gui/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_GUI_FIXTURE_DIR} / file;
}

struct Expected {
    const char* file;
    int channels;
    std::uint64_t frames;
    const char* sha256;
    bool mlp;
};

constexpr std::array<Expected, 4> kFixtures{{
    {"truehd_m51_s24.thd", 6, 12000, "b6b891b6eaf135e54e950c59a48fcffeb385428e4d722044b474124c034a4cbd", false},
    {"truehd_st48_s24.thd", 2, 12000, "a2ff2c430c67c0e3ff5205f8d7d0da00bb0810dc9bee2fd444b4fea86fc6b8a0", false},
    {"mlp_st48_s24.mlp", 2, 12000, "92de6f6dcd7bcd7278a5d129c148baaaec8b92885816b90e3e1348142982bb02", true},
    {"truehd_st48_s24_gap128.thd", 2, 6720, "7989d9ff3dbce42ad0e6ccbcb15d6188ad5ff245dbbe0ff7fff1d5efe59cb6ae", false},
}};

std::string hex(const std::array<std::byte, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

// The samples as FFmpeg's `-f s32le` gives them: the 24-bit value shifted left by 8, channels
// interleaved.
void append_s32le(std::vector<std::byte>& out, const std::vector<std::vector<float>>& planar,
                  std::size_t frames) {
    for (std::size_t s = 0; s < frames; ++s) {
        for (const auto& plane : planar) {
            const auto value = static_cast<std::int32_t>(std::lround(static_cast<double>(plane[s]) * 8388608.0));
            const auto bits = static_cast<std::uint32_t>(value) << 8U;
            for (unsigned i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>((bits >> (8U * i)) & 0xFFU));
            }
        }
    }
}

std::vector<std::byte> decode_all(forge_gui::TruehdPcmSource& source, std::size_t window,
                                  std::vector<std::vector<float>>* keep = nullptr) {
    std::vector<std::byte> out;
    std::vector<std::vector<float>> planar;
    std::uint64_t at = 0;
    while (true) {
        const std::size_t got = source.read(at, window, planar);
        if (got == 0) {
            break;
        }
        append_s32le(out, planar, got);
        if (keep != nullptr) {
            keep->resize(planar.size());
            for (std::size_t c = 0; c < planar.size(); ++c) {
                keep->at(c).insert(keep->at(c).end(), planar[c].begin(), planar[c].begin() + static_cast<std::ptrdiff_t>(got));
            }
        }
        at += got;
    }
    return out;
}

}  // namespace

TEST_CASE("the TrueHD player's windows add up to FFmpeg's decode", "[gui][truehd]") {
    for (const Expected& item : kFixtures) {
        DYNAMIC_SECTION(item.file) {
            auto source = forge_gui::TruehdPcmSource::open(fixture(item.file));
            REQUIRE(source.has_value());
            const forge_gui::TruehdPcmInfo& info = (*source)->info();
            CHECK(info.channels == item.channels);
            CHECK(info.mlp == item.mlp);
            CHECK(info.sample_rate == 48000);
            CHECK(info.frames == item.frames);
            CHECK(info.units * 40 == item.frames);
            CHECK(info.restarts >= 1);
            // A window that is not a whole number of units, and one that is several.
            for (const std::size_t window : {std::size_t{1000}, std::size_t{4096}, std::size_t{7}}) {
                const std::vector<std::byte> pcm = decode_all(**source, window);
                CHECK(pcm.size() == item.frames * static_cast<std::size_t>(item.channels) * 4);
                CHECK(hex(iclforge::base::crypto::sha256(pcm)) == item.sha256);
            }
            CHECK((*source)->decode_errors() == 0);
        }
    }
}

TEST_CASE("the padding of the last unit is not part of the stream the player delivers", "[gui][truehd]") {
    // 44.1 kHz: 276 units of 40 samples is 11,040 samples, and the stream codes 11,025: the
    // end-of-stream marker counts the last 15 as padding (TH spec section 16.5). The player's length
    // and its last window stop where the stream does, and the samples are FFmpeg's decode
    // (pcm_sha256 of truehd_st44_s24 in testdata/truehd/manifest.json).
    auto source = forge_gui::TruehdPcmSource::open(fixture("truehd_st44_s24.thd"));
    REQUIRE(source.has_value());
    const forge_gui::TruehdPcmInfo& info = (*source)->info();
    CHECK(info.units == 276);
    CHECK(info.frames == 11025);
    CHECK(info.sample_rate == 44100);
    for (const std::size_t window : {std::size_t{1000}, std::size_t{4096}}) {
        const std::vector<std::byte> pcm = decode_all(**source, window);
        CHECK(pcm.size() == 11025 * 2 * 4);
        CHECK(hex(iclforge::base::crypto::sha256(pcm)) ==
              "9b494e652bc2f58753d92d3e5acf544c1fa87913483af46425802e78588f4db9");
    }
    std::vector<std::vector<float>> planar;
    CHECK((*source)->read(11025, 100, planar) == 0);
    CHECK((*source)->read(11020, 100, planar) == 5);
}

TEST_CASE("the TrueHD player names the speakers and fills its rate model", "[gui][truehd]") {
    auto source = forge_gui::TruehdPcmSource::open(fixture("truehd_m51_s24.thd"));
    REQUIRE(source.has_value());
    const auto& info = (*source)->info();
    REQUIRE(info.layout.size() == 6);
    CHECK(forge_gui::truehd_wav_channel_mask(info.layout, info.channels) == 0x60F);
    CHECK(info.rate.declared_bps > 0);
    CHECK(info.rate.max_unit_rate_bps > 0);
    CHECK(info.rate.violations == 0);
}

TEST_CASE("a seek in a TrueHD stream lands on the sample asked for", "[gui][truehd]") {
    auto source = forge_gui::TruehdPcmSource::open(fixture("truehd_m51_s24.thd"));
    REQUIRE(source.has_value());
    std::vector<std::vector<float>> whole;
    const std::vector<std::byte> all = decode_all(**source, 1024, &whole);
    REQUIRE(whole.size() == 6);
    REQUIRE(whole[0].size() == 12000);

    // Forward past several restarts, back to near the start, and a sample in the middle of a unit.
    for (const std::uint64_t at : {std::uint64_t{7001}, std::uint64_t{17}, std::uint64_t{11990},
                                   std::uint64_t{3000}, std::uint64_t{0}}) {
        DYNAMIC_SECTION("at " << at) {
            std::vector<std::vector<float>> planar;
            const std::size_t got = (*source)->read(at, 500, planar);
            const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(500, 12000 - at));
            REQUIRE(got == wanted);
            for (std::size_t c = 0; c < 6; ++c) {
                REQUIRE(planar[c].size() == wanted);
                CHECK(std::equal(planar[c].begin(), planar[c].end(), whole[c].begin() + static_cast<std::ptrdiff_t>(at)));
            }
        }
    }
    SECTION("at and past the end nothing is delivered") {
        std::vector<std::vector<float>> planar;
        CHECK((*source)->read(12000, 100, planar) == 0);
        CHECK((*source)->read(99999, 100, planar) == 0);
        CHECK((*source)->read(0, 100, planar) == 100);  // and it can still be read from the start
    }
}

TEST_CASE("a seek past the last restart of a stream with few of them", "[gui][truehd]") {
    // 168 units with a major sync at unit 0 and unit 128: a seek into the last 40 units decodes from
    // unit 128, and one into the first 128 from unit 0.
    auto source = forge_gui::TruehdPcmSource::open(fixture("truehd_st48_s24_gap128.thd"));
    REQUIRE(source.has_value());
    CHECK((*source)->info().restarts == 2);
    std::vector<std::vector<float>> whole;
    (void)decode_all(**source, 2048, &whole);
    std::vector<std::vector<float>> planar;
    REQUIRE((*source)->read(6000, 700, planar) == 700);
    CHECK(std::equal(planar[1].begin(), planar[1].end(), whole[1].begin() + 6000));
}

TEST_CASE("a file that is not a TrueHD stream is refused with a sentence", "[gui][truehd]") {
    const auto source = forge_gui::TruehdPcmSource::open(fixture("two-programmes.ec3"));
    REQUIRE_FALSE(source.has_value());
    CHECK(source.error().find("not a TrueHD or MLP stream") != std::string::npos);
    const auto missing = forge_gui::TruehdPcmSource::open(fixture("no-such-file.thd"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("Could not open") != std::string::npos);
}

TEST_CASE("the exported WAV is the 24-bit WAV forge decode writes", "[gui][truehd]") {
    auto source = forge_gui::TruehdPcmSource::open(fixture("truehd_m51_s24.thd"));
    REQUIRE(source.has_value());
    const fs::path wav = fs::temp_directory_path() / ("forge-gui-truehd-export-" + std::to_string(std::hash<std::string>{}(fixture("x").string())) + ".wav");
    REQUIRE(forge_gui::write_truehd_wav24(**source, wav).empty());
    std::ifstream in{wav, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    in.close();
    fs::remove(wav);
    REQUIRE(raw.size() == 68 + 12000 * 6 * 3);
    const auto le = [&raw](std::size_t at, int width) {
        std::uint32_t value = 0;
        for (int i = width - 1; i >= 0; --i) {
            value = (value << 8U) | static_cast<unsigned char>(raw[at + static_cast<std::size_t>(i)]);
        }
        return value;
    };
    CHECK(std::string(raw.begin(), raw.begin() + 4) == "RIFF");
    CHECK(le(4, 4) == raw.size() - 8);
    CHECK(le(20, 2) == 0xFFFE);
    CHECK(le(22, 2) == 6);
    CHECK(le(24, 4) == 48000);
    CHECK(le(34, 2) == 24);
    CHECK(le(40, 4) == 0x60F);
    CHECK(le(64, 4) == 12000 * 6 * 3);
    // The samples, shifted to 32 bits, are FFmpeg's decode.
    std::vector<std::byte> s32le;
    for (std::size_t at = 68; at + 3 <= raw.size(); at += 3) {
        s32le.push_back(std::byte{0});
        for (std::size_t i = 0; i < 3; ++i) {
            s32le.push_back(static_cast<std::byte>(raw[at + i]));
        }
    }
    CHECK(hex(iclforge::base::crypto::sha256(s32le)) == kFixtures[0].sha256);
}

// --- a TrueHD track in a container ---------------------------------------------------------------
//
// The player opens a Matroska, MP4 or Blu-ray transport stream whose first track is TrueHD, and
// delivers the samples the bare stream does: the files are made here from the fixture's own access
// units by the project's muxers, and the SHA-256 is the one FFmpeg's decode of the bare stream gives.

namespace {

namespace th = iclforge::truehd;
namespace mkv = iclforge::containers::matroska;
namespace mp4 = iclforge::containers::mp4;
namespace ts = iclforge::containers::mpegts;

enum class Wrap { kMatroska, kMp4, kBluRayTs };

std::vector<std::byte> read_all(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return out;
}

void write_all(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(!out.fail());
}

// The fixture's access units muxed into a container, written to a temporary file that the caller
// removes. `edit` is an MP4's edit list: the priming skipped and the samples played.
fs::path write_wrapped(const std::string& fixture_file, Wrap wrap, const std::string& name,
                       std::optional<mp4::MuxOptions::Edit> edit = std::nullopt) {
    const std::vector<std::byte> stream = read_all(fixture(fixture_file));
    const auto info = th::probe_stream(stream);
    REQUIRE(info.has_value());
    std::vector<std::span<const std::byte>> units;
    std::vector<bool> major;
    th::ElementaryReader reader(stream);
    th::Decoder decoder(th::DecoderConfig{.strictness = th::Strictness::kLenient});
    int channels = 0;
    while (const auto unit = reader.next()) {
        units.push_back(*unit);
        major.push_back(th::starts_with_major_sync(unit->subspan(4)));
        const auto decoded = decoder.decode(*unit);
        if (decoded && *decoded) {
            channels = (*decoded)->channels;
        }
    }
    REQUIRE(!units.empty());
    REQUIRE(channels > 0);
    const auto per_unit = static_cast<std::uint32_t>(info->samples_per_access_unit);
    std::vector<std::byte> file;
    if (wrap == Wrap::kMatroska) {
        mkv::MuxOptions options;
        options.key_frames = major;
        auto made = mkv::mux(mkv::AudioTrack{.codec_id = std::string{mkv::kCodecTruehd},
                                             .sample_rate = info->sample_rate,
                                             .channels = channels,
                                             .samples_per_frame = per_unit},
                             units, options);
        REQUIRE(made.has_value());
        file = std::move(*made);
    } else if (wrap == Wrap::kMp4) {
        mp4::MuxOptions options;
        options.sync_samples = major;
        options.edit = edit;
        auto made = mp4::mux(
            mp4::AudioTrack{
                .codec_id = std::string{mp4::kCodecTruehd},
                .sample_rate = info->sample_rate,
                .channels = channels,
                .samples_per_frame = per_unit,
                .codec_config = mp4::build_dmlp_payload({.format_info = info->major_sync.format_info,
                                                         .peak_data_rate = info->major_sync.peak_data_rate}),
            },
            units, options);
        REQUIRE(made.has_value());
        file = std::move(*made);
    } else {
        auto made = ts::mux(ts::AudioTrack{.codec = ts::AudioCodec::kTruehd,
                                           .sample_rate = info->sample_rate,
                                           .channels = channels,
                                           .samples_per_frame = per_unit},
                            units, ts::MuxOptions{.profile = ts::BroadcastProfile::kBluray});
        REQUIRE(made.has_value());
        file = std::move(*made);
    }
    const fs::path path = fs::temp_directory_path() / ("forge-gui-truehd-wrapped-" + name);
    write_all(path, file);
    return path;
}

}  // namespace

TEST_CASE("a TrueHD track in Matroska, MP4 or a Blu-ray transport stream plays as the bare stream does",
          "[gui][truehd][container]") {
    struct Case {
        Wrap wrap;
        const char* name;
        iclforge::apps::ContainerKind kind;
    };
    const std::array<Case, 3> cases{{{Wrap::kMatroska, "m51.mkv", iclforge::apps::ContainerKind::kMatroska},
                                     {Wrap::kMp4, "m51.mp4", iclforge::apps::ContainerKind::kMp4},
                                     {Wrap::kBluRayTs, "m51.m2ts", iclforge::apps::ContainerKind::kMpegTs}}};
    for (const Case& item : cases) {
        DYNAMIC_SECTION(item.name) {
            const fs::path path = write_wrapped("truehd_m51_s24.thd", item.wrap, item.name);
            bool no_truehd_track = true;
            auto source = forge_gui::TruehdPcmSource::open(path, th::Presentation::kBest, &no_truehd_track);
            REQUIRE(source.has_value());
            CHECK_FALSE(no_truehd_track);
            const forge_gui::TruehdPcmInfo& info = (*source)->info();
            CHECK(info.container == item.kind);
            CHECK(info.channels == 6);
            CHECK(info.sample_rate == 48000);
            CHECK(info.frames == 12000);
            CHECK(info.skipped_frames == 0);
            REQUIRE(info.layout.size() == 6);
            CHECK(info.layout.front() == th::Channel::kL);
            CHECK(info.layout[3] == th::Channel::kLfe);
            for (const std::size_t window : {std::size_t{4096}, std::size_t{999}}) {
                const std::vector<std::byte> pcm = decode_all(**source, window);
                CHECK(pcm.size() == std::size_t{12000} * 6 * 4);
                CHECK(hex(iclforge::base::crypto::sha256(pcm)) == kFixtures[0].sha256);
            }
            // A seek lands on the sample asked for, as it does in the bare stream.
            auto bare = forge_gui::TruehdPcmSource::open(fixture("truehd_m51_s24.thd"));
            REQUIRE(bare.has_value());
            std::vector<std::vector<float>> from_container;
            std::vector<std::vector<float>> from_bare;
            REQUIRE((*source)->read(7321, 500, from_container) == 500);
            REQUIRE((*bare)->read(7321, 500, from_bare) == 500);
            CHECK(from_container == from_bare);
            CHECK((*source)->decode_errors() == 0);
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    }
}

TEST_CASE("an MP4 edit list's priming is not delivered and its end is cut", "[gui][truehd][container]") {
    // 100 samples in and 5,000 played: the player starts 100 samples into the stream and ends at
    // 5,100, and what it delivers is those samples of the bare stream.
    const fs::path path = write_wrapped("truehd_m51_s24.thd", Wrap::kMp4, "trimmed.mp4",
                                        mp4::MuxOptions::Edit{.start_samples = 100, .duration_samples = 5000});
    auto source = forge_gui::TruehdPcmSource::open(path);
    REQUIRE(source.has_value());
    CHECK((*source)->info().frames == 5000);
    CHECK((*source)->info().skipped_frames == 100);
    auto bare = forge_gui::TruehdPcmSource::open(fixture("truehd_m51_s24.thd"));
    REQUIRE(bare.has_value());
    std::vector<std::vector<float>> trimmed;
    std::vector<std::vector<float>> whole;
    REQUIRE((*source)->read(0, 5000, trimmed) == 5000);
    REQUIRE((*bare)->read(100, 5000, whole) == 5000);
    CHECK(trimmed == whole);
    // Past the end of the edit nothing is delivered, though the stream goes on.
    CHECK((*source)->read(5000, 100, trimmed) == 0);
    // A seek into the trimmed stream counts from its start.
    REQUIRE((*source)->read(2500, 300, trimmed) == 300);
    REQUIRE((*bare)->read(2600, 300, whole) == 300);
    CHECK(trimmed == whole);
    std::error_code ignored;
    fs::remove(path, ignored);
}

TEST_CASE("a container with no TrueHD track is left to the readers of the other codecs",
          "[gui][truehd][container]") {
    const std::vector<std::byte> ec3 = read_all(fixture("two-programmes.ec3"));
    const auto scanned = iclforge::ac3::io::scan(ec3);
    REQUIRE(scanned.has_value());
    REQUIRE_FALSE(scanned->access_units.empty());
    const auto file = mkv::mux(mkv::AudioTrack{.codec_id = std::string{mkv::kCodecEac3}, .channels = 6},
                               scanned->access_units, {});
    REQUIRE(file.has_value());
    const fs::path path = fs::temp_directory_path() / "forge-gui-truehd-wrapped-eac3.mkv";
    write_all(path, *file);
    bool no_truehd_track = false;
    const auto source = forge_gui::TruehdPcmSource::open(path, th::Presentation::kBest, &no_truehd_track);
    REQUIRE_FALSE(source.has_value());
    CHECK(no_truehd_track);
    std::error_code ignored;
    fs::remove(path, ignored);
}
