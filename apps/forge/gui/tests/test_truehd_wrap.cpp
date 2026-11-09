#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "container_input.hpp"
#include "truehd_wrap.hpp"

// A bare TrueHD stream wrapped as Matroska, MP4 or a Blu-ray transport stream
// (apps/shared/media/src/truehd_wrap.hpp), which the Forge window's Encode TrueHD dialog and
// `forge truehd-encode`, `mkv`, `mp4` and `ts` all write through. What is held here is that each
// container gives back, through the reader every command uses, exactly the stream that went in.
//
// ICLFORGE_GUI_FIXTURE_DIR comes from apps/forge/gui/tests/CMakeLists.txt.

namespace fs = std::filesystem;
namespace wrap = iclforge::apps::truehd_wrap;

namespace {

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

const fs::path kStream = fs::path{ICLFORGE_GUI_FIXTURE_DIR} / "truehd_m51_s24.thd";

}  // namespace

TEST_CASE("each container hands back the TrueHD stream it was given", "[truehd][wrap]") {
    const std::vector<std::byte> stream = read_file(kStream);
    REQUIRE_FALSE(stream.empty());
    const auto scan = wrap::scan_stream(stream, "fixture");
    REQUIRE(scan.has_value());
    CHECK(scan->channels == 6);
    CHECK(scan->info.sample_rate == 48000);
    CHECK(scan->warnings.empty());

    struct Case {
        wrap::Container container;
        iclforge::apps::ContainerKind kind;
    };
    for (const Case& c : {Case{wrap::Container::kMatroska, iclforge::apps::ContainerKind::kMatroska},
                          Case{wrap::Container::kMp4, iclforge::apps::ContainerKind::kMp4},
                          Case{wrap::Container::kTransport, iclforge::apps::ContainerKind::kMpegTs}}) {
        const auto file = wrap::mux(c.container, *scan);
        REQUIRE(file.has_value());
        CHECK(iclforge::apps::sniff_container(*file) == c.kind);
        const auto back = iclforge::apps::elementary_stream_from_bytes(
            *file, iclforge::apps::CodecSet{.truehd = true});
        CHECK(back.error.empty());
        CHECK(back.codec == iclforge::apps::TrackCodec::kTruehd);
        // The reader gives the access units back in a bare stream: the same bytes.
        CHECK(back.bytes == stream);
    }
}

TEST_CASE("a stream that is not TrueHD is refused with a sentence that names it", "[truehd][wrap]") {
    const std::vector<std::byte> junk(256, std::byte{0x55});
    const auto scan = wrap::scan_stream(junk, "junk.bin");
    REQUIRE_FALSE(scan.has_value());
    CHECK(scan.error().rfind("junk.bin: ", 0) == 0);
}

TEST_CASE("the extension of a name picks the container, in any case", "[truehd][wrap]") {
    CHECK(wrap::container_of("a.mkv") == wrap::Container::kMatroska);
    CHECK(wrap::container_of("a.MKA") == wrap::Container::kMatroska);
    CHECK(wrap::container_of("a.webm") == wrap::Container::kMatroska);
    CHECK(wrap::container_of("dir.mkv/a.Mp4") == wrap::Container::kMp4);
    CHECK(wrap::container_of("a.m4a") == wrap::Container::kMp4);
    CHECK(wrap::container_of("a.m2ts") == wrap::Container::kTransport);
    CHECK(wrap::container_of("a.TS") == wrap::Container::kTransport);
    CHECK_FALSE(wrap::container_of("a.thd").has_value());
    CHECK_FALSE(wrap::container_of("a.mlp").has_value());
    CHECK_FALSE(wrap::container_of("mkv").has_value());
}
