#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/adm/ac3adm.hpp"
#include "iclforge/adm/bridge.hpp"
#include "iclforge/adm/coordinates.hpp"
#include "iclforge/objects/oamd.hpp"

// forge's 'decode ... adm_out=' path (the ADM write direction -
// apps/forge/cli/src/commands/decode.cpp's accumulate_adm/run_decode_eac3). Real, subprocess-level
// integration test: the same "run the actual built binary, inspect what it wrote" shape
// apps/forge/cli/tests/test_cli_atmos_adm.cpp (the read direction) and
// apps/forge/cli/tests/test_cli.cpp's own atmos-encode tests use, and for the same reason - see
// test_cli_atmos_adm.cpp's own top comment on why decode.cpp's own logic cannot be linked into this
// test binary and called directly. A separate file for the same two-part reason as that file's own
// top comment: this only makes sense with ICLFORGE_BUILD_ADM AND forge both on - see
// apps/forge/cli/tests/CMakeLists.txt's own gating comment on the block this file's source is added
// to.
//
// What this proves: a JOC-reconstructed object comes out of decode_access_unit
// oba::joc::reconstruction_delay(domain) samples (576 under Domain::kQmf, the DecoderConfig
// default) behind the bed it was pulled from (docs/library/decoding.md, "Atmos objects lag the
// bed"; libs/ac3/tests/decoder/test_latency.cpp measures it end to end). accumulate_adm appends
// each decoded unit's object_audio and the bed's LFE channel side by side, unit by unit, into the
// ADM master - carrying that same 576-sample gap straight into the exported file unless something
// delays the LFE to match.
//
// Regression test: encode a real Atmos stream with one object sent entirely to the LFE
// (lfe_send=1.0, via atmos-encode's own keyframes file - there is no lfe_send CLI flag, see
// run_atmos_encode's own default placement in apps/forge/cli/src/commands/atmos.cpp), decode it
// with adm_out= set, read the BW64/ADM master back and cross-correlate its LFE channel against its
// one object channel. Before the fix this PR made, the object trails the LFE by 576 samples in the
// written file; fixed, they line up.

namespace fs = std::filesystem;

namespace {

// See apps/forge/cli/tests/test_cli.cpp's own scratch_dir for the reasoning this copy shares (this
// project's established per-file test-helper convention - test_cli_atmos_adm.cpp's own top
// comment), including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() { return iclforge::test::platform::process_id(); }

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cli_decode_adm_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

// Runs `forge <args>` with both streams redirected to `log`. The platform
// differences - cmd.exe's quoting and std::system()'s two return-value
// shapes - live in tests/support/platform/process.hpp's run_shell, not here.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// Makes `path` with one forge run - `command` and `path`, then `rest` -
// once per test process. Same trimmed copy as test_cli_inspect_edges.cpp's own.
fs::path generated(const fs::path& path, std::string_view command, std::string_view rest) {
    if (!fs::exists(path)) {
        const auto log = fs::path{path}.replace_extension(".gen.log");
        REQUIRE(run_cli(std::string{command} + " " + quoted(path) + " " + std::string{rest}, log) ==
                0);
    }
    return path;
}

// Runs `args`, checks the exit code, and returns everything it printed.
std::string run_expecting(const std::string& args, const fs::path& log, int exit_code) {
    const auto rc = run_cli(args, log);
    auto text = read_log(log);
    INFO(text);
    CHECK(rc == exit_code);
    return text;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

constexpr int kFrames = 8;
// Frame 3, as libs/ac3/tests/decoder/test_latency.cpp and libs/render/tests/test_object_lfe_timing.cpp (the header-
// only renderer's own regression test for this same class of bug) both place their own marker: past
// every encoder's/decoder's own priming, early enough that reconstruction_delay(kQmf)'s 576 samples
// still leave it well inside an 8-frame stream.
constexpr int kPulseAt = 3 * iclforge::ac3::kSamplesPerFrame + 512;

// The object's whole-clip audio: a decaying tone burst riding a quiet noise floor. The floor is not
// decoration - JOC's reconstruction matrix is solved per frame from the object's own energy that
// frame (TS 103 420 §6.6.5 interpolates each frame's from the one before), so a pulse arriving out
// of true silence comes back through a matrix still ramping up across it, smearing exactly the peak
// a correlation is trying to locate. Flat, low-level energy in every frame keeps the matrix settled
// - libs/render/tests/test_object_lfe_timing.cpp's own programme() makes the identical argument for
// the header-only renderer's regression test. The burst itself, not a steady tone, for the reason
// libs/ac3/tests/decoder/test_latency.cpp's own burst() gives: a sinusoid correlates with itself a
// period away almost as well as at zero lag, which would make a lag search meaningless.
std::vector<float> object_pulse_with_floor(int samples, int at) {
    std::vector<float> pcm(static_cast<std::size_t>(samples), 0.0F);
    std::uint32_t state = 0x9E3779B9U;
    for (float& sample : pcm) {
        state = (state * 1664525U) + 1013904223U;
        const double white = (static_cast<double>(state >> 8U) / 8388608.0) - 1.0;
        sample = static_cast<float>(0.02 * white);
    }
    for (int n = 0; n < 700; ++n) {
        const int index = at + n;
        if (index >= samples) {
            break;
        }
        const double t = static_cast<double>(n) / 48000.0;
        const double envelope = std::exp(-t * 90.0);
        pcm[static_cast<std::size_t>(index)] +=
            static_cast<float>(0.6 * envelope * std::sin(2.0 * std::numbers::pi * 120.0 * t));
    }
    return pcm;
}

float peak(std::span<const float> pcm) {
    float out = 0.0F;
    for (const float sample : pcm) {
        out = std::max(out, std::abs(sample));
    }
    return out;
}

// The lag (later[n + lag] against earlier[n]) maximising their correlation over a window around the
// pulse - libs/ac3/tests/decoder/test_latency.cpp's own best_lag, searched both directions since this test,
// unlike that one, does not already know which of the two channels leads.
int best_lag(std::span<const float> earlier, std::span<const float> later, int min_lag, int max_lag) {
    const int from = std::max(0, kPulseAt - 2048);
    const int to = std::min(static_cast<int>(earlier.size()), kPulseAt + 4096);
    int best = min_lag;
    double best_score = -1.0;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double score = 0.0;
        for (int n = from; n < to; ++n) {
            const int m = n + lag;
            if (m < 0 || m >= static_cast<int>(later.size())) {
                continue;
            }
            score += static_cast<double>(earlier[static_cast<std::size_t>(n)]) *
                     static_cast<double>(later[static_cast<std::size_t>(m)]);
        }
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    return best;
}

// DEE's own cbi_wav channel order for 5.1.4 (tools/generators/gen_object_fixture.py), the same
// tones apps/forge/cli/tests/test_cli_atmos_cbi.cpp gives each bed channel so a channel is
// identified by what it carries rather than by where it sits.
struct BedChannel {
    const char* label;
    double frequency;
};
constexpr std::array<BedChannel, 10> kInput514 = {{
    {"L", 220.0},
    {"R", 277.2},
    {"C", 330.0},
    {"LFE", 55.0},
    {"Ls", 554.4},
    {"Rs", 660.0},
    {"Tfl", 740.0},
    {"Tfr", 831.6},
    {"Tbl", 880.0},
    {"Tbr", 1108.8},
}};

double tone_magnitude(std::span<const float> signal, double frequency, double sample_rate) {
    double real = 0.0;
    double imag = 0.0;
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sample_rate;
        real += static_cast<double>(signal[n]) * std::cos(phase);
        imag += static_cast<double>(signal[n]) * std::sin(phase);
    }
    return std::hypot(real, imag) / static_cast<double>(signal.size());
}

}  // namespace

TEST_CASE("decode's ADM master lines the bed's LFE up with the object it was pulled beside",
          "[cli][decode][adm]") {
    const auto dir = scratch_dir();

    const auto wav_path = dir / "decode_adm_in.wav";
    const std::vector<std::vector<float>> channels{
        object_pulse_with_floor(kFrames * iclforge::ac3::kSamplesPerFrame, kPulseAt)};
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    // One static keyframe (ac3/oba/scene.hpp: "a single keyframe holds its placement everywhere")
    // sending the object entirely to the LFE - run_atmos_encode's own default placement is
    // lfe_send=0.0 (apps/forge/cli/src/commands/atmos.cpp), and the only way to override it is this
    // keyframes file (main.cpp's own 'atmos-encode' doc string names no lfe_send flag).
    const auto paths_path = dir / "decode_adm_paths.txt";
    {
        std::ofstream paths{paths_path};
        REQUIRE(paths.is_open());
        paths << "0 0.0 0.5 0.5 0.0 1.0 1.0\n";
    }

    const auto ec3_path = dir / "decode_adm_in.ec3";
    const auto encode_log = dir / "decode_adm_encode.log";
    const auto encode_rc = run_cli("atmos-encode \"" + wav_path.string() + "\" \"" +
                                        ec3_path.string() + "\" 448 1 \"" + paths_path.string() + "\"",
                                    encode_log);
    INFO(read_log(encode_log));
    REQUIRE(encode_rc == 0);

    const auto out_wav = dir / "decode_adm_out.wav";
    const auto adm_out = dir / "decode_adm_master.wav";
    const auto decode_log = dir / "decode_adm_decode.log";
    // Positional 3 (objects_dir) skipped with an explicit empty argument so positional 4 (adm_out)
    // still lands correctly - apps/forge/cli/tests/test_cli_stream_tools.cpp's own transcode test uses the same
    // trick for a skipped middle positional.
    const auto decode_rc =
        run_cli("decode \"" + ec3_path.string() + "\" \"" + out_wav.string() + "\" \"\" \"" +
                    adm_out.string() + "\"",
                decode_log);
    INFO(read_log(decode_log));
    REQUIRE(decode_rc == 0);
    REQUIRE(fs::exists(adm_out));

    const auto parsed = iclforge::adm::parse_bw64(adm_out.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 2);

    // accumulate_adm always writes the dynamic object(s) first, the bed's LFE last
    // (AdmMasterInput::channels.resize(object_audio.size() + (have_lfe ? 1 : 0)), decode.cpp) - but
    // looked up here by is_bed/is_lfe rather than trusted by position, so a change to that order fails
    // the REQUIREs below rather than silently comparing the wrong channels.
    std::size_t object_i = bridged->channel_count();
    std::size_t lfe_i = bridged->channel_count();
    for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
        if (bridged->is_bed[i] && bridged->is_lfe[i]) {
            lfe_i = i;
        } else if (!bridged->is_bed[i]) {
            object_i = i;
        }
    }
    REQUIRE(object_i < bridged->channel_count());
    REQUIRE(lfe_i < bridged->channel_count());

    const auto object_pcm = bridged->pcm[object_i];
    const auto lfe_pcm = bridged->pcm[lfe_i];
    REQUIRE(object_pcm.size() == lfe_pcm.size());

    // Both channels must actually carry the burst, or the lag search below measures noise.
    REQUIRE(peak(object_pcm) > 0.05F);
    REQUIRE(peak(lfe_pcm) > 0.05F);

    const int lag = best_lag(lfe_pcm, object_pcm, -2 * iclforge::ac3::kSamplesPerFrame,
                             2 * iclforge::ac3::kSamplesPerFrame);
    CAPTURE(lag);
    // Before this fix: the LFE channel was written straight from the decoded bed, undelayed, while
    // the object channel is JOC-reconstructed and so already reconstruction_delay(kQmf) samples (576)
    // behind it - the object trailed the LFE by 576 in the unfixed file (measured on this exact
    // fixture while confirming the bug - see the PR description). Fixed, the two line up.
    CHECK(lag == 0);
}

// Moved from apps/forge/cli/tests/test_cli_inspect_edges.cpp - see that file's own comment on its
// sibling, plain-AC-3 case. decode.cpp's run_decode_eac3 checks forge_cli::adm_capability() up
// front, before it can tell whether this specific programme has an object layer, so an E-AC-3
// stream only reaches these two warnings (rather than exiting 2 with "this build was not configured
// with -DICLFORGE_BUILD_ADM=ON") when ADM support was actually built - which is exactly this file's
// own gate (apps/forge/cli/tests/CMakeLists.txt's ICLFORGE_BUILD_ADM block).
TEST_CASE("decode warns when ADM output is asked of an E-AC-3 stream with no object layer",
          "[cli][decode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_no_objects_eac3.log";
    const auto objects_dir = dir / "unused_objects";
    const auto adm = dir / "unused_adm.wav";

    const auto ec3_in = generated(dir / "clean.ec3", "eac3-silence", "1 192 stereo");
    const auto text = run_expecting("decode " + quoted(ec3_in) + " " +
                                         quoted(dir / "plain_ec3.wav") + " " + quoted(objects_dir) +
                                         " " + quoted(adm),
                                     log, 0);
    CHECK(contains(text, "warning: " + adm.string() +
                             " given but no Atmos programme with an object layer was decoded"));
    CHECK(contains(text,
                   "warning: objects_dir given but there is no reconstructed object audio to "
                   "export"));
    CHECK_FALSE(fs::exists(adm));
}

// A bed programme, not a dynamic-object-only one: `atmos-cbi` writes a 5.1.4 bed with no dynamic
// objects, and decode's adm_out used to refuse any such stream with a warning. Each JOC output is a
// bed channel named by its Table 12 label (nine of them: the LFE is bypassed by JOC and comes from
// the decoded bed), and each is written as a DirectSpeakers channel at its label's room position.
TEST_CASE("decode writes a channel-based-immersive bed programme as DirectSpeakers channels",
          "[cli][decode][adm]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kSampleRate = 48000;
    const auto frames = static_cast<std::size_t>(8 * iclforge::ac3::kSamplesPerFrame);

    std::vector<std::vector<float>> channels(kInput514.size(), std::vector<float>(frames));
    for (std::size_t c = 0; c < kInput514.size(); ++c) {
        for (std::size_t n = 0; n < frames; ++n) {
            channels[c][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * kInput514[c].frequency *
                               static_cast<double>(n) / static_cast<double>(kSampleRate)));
        }
    }
    const auto wav_path = dir / "decode_adm_bed_in.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto ec3_path = dir / "decode_adm_bed_in.ec3";
    const auto encode_log = dir / "decode_adm_bed_encode.log";
    const auto encode_rc =
        run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + ec3_path.string() + "\" 448 5.1.4",
                encode_log);
    INFO(read_log(encode_log));
    REQUIRE(encode_rc == 0);

    const auto out_wav = dir / "decode_adm_bed_out.wav";
    const auto adm_out = dir / "decode_adm_bed_master.wav";
    const auto decode_log = dir / "decode_adm_bed_decode.log";
    const auto decode_rc = run_cli("decode \"" + ec3_path.string() + "\" \"" + out_wav.string() +
                                       "\" \"\" \"" + adm_out.string() + "\"",
                                   decode_log);
    const auto decode_text = read_log(decode_log);
    INFO(decode_text);
    REQUIRE(decode_rc == 0);
    CHECK_FALSE(contains(decode_text, "no ADM master written"));
    REQUIRE(fs::exists(adm_out));

    const auto parsed = iclforge::adm::parse_bw64(adm_out.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    // Nine JOC outputs and the LFE.
    REQUIRE(bridged->channel_count() == 10);
    CHECK(std::ranges::all_of(bridged->is_bed, [](bool bed) { return bed; }));
    CHECK(std::ranges::count(bridged->is_lfe, true) == 1);

    // The bed channels in joc_object_indices() order, then the LFE (decode.cpp's accumulate_adm
    // appends it last). Each carries its own channel's tone and is pinned at its label's position.
    constexpr std::array<const char*, 9> kExpected = {"L",   "R",   "C",   "Ls", "Rs",
                                                      "Tfl", "Tfr", "Tbl", "Tbr"};
    constexpr std::array<iclforge::objects::oba::BedLabel, 9> kLabels = {
        iclforge::objects::oba::BedLabel::kL,   iclforge::objects::oba::BedLabel::kR,
        iclforge::objects::oba::BedLabel::kC,   iclforge::objects::oba::BedLabel::kLs,
        iclforge::objects::oba::BedLabel::kRs,  iclforge::objects::oba::BedLabel::kTfl,
        iclforge::objects::oba::BedLabel::kTfr, iclforge::objects::oba::BedLabel::kTbl,
        iclforge::objects::oba::BedLabel::kTbr};
    for (std::size_t i = 0; i < kExpected.size(); ++i) {
        INFO("bed channel " << i << " should be " << kExpected[i]);
        CHECK_FALSE(bridged->is_lfe[i]);

        const auto channel =
            std::ranges::find(parsed->model.channel_formats, bridged->channel_ids[i],
                              &iclforge::adm::AudioChannelFormat::id);
        REQUIRE(channel != parsed->model.channel_formats.end());
        CHECK(channel->type == iclforge::adm::TypeDefinition::kDirectSpeakers);
        REQUIRE(channel->block_formats.size() == 1);
        const auto& block = channel->block_formats.front();
        CHECK(block.speaker_labels == std::vector<std::string>{kExpected[i]});
        REQUIRE(std::holds_alternative<iclforge::adm::CartesianPosition>(block.position));
        const auto expected_position = iclforge::adm::room_to_adm_cartesian(
            iclforge::objects::oba::bed_label_position(kLabels[i]));
        const auto& position = std::get<iclforge::adm::CartesianPosition>(block.position);
        CHECK(position.x == Catch::Approx(expected_position.x));
        CHECK(position.y == Catch::Approx(expected_position.y));
        CHECK(position.z == Catch::Approx(expected_position.z));

        // The channel carries its own tone, and not the next one's.
        std::size_t strongest = 0;
        double strongest_magnitude = -1.0;
        for (std::size_t source = 0; source < kInput514.size(); ++source) {
            const auto magnitude =
                tone_magnitude(bridged->pcm[i], kInput514[source].frequency, kSampleRate);
            if (magnitude > strongest_magnitude) {
                strongest_magnitude = magnitude;
                strongest = source;
            }
        }
        CHECK(std::string{kInput514[strongest].label} == std::string{kExpected[i]});
    }
    CHECK(bridged->is_lfe[9]);
    CHECK(tone_magnitude(bridged->pcm[9], 55.0, kSampleRate) > 0.05);
}

namespace {

// The 55 Hz of the LFE tone every bed below puts in its LFE channel, and how loud it has to be in a
// master to count as carried: the tone is 0.3 of full scale, so 0.15 once measured, and a silent
// channel measures 0. Well clear of both.
constexpr double kLfeHz = 55.0;
constexpr double kLfeAudible = 0.05;

// What a channel-based-immersive bed comes to after forge reads it back out of an ADM master:
//
//   bed.wav --atmos-cbi--> .ec3 --decode--> ADM master --atmos-adm--> .ec3 --decode--> ADM master
//
// which is how a bed gets into atmos-adm at all (issue 1627). The LFE is the fourth channel (index 3)
// of every layout (Table 12: L R C LFE ...); `frequencies` is one tone a channel, that one's 55 Hz.
struct LfeRoundTrip {
    std::string encode_text;        // what atmos-adm printed
    int encode_rc = -1;
    double lfe_before = 0.0;        // the LFE tone in the first master: what the bed carried
    double lfe_after = 0.0;         // ... in the second, after atmos-adm: what the stream carries
    std::size_t lfe_channels = 0;   // how many channels the second master marks as the LFE
};

constexpr std::uint32_t kLfeSampleRate = 48000;

// The first two hops, which are the reporter's: a bed of `frequencies.size()` channels, one tone
// each, encoded with atmos-cbi and decoded back to an ADM master. Returns the master.
fs::path decoded_bed_master(const fs::path& dir, std::string_view layout,
                            std::span<const double> frequencies) {
    constexpr std::uint32_t kSampleRate = kLfeSampleRate;
    const auto frames = static_cast<std::size_t>(8 * iclforge::ac3::kSamplesPerFrame);
    const std::string tag{layout};

    std::vector<std::vector<float>> channels(frequencies.size(), std::vector<float>(frames));
    for (std::size_t c = 0; c < frequencies.size(); ++c) {
        for (std::size_t n = 0; n < frames; ++n) {
            channels[c][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * frequencies[c] * static_cast<double>(n) /
                               static_cast<double>(kSampleRate)));
        }
    }
    const auto wav = dir / ("lfe_rt_" + tag + ".wav");
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), channels, kSampleRate).has_value());

    const auto cbi = dir / ("lfe_rt_" + tag + ".ec3");
    const auto master = dir / ("lfe_rt_" + tag + "_master.wav");
    REQUIRE(run_cli("atmos-cbi " + quoted(wav) + " " + quoted(cbi) + " 768 " + tag,
                    dir / ("lfe_rt_" + tag + "_cbi.log")) == 0);
    REQUIRE(run_cli("decode " + quoted(cbi) + " " + quoted(dir / ("lfe_rt_" + tag + "_out.wav")) +
                        " \"\" " + quoted(master),
                    dir / ("lfe_rt_" + tag + "_decode.log")) == 0);
    return master;
}

LfeRoundTrip lfe_round_trip(const fs::path& dir, std::string_view layout,
                            std::span<const double> frequencies) {
    constexpr std::uint32_t kSampleRate = kLfeSampleRate;
    const std::string tag{layout};
    const auto master = decoded_bed_master(dir, layout, frequencies);

    LfeRoundTrip out;
    const auto find_lfe = [](const iclforge::adm::BridgeResult& bridged)
        -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < bridged.channel_count(); ++i) {
            if (bridged.is_lfe[i]) {
                return i;
            }
        }
        return std::nullopt;
    };
    {
        const auto parsed = iclforge::adm::parse_bw64(master.string());
        REQUIRE(parsed.has_value());
        const auto bridged = iclforge::adm::build(*parsed);
        REQUIRE(bridged.has_value());
        const auto lfe = find_lfe(*bridged);
        REQUIRE(lfe.has_value());
        out.lfe_before = tone_magnitude(bridged->pcm[*lfe], kLfeHz, kSampleRate);
    }

    // The hop issue 1627 is about: the master back into an Atmos stream, and that stream decoded.
    const auto again = dir / ("lfe_rt_" + tag + "_again.ec3");
    const auto encode_log = dir / ("lfe_rt_" + tag + "_encode.log");
    out.encode_rc =
        run_cli("atmos-adm " + quoted(master) + " " + quoted(again) + " 768", encode_log);
    out.encode_text = read_log(encode_log);
    if (out.encode_rc != 0) {
        return out;
    }
    const auto master_again = dir / ("lfe_rt_" + tag + "_master_again.wav");
    REQUIRE(run_cli("decode " + quoted(again) + " " +
                        quoted(dir / ("lfe_rt_" + tag + "_out_again.wav")) + " \"\" " +
                        quoted(master_again),
                    dir / ("lfe_rt_" + tag + "_decode_again.log")) == 0);
    const auto parsed = iclforge::adm::parse_bw64(master_again.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    out.lfe_channels = bridged->lfe_count();
    if (const auto lfe = find_lfe(*bridged)) {
        out.lfe_after = tone_magnitude(bridged->pcm[*lfe], kLfeHz, kSampleRate);
    }
    return out;
}

}  // namespace

// Issue 1627, first repro. A 5.1.4 bed read out of its own ADM master used to come back with a
// silent LFE (the bridge gave it gain 0, and an object reaches the LFE as lfe_send * gain) and a
// status line counting the LFE twice: ten channels and "the bed's LFE" made eleven objects.
TEST_CASE("atmos-adm carries a bed's LFE through, and counts it once", "[cli][atmos-adm][lfe]") {
    const auto dir = scratch_dir();
    const auto result = lfe_round_trip(dir, "5.1.4", std::array<double, 10>{
                                                         220.0, 277.2, 330.0, kLfeHz, 554.4, 660.0,
                                                         740.0, 831.6, 880.0, 1108.8});
    INFO(result.encode_text);
    REQUIRE(result.encode_rc == 0);

    // Nine speaker feeds and the bed's LFE are ten objects: the LFE is the implicit one, not a channel
    // that also needs a slot.
    CHECK(contains(result.encode_text,
                   "9 bed speaker feed(s) + 0 dynamic object(s) + the bed's LFE = 10 objects"));

    // And it is still there, at the level it went in: the first master's LFE tone was audible, and the
    // stream atmos-adm made from it carries it too (-120 dBFS before, which is silence).
    CHECK(result.lfe_before > kLfeAudible);
    CHECK(result.lfe_channels == 1);
    CHECK(result.lfe_after > kLfeAudible);
    CHECK(result.lfe_after > 0.5 * result.lfe_before);
}

// Issue 1627, second repro. A 9.1.6 bed is fifteen speaker feeds and the LFE, which is what
// TS 103 420 8.3.2.2's sixteen are; atmos-adm refused it ("more channels than AtmosEncoder
// supports") because it counted the LFE against the fifteen as well.
TEST_CASE("atmos-adm takes a 9.1.6 bed: fifteen speaker feeds and the LFE", "[cli][atmos-adm][lfe]") {
    const auto dir = scratch_dir();
    const auto result = lfe_round_trip(
        dir, "9.1.6",
        std::array<double, 16>{220.0, 277.2, 330.0, kLfeHz, 554.4, 660.0, 740.0, 831.6, 880.0,
                               1108.8, 1245.0, 1318.5, 1480.0, 1661.2, 1864.7, 2093.0});
    INFO(result.encode_text);
    REQUIRE(result.encode_rc == 0);
    CHECK(contains(result.encode_text,
                   "15 bed speaker feed(s) + 0 dynamic object(s) + the bed's LFE = 16 objects"));
    CHECK(result.lfe_channels == 1);
    CHECK(result.lfe_after > kLfeAudible);
}

// The same master into AC-4 (codec=ac4). Its objects have a stream LFE object of their own, which
// the writer marks and the decoder renders to the LFE; the bridge's LFE channel used to go in as an
// ordinary object at gain 0, which AC-4 writes as -infinity dB.
TEST_CASE("atmos-adm codec=ac4 writes a bed's LFE as the stream's LFE object",
          "[cli][atmos-adm][lfe][ac4]") {
    const auto dir = scratch_dir();
    const auto master = decoded_bed_master(
        dir, "5.1.4",
        std::array<double, 10>{220.0, 277.2, 330.0, kLfeHz, 554.4, 660.0, 740.0, 831.6, 880.0,
                               1108.8});

    const auto ac4 = dir / "lfe_rt_ac4.ac4";
    const auto encode_log = dir / "lfe_rt_ac4_encode.log";
    const auto encode_rc =
        run_cli("atmos-adm " + quoted(master) + " " + quoted(ac4) + " 384 \"\" codec=ac4", encode_log);
    const auto encode_text = read_log(encode_log);
    INFO(encode_text);
    REQUIRE(encode_rc == 0);
    // Nine objects and the LFE object: the status says which is which.
    CHECK(contains(encode_text, "9 bed speaker feed(s) + 0 dynamic object(s) + the LFE object"));

    const auto rendered = dir / "lfe_rt_ac4_rendered.wav";
    REQUIRE(run_cli("decode " + quoted(ac4) + " " + quoted(rendered),
                    dir / "lfe_rt_ac4_decode.log") == 0);
    const auto wav = iclforge::ac3::io::read_wav(rendered.string());
    REQUIRE(wav.has_value());
    double loudest = 0.0;
    for (std::size_t c = 0; c < wav->channels.size(); ++c) {
        const auto magnitude = tone_magnitude(wav->channels[c], kLfeHz, wav->sample_rate);
        CAPTURE(c, magnitude);
        loudest = std::max(loudest, magnitude);
    }
    // The LFE tone is in what was rendered, in some channel: it was 0 (silence) when it went in as
    // a gain-0 object.
    CHECK(loudest > kLfeAudible);
}

namespace {

using iclforge::objects::oba::BedLabel;

// An ADM master of bed channels alone, one tone each, written the way forge's decode writes one
// (iclforge::adm::write, then write_bw64): `beds` is each channel's label and its tone's frequency.
fs::path write_bed_master(const fs::path& path,
                          std::span<const std::pair<BedLabel, double>> beds) {
    const auto frames = static_cast<std::size_t>(8 * iclforge::ac3::kSamplesPerFrame);
    std::vector<std::vector<float>> pcm(beds.size(), std::vector<float>(frames));
    std::vector<std::string> names;
    for (std::size_t c = 0; c < beds.size(); ++c) {
        names.emplace_back(iclforge::objects::oba::describe(beds[c].first));
        for (std::size_t n = 0; n < frames; ++n) {
            pcm[c][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * beds[c].second * static_cast<double>(n) /
                               static_cast<double>(kLfeSampleRate)));
        }
    }
    iclforge::adm::WriteInput input;
    input.sample_rate = kLfeSampleRate;
    for (std::size_t c = 0; c < beds.size(); ++c) {
        input.channels.push_back(
            {.name = names[c], .pcm = pcm[c], .bed_label = beds[c].first, .updates = {}});
    }
    const auto document = iclforge::adm::write(input);
    REQUIRE(document.has_value());
    REQUIRE(iclforge::adm::write_bw64(path.string(), *document).has_value());
    return path;
}

}  // namespace

// A stream has one LFE and a dual-LFE master (LFE and LFE2) has two. Neither takes an object slot;
// both are added into the one, and the run says so rather than leaving a second LFE unaccounted for.
TEST_CASE("atmos-adm adds a second LFE into the stream's one, and says so",
          "[cli][atmos-adm][lfe]") {
    const auto dir = scratch_dir();
    constexpr double kSecondLfeHz = 70.0;
    const std::array<std::pair<BedLabel, double>, 5> beds{{{BedLabel::kL, 220.0},
                                                           {BedLabel::kR, 277.2},
                                                           {BedLabel::kC, 330.0},
                                                           {BedLabel::kLfe, kLfeHz},
                                                           {BedLabel::kLfe2, kSecondLfeHz}}};
    const auto master = write_bed_master(dir / "two_lfe_master.wav", beds);

    const auto out = dir / "two_lfe.ec3";
    const auto log = dir / "two_lfe_encode.log";
    const auto rc = run_cli("atmos-adm " + quoted(master) + " " + quoted(out) + " 768", log);
    const auto text = read_log(log);
    INFO(text);
    REQUIRE(rc == 0);
    CHECK(contains(text, "warning: " + master.string() +
                             ": 2 LFE channels are summed into the stream's one LFE"));
    // Three speaker feeds and the one LFE: the second LFE is not a fifth object.
    CHECK(contains(text, "3 bed speaker feed(s) + 0 dynamic object(s) + the bed's LFE = 4 objects"));

    // Both of them are in the stream's LFE.
    const auto decoded_master = dir / "two_lfe_decoded_master.wav";
    REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(dir / "two_lfe_decoded.wav") + " \"\" " +
                        quoted(decoded_master),
                    dir / "two_lfe_decode.log") == 0);
    const auto parsed = iclforge::adm::parse_bw64(decoded_master.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->lfe_count() == 1);
    std::size_t lfe = 0;
    while (!bridged->is_lfe[lfe]) {
        ++lfe;
    }
    CHECK(tone_magnitude(bridged->pcm[lfe], kLfeHz, kLfeSampleRate) > kLfeAudible);
    CHECK(tone_magnitude(bridged->pcm[lfe], kSecondLfeHz, kLfeSampleRate) > kLfeAudible);
}

// JOC reconstructs objects from a 5.1 downmix of them, and the LFE is not one, so a master of nothing
// but its LFE has nothing to build a stream on. Refused as the input it is, for both codecs, before
// anything is written.
TEST_CASE("atmos-adm refuses a master of nothing but its LFE", "[cli][atmos-adm][lfe]") {
    const auto dir = scratch_dir();
    const std::array<std::pair<BedLabel, double>, 1> beds{{{BedLabel::kLfe, kLfeHz}}};
    const auto master = write_bed_master(dir / "lfe_only_master.wav", beds);

    for (const bool ac4 : {false, true}) {
        const auto out = dir / (ac4 ? "lfe_only.ac4" : "lfe_only.ec3");
        const auto log = dir / (ac4 ? "lfe_only_ac4.log" : "lfe_only_ec3.log");
        const auto rc = run_cli("atmos-adm " + quoted(master) + " " + quoted(out) +
                                    (ac4 ? " 384 \"\" codec=ac4" : " 768"),
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 2);  // exit_codes.hpp's kExitInput
        CHECK(contains(text, "names only LFE channel(s)"));
        CHECK_FALSE(fs::exists(out));
    }
}
