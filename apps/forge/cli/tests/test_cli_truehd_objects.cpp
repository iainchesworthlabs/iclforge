#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/evolution.hpp"
#include "platform/process.hpp"

// `forge decode` on the object layer of a TrueHD Atmos stream (libs/truehd/ERRATA.md entry 61): the
// real binary, over the hand-built streams of testdata/truehd/handbuilt. hb_atmos_objects is twelve
// channels in substream 3 and two evolution frames (sample 0 and sample 640) holding object audio
// metadata made by iclforge::objects::oba::build_payload: a dynamic-object-only programme with an
// LFE and eleven dynamic objects, so channel 0 is the LFE and channels 1 to 11 are the objects.
//
// ICLFORGE_CLI_EXE and ICLFORGE_TRUEHD_FIXTURE_DIR come from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_objects_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "handbuilt" / file;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

// forge's 24-bit WAV (a 68-byte WAVE_FORMAT_EXTENSIBLE header): the channels and the samples
// interleaved, sign-extended.
struct Pcm24 {
    std::uint32_t channels = 0;
    std::vector<std::int32_t> samples;
};

Pcm24 read_pcm24(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    REQUIRE(raw.size() >= 68);
    const auto byte = [&raw](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(raw[at]));
    };
    Pcm24 wav;
    wav.channels = byte(22) | (byte(23) << 8U);
    REQUIRE((byte(34) | (byte(35) << 8U)) == 24);
    for (std::size_t at = 68; at + 3 <= raw.size(); at += 3) {
        const std::uint32_t value = byte(at) | (byte(at + 1) << 8U) | (byte(at + 2) << 16U);
        wav.samples.push_back(static_cast<std::int32_t>(value << 8U) >> 8);
    }
    return wav;
}

}  // namespace

TEST_CASE("decode writes each channel of substream 3 as an object WAV, named by the metadata",
          "[forge-cli][truehd][atmos]") {
    const fs::path dir = scratch_dir();
    const fs::path wav = dir / "atmos.wav";
    const fs::path objects = dir / "atmos_objects";
    fs::remove_all(objects);
    const fs::path log = dir / "atmos.log";
    REQUIRE(run_cli("decode " + quoted(fixture("hb_atmos_objects.thd")) + " " + quoted(wav) + " " +
                        quoted(objects) + " truehd-presentation=all",
                    log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("object audio metadata: 2 payload(s), 12 object(s)") != std::string::npos);
    CHECK(text.find("wrote 12 channel(s) as mono WAVs") != std::string::npos);
    CHECK(text.find("warning") == std::string::npos);

    // Channel c of the multichannel WAV is object_cc.wav, sample for sample: the 24-bit samples are
    // exact in a float.
    const Pcm24 all = read_pcm24(wav);
    REQUIRE(all.channels == 12);
    const std::size_t frames = all.samples.size() / 12;
    REQUIRE(frames == 1280);
    for (std::size_t c = 0; c < 12; ++c) {
        char name[32];
        std::snprintf(name, sizeof name, "object_%02zu.wav", c);
        const auto decoded = iclforge::ac3::io::read_wav((objects / name).string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 1);
        REQUIRE(decoded->channels[0].size() == frames);
        bool equal = true;
        bool silent = true;
        for (std::size_t i = 0; i < frames && equal; ++i) {
            const float expected = static_cast<float>(all.samples[i * 12 + c]) / 8388608.0F;
            equal = decoded->channels[0][i] == expected;
            silent = silent && expected == 0.0F;
        }
        CHECK(equal);
        CHECK_FALSE(silent);
    }
    CHECK_FALSE(fs::exists(objects / "object_12.wav"));
}

TEST_CASE("decode objects_dir without object audio metadata still writes the channels",
          "[forge-cli][truehd][atmos]") {
    const fs::path dir = scratch_dir();
    const fs::path objects = dir / "no_metadata_objects";
    fs::remove_all(objects);
    const fs::path log = dir / "no_metadata.log";
    REQUIRE(run_cli("decode " + quoted(fixture("hb_substream3_fine_matrices.thd")) + " " +
                        quoted(dir / "no_metadata.wav") + " " + quoted(objects) +
                        " truehd-presentation=all",
                    log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("no object audio metadata") != std::string::npos);
    CHECK(fs::exists(objects / "object_11.wav"));
}

TEST_CASE("decode objects_dir is refused where there are no objects to write",
          "[forge-cli][truehd][atmos]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "refused.log";
    const fs::path objects = dir / "refused_objects";
    fs::remove_all(objects);

    // Substream 3 is decoded by truehd-presentation=all alone.
    CHECK(run_cli("decode " + quoted(fixture("hb_atmos_objects.thd")) + " " +
                      quoted(dir / "refused.wav") + " " + quoted(objects),
                  log) == kExitUsage);
    CHECK(read_text(log).find("only truehd-presentation=all decodes") != std::string::npos);
    CHECK_FALSE(fs::exists(objects));

    // A stream with three substreams has none.
    CHECK(run_cli("decode " + quoted(fixture("hb_eight_channels.thd")) + " " +
                      quoted(dir / "refused.wav") + " " + quoted(objects) +
                      " truehd-presentation=all",
                  log) == kExitInput);
    CHECK(read_text(log).find("no substream 3") != std::string::npos);
}

namespace {

// The object audio metadata of a stream's evolution frames, as the library reads them, and the
// first sample of the access unit each came in.
struct OamdFrame {
    std::uint64_t first_sample = 0;
    iclforge::objects::oba::DecodedProgram program;
};

std::vector<OamdFrame> read_oamd_frames(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    namespace th = iclforge::truehd;
    th::ElementaryReader reader(bytes);
    std::vector<OamdFrame> frames;
    std::uint64_t sample = 0;
    int substreams = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = th::parse_access_unit(*unit, substreams, true);
        REQUIRE(parsed.has_value());
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
        }
        if (parsed->extra_data) {
            const auto evolution = th::read_evolution(*parsed->extra_data);
            REQUIRE(evolution.container.has_value());
            const auto program = iclforge::objects::oba::parse_payload(evolution.container->oamd());
            REQUIRE(program.has_value());
            frames.push_back({sample, *program});
        }
        sample += 40;
    }
    return frames;
}

}  // namespace

TEST_CASE("truehd-render places the objects of substream 3 on the layout by their metadata",
          "[forge-cli][truehd][atmos][render]") {
    namespace oba = iclforge::objects::oba;
    namespace render = iclforge::render;
    const fs::path dir = scratch_dir();
    const std::string layout_text = "L,R,C,LFE,Ls,Rs,Lrs,Rrs";
    const fs::path raw_wav = dir / "render_raw.wav";
    const fs::path fed_wav = dir / "render_fed.wav";
    const fs::path log = dir / "render.log";
    const std::string input = quoted(fixture("hb_atmos_objects.thd"));
    REQUIRE(run_cli("decode " + input + " " + quoted(raw_wav) + " truehd-presentation=all", log) ==
            0);
    REQUIRE(run_cli("decode " + input + " " + quoted(fed_wav) + " truehd-presentation=all " +
                        "truehd-render=" + layout_text,
                    log) == 0);
    CHECK(read_text(log).find("8 channel(s) (L R C LFE Ls Rs Lrs Rrs)") != std::string::npos);

    const Pcm24 raw = read_pcm24(raw_wav);
    const Pcm24 fed = read_pcm24(fed_wav);
    REQUIRE(raw.channels == 12);
    REQUIRE(fed.channels == 8);
    const std::size_t frames = raw.samples.size() / 12;
    REQUIRE(fed.samples.size() / 8 == frames);

    const auto oamd = read_oamd_frames(fixture("hb_atmos_objects.thd"));
    REQUIRE(oamd.size() == 2);
    REQUIRE(oamd[1].first_sample == 640);

    // The gains the layout renderer gives each object at each frame's positions, asked directly.
    const auto layout = render::OutputLayout::parse(layout_text);
    REQUIRE(layout.has_value());
    render::LayoutRenderer panner(*layout, 48000);
    const auto gains_of = [&](const oba::DynamicObject& state) {
        oba::DisplayObject object;
        object.position = state.position;
        object.size = state.size;
        object.gain_db = state.gain_db;
        object.active = state.active;
        panner.set_objects(std::span<const oba::DisplayObject>(&object, 1));
        std::array<double, 8> gains{};
        for (std::size_t slot = 0; slot < 8; ++slot) {
            gains[slot] = static_cast<double>(panner.object_gain(0, slot));
        }
        return gains;
    };
    std::array<std::array<std::array<double, 8>, 11>, 2> gains{};
    for (std::size_t f = 0; f < 2; ++f) {
        REQUIRE(oamd[f].program.blocks.size() == 1);
        REQUIRE(oamd[f].program.blocks[0].objects.size() == 11);
        for (std::size_t i = 0; i < 11; ++i) {
            gains[f][i] = gains_of(oamd[f].program.blocks[0].objects[i]);
        }
    }

    // Channel 0 is the LFE and goes to the LFE feed, slot 3, at unity; channels 1 to 11 are the
    // objects. Before sample 640 the first frame's positions hold (the first update takes effect at
    // once); from 640 each speaker's gain moves to the second frame's over the update's 1536
    // samples.
    int worst = 0;
    std::size_t worst_at = 0;
    std::size_t worst_slot = 0;
    for (std::size_t n = 0; n < frames; ++n) {
        const double progress = n < 640 ? 0.0 : static_cast<double>(n - 640) / 1536.0;
        for (std::size_t slot = 0; slot < 8; ++slot) {
            double expected = slot == 3 ? static_cast<double>(raw.samples[n * 12]) : 0.0;
            for (std::size_t i = 0; i < 11; ++i) {
                const double gain =
                    gains[0][i][slot] + (gains[1][i][slot] - gains[0][i][slot]) * progress;
                expected += gain * static_cast<double>(raw.samples[n * 12 + 1 + i]);
            }
            const int diff = static_cast<int>(std::abs(
                std::llround(expected) - static_cast<long long>(fed.samples[n * 8 + slot])));
            if (diff > worst) {
                worst = diff;
                worst_at = n;
                worst_slot = slot;
            }
        }
    }
    CAPTURE(worst_at, worst_slot);
    CHECK(worst <= 1);
    // The test means something only if the second frame moved the objects.
    bool moved = false;
    for (std::size_t i = 0; i < 11; ++i) {
        moved = moved || gains[0][i] != gains[1][i];
    }
    CHECK(moved);
}

TEST_CASE("truehd-render is refused without the objects it places", "[forge-cli][truehd][atmos]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "render_refused.log";
    const std::string input = quoted(fixture("hb_atmos_objects.thd"));
    CHECK(run_cli("decode " + input + " " + quoted(dir / "r.wav") + " truehd-render=7.1", log) ==
          kExitUsage);
    CHECK(read_text(log).find("only truehd-presentation=all decodes") != std::string::npos);
    CHECK(run_cli("decode " + input + " " + quoted(dir / "r.wav") +
                      " truehd-presentation=all truehd-render=7.1.9.9",
                  log) == kExitUsage);
    CHECK(read_text(log).find("truehd-render is a speaker layout") != std::string::npos);
}
