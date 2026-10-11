#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/io/wav.hpp"

// The encode front ends (apps/forge/cli/src/commands/encode.cpp: encode, eac3-encode,
// their src=/map= multi-source twins in apps/forge/cli/src/multi_source.cpp, and
// programmeN='s extra programmes) along the paths a user takes when
// something about the input or the request is wrong: every refusal's exit
// code and reason, and - for the requests that should work - what the
// encoder reports and writes. test_cli.cpp holds the main encode contracts;
// this file holds the edges of the same commands.
//
// src=/map= and the single-file path are separate implementations
// (run_encode vs run_encode_multi, and the same pair for E-AC-3), so a
// refusal that both share is asked of both.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention - including
// the Windows double-quote wrapping explained in test_cli.cpp's run_cli.

using Catch::Approx;

namespace fs = std::filesystem;

namespace {

// See apps/forge/cli/tests/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
    return iclforge::test::platform::process_id();
}

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cli_encode_paths_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(raw[i]);
    }
    return out;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// A tone per channel (a different pitch on each, 0.3 amplitude), except the
// channels listed in `silent`, which carry digital silence. 0.5 s by default:
// longer than BS.1770's 400 ms gating block, so dialnorm=auto has something
// to measure, and still only 16 AC-3 frames to encode.
fs::path write_wav(const fs::path& path, std::size_t channels, std::uint32_t rate = 48000,
                   std::initializer_list<std::size_t> silent = {}, double seconds = 0.5) {
    const auto frames = static_cast<std::size_t>(seconds * rate);
    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        if (std::find(silent.begin(), silent.end(), c) != silent.end()) {
            continue;
        }
        const double hz = 301.0 + 97.0 * static_cast<double>(c);
        for (std::size_t n = 0; n < frames; ++n) {
            data[c][n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz *
                                                           static_cast<double>(n) / rate));
        }
    }
    REQUIRE(iclforge::ac3::io::write_wav_f32(path.string(), data, rate).has_value());
    return path;
}

struct Expectation {
    std::string args;
    int exit_code;
    std::string message;
};

// Runs each row, holding it to its exit code and message and to leaving no
// output behind at `out_path` (which every row's own args name).
void check_rows(std::initializer_list<Expectation> rows, const fs::path& out_path,
                const fs::path& log) {
    for (const auto& row : rows) {
        CAPTURE(row.args);
        fs::remove(out_path);
        const auto rc = run_cli(row.args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == row.exit_code);
        CHECK(text.find(row.message) != std::string::npos);
        CHECK_FALSE(fs::exists(out_path));
    }
}

// The number `forge` printed straight after `label`, or -1.
long number_after(const std::string& text, std::string_view label) {
    const auto at = text.find(label);
    if (at == std::string::npos) {
        return -1;
    }
    return std::strtol(text.c_str() + at + label.size(), nullptr, 10);
}

// A two-source map= that keeps source 0 as stereo and drops source 1.
constexpr std::string_view kStereoMap = "map=0.0:L,0.1:R,1.0:none,1.1:none";

}  // namespace

TEST_CASE("eac3-encode refuses an unknown tool set and vbr setting on both input paths",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "tools_stereo.wav", 2);
    const auto out_path = dir / "tools.ec3";
    const auto log = dir / "tools.log";
    const std::string head = "eac3-encode " + quoted(wav) + " " + quoted(out_path) + " 192 ";
    const std::string multi = " src=" + quoted(wav) + " " + std::string{kStereoMap};
    check_rows({{head + "bogus stereo", 1, "error: unknown tool set 'bogus' (none | auto | cpl"},
                {head + "bogus stereo" + multi, 1, "error: unknown tool set 'bogus'"},
                {head + "none stereo wild", 1,
                 "error: unrecognised vbr setting 'wild' (off | q:0..1[,min:kbps][,max:kbps] | "
                 "avg:kbps"},
                {head + "none stereo wild" + multi, 1, "error: unrecognised vbr setting 'wild'"},
                {head + "none bogus" + multi, 1,
                 "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714)"}},
               out_path, log);
}

TEST_CASE("encode and eac3-encode refuse an unwritable output on both input paths",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "unwritable_stereo.wav", 2);
    const auto nowhere = dir / "no_such_directory" / "out.bin";
    const auto log = dir / "unwritable.log";
    const std::string multi = " src=" + quoted(wav) + " " + std::string{kStereoMap};
    const std::string message = "error: cannot open " + nowhere.string() + " for writing";
    check_rows({{"encode " + quoted(wav) + " " + quoted(nowhere) + " 192", 3, message},
                {"encode " + quoted(wav) + " " + quoted(nowhere) + " 192 stereo" + multi, 3,
                 message},
                {"eac3-encode " + quoted(wav) + " " + quoted(nowhere) + " 192", 3, message},
                {"eac3-encode " + quoted(wav) + " " + quoted(nowhere) + " 192 none stereo" + multi,
                 3, message}},
               nowhere, log);
}

TEST_CASE("a VBR E-AC-3 encode reports the access-unit sizes it really wrote, on both input paths",
          "[cli][encode][vbr]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "vbr_stereo.wav", 2);
    const auto log = dir / "vbr.log";
    for (const std::string& extra : {std::string{}, " src=" + quoted(wav) + " " +
                                                       std::string{kStereoMap}}) {
        CAPTURE(extra);
        const auto out_path = dir / "vbr.ec3";
        fs::remove(out_path);
        REQUIRE(run_cli("eac3-encode " + quoted(wav) + " " + quoted(out_path) +
                            " 192 none stereo q:0.5" + extra,
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("encoded 16 E-AC-3 access units (vbr q:0.500000, 48000 Hz, 2/0 stereo, "
                        "2 coded channels, tools: none)") != std::string::npos);
        const auto min_bytes = number_after(text, "access unit size: ");
        const auto max_bytes = number_after(text, std::to_string(min_bytes) + "-");
        const auto bytes = read_bytes(out_path);
        const auto frames = iclforge::ac3::split_frames(bytes);
        REQUIRE(frames.has_value());
        REQUIRE(frames->size() == 16u);
        const auto [smallest, largest] = std::ranges::minmax_element(
            *frames, {}, [](const auto& frame) { return frame.size(); });
        CHECK(min_bytes == static_cast<long>(smallest->size()));
        CHECK(max_bytes == static_cast<long>(largest->size()));
        // Variable rate means the units really do differ in size.
        CHECK(min_bytes < max_bytes);
    }
}

TEST_CASE("verify on a src=/map= E-AC-3 encode checks every access unit against the decoder",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "verify_multi.wav", 2);
    const auto out_path = dir / "verify_multi.ec3";
    const auto log = dir / "verify_multi.log";
    REQUIRE(run_cli("eac3-encode " + quoted(wav) + " " + quoted(out_path) +
                        " 192 none stereo off verify src=" + quoted(wav) + " " +
                        std::string{kStereoMap},
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("  verify: encoder and decoder agree on all 16 access units") !=
          std::string::npos);
    CHECK(text.find("  4 source channels rendered onto 2/0 stereo") != std::string::npos);
}

TEST_CASE("a src=/map= encode with no layout named follows the total source channel count",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "auto_layout.wav", 2);
    const auto log = dir / "auto_layout.log";
    // Two stereo files are four channels, which no layout carries exactly -
    // layout_for_source picks the smallest that fits, 5.1, and the two
    // dropped channels leave the rest of it silent.
    for (const std::string_view command : {"encode", "eac3-encode"}) {
        CAPTURE(command);
        const auto out_path = dir / "auto_layout.bin";
        fs::remove(out_path);
        const std::string tools = command == "eac3-encode" ? " none" : "";
        REQUIRE(run_cli(std::string{command} + " " + quoted(wav) + " " + quoted(out_path) + " 192" +
                            tools + " src=" + quoted(wav) + " " + std::string{kStereoMap},
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("  4 source channels rendered onto 5.1") != std::string::npos);
        CHECK(text.find("  silent (the source carries nothing that belongs there): C Ls Rs LFE") !=
              std::string::npos);
    }
}

TEST_CASE("the src=/map= input path refuses sources it cannot load", "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto stereo = write_wav(dir / "multi_stereo.wav", 2);
    const auto slow = write_wav(dir / "multi_22k.wav", 2, 22050);
    const auto other_rate = write_wav(dir / "multi_44k.wav", 2, 44100);
    const auto missing = dir / "multi_missing.wav";
    fs::remove(missing);
    const auto out_path = dir / "multi_refused.bin";
    const auto log = dir / "multi_refused.log";
    const std::string map{kStereoMap};
    const auto encode = [&](const fs::path& in, std::string_view rest) {
        return "encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    const auto eac3 = [&](const fs::path& in, std::string_view rest) {
        return "eac3-encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    check_rows(
        {{encode(missing, "192 stereo src=" + quoted(stereo) + " " + map), 2,
          "error: " + missing.string() + ": cannot open file"},
         {eac3(stereo, "192 none stereo src=" + quoted(missing) + " " + map), 2,
          "error: " + missing.string() + ": cannot open file"},
         {encode(stereo, "192 stereo src=" + quoted(other_rate) + " " + map), 2,
          "error: " + other_rate.string() + " is 44100 Hz, but " + stereo.string() +
              " is 48000 Hz - every source must share a sample rate"},
         {encode(slow, "192 stereo src=" + quoted(slow) + " " + map), 2,
          "error: sample rate 22050 is not legal for AC-3 (need 32/44.1/48 kHz)"}},
        out_path, log);
}

TEST_CASE("the src=/map= input path refuses a map, layout or request it cannot route",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto stereo = write_wav(dir / "route_stereo.wav", 2);
    const auto wide = write_wav(dir / "route_7ch.wav", 7);
    const auto mono = write_wav(dir / "route_mono.wav", 1);
    const auto out_path = dir / "route_refused.bin";
    const auto log = dir / "route_refused.log";
    const std::string map{kStereoMap};
    const auto encode = [&](const fs::path& in, std::string_view rest) {
        return "encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    const auto eac3 = [&](const fs::path& in, std::string_view rest) {
        return "eac3-encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    check_rows(
        {{eac3(stereo, "192 none stereo src=" + quoted(stereo)), 1,
          "error: more than one source needs map= to say where each channel goes"},
         {encode(stereo, "192 stereo src=" + quoted(stereo) + " map=0.0:L"), 1,
          "error: bad map= spec (<source>.<channel>"},
         // Ls has nowhere to go in a stereo programme.
         {eac3(stereo, "192 none stereo src=" + quoted(stereo) +
                           " map=0.0:Ls,0.1:R,1.0:none,1.1:none"),
          1, "error: map= does not resolve to a valid routing for this format"},
         {encode(stereo, "192 bogus src=" + quoted(stereo) + " " + map), 1,
          "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51)"},
         {encode(stereo, "191 stereo src=" + quoted(stereo) + " " + map), 1,
          "error: AC-3 takes only the 19 nominal rates of Table 5.18"},
         {encode(wide, "192 src=" + quoted(stereo) +
                           " map=0.0:L,0.1:R,0.2:none,0.3:none,0.4:none,0.5:none,0.6:none,"
                           "1.0:none,1.1:none"),
          1,
          "error: encode handles 1 to 6 channels (9 given); no AC-3 coding mode is wider than "
          "3/2 + LFE"},
         {encode(stereo, "192 stereo " + quoted(mono) + " src=" + quoted(stereo)), 1,
          "error: use either a second positional file or src=/map=, not both"},
         {eac3(stereo, "192 none stereo off " + quoted(mono) + " src=" + quoted(stereo)), 1,
          "error: use either a second positional file or src=/map=, not both"},
         {eac3(stereo, "192 none stereo src=" + quoted(stereo) + " programme2=" + quoted(mono)), 1,
          "error: programmeN= and src=/map= cannot be combined yet - the multi-source router "
          "assigns channels to one programme"}},
        out_path, log);
}

TEST_CASE("an AC-3 src=/map= destination the command cannot carry is warned about, not dropped "
          "silently",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "obj_warning.wav", 2);
    const auto out_path = dir / "obj_warning.ac3";
    const auto log = dir / "obj_warning.log";
    REQUIRE(run_cli("encode " + quoted(wav) + " " + quoted(out_path) + " 192 stereo src=" +
                        quoted(wav) + " map=0.0:L,0.1:R,1.0:obj,1.1:none",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("warning: 1.0 maps to 'obj', which this command has no way to carry - that "
                    "channel contributes nothing to the output") != std::string::npos);
}

TEST_CASE("offset= on a src=/map= encode delays its source and lengthens the programme",
          "[cli][encode][offset]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "offset_multi.wav", 2);
    const auto log = dir / "offset_multi.log";
    // 0.5 s is 24000 samples, 16 frames once rounded up; shifting source 1
    // by 0.05 s pushes the programme's end to 26400 samples, which is 18.
    for (const std::string_view command : {"encode", "eac3-encode"}) {
        CAPTURE(command);
        const auto out_path = dir / "offset_multi.bin";
        const std::string tools = command == "eac3-encode" ? " none" : "";
        REQUIRE(run_cli(std::string{command} + " " + quoted(wav) + " " + quoted(out_path) + " 192" +
                            tools + " stereo src=" + quoted(wav) + " " + std::string{kStereoMap} +
                            " offset=1:0.05",
                        log) == 0);
        const auto bytes = read_bytes(out_path);
        const auto frames = iclforge::ac3::split_frames(bytes);
        REQUIRE(frames.has_value());
        CHECK(frames->size() == 18u);
    }
}

TEST_CASE("the single-file encode paths refuse a rate, width or configuration they cannot code",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto slow = write_wav(dir / "single_22k.wav", 2, 22050);
    const auto wide = write_wav(dir / "single_7ch.wav", 7);
    const auto six = write_wav(dir / "single_51.wav", 6);
    const auto mono = write_wav(dir / "single_mono.wav", 1);
    const auto out_path = dir / "single_refused.bin";
    const auto log = dir / "single_refused.log";
    const auto run = [&](std::string_view command, const fs::path& in, std::string_view rest) {
        return std::string{command} + " " + quoted(in) + " " + quoted(out_path) + " " +
               std::string{rest};
    };
    check_rows({{run("encode", slow, "192"), 2,
                 "error: sample rate 22050 is not legal for AC-3 (need 32/44.1/48 kHz)"},
                {run("encode", wide, "192"), 1,
                 "error: encode handles 1 to 6 channels (7 given); no AC-3 coding mode is wider "
                 "than 3/2 + LFE"},
                {run("eac3-encode", wide, "192"), 1,
                 "error: 7 channels - no standard speaker layout has that many channels"},
                {run("eac3-encode", wide, "192 none 51"), 1,
                 "error: 7 channels - no standard speaker layout has that many channels"},
                {run("eac3-encode", six, "32 none 51"), 1,
                 "error: the encoder cannot express this configuration"},
                // One mono file is half of a 1+1 programme, on the streaming
                // path as much as the whole-file one.
                {run("eac3-encode", mono, "192 none 1+1"), 1,
                 "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or two mono "
                 "files; the source has 1 channel(s) and no second file was given"},
                {run("encode", mono, "192 1+1"), 1,
                 "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or two mono "
                 "files; the source has 1 channel(s)"}},
               out_path, log);
}

TEST_CASE("a second positional file is refused unless it can be layout 1+1's mono Ch2",
          "[cli][encode][dual-mono]") {
    const auto dir = scratch_dir();
    const auto mono = write_wav(dir / "ch2_mono.wav", 1);
    const auto stereo = write_wav(dir / "ch2_stereo.wav", 2);
    const auto other_rate = write_wav(dir / "ch2_44k.wav", 1, 44100);
    const auto missing = dir / "ch2_missing.wav";
    fs::remove(missing);
    const auto out_path = dir / "ch2_refused.ac3";
    const auto log = dir / "ch2_refused.log";
    const auto run = [&](const fs::path& first, std::string_view layout, const fs::path& second) {
        return "encode " + quoted(first) + " " + quoted(out_path) + " 192 " + std::string{layout} +
               " " + quoted(second);
    };
    check_rows({{run(stereo, "1+1", mono), 2,
                 "error: layout 1+1 with a second input file needs the first file to be mono "
                 "(Ch1); it has 2 channels"},
                {run(mono, "1+1", missing), 2, "error: " + missing.string() + ": cannot open file"},
                {run(mono, "1+1", stereo), 2,
                 "error: " + stereo.string() + " must be mono (Ch2); it has 2 channels"},
                {run(mono, "1+1", other_rate), 2,
                 "error: " + other_rate.string() +
                     " is 44100 Hz, but the first file is 48000 Hz - both programmes must share "
                     "a sample rate"},
                {run(stereo, "stereo", mono), 2,
                 "error: a second input file is only meaningful with layout 1+1 (got layout "
                 "'stereo')"}},
               out_path, log);
}

namespace {

// dialnorm=auto against a programme with nothing to measure, on one input
// path: every combination of codec and which programme is silent, since
// each of the four encode implementations measures on its own.
void check_silent_dialnorm_refusals(bool multi, std::string_view leaf) {
    const auto dir = scratch_dir();
    const auto silent = write_wav(dir / "auto_silent.wav", 2, 48000, {0, 1});
    const auto ch2_silent = write_wav(dir / "auto_ch2_silent.wav", 2, 48000, {1});
    const auto out_path = dir / (std::string{leaf} + ".bin");
    const auto log = dir / (std::string{leaf} + ".log");
    const std::string whole = "error: no audio above the -70 LKFS absolute gate; pass "
                              "dialnorm=<1..31> explicitly";
    const std::string ch1 = "error: Ch1 has no audio above the -70 LKFS absolute gate; pass "
                            "dialnorm=<1..31> explicitly";
    const std::string ch2 = "error: Ch2 has no audio above the -70 LKFS absolute gate; pass "
                            "dialnorm2=<1..31> explicitly";
    const auto run = [&](std::string_view command, const fs::path& in, std::string_view rest) {
        std::string args = std::string{command} + " " + quoted(in) + " " + quoted(out_path) +
                           " 192 " + std::string{rest};
        if (multi) {
            args += " src=" + quoted(in) + " map=0.0:" +
                    std::string{rest.find("1+1") != std::string_view::npos ? "p1,0.1:p2"
                                                                           : "L,0.1:R"} +
                    ",1.0:none,1.1:none";
        }
        return args;
    };
    check_rows(
            {{run("encode", silent, "stereo dialnorm=auto"), 5, whole},
             {run("encode", silent, "1+1 dialnorm=auto"), 5, ch1},
             {run("encode", ch2_silent, "1+1 dialnorm2=auto"), 5, ch2},
             {run("eac3-encode", silent, "none stereo dialnorm=auto"), 5, whole},
             {run("eac3-encode", silent, "none 1+1 dialnorm=auto"), 5, ch1},
             {run("eac3-encode", ch2_silent, "none 1+1 dialnorm2=auto"), 5, ch2}},
            out_path, log);
}

}  // namespace

TEST_CASE("dialnorm=auto on a single-file programme with nothing to measure is refused, not "
          "guessed",
          "[cli][encode][dialnorm]") {
    check_silent_dialnorm_refusals(false, "auto_refused_single");
}

TEST_CASE("dialnorm=auto on a src=/map= programme with nothing to measure is refused, not guessed",
          "[cli][encode][dialnorm]") {
    check_silent_dialnorm_refusals(true, "auto_refused_multi");
}

TEST_CASE("dialnorm=auto measures each 1+1 programme on its own, from one file or from src=/map=",
          "[cli][encode][dialnorm]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "auto_dual.wav", 2);
    const auto log = dir / "auto_dual.log";
    for (const bool multi : {false, true}) {
        for (const std::string_view command : {"encode", "eac3-encode"}) {
            CAPTURE(multi, command);
            const auto out_path = dir / "auto_dual.bin";
            std::string args = std::string{command} + " " + quoted(wav) + " " + quoted(out_path) +
                               " 192" + (command == "eac3-encode" ? " none" : "") +
                               " 1+1 dialnorm=auto dialnorm2=auto";
            if (multi) {
                args += " src=" + quoted(wav) + " map=0.0:p1,0.1:p2,1.0:none,1.1:none";
            }
            REQUIRE(run_cli(args, log) == 0);
            const auto text = read_log(log);
            INFO(text);
            const auto dialnorm = number_after(text, "-> dialnorm ");
            const auto dialnorm2 = number_after(text, "-> dialnorm2 ");
            CHECK(text.find("Ch1 measured ") != std::string::npos);
            CHECK(text.find("Ch2 measured ") != std::string::npos);
            // Two 0.3-amplitude tones measure about -14 LKFS apiece; what
            // matters is that each channel got its own figure in range.
            CHECK(dialnorm >= 13);
            CHECK(dialnorm <= 15);
            CHECK(dialnorm2 >= 13);
            CHECK(dialnorm2 <= 15);
        }
    }
}

TEST_CASE("an extra programme is planned from its own file: its own layout, rate and dialnorm",
          "[cli][encode][programme2]") {
    const auto dir = scratch_dir();
    const auto primary = write_wav(dir / "extra_primary.wav", 6);
    const auto mono = write_wav(dir / "extra_mono.wav", 1);
    const auto log = dir / "extra.log";

    SECTION("no programme2-layout= follows the file, at half the primary's rate") {
        const auto out_path = dir / "extra_default.ec3";
        REQUIRE(run_cli("eac3-encode " + quoted(primary) + " " + quoted(out_path) +
                            " 256 none 51 programme2=" + quoted(mono),
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("  programme 1 (\xC2\xA7" "E2.3.1.2 I1): 128 kbps, 1/0 mono, 1 coded "
                        "channels, dialnorm 31 from " +
                        mono.string()) != std::string::npos);
    }

    SECTION("programme2-dialnorm=auto measures that programme alone") {
        const auto out_path = dir / "extra_auto.ec3";
        REQUIRE(run_cli("eac3-encode " + quoted(primary) + " " + quoted(out_path) +
                            " 256 none 51 programme2=" + quoted(mono) +
                            " programme2-dialnorm=auto",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        const auto measured = number_after(text, "programme2 measured ");
        CHECK(measured == -14);
        const auto dialnorm = number_after(text, "(BS.1770-4, gated) -> dialnorm ");
        CHECK(dialnorm == 14);
        CHECK(text.find("dialnorm 14 from " + mono.string()) != std::string::npos);
    }
}

TEST_CASE("an extra programme's own file is refused when it cannot ride the primary's access units",
          "[cli][encode][programme2]") {
    const auto dir = scratch_dir();
    const auto primary = write_wav(dir / "extra_refused_primary.wav", 6);
    const auto other_rate = write_wav(dir / "extra_44k.wav", 1, 44100);
    const auto silent = write_wav(dir / "extra_silent.wav", 1, 48000, {0});
    const auto wide = write_wav(dir / "extra_7ch.wav", 7);
    const auto mono = write_wav(dir / "extra_refused_mono.wav", 1);
    const auto out_path = dir / "extra_refused.ec3";
    const auto log = dir / "extra_refused.log";
    const std::string head =
        "eac3-encode " + quoted(primary) + " " + quoted(out_path) + " 256 none 51 programme2=";
    check_rows({{head + quoted(other_rate), 1,
                 "error: programme2= is 44100 Hz but the primary programme is 48000 Hz - every "
                 "substream of an access unit codes the same frame period"},
                // Nothing above the gate is a runtime outcome (exit 5), as it
                // is for the primary programme's own dialnorm=auto.
                {head + quoted(silent) + " programme2-dialnorm=auto", 5,
                 "error: programme2 has no audio above the -70 LKFS absolute gate; pass "
                 "programme2-dialnorm=<1..31> explicitly"},
                {head + quoted(mono) + " programme2-layout=bogus", 1,
                 "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714)"},
                {head + quoted(wide) + " programme2-layout=51", 1,
                 "error: 7 channels - no standard speaker layout has that many channels"}},
               out_path, log);
}

// ---------------------------------------------------------------------------
// A WAV that states which speakers its channels are
// ---------------------------------------------------------------------------
//
// Three channels are 3/0 as FL FR FC and 2/1 as FL FR BC; four are 2/2 as
// FL FR BL BR and 3/1 as FL FR FC BC. Width cannot tell them apart, so a file
// that states its speakers (WAVE_FORMAT_EXTENSIBLE's dwChannelMask) has to be
// read by them - and what `decode` writes has to state them, or forge's own
// output could not be encoded back into the mode it came from.

namespace {

constexpr std::uint32_t kFL = 0x1;
constexpr std::uint32_t kFR = 0x2;
constexpr std::uint32_t kFC = 0x4;
constexpr std::uint32_t kLFE = 0x8;
constexpr std::uint32_t kBL = 0x10;
constexpr std::uint32_t kBR = 0x20;
constexpr std::uint32_t kBC = 0x100;
constexpr std::uint32_t kSL = 0x200;
constexpr std::uint32_t kSR = 0x400;

// File channel c carries its own pitch, so a decoded channel can be traced
// back to the file channel it came from. The LFE channel is low-passed to
// about 120 Hz by the format, so it gets a pitch inside that band.
std::vector<double> stated_tones(std::uint32_t mask, std::size_t channels) {
    std::vector<double> hz(channels);
    for (std::size_t c = 0; c < channels; ++c) {
        hz[c] = 400.0 + 150.0 * static_cast<double>(c);
    }
    if ((mask & kLFE) != 0) {
        // Channels interleave in increasing speaker-bit order, so the LFE is
        // preceded by as many channels as there are bits below it.
        hz[static_cast<std::size_t>(std::popcount(mask & (kLFE - 1)))] = 60.0;
    }
    return hz;
}

double stated_tone_hz(std::size_t c) { return 400.0 + 150.0 * static_cast<double>(c); }

fs::path write_stated_wav(const fs::path& path, std::size_t channels, std::uint32_t mask,
                          double seconds = 0.5) {
    constexpr std::uint32_t kRate = 48000;
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    const auto hz = stated_tones(mask, channels);
    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t n = 0; n < frames; ++n) {
            data[c][n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz[c] *
                                                           static_cast<double>(n) / kRate));
        }
    }
    REQUIRE(iclforge::ac3::io::write_wav_f32(path.string(), data, kRate, {}, mask).has_value());
    return path;
}

// Power at one frequency, whatever its phase (Goertzel), over the samples past
// the encoder's first two frames so the start-up transient is not measured.
double power_at(std::span<const float> x, double hz) {
    constexpr std::size_t kSkip = 3072;
    if (x.size() <= kSkip) {
        return 0.0;
    }
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (std::size_t i = kSkip; i < x.size(); ++i) {
        const double s0 = static_cast<double>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double n = static_cast<double>(x.size() - kSkip);
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (n * n);
}

// Every decoded channel must carry the tone of the file channel at its own
// WAV position, and not a trace of the others'.
void check_channels_carry_their_own_tone(const iclforge::ac3::io::WavData& decoded,
                                         std::uint32_t source_mask) {
    const auto hz = stated_tones(source_mask, decoded.channels.size());
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        const double own = power_at(decoded.channels[c], hz[c]);
        // The LFE tone is the one that must not be audible anywhere else, and
        // a channel that is silent where it should not be is no match either.
        CHECK(own > 1e-6);
        for (std::size_t other = 0; other < decoded.channels.size(); ++other) {
            if (other == c) {
                continue;
            }
            INFO("decoded channel " << c << " against the tone of file channel " << other);
            CHECK(own > 100.0 * power_at(decoded.channels[c], hz[other]));
        }
    }
}

}  // namespace

TEST_CASE("a WAV that states its speakers is encoded in the coding mode they are",
          "[cli][encode][layout][channel_mask]") {
    struct Mode {
        const char* name;
        std::uint32_t mask;
        std::size_t channels;
        // What `decode` states for that mode: the speakers of its channels in
        // the order the file interleaves them.
        std::uint32_t decoded_mask;
    };
    const Mode modes[] = {
        {"3/0", kFL | kFR | kFC, 3, kFL | kFR | kFC},
        {"2/1", kFL | kFR | kBC, 3, kFL | kFR | kBC},
        {"3/1", kFL | kFR | kFC | kBC, 4, kFL | kFR | kFC | kBC},
        {"2/2 with back speakers", kFL | kFR | kBL | kBR, 4, kFL | kFR | kBL | kBR},
        {"2/2 with side speakers", kFL | kFR | kSL | kSR, 4, kFL | kFR | kBL | kBR},
        {"3/0 + LFE", kFL | kFR | kFC | kLFE, 4, kFL | kFR | kFC | kLFE},
        {"2/1 + LFE", kFL | kFR | kLFE | kBC, 4, kFL | kFR | kLFE | kBC},
        {"3/2", kFL | kFR | kFC | kBL | kBR, 5, kFL | kFR | kFC | kBL | kBR},
        {"3/2 + LFE with side speakers", kFL | kFR | kFC | kLFE | kSL | kSR, 6,
         kFL | kFR | kFC | kLFE | kBL | kBR},
    };
    const auto dir = scratch_dir();
    for (const auto& mode : modes) {
        INFO(mode.name);
        const auto in = write_stated_wav(dir / "stated_in.wav", mode.channels, mode.mask);
        const auto ac3 = dir / "stated.ac3";
        const auto back = dir / "stated_back.wav";
        const auto log = dir / "stated.log";
        fs::remove(ac3);
        fs::remove(back);

        // No layout named: the layout follows the file, and the file said.
        REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(ac3) + " 192", log) == 0);
        INFO(read_log(log));
        REQUIRE(run_cli("decode " + quoted(ac3) + " " + quoted(back), log) == 0);
        const auto decoded = iclforge::ac3::io::read_wav(back.string());
        REQUIRE(decoded.has_value());

        // As many channels back as went in - no silent centre, LFE or
        // surround added to fill a 5.1 - each carrying its own tone, and the
        // file says which speakers they are.
        CHECK(decoded->channels.size() == mode.channels);
        CHECK(decoded->channel_mask == mode.decoded_mask);
        check_channels_carry_their_own_tone(*decoded, mode.mask);
    }
}

TEST_CASE("what decode writes is encoded back into the mode it came from",
          "[cli][encode][decode][layout][channel_mask]") {
    // The round trip a user takes without naming a layout: the decoder's own
    // WAV, handed straight back to the encoder.
    const auto dir = scratch_dir();
    const struct {
        const char* name;
        const char* layout;  // how the first stream is made
        std::size_t channels;
    } modes[] = {{"3/0", "L,C,R", 3},     {"2/1", "L,R,Cs", 3},       {"3/1", "L,C,R,Cs", 4},
                 {"2/2", "L,R,Ls,Rs", 4}, {"3/0 + LFE", "L,C,R,LFE", 4}, {"2/1 + LFE", "L,R,Cs,LFE", 4}};
    for (const auto& mode : modes) {
        INFO(mode.name);
        const auto source_wav = write_wav(dir / "rt_source.wav", mode.channels);
        const auto first = dir / "rt_first.ac3";
        const auto wav = dir / "rt_first.wav";
        const auto second = dir / "rt_second.ac3";
        const auto wav2 = dir / "rt_second.wav";
        const auto log = dir / "rt.log";
        for (const auto& p : {first, wav, second, wav2}) {
            fs::remove(p);
        }
        // The first stream names its layout as a Table E2.5 list: a plain WAV
        // has no speakers to state.
        REQUIRE(run_cli("encode " + quoted(source_wav) + " " + quoted(first) + " 192 " +
                            mode.layout,
                        log) == 0);
        INFO(read_log(log));
        REQUIRE(run_cli("decode " + quoted(first) + " " + quoted(wav), log) == 0);
        // ... and the second names nothing.
        REQUIRE(run_cli("encode " + quoted(wav) + " " + quoted(second) + " 192", log) == 0);
        INFO(read_log(log));
        REQUIRE(run_cli("decode " + quoted(second) + " " + quoted(wav2), log) == 0);

        const auto a = iclforge::ac3::io::read_wav(wav.string());
        const auto b = iclforge::ac3::io::read_wav(wav2.string());
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->channels.size() == mode.channels);
        CHECK(b->channels.size() == mode.channels);
        CHECK(a->channel_mask != 0);
        CHECK(b->channel_mask == a->channel_mask);
        // The two streams are the same mode, and the same bytes: encoding a
        // file the decoder wrote is a no-op on the speakers.
        CHECK(read_bytes(first).size() == read_bytes(second).size());
    }
}

TEST_CASE("a file that states no speakers is read by its width, as it always was",
          "[cli][encode][layout][channel_mask]") {
    // The unstated cases keep the answer they had: three channels are L R C,
    // placed on a 5.1 whose other channels stay silent. Nothing about a plain
    // WAV changes.
    const auto dir = scratch_dir();
    const auto in = write_wav(dir / "unstated_in.wav", 3);
    const auto ac3 = dir / "unstated.ac3";
    const auto back = dir / "unstated_back.wav";
    const auto log = dir / "unstated.log";
    fs::remove(ac3);
    REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(ac3) + " 192", log) == 0);
    REQUIRE(run_cli("decode " + quoted(ac3) + " " + quoted(back), log) == 0);
    const auto decoded = iclforge::ac3::io::read_wav(back.string());
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels.size() == 6);
    CHECK(decoded->channel_mask == (kFL | kFR | kFC | kLFE | kBL | kBR));

    // A stereo decode stays the plain header it was: nothing to disambiguate.
    const auto stereo_in = write_wav(dir / "unstated_stereo.wav", 2);
    const auto stereo_ac3 = dir / "unstated_stereo.ac3";
    const auto stereo_back = dir / "unstated_stereo_back.wav";
    REQUIRE(run_cli("encode " + quoted(stereo_in) + " " + quoted(stereo_ac3) + " 192", log) == 0);
    REQUIRE(run_cli("decode " + quoted(stereo_ac3) + " " + quoted(stereo_back), log) == 0);
    const auto stereo = iclforge::ac3::io::read_wav(stereo_back.string());
    REQUIRE(stereo.has_value());
    CHECK(stereo->channel_mask == 0);
    CHECK(fs::file_size(stereo_back) == 44 + stereo->frame_count() * 2 * 4);
}

TEST_CASE("a 2/1 source named onto stereo folds as 2/1", "[cli][encode][layout][channel_mask]") {
    // Lo = L + 0.707 * slev * S, per §7.8.2. By width the file is L R C and its
    // lone surround would be folded at the CENTRE level instead.
    const auto dir = scratch_dir();
    const auto in = write_stated_wav(dir / "fold_in.wav", 3, kFL | kFR | kBC);
    const auto ac3 = dir / "fold.ac3";
    const auto back = dir / "fold_back.wav";
    const auto log = dir / "fold.log";
    fs::remove(ac3);
    REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(ac3) + " 192 stereo cmixlev=-3 "
                        "surmixlev=-6",
                    log) == 0);
    INFO(read_log(log));
    REQUIRE(run_cli("decode " + quoted(ac3) + " " + quoted(back), log) == 0);
    const auto decoded = iclforge::ac3::io::read_wav(back.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2);

    // File channel 0 is L (400 Hz) and 2 is S (700 Hz): in Lo, S sits at 0.707
    // x 0.5 of L. Power ratio 0.125; the centre level (0.707) would read 0.5.
    const double l = power_at(decoded->channels[0], stated_tone_hz(0));
    const double s = power_at(decoded->channels[0], stated_tone_hz(2));
    REQUIRE(l > 0.0);
    CHECK(s / l == Approx(0.125).epsilon(0.1));
    // R carries its own tone and the same share of S.
    const double r = power_at(decoded->channels[1], stated_tone_hz(1));
    const double s_in_r = power_at(decoded->channels[1], stated_tone_hz(2));
    CHECK(s_in_r / r == Approx(0.125).epsilon(0.1));
}

TEST_CASE("a named layout sends a stated 2/1 surround to the surrounds, not the centre",
          "[cli][encode][layout][channel_mask]") {
    const auto dir = scratch_dir();
    const auto in = write_stated_wav(dir / "named_in.wav", 3, kFL | kFR | kBC);
    const auto ac3 = dir / "named.ac3";
    const auto back = dir / "named_back.wav";
    const auto log = dir / "named.log";
    fs::remove(ac3);
    REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(ac3) + " 192 51", log) == 0);
    INFO(read_log(log));
    REQUIRE(run_cli("decode " + quoted(ac3) + " " + quoted(back), log) == 0);
    const auto decoded = iclforge::ac3::io::read_wav(back.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 6);
    // WAV order of a 5.1: FL FR FC LFE BL BR. The surround tone is file
    // channel 2's.
    const double s_tone = stated_tone_hz(2);
    CHECK(power_at(decoded->channels[2], s_tone) < 1e-6);  // not the centre
    CHECK(power_at(decoded->channels[4], s_tone) > 1e-4);  // Ls
    CHECK(power_at(decoded->channels[5], s_tone) > 1e-4);  // Rs
}

TEST_CASE("transcode keeps a stream in the coding mode it is", "[cli][transcode][channel_mask]") {
    const auto dir = scratch_dir();
    const auto in = write_stated_wav(dir / "tc_in.wav", 3, kFL | kFR | kBC);
    const auto ac3 = dir / "tc.ac3";
    const auto ec3 = dir / "tc.ec3";
    const auto back = dir / "tc_back.wav";
    const auto log = dir / "tc.log";
    fs::remove(ac3);
    fs::remove(ec3);
    REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(ac3) + " 192", log) == 0);
    REQUIRE(run_cli("transcode " + quoted(ac3) + " " + quoted(ec3) + " 192", log) == 0);
    INFO(read_log(log));
    REQUIRE(run_cli("decode " + quoted(ec3) + " " + quoted(back), log) == 0);
    const auto decoded = iclforge::ac3::io::read_wav(back.string());
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels.size() == 3);
    CHECK(decoded->channel_mask == (kFL | kFR | kBC));
    check_channels_carry_their_own_tone(*decoded, kFL | kFR | kBC);
}

TEST_CASE("dialnorm=auto meters a stated 2/1 by its speakers", "[cli][encode][dialnorm][channel_mask]") {
    // The same samples, stated and unstated. All the energy is in the third
    // channel: BS.1770 weights a surround +1.5 dB and a centre 0 dB, so a file
    // that says its third channel is the back centre measures 1.5 LKFS louder
    // than the guess that it is the centre.
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    const std::size_t frames = 2 * kRate;
    std::vector<std::vector<float>> data(3, std::vector<float>(frames, 0.0f));
    for (std::size_t n = 0; n < frames; ++n) {
        data[2][n] = static_cast<float>(
            0.2 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(n) / kRate));
    }
    const auto stated = dir / "dn_stated.wav";
    const auto plain = dir / "dn_plain.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(stated.string(), data, kRate, {}, kFL | kFR | kBC)
                .has_value());
    REQUIRE(iclforge::ac3::io::write_wav_f32(plain.string(), data, kRate).has_value());

    const auto measured_lkfs = [&](const fs::path& in) -> double {
        const auto log = dir / "dn.log";
        const auto out = dir / "dn.ac3";
        REQUIRE(run_cli("encode " + quoted(in) + " " + quoted(out) + " 192 dialnorm=auto", log) ==
                0);
        const auto text = read_log(log);
        const auto at = text.find("measured ");
        REQUIRE(at != std::string::npos);
        return std::stod(text.substr(at + std::string_view{"measured "}.size()));
    };
    const double as_surround = measured_lkfs(stated);
    const double as_centre = measured_lkfs(plain);
    CHECK(as_surround - as_centre == Approx(1.5).margin(0.1));
}

TEST_CASE("levels names a WAV's channels by the speakers it states", "[cli][levels][channel_mask]") {
    // Three channels, one meter row each: the third is the lone surround of a
    // 2/1 programme when the file says so, and the right channel of a 3/0 one
    // (A/52 order L C R) when it says nothing.
    const auto dir = scratch_dir();
    const auto log = dir / "levels.log";

    const auto stated = write_stated_wav(dir / "levels_stated.wav", 3, kFL | kFR | kBC);
    REQUIRE(run_cli("levels " + quoted(stated), log) == 0);
    const auto with_mask = read_log(log);
    INFO(with_mask);
    CHECK(with_mask.find("as 2/1") != std::string::npos);
    CHECK(with_mask.find("  S ") != std::string::npos);

    const auto plain = write_wav(dir / "levels_plain.wav", 3);
    REQUIRE(run_cli("levels " + quoted(plain), log) == 0);
    const auto without = read_log(log);
    INFO(without);
    CHECK(without.find("as 3/0") != std::string::npos);
    CHECK(without.find("  S ") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Annex C karaoke: `decode ... karaoke`
// ---------------------------------------------------------------------------

namespace {

// A 3/2 stream - L, M, R, V1, V2 as five tones - flagged `bsmod` and with the
// surround level -6 dB (0.5), the one the karaoke vocals are mixed at.
fs::path write_karaoke_candidate(const fs::path& dir, const std::string& name,
                                 const std::string& bsmod_option) {
    const auto source = write_wav(dir / (name + "_src.wav"), 5);
    const auto stream = dir / (name + ".ac3");
    fs::remove(stream);
    const auto log = dir / (name + "_encode.log");
    REQUIRE(run_cli("encode " + quoted(source) + " " + quoted(stream) +
                        " 448 L,C,R,Ls,Rs surmixlev=-6 cmixlev=-3 " + bsmod_option,
                    log) == 0);
    return stream;
}

iclforge::ac3::io::WavData decode_to_wav(const fs::path& stream, const fs::path& wav,
                                         const std::string& options, std::string* report = nullptr) {
    fs::remove(wav);
    const auto log = wav.parent_path() / (wav.stem().string() + ".log");
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(wav) + " " + options, log) == 0);
    if (report != nullptr) {
        *report = read_log(log);
    }
    const auto read = iclforge::ac3::io::read_wav(wav.string());
    REQUIRE(read.has_value());
    return *read;
}

}  // namespace

TEST_CASE("decode karaoke writes a karaoke stream as L C R at Table C.2.2's levels",
          "[cli][decode][karaoke]") {
    const auto dir = scratch_dir();
    const auto stream = write_karaoke_candidate(dir, "k32", "bsmod=7");

    std::string report;
    const auto coded = decode_to_wav(stream, dir / "k32_coded.wav", "");
    const auto heard = decode_to_wav(stream, dir / "k32_heard.wav", "karaoke", &report);
    INFO(report);

    // Three channels where five were coded, and the file says which: FL FR FC,
    // which is L, R, C in a WAV and Lk, Rk, Ck here.
    REQUIRE(coded.channels.size() == 5);
    REQUIRE(heard.channels.size() == 3);
    CHECK(heard.channel_mask == (kFL | kFR | kFC));
    CHECK(report.find("karaoke 3/0") != std::string::npos);

    // The coded WAV is FL FR FC BL BR = L R M V1 V2. Table C.2.2, 3/0: the
    // pair of vocals into the left and the right at the stream's surround
    // level (0.5), the melody into the centre, all scaled by 1 / (1 + 0.5).
    constexpr double kScale = 1.0 / 1.5;
    REQUIRE(heard.frame_count() == coded.frame_count());
    double worst = 0.0;
    double loudest = 0.0;
    for (std::size_t i = 0; i < heard.frame_count(); ++i) {
        const double l = static_cast<double>(coded.channels[0][i]);
        const double r = static_cast<double>(coded.channels[1][i]);
        const double m = static_cast<double>(coded.channels[2][i]);
        const double v1 = static_cast<double>(coded.channels[3][i]);
        const double v2 = static_cast<double>(coded.channels[4][i]);
        worst = std::max(worst, std::abs(static_cast<double>(heard.channels[0][i]) - (l + 0.5 * v1) * kScale));
        worst = std::max(worst, std::abs(static_cast<double>(heard.channels[1][i]) - (r + 0.5 * v2) * kScale));
        worst = std::max(worst, std::abs(static_cast<double>(heard.channels[2][i]) - m * kScale));
        loudest = std::max(loudest, std::abs(static_cast<double>(heard.channels[2][i])));
    }
    CHECK(worst < 1e-6);
    CHECK(loudest > 0.05);  // a real signal in the centre, not silence agreeing with silence
}

TEST_CASE("decode karaoke at a stereo or mono target is the Lo/Ro downmix it always was",
          "[cli][decode][karaoke]") {
    // Annex C: the 2/0 reproduction IS the Lo/Ro downmix, so asking for
    // karaoke at channels=2 changes nothing, and the report says why.
    const auto dir = scratch_dir();
    const auto stream = write_karaoke_candidate(dir, "k32s", "bsmod=7");
    std::string report;
    const auto plain = decode_to_wav(stream, dir / "k32s_plain.wav", "channels=2");
    const auto karaoke = decode_to_wav(stream, dir / "k32s_karaoke.wav", "channels=2 karaoke", &report);
    INFO(report);
    REQUIRE(plain.channels.size() == 2);
    CHECK(karaoke.channels == plain.channels);
    CHECK(report.find("2/0 reproduction") != std::string::npos);

    // There is no Lt/Rt karaoke in Annex C, so the combination is refused
    // rather than quietly taking one or the other.
    const auto log = dir / "k32s_ltrt.log";
    const auto refused = dir / "k32s_ltrt.wav";
    fs::remove(refused);
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(refused) + " downmix=ltrt karaoke",
                  log) != 0);
    CHECK(read_log(log).find("Lt/Rt") != std::string::npos);
    CHECK_FALSE(fs::exists(refused));
}

TEST_CASE("decode karaoke leaves a stream that is not karaoke as it was, and says so",
          "[cli][decode][karaoke]") {
    const auto dir = scratch_dir();
    // The same five tones as a complete main service: bsmod 0.
    const auto stream = write_karaoke_candidate(dir, "k32n", "bsmod=0");
    std::string report;
    const auto plain = decode_to_wav(stream, dir / "k32n_plain.wav", "");
    const auto asked = decode_to_wav(stream, dir / "k32n_asked.wav", "karaoke", &report);
    INFO(report);
    REQUIRE(plain.channels.size() == 5);
    CHECK(asked.channels == plain.channels);
    CHECK(asked.channel_mask == plain.channel_mask);
    CHECK(report.find("bsmod 0") != std::string::npos);
    CHECK(report.find("decoded as coded") != std::string::npos);

    // karaoke=off is the default spelled out; anything else is not a value.
    const auto off = decode_to_wav(stream, dir / "k32n_off.wav", "karaoke=off");
    CHECK(off.channels == plain.channels);
    const auto log = dir / "k32n_bad.log";
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(dir / "k32n_bad.wav") + " karaoke=maybe",
                  log) != 0);
    CHECK(read_log(log).find("'v1+v2'") != std::string::npos);
}

TEST_CASE("decode karaoke=none|v1|v2|v1+v2 is the karaoke-capable decoder's Table C.2.3",
          "[cli][decode][karaoke]") {
    const auto dir = scratch_dir();
    const auto stream = write_karaoke_candidate(dir, "k32c", "bsmod=7");
    const auto coded = decode_to_wav(stream, dir / "k32c_coded.wav", "");
    REQUIRE(coded.channels.size() == 5);  // FL FR FC BL BR = L R M V1 V2
    const std::size_t frames = coded.frame_count();
    const auto at = [&](std::size_t channel, std::size_t i) {
        return static_cast<double>(coded.channels[channel][i]);
    };
    const double clev = 0.7071067811865476;  // the candidate's cmixlev=-3

    // 3/0 reproduction. Each choice is a column of the table's right-hand half,
    // and all three outputs share one scale-down factor.
    const auto three = [&](const std::string& choice, double scale, auto left, auto centre,
                           auto right) {
        std::string report;
        const auto heard =
            decode_to_wav(stream, dir / ("k32c_" + choice + ".wav"), "karaoke=" + choice, &report);
        INFO(report);
        REQUIRE(heard.channels.size() == 3);
        CHECK(heard.channel_mask == (kFL | kFR | kFC));
        CHECK(report.find("C.2.3.2") != std::string::npos);
        REQUIRE(heard.frame_count() == frames);
        double worst = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            worst = std::max(worst, std::abs(static_cast<double>(heard.channels[0][i]) - scale * left(i)));
            worst = std::max(worst, std::abs(static_cast<double>(heard.channels[2][i]) - scale * centre(i)));
            worst = std::max(worst, std::abs(static_cast<double>(heard.channels[1][i]) - scale * right(i)));
        }
        CHECK(worst < 1e-6);
    };
    // heard.channels is in the WAV's FL FR FC order: Lk, Rk, Ck.
    const auto l = [&](std::size_t i) { return at(0, i); };
    const auto r = [&](std::size_t i) { return at(1, i); };
    const auto m = [&](std::size_t i) { return at(2, i); };
    const auto v1 = [&](std::size_t i) { return at(3, i); };
    const auto v2 = [&](std::size_t i) { return at(4, i); };
    three("none", 1.0, l, m, r);
    three("v1", 0.5, l, [&](std::size_t i) { return m(i) + v1(i); }, r);
    three("v2", 0.5, l, [&](std::size_t i) { return m(i) + v2(i); }, r);
    three("v1+v2", 0.5, [&](std::size_t i) { return l(i) + v1(i); }, m,
          [&](std::size_t i) { return r(i) + v2(i); });

    // 2/0 reproduction at a stereo and a mono target: the melody at clev, a
    // single vocal at 0.7 in both channels, a pair left and right at unity.
    const auto two = [&](const std::string& choice, auto lo, auto ro, double scale) {
        const auto heard = decode_to_wav(stream, dir / ("k32c_s_" + choice + ".wav"),
                                         "channels=2 karaoke=" + choice);
        REQUIRE(heard.channels.size() == 2);
        double worst = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            worst = std::max(worst, std::abs(static_cast<double>(heard.channels[0][i]) - scale * lo(i)));
            worst = std::max(worst, std::abs(static_cast<double>(heard.channels[1][i]) - scale * ro(i)));
        }
        CHECK(worst < 1e-6);
    };
    two("v2", [&](std::size_t i) { return l(i) + clev * v2(i) + clev * m(i); },
        [&](std::size_t i) { return r(i) + clev * v2(i) + clev * m(i); }, 1.0 / (1.0 + 2.0 * clev));
    two("v1+v2", [&](std::size_t i) { return l(i) + v1(i) + clev * m(i); },
        [&](std::size_t i) { return r(i) + v2(i) + clev * m(i); }, 1.0 / (2.0 + clev));

    const auto mono = decode_to_wav(stream, dir / "k32c_mono.wav", "channels=1 karaoke=v1+v2");
    REQUIRE(mono.channels.size() == 1);
    double worst = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double expected =
            (l(i) + v1(i) + clev * m(i) + r(i) + v2(i) + clev * m(i)) / (4.0 + 2.0 * clev);
        worst = std::max(worst, std::abs(static_cast<double>(mono.channels[0][i]) - expected));
    }
    CHECK(worst < 1e-6);

    // Annex C has no Lt/Rt reproduction, for either kind of decoder.
    const auto log = dir / "k32c_ltrt.log";
    const auto refused = dir / "k32c_ltrt.wav";
    fs::remove(refused);
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(refused) + " downmix=ltrt karaoke=v1",
                  log) != 0);
    CHECK_FALSE(fs::exists(refused));
}

TEST_CASE("karaoke is decode's option and AC-3's: other commands refuse it", "[cli][decode][karaoke]") {
    const auto dir = scratch_dir();
    const auto log = dir / "k_other.log";
    // monitor and play name channels by the coded layout the reproduction
    // replaces, so the option is not theirs; an unknown option is refused.
    CHECK(run_cli("levels " + quoted(write_karaoke_candidate(dir, "k_other", "bsmod=7")) +
                      " karaoke",
                  log) != 0);
    CHECK(read_log(log).find("unknown option") != std::string::npos);
}
