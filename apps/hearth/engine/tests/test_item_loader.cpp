#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/dec3.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "container_input.hpp"
#include "item_loader.hpp"
#include "multi_track_files.hpp"

// make_file_item_loader() and list_folder_items() (apps/hearth/ui/src/item_loader.cpp)
// against files on disk: the elementary streams it always read, and the MP4, Matroska and
// MPEG-TS files it now demuxes through apps/shared/media/src/container_input.hpp. The streams
// are encoded here and wrapped by the project's own muxers, so nothing depends on a fixture.

namespace {

namespace fs = std::filesystem;

using iclforge::apps::ContainerKind;
using iclforge::hearth::LoadedItem;

constexpr int kFrames = 12;
constexpr std::uint32_t kFrameSamples = 1536;

std::vector<float> tone(double hz, std::uint32_t rate, std::size_t offset) {
    std::vector<float> out(iclforge::ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(
            0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n + offset) / rate));
    }
    return out;
}

std::vector<std::byte> ac3_stream(int frames) {
    iclforge::ac3::EncoderConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 384;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    iclforge::ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(660.0, 48000, static_cast<std::size_t>(f) * kFrameSamples);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

std::vector<std::byte> eac3_stream(int frames) {
    iclforge::ac3::eac3::FrameConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 384;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    iclforge::ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(440.0, 48000, static_cast<std::size_t>(f) * kFrameSamples);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

std::vector<std::byte> in_mp4(
    const std::vector<std::byte>& stream,
    std::optional<iclforge::containers::mp4::MuxOptions::Edit> edit = std::nullopt) {
    const auto scanned = iclforge::ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    iclforge::containers::mp4::AudioTrack track;
    track.codec_id = std::string{scanned->kind == iclforge::ac3::io::StreamKind::kAc3
                                     ? iclforge::containers::mp4::kCodecAc3
                                     : iclforge::containers::mp4::kCodecEac3};
    track.sample_rate = iclforge::ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    track.codec_config = iclforge::ac3::io::build_codec_config_box(*scanned);
    iclforge::containers::mp4::MuxOptions options;
    options.edit = edit;
    const auto muxed = iclforge::containers::mp4::mux(
        track, std::span<const std::span<const std::byte>>(scanned->access_units), options);
    REQUIRE(muxed.has_value());
    return *muxed;
}

std::vector<std::byte> in_mkv(const std::vector<std::byte>& stream) {
    const auto scanned = iclforge::ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    iclforge::containers::matroska::AudioTrack track;
    track.codec_id = std::string{scanned->kind == iclforge::ac3::io::StreamKind::kAc3
                                     ? iclforge::containers::matroska::kCodecAc3
                                     : iclforge::containers::matroska::kCodecEac3};
    track.sample_rate = iclforge::ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    const auto muxed = iclforge::containers::matroska::mux(
        track, std::span<const std::span<const std::byte>>(scanned->access_units));
    REQUIRE(muxed.has_value());
    return *muxed;
}

std::vector<std::byte> in_ts(const std::vector<std::byte>& stream) {
    const auto scanned = iclforge::ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    iclforge::containers::mpegts::AudioTrack track;
    track.codec = scanned->kind == iclforge::ac3::io::StreamKind::kAc3
                      ? iclforge::containers::mpegts::AudioCodec::kAc3
                      : iclforge::containers::mpegts::AudioCodec::kEac3;
    track.sample_rate = iclforge::ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    const auto muxed = iclforge::containers::mpegts::mux(
        track, std::span<const std::span<const std::byte>>(scanned->access_units));
    REQUIRE(muxed.has_value());
    return *muxed;
}

// A folder of this process's own under the test scratch directory, emptied
// on the way in and on the way out.
class Folder {
public:
    Folder()
        : path_(fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("hearth_item_loader_" + iclforge::test::platform::process_id())) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    // Writes `bytes` to `name` (which may name a subfolder) and returns its path.
    std::string write(const std::string& name, const std::vector<std::byte>& bytes) const {
        const fs::path file = path_ / name;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
        return file.generic_string();
    }

    [[nodiscard]] std::string root() const { return path_.generic_string(); }

private:
    fs::path path_;
};

std::expected<LoadedItem, std::string> load(const std::string& path) {
    return iclforge::hearth::ui::make_file_item_loader()(path);
}

namespace mt = iclforge::test::multi_track;

// The stream an item holds, whether it holds it in memory or reads it from a source.
std::vector<std::byte> stream_of(const LoadedItem& item) {
    if (!item.source) {
        return item.bytes;
    }
    std::vector<std::byte> out(static_cast<std::size_t>(item.source->size()));
    out.resize(item.source->read(0, out));
    return out;
}

// A TrueHD file FFmpeg made (testdata/truehd), read whole.
std::vector<std::byte> truehd_fixture(const std::string& name) {
    std::ifstream in(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

// The three containers a Blu-ray remux comes in, each built with two or more audio tracks.
struct Remux {
    const char* file_name;
    std::vector<std::byte> (*build)(std::span<const mt::Track>);
    ContainerKind kind;
};

constexpr Remux kRemuxes[] = {
    {"remux.mkv", &mt::matroska_file, ContainerKind::kMatroska},
    {"remux.mp4", &mt::mp4_file, ContainerKind::kMp4},
    {"remux.ts", &mt::mpegts_file, ContainerKind::kMpegTs},
};


}  // namespace

TEST_CASE("item loader: a raw stream is read byte for byte", "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = ac3_stream(kFrames);
    const auto loaded = load(folder.write("raw.ac3", stream));
    REQUIRE(loaded.has_value());
    CHECK(loaded->bytes == stream);
    CHECK(loaded->skip_samples == 0);
    CHECK_FALSE(loaded->play_samples.has_value());
    CHECK(loaded->note.empty());
    CHECK(loaded->container.kind == ContainerKind::kUnknown);
}

TEST_CASE("item loader: an MP4 holding AC-3 is read as its elementary stream",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = ac3_stream(kFrames);
    const auto loaded = load(folder.write("movie.mp4", in_mp4(stream)));
    REQUIRE(loaded.has_value());
    CHECK(loaded->bytes == stream);
    CHECK(loaded->container.kind == ContainerKind::kMp4);
    CHECK(loaded->container.codec_id == iclforge::containers::mp4::kCodecAc3);
    CHECK(loaded->container.sample_rate == 48000);
    CHECK(loaded->container.samples == static_cast<std::uint64_t>(kFrames));
}

TEST_CASE("item loader: an MP4 holding E-AC-3 is read as its elementary stream",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = eac3_stream(kFrames);
    const auto loaded = load(folder.write("movie.mp4", in_mp4(stream)));
    REQUIRE(loaded.has_value());
    CHECK(loaded->bytes == stream);
    CHECK(loaded->container.kind == ContainerKind::kMp4);
    CHECK(loaded->container.codec_id == iclforge::containers::mp4::kCodecEac3);
}

TEST_CASE("item loader: a Matroska file holding AC-3 is read as its elementary stream",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = ac3_stream(kFrames);
    const auto loaded = load(folder.write("show.mkv", in_mkv(stream)));
    REQUIRE(loaded.has_value());
    CHECK(loaded->bytes == stream);
    CHECK(loaded->container.kind == ContainerKind::kMatroska);
    CHECK(loaded->container.codec_id == iclforge::containers::matroska::kCodecAc3);
}

TEST_CASE("item loader: a Transport Stream holding AC-3 is read as its elementary stream",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = ac3_stream(kFrames);
    const auto loaded = load(folder.write("capture.ts", in_ts(stream)));
    REQUIRE(loaded.has_value());
    CHECK(loaded->bytes == stream);
    CHECK(loaded->container.kind == ContainerKind::kMpegTs);
}

TEST_CASE("item loader: an MP4 edit list's priming and padding reach the item",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::uint64_t kept = (kFrames * kFrameSamples) - 256 - 1000;
    const auto loaded = load(folder.write(
        "edited.mp4",
        in_mp4(eac3_stream(kFrames), iclforge::containers::mp4::MuxOptions::Edit{
                                         .start_samples = 256, .duration_samples = kept})));
    REQUIRE(loaded.has_value());
    CHECK(loaded->skip_samples == 256);
    REQUIRE(loaded->play_samples.has_value());
    CHECK(*loaded->play_samples == kept);
    CHECK(loaded->note.empty());
    CHECK(loaded->container.edits == 1);
}

TEST_CASE("item loader: a container is told by its bytes, not its name", "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> stream = ac3_stream(kFrames);

    // An MP4 named like a raw stream is still demuxed ...
    const auto disguised = load(folder.write("disguised.ac3", in_mp4(stream)));
    REQUIRE(disguised.has_value());
    CHECK(disguised->bytes == stream);
    CHECK(disguised->container.kind == ContainerKind::kMp4);

    // ... and a raw stream named like a container is not.
    const auto plain = load(folder.write("plain.mkv", stream));
    REQUIRE(plain.has_value());
    CHECK(plain->bytes == stream);
    CHECK(plain->container.kind == ContainerKind::kUnknown);
}

TEST_CASE("item loader: a file that is not a media file is refused by its name",
          "[hearth][item-loader]") {
    const Folder folder;
    const auto loaded = load(folder.write("notes.txt", ac3_stream(2)));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find(".mp4") != std::string::npos);
}

TEST_CASE("item loader: a file that is not there says so", "[hearth][item-loader]") {
    const Folder folder;
    const auto loaded = load(folder.root() + "/missing.ac3");
    REQUIRE_FALSE(loaded.has_value());
    CHECK_FALSE(loaded.error().empty());
}

TEST_CASE("item loader: a container that cannot be demuxed gives its reason",
          "[hearth][item-loader]") {
    const Folder folder;
    std::vector<std::byte> truncated = in_mp4(ac3_stream(kFrames));
    truncated.resize(32);
    REQUIRE(iclforge::apps::sniff_container(truncated) == ContainerKind::kMp4);
    const auto loaded = load(folder.write("cut.mp4", truncated));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("MP4") != std::string::npos);
}

TEST_CASE("item loader: a folder lists every file it opens, in any case and in order",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> bytes = ac3_stream(1);
    folder.write("a.AC3", bytes);
    folder.write("b.ec3", bytes);
    folder.write("c.ac4", bytes);
    folder.write("sub/d.mp4", bytes);
    folder.write("sub/e.MKV", bytes);
    folder.write("f.ts", bytes);
    folder.write("g.wav", bytes);
    folder.write("h.txt", bytes);
    folder.write("i.THD", bytes);
    folder.write("sub/j.mlp", bytes);

    const auto listed = iclforge::hearth::ui::list_folder_items(folder.root());
    REQUIRE(listed.size() == 8);
    CHECK(std::is_sorted(listed.begin(), listed.end()));
    for (const std::string& path : listed) {
        std::string extension = fs::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        CHECK((extension == ".ac3" || extension == ".ec3" || extension == ".ac4" ||
               extension == ".thd" || extension == ".mlp" || extension == ".mp4" ||
               extension == ".mkv" || extension == ".ts"));
    }
    CHECK(iclforge::hearth::ui::list_folder_items(folder.root() + "/nowhere").empty());
}

// A Blu-ray remux has its TrueHD track first and an AC-3 or E-AC-3 track after it. Hearth played
// the second before the containers' readers took TrueHD tracks; it then took the first, which it
// could not play, and the file failed to open (H0 left the TrueHD track out of what it takes). It
// decodes TrueHD now, so the first track it can play is the TrueHD one.
TEST_CASE("item loader: a file with TrueHD first plays the TrueHD track, and says it passed over the other",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> truehd_stream = truehd_fixture("truehd_m51_s24.thd");
    const std::vector<std::byte> ac3 = ac3_stream(kFrames);
    const std::vector<std::byte> eac3 = eac3_stream(kFrames);
    const auto truehd_units = mt::split_truehd(truehd_stream);
    REQUIRE(truehd_units.size() > 3);

    for (const Remux& remux : kRemuxes) {
        for (const bool use_eac3 : {false, true}) {
            const std::vector<std::byte>& other = use_eac3 ? eac3 : ac3;
            const auto scanned = iclforge::ac3::io::scan(other);
            REQUIRE(scanned.has_value());
            const std::array<mt::Track, 2> tracks{
                mt::Track{.codec = mt::Codec::kTruehd, .units = truehd_units},
                mt::Track{.codec = use_eac3 ? mt::Codec::kEac3 : mt::Codec::kAc3,
                          .units = scanned->access_units}};
            const std::string path = folder.write(
                std::string{use_eac3 ? "eac3-" : "ac3-"} + remux.file_name, remux.build(tracks));
            CAPTURE(path);

            const auto loaded = load(path);
            REQUIRE(loaded.has_value());
            CHECK(loaded->truehd);
            CHECK(stream_of(*loaded) == truehd_stream);
            // A Matroska or MP4 track is read where it lies; a transport stream is read whole.
            CHECK((loaded->source != nullptr) == (remux.kind != ContainerKind::kMpegTs));
            CHECK(loaded->container.kind == remux.kind);
            if (remux.kind == ContainerKind::kMpegTs) {
                CHECK(loaded->container.stream_type == 0x83);
            } else {
                // The AC-3 or E-AC-3 track is the one passed over, and the file says so.
                CHECK(loaded->container.track == 1);
                REQUIRE(loaded->container.other_tracks.size() == 1);
                CHECK(loaded->container.other_tracks[0].id == 2);
            }
            // And the session takes it for what it is.
            const iclforge::hearth::ItemLoader loader = [&loaded](const std::string&) {
                return std::expected<LoadedItem, std::string>{*loaded};
            };
            auto session = iclforge::hearth::Session::open("remux", loader);
            REQUIRE(session.has_value());
            CHECK(session->truehd());
        }
    }
}

TEST_CASE("item loader: a file whose only audio is TrueHD is played, as TrueHD",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> truehd_stream = truehd_fixture("truehd_st48_s24.thd");
    const auto truehd_units = mt::split_truehd(truehd_stream);
    REQUIRE(truehd_units.size() > 3);
    const std::array<mt::Track, 1> tracks{
        mt::Track{.codec = mt::Codec::kTruehd, .channels = 2, .units = truehd_units}};

    for (const Remux& remux : kRemuxes) {
        const std::string path = folder.write(remux.file_name, remux.build(tracks));
        CAPTURE(path);
        REQUIRE(iclforge::apps::sniff_container(remux.build(tracks)) == remux.kind);
        const auto loaded = load(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->truehd);
        CHECK(stream_of(*loaded) == truehd_stream);
        CHECK((loaded->source != nullptr) == (remux.kind != ContainerKind::kMpegTs));
    }
}

TEST_CASE("item loader: a bare TrueHD or MLP file is not read into memory, whatever it is named",
          "[hearth][item-loader]") {
    const Folder folder;
    const std::vector<std::byte> truehd_stream = truehd_fixture("truehd_st48_s24.thd");
    const std::vector<std::byte> mlp_stream = truehd_fixture("mlp_st48_s24.mlp");
    // Named for TrueHD, for MLP, and for other codecs: the first bytes say what it is.
    for (const char* name : {"movie.thd", "movie.mlp", "misnamed.ac3", "misnamed.ec3", "misnamed.ac4"}) {
        CAPTURE(name);
        const auto loaded = load(folder.write(name, truehd_stream));
        REQUIRE(loaded.has_value());
        CHECK(loaded->truehd);
        CHECK(loaded->bytes.empty());
        REQUIRE(loaded->source != nullptr);
        CHECK(loaded->source->size() == truehd_stream.size());
        // Read by offset, in pieces.
        std::vector<std::byte> piece(100);
        REQUIRE(loaded->source->read(1000, piece) == 100);
        CHECK(std::equal(piece.begin(), piece.end(), truehd_stream.begin() + 1000));
        CHECK(loaded->source->read(truehd_stream.size() - 10, piece) == 10);
        CHECK(loaded->source->read(truehd_stream.size(), piece) == 0);

        const iclforge::hearth::ItemLoader loader = [&loaded](const std::string&) {
            return std::expected<LoadedItem, std::string>{*loaded};
        };
        auto session = iclforge::hearth::Session::open(name, loader);
        REQUIRE(session.has_value());
        CHECK(session->truehd());
        CHECK(session->facts().codec == iclforge::hearth::ItemCodec::kTruehd);
    }
    const auto mlp = load(folder.write("dvd-audio.mlp", mlp_stream));
    REQUIRE(mlp.has_value());
    CHECK(mlp->truehd);
    CHECK(mlp->source != nullptr);
    // The same file under AC-3 syncframes is still read whole.
    const auto ac3 = load(folder.write("plain.ac3", ac3_stream(kFrames)));
    REQUIRE(ac3.has_value());
    CHECK_FALSE(ac3->truehd);
    CHECK(ac3->source == nullptr);
    CHECK_FALSE(ac3->bytes.empty());
}

TEST_CASE("item loader: a file with E-AC-3 first still plays the E-AC-3 track",
          "[hearth][item-loader]") {
    // E-AC-3 ahead of the TrueHD track was never broken; it must stay the one taken.
    const Folder folder;
    const std::vector<std::byte> truehd_stream = truehd_fixture("truehd_st48_s24.thd");
    const auto truehd_units = mt::split_truehd(truehd_stream);
    const std::vector<std::byte> eac3 = eac3_stream(kFrames);
    const auto scanned = iclforge::ac3::io::scan(eac3);
    REQUIRE(scanned.has_value());
    const std::array<mt::Track, 2> tracks{
        mt::Track{.codec = mt::Codec::kEac3, .units = scanned->access_units},
        mt::Track{.codec = mt::Codec::kTruehd, .channels = 2, .units = truehd_units}};

    for (const Remux& remux : kRemuxes) {
        const auto loaded = load(folder.write(remux.file_name, remux.build(tracks)));
        REQUIRE(loaded.has_value());
        CHECK(loaded->bytes == eac3);
    }
}

// The compressed track of a remux is offered beside the TrueHD one, for an output that takes it as
// it is (Session::open's TrackPreference). Nothing is read for it until it is asked for.
TEST_CASE("item loader: a remux offers its compressed track, and a file without one offers none",
          "[hearth][item-loader][track]") {
    const Folder folder;
    const std::vector<std::byte> truehd_stream = truehd_fixture("truehd_m51_s24.thd");
    const std::vector<std::byte> eac3 = eac3_stream(kFrames);
    const auto truehd_units = mt::split_truehd(truehd_stream);
    const auto scanned = iclforge::ac3::io::scan(eac3);
    REQUIRE(scanned.has_value());
    const std::array<mt::Track, 2> remux_tracks{
        mt::Track{.codec = mt::Codec::kTruehd, .units = truehd_units},
        mt::Track{.codec = mt::Codec::kEac3, .units = scanned->access_units}};
    const std::array<mt::Track, 1> truehd_only{
        mt::Track{.codec = mt::Codec::kTruehd, .channels = 2, .units = truehd_units}};

    for (const Remux& remux : kRemuxes) {
        CAPTURE(remux.file_name);
        const auto both = load(folder.write(remux.file_name, remux.build(remux_tracks)));
        REQUIRE(both.has_value());
        REQUIRE(both->compressed_track);
        const auto other = both->compressed_track();
        REQUIRE(other.has_value());
        CHECK_FALSE(other->truehd);
        CHECK(other->bytes == eac3);
        CHECK_FALSE(other->compressed_track);

        // A Session asked for the compressed track plays it, and one not asked plays the TrueHD.
        const iclforge::hearth::ItemLoader loader = [&both](const std::string&) {
            return std::expected<LoadedItem, std::string>{*both};
        };
        auto best = iclforge::hearth::Session::open("remux", loader);
        REQUIRE(best.has_value());
        CHECK(best->truehd());
        CHECK(best->has_compressed_track());
        auto compressed = iclforge::hearth::Session::open(
            "remux", loader, std::nullopt, {}, iclforge::hearth::TrackPreference::kCompressed);
        REQUIRE(compressed.has_value());
        CHECK_FALSE(compressed->truehd());
        CHECK_FALSE(compressed->has_compressed_track());
        CHECK(compressed->facts().codec == iclforge::hearth::ItemCodec::kEac3);

        const auto alone =
            load(folder.write(std::string{"alone-"} + remux.file_name, remux.build(truehd_only)));
        REQUIRE(alone.has_value());
        CHECK(alone->truehd);
        // A transport stream does not say what else it holds until it is read, so it offers the
        // lookup and the lookup fails; the item stays TrueHD.
        if (remux.kind == ContainerKind::kMpegTs) {
            REQUIRE(alone->compressed_track);
            CHECK_FALSE(alone->compressed_track().has_value());
        } else {
            CHECK_FALSE(alone->compressed_track);
        }
    }
}
