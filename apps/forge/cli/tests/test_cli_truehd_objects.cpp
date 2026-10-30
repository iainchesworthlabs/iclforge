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
#include "iclforge/base/wav.hpp"
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

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::vector<unsigned char>{std::istreambuf_iterator<char>{in},
                                      std::istreambuf_iterator<char>{}};
}

std::uint32_t crc32_mpeg2(const std::vector<unsigned char>& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char b : data) {
        crc ^= static_cast<std::uint32_t>(b) << 24U;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
        }
    }
    return crc;
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

namespace {

// Twelve object channels from the DEE gold set's real audio, the LFE first: the six of
// fx_music_51, the two of fx_music_20 and four channels of fx_tones_71 from a different place in
// it.
std::vector<std::vector<float>> twelve_object_channels() {
    const fs::path sources =
        fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / ".." / "truehd-dee" / "sources";
    const auto music = iclforge::ac3::io::read_wav((sources / "fx_music_51.wav").string());
    const auto stereo = iclforge::ac3::io::read_wav((sources / "fx_music_20.wav").string());
    const auto tones = iclforge::ac3::io::read_wav((sources / "fx_tones_71.wav").string());
    REQUIRE(music.has_value());
    REQUIRE(stereo.has_value());
    REQUIRE(tones.has_value());
    const std::size_t frames =
        std::min({music->frame_count(), stereo->frame_count(), tones->frame_count()});
    std::vector<std::vector<float>> channels(12, std::vector<float>(frames));
    for (std::size_t t = 0; t < frames; ++t) {
        for (std::size_t c = 0; c < 12; ++c) {
            if (c < 6) {
                channels[c][t] = music->channels[c][t];
            } else if (c < 8) {
                channels[c][t] = stereo->channels[c - 6][t];
            } else {
                channels[c][t] = tones->channels[c - 8][(t + 7 * c) % frames];
            }
        }
    }
    return channels;
}

}  // namespace

TEST_CASE("truehd-atmos writes a stream whose substream 3 is the WAV's channels, frame by frame",
          "[forge-cli][truehd][atmos][truehd-atmos]") {
    namespace oba = iclforge::objects::oba;
    const fs::path dir = scratch_dir();
    const auto channels = twelve_object_channels();
    const fs::path wav = dir / "atmos_in.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), channels, 48000).has_value());
    // Object 0 (channel 1, after the LFE) sweeps across the room; the others keep the fan.
    const fs::path paths = dir / "atmos_in_paths.txt";
    {
        std::ofstream out{paths};
        out << "0 0.0  0.9 0.5 0.0  1.0 0.0\n0 0.25 0.1 0.5 0.0  1.0 0.0\n";
    }
    const fs::path stream = dir / "atmos_out.thd";
    const fs::path log = dir / "atmos_out.log";
    REQUIRE(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(stream) + " " + quoted(paths) +
                        " verify",
                    log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("12 object channel(s)") != std::string::npos);
    CHECK(text.find("4 substream(s)") != std::string::npos);
    CHECK(text.find("verified: 300 access units") != std::string::npos);

    // The object channels decode to the WAV, bit for bit, and the stream says what it is.
    const fs::path objects = dir / "atmos_out_objects";
    fs::remove_all(objects);
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(dir / "atmos_out_all.wav") + " " +
                        quoted(objects) + " truehd-presentation=all",
                    log) == 0);
    CHECK(read_text(log).find("object audio metadata: 8 payload(s), 12 object(s)") !=
          std::string::npos);
    for (std::size_t c = 0; c < 12; ++c) {
        char name[32];
        std::snprintf(name, sizeof name, "object_%02zu.wav", c);
        const auto decoded = iclforge::ac3::io::read_wav((objects / name).string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 1);
        CHECK(decoded->channels[0] == channels[c]);
    }

    // Eight metadata frames, one every 1536 samples, each starting where it says: the access unit's
    // first sample, the update's sample offset and 32 samples a unit of the block's offset factor.
    const auto frames = read_oamd_frames(stream);
    REQUIRE(frames.size() == 8);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        const auto& program = frames[k].program;
        CHECK(program.program.dynamic_only);
        CHECK(program.program.lfe);
        CHECK(program.program.dynamic_objects == 11);
        REQUIRE(program.blocks.size() == 1);
        const auto& block = program.blocks.front();
        CHECK(frames[k].first_sample + static_cast<std::uint64_t>(block.sample_offset) +
                  32U * static_cast<std::uint64_t>(block.block_offset_factor) ==
              1536U * k);
        CHECK(block.ramp_duration == 1536);
        REQUIRE(block.objects.size() == 11);
        // Object 0 follows the file, at the frame's end; the quantisation of a position is under
        // 0.02. Object 1 is on the fan, where it was.
        const double t = static_cast<double>(1536 * (k + 1)) / 48000.0;
        const double expected_x = std::max(0.1, 0.9 - 0.8 * t / 0.25);
        CHECK(std::abs(block.objects[0].position.x - expected_x) < 0.02);
        CHECK(std::abs(block.objects[0].position.y - 0.5) < 0.02);
        CHECK(std::abs(block.objects[1].position.x -
                       frames[0].program.blocks[0].objects[1].position.x) < 1e-9);
    }
    CHECK(frames[0].first_sample == 0);
}

TEST_CASE(
    "the bed of a truehd-atmos stream is the objects the renderer places, as the decode renders",
    "[forge-cli][truehd][atmos][truehd-atmos][render]") {
    const fs::path dir = scratch_dir();
    const auto channels = twelve_object_channels();
    const fs::path wav = dir / "atmos_bed_in.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), channels, 48000).has_value());
    const fs::path stream = dir / "atmos_bed.thd";
    const fs::path log = dir / "atmos_bed.log";
    REQUIRE(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(stream), log) == 0);
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(dir / "atmos_bed_best.wav"), log) ==
            0);
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(dir / "atmos_bed_fed.wav") +
                        " truehd-presentation=all truehd-render=L,R,C,LFE,Ls,Rs,Lrs,Rrs",
                    log) == 0);
    // A decoder that plays the 7.1 bed and one that renders the objects play the same programme.
    const Pcm24 best = read_pcm24(dir / "atmos_bed_best.wav");
    const Pcm24 fed = read_pcm24(dir / "atmos_bed_fed.wav");
    REQUIRE(best.channels == 8);
    REQUIRE(fed.channels == 8);
    CHECK(best.samples == fed.samples);
    bool silent = true;
    for (const std::int32_t v : best.samples) {
        silent = silent && v == 0;
    }
    CHECK_FALSE(silent);

    // A bed given as a WAV is the bed, whatever the objects are; one of the wrong shape is refused.
    std::vector<std::vector<float>> bed(8, std::vector<float>(channels[0].size(), 0.25F));
    const fs::path bed_wav = dir / "atmos_bed_given.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(bed_wav.string(), bed, 48000).has_value());
    const fs::path with_bed = dir / "atmos_with_bed.thd";
    REQUIRE(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(with_bed) +
                        " truehd-bed=" + quoted(bed_wav),
                    log) == 0);
    REQUIRE(run_cli("decode " + quoted(with_bed) + " " + quoted(dir / "atmos_with_bed.wav"), log) ==
            0);
    const Pcm24 given = read_pcm24(dir / "atmos_with_bed.wav");
    REQUIRE(given.samples.size() == best.samples.size());
    CHECK(given.samples.front() == 2097152);  // 0.25 of full scale, 24 bit
    CHECK(given.samples != best.samples);
    bed.pop_back();
    REQUIRE(iclforge::ac3::io::write_wav_f32(bed_wav.string(), bed, 48000).has_value());
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(with_bed) +
                      " truehd-bed=" + quoted(bed_wav),
                  log) == kExitInput);
    CHECK(read_text(log).find("the bed has to be 8 channels") != std::string::npos);
}

TEST_CASE("truehd-atmos refuses what substream 3 cannot carry",
          "[forge-cli][truehd][atmos][truehd-atmos]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "atmos_refused.log";
    std::vector<std::vector<float>> mono(1, std::vector<float>(4800, 0.1F));
    const fs::path wav = dir / "atmos_one.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), mono, 48000).has_value());
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "o.thd"), log) == kExitInput);
    CHECK(read_text(log).find("2 to 16") != std::string::npos);
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "o.wav"), log) == kExitUsage);
    std::vector<std::vector<float>> twelve(12, std::vector<float>(4800, 0.1F));
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), twelve, 96000).has_value());
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "o.thd"), log) == kExitUsage);
    CHECK(read_text(log).find("48 kHz only") != std::string::npos);
}

TEST_CASE("truehd-atmos takes a RIFF file with an axml chunk for an ADM master and says so",
          "[forge-cli][truehd][atmos][truehd-atmos]") {
    // 'RIFF', a size, 'WAVE', an empty 'axml' chunk: no master that reads (forge built with or
    // without ADM refuses it, exit 2), but not a WAV of object channels either.
    const fs::path dir = scratch_dir();
    const fs::path file = dir / "not_a_master.wav";
    {
        std::ofstream out{file, std::ios::binary};
        const char bytes[] = {'R', 'I', 'F', 'F', 12,  0,   0, 0, 'W', 'A',
                              'V', 'E', 'a', 'x', 'm', 'l', 0, 0, 0,   0};
        out.write(bytes, sizeof bytes);
    }
    const fs::path log = dir / "not_a_master.log";
    CHECK(run_cli("truehd-atmos " + quoted(file) + " " + quoted(dir / "o.thd"), log) == kExitInput);
    const std::string text = read_text(log);
    CHECK((text.find("ICLFORGE_BUILD_ADM") != std::string::npos ||
           text.find("not_a_master.wav") != std::string::npos));
}

TEST_CASE("demux to .thd takes the TrueHD track of a programme that lists an AC-3 track first",
          "[forge-cli][truehd][demux]") {
    // A Blu-ray programme map names its AC-3 and other audio tracks beside the TrueHD one. This one
    // is the PMT forge ts writes with an AC-3 entry (stream_type 0x81, no packets) put before the
    // TrueHD entry: the reader's first usable stream is the AC-3 one unless the output asks for
    // TrueHD.
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "two_tracks.log";
    const fs::path plain = dir / "two_tracks_plain.ts";
    REQUIRE(run_cli("ts " + quoted(fixture("hb_atmos_objects.thd")) + " " + quoted(plain), log) ==
            0);
    auto bytes = read_bytes(plain);
    constexpr std::size_t kPacket = 188;
    constexpr unsigned kPmtPid = 0x1000;
    std::size_t patched = 0;
    for (std::size_t at = 0; at + kPacket <= bytes.size(); at += kPacket) {
        const unsigned pid = ((bytes[at + 1] & 0x1FU) << 8U) | bytes[at + 2];
        if (pid != kPmtPid || (bytes[at + 1] & 0x40U) == 0) {
            continue;
        }
        const std::size_t section = at + 5 + bytes[at + 4];
        const std::size_t length = 3 + (((bytes[section + 1] & 0x0FU) << 8U) | bytes[section + 2]);
        const std::size_t info = ((bytes[section + 10] & 0x0FU) << 8U) | bytes[section + 11];
        std::vector<unsigned char> edited(
            bytes.begin() + static_cast<std::ptrdiff_t>(section),
            bytes.begin() + static_cast<std::ptrdiff_t>(section + 12 + info));
        // stream_type 0x81, elementary_PID 0x0200, ES_info_length 0, then the TrueHD entries.
        edited.insert(edited.end(), {0x81, 0xE2, 0x00, 0xF0, 0x00});
        edited.insert(edited.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(section + 12 + info),
                      bytes.begin() + static_cast<std::ptrdiff_t>(section + length - 4));
        const std::size_t section_length = edited.size() - 3 + 4;
        edited[1] =
            static_cast<unsigned char>((edited[1] & 0xF0U) | ((section_length >> 8U) & 0x0FU));
        edited[2] = static_cast<unsigned char>(section_length & 0xFFU);
        const std::uint32_t crc = crc32_mpeg2(edited);
        for (int shift = 24; shift >= 0; shift -= 8) {
            edited.push_back(
                static_cast<unsigned char>((crc >> static_cast<unsigned>(shift)) & 0xFFU));
        }
        REQUIRE(section + edited.size() <= at + kPacket);
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(section),
                  bytes.begin() + static_cast<std::ptrdiff_t>(at + kPacket), 0xFF);
        std::copy(edited.begin(), edited.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(section));
        ++patched;
    }
    REQUIRE(patched > 0);
    const fs::path two = dir / "two_tracks.ts";
    {
        std::ofstream out{two, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const fs::path thd = dir / "two_tracks.thd";
    fs::remove(thd);
    REQUIRE(run_cli("demux " + quoted(two) + " " + quoted(thd), log) == 0);
    CHECK(read_text(log).find("TrueHD") != std::string::npos);
    CHECK(read_bytes(thd) == read_bytes(fixture("hb_atmos_objects.thd")));

    // Any other output name takes the first usable stream, here the AC-3 entry with no packets.
    CHECK(run_cli("demux " + quoted(two) + " " + quoted(dir / "two_tracks.bin"), log) ==
          kExitInput);
}
