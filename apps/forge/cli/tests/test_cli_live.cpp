#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/core/crc16.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/objects/emdf.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/objects/oamd.hpp"

// The device-facing half of forge: devices/outputs/record/live/monitor.
//
// These commands are the only place in this repository where the audio
// backend, the lock-free SPSC ring, the silence watchdog and the clock-drift
// servo are driven together by real code rather than by a unit test - and
// they were also the only part of apps/forge/cli/src that no test touched at all
// (commands/audio_io.cpp and commands/live_audio.cpp both measured 0.0% line
// coverage when tools/checks/coverage_report.sh was first pointed at apps/,
// coverage floors).
//
// Every case here is written to hold on a machine with a working capture or
// render endpoint AND on a headless CI container with neither, because that
// is the only assertion worth making about a device path in CI: whichever
// way the enumeration goes, the command must terminate, say which way it
// went, and never fail silently. That is a weaker claim than "recording
// works" and deliberately so - it is the claim that can actually be checked
// without hardware, and it is exactly the claim the stdout/stderr-leak and
// argv-mangling bugs in this CLI's history broke.
//
// [concurrency] on every case: this file and libs/audio/tests/ are what the
// ThreadSanitizer leg runs (`ctest -L concurrency` - see
// CMakePresets.json's test-linux-llvm-tsan preset). A race between the
// capture callback thread and the encoder thread is invisible to the
// ASan+UBSan leg, and these are the paths that start those threads.

namespace fs = std::filesystem;

namespace {

// See apps/forge/cli/tests/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() { return iclforge::test::platform::process_id(); }

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cli_live_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

// Same subprocess shape as apps/forge/cli/tests/test_cli.cpp's own run_cli; the platform
// differences live in tests/support/platform/process.hpp's run_shell, not here.
// `redirects` follows the arguments on the command line.
int run_cli_redirected(const std::string& args, const std::string& redirects) {
    const std::string command = "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + redirects;
    return iclforge::test::platform::run_shell(command);
}

int run_cli(const std::string& args, const fs::path& log) {
    return run_cli_redirected(args, " > \"" + log.string() + "\" 2>&1");
}

// stdout and stderr in separate files. quiet's contract is about which of the
// two a line reaches - nothing on stdout, errors still on stderr - and the one
// log run_cli merges them into cannot show that.
int run_cli_split(const std::string& args, const fs::path& out, const fs::path& err) {
    return run_cli_redirected(args, " > \"" + out.string() + "\" 2> \"" + err.string() + "\"");
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Half a second of silent 5.1 E-AC-3 whose every frame carries `program` as an
// OAMD payload (TS 103 420 §5.5) in an EMDF container, with no JOC payload
// beside it: a decoder reads the object layer and reconstructs no object
// audio. This is for the program shapes this project's own encoder never
// writes - AtmosEncoder's programs are always dynamic objects plus the bed's
// LFE.
void write_oamd_stream(const fs::path& path, const iclforge::objects::oba::Program& program,
                       std::span<const iclforge::objects::oba::DynamicObject> objects) {
    const auto payload = iclforge::objects::oba::build_payload(program, objects);
    const std::vector<iclforge::objects::emdf::Payload> payloads = {
        {.id = iclforge::objects::emdf::kPayloadIdOamd, .bytes = payload}};
    const auto container = iclforge::objects::emdf::build_container(payloads);

    iclforge::ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true}};
    const std::vector<float> silence(static_cast<std::size_t>(encoder.samples_per_frame()), 0.0F);
    const std::vector<std::span<const float>> channels(
        static_cast<std::size_t>(encoder.channel_count()), silence);
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    for (int frame_index = 0; frame_index < 16; ++frame_index) {
        const auto frame = encoder.encode_frame(channels, container);
        REQUIRE(frame.has_value());
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
}

// A §E2.3.1.2 legacy-core delivery whose object layer rides in the Annex E
// dependent, not the core: an AC-3 syncframe carrying a silent 5.1 bed,
// immediately followed by a dependent extending it to 7.1 (same shape as
// libs/ac3/tests/decoder/test_eac3_decoder.cpp's "an AC-3 core plus an E-AC-3
// dependent decodes to 7.1"), whose skip field carries `program`/`objects`
// as an OAMD payload with no JOC beside it - write_oamd_stream's own
// convention, above. The core cannot carry the container itself - plain
// AC-3 has no skip-field syntax at all - which is exactly why a real
// legacy-core Atmos delivery puts its object layer in the dependent instead
// (decoder.hpp's DecodedAccessUnit::object_metadata comment).
void write_legacy_core_oamd_stream(const fs::path& path,
                                   const iclforge::objects::oba::Program& program,
                                   std::span<const iclforge::objects::oba::DynamicObject> objects) {
    const auto payload = iclforge::objects::oba::build_payload(program, objects);
    const std::vector<iclforge::objects::emdf::Payload> payloads = {
        {.id = iclforge::objects::emdf::kPayloadIdOamd, .bytes = payload}};
    const auto container = iclforge::objects::emdf::build_container(payloads);

    iclforge::ac3::FrameEncoder core{
        {.bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true}};
    iclforge::ac3::eac3::FrameEncoder dependent{{.bitrate_kbps = 192,
                                       .acmod = iclforge::ac3::Acmod::k2_2,
                                       .strmtyp = iclforge::ac3::eac3::StreamType::kDependent,
                                       .substreamid = 0,
                                       .chanmap = iclforge::ac3::eac3::chanmap::k71Rear,
                                       .last_dependent = true}};

    const std::vector<float> core_silence(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame),
                                          0.0F);
    const std::vector<std::span<const float>> core_channels(
        static_cast<std::size_t>(core.channel_count()), core_silence);
    const std::vector<float> dep_silence(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame),
                                         0.0F);
    const std::vector<std::span<const float>> dep_channels(
        static_cast<std::size_t>(dependent.channel_count()), dep_silence);

    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    for (int frame_index = 0; frame_index < 16; ++frame_index) {
        const auto core_frame = core.encode_frame(core_channels);
        REQUIRE(core_frame.has_value());
        out.write(reinterpret_cast<const char*>(core_frame->data()),
                  static_cast<std::streamsize>(core_frame->size()));

        // TS 103 420 §8.2: the container rides the last (here, only)
        // dependent substream of the programme.
        const auto dep_frame = dependent.encode_frame(dep_channels, container);
        REQUIRE(dep_frame.has_value());
        out.write(reinterpret_cast<const char*>(dep_frame->data()),
                  static_cast<std::streamsize>(dep_frame->size()));
    }
}

// A device command has two legitimate outcomes and no third one: it did the
// work, or it said why it could not. What must never happen is a zero exit
// with nothing written, or a non-zero exit with nothing written - both of
// which look identical to a caller and are how a silently broken device path
// survives CI.
void check_spoke_either_way(int rc, const std::string& output) {
    CHECK_FALSE(output.empty());
    if (rc != 0) {
        CHECK(output.find("error") != std::string::npos);
    }
    // Under the ThreadSanitizer leg this binary is instrumented too, and a
    // race inside forge would end the SUBPROCESS - which every check above
    // would otherwise read as an ordinary "no device here" refusal. TSan's
    // report never says "error", so the check above already fails on one by
    // accident; this says so on purpose, and names the reason in the output.
    CHECK(output.find("ThreadSanitizer") == std::string::npos);
    CHECK(output.find("AddressSanitizer") == std::string::npos);
    CHECK(output.find("runtime error:") == std::string::npos);
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& data) {
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
}

// Overwrite `count` bits at `offset` and restore the syncframe's trailing
// crc2, so a patched frame is still a legal, CRC-clean syncframe and the
// decoder's own semantic checks (not a CRC failure) are what reject it.
// Copied from libs/ac3/tests/decoder/test_eac3_decoder.cpp's own helper of the same
// name - see that file's "the E-AC-3 decoder rejects malformed spectral
// extension streams" test, which this file's own "monitor reports a decode
// failure" test below reuses field-for-field.
void patch_bits(std::vector<std::byte>& frame, std::size_t offset, int count,
                std::uint32_t value) {
    for (int i = 0; i < count; ++i) {
        const std::size_t bit = offset + static_cast<std::size_t>(i);
        const auto mask = static_cast<std::uint8_t>(0x80U >> (bit & 7U));
        const auto set = (value >> (count - 1 - i)) & 1U;
        auto& target = frame[bit >> 3];
        target = set != 0 ? (target | std::byte{mask}) : (target & static_cast<std::byte>(~mask));
    }
    const auto bytes = frame.size();
    const std::uint16_t crc2 =
        iclforge::ac3::crc16(std::span<const std::byte>{frame}.subspan(2, bytes - 4));
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
}

// A second of a 5.1 main and a second of a mono description as two independent
// substreams of one E-AC-3 stream (§E2.3.1.2's I0 and I1), written the way the
// documentation writes one: eac3-encode with programme2=. Silent, because on a
// machine with speakers 'monitor' plays what it is given. `description_options`
// are more programme2- tokens (the service's bsmod, its gains).
fs::path write_two_programme_stream(const std::string& name,
                                    const std::string& description_options = "") {
    const auto dir = scratch_dir();
    const auto main_wav = dir / (name + "_main.wav");
    const auto description_wav = dir / (name + "_description.wav");
    const std::vector<std::vector<float>> bed(6, std::vector<float>(48000, 0.0F));
    const std::vector<std::vector<float>> voice(1, std::vector<float>(48000, 0.0F));
    REQUIRE(iclforge::ac3::io::write_wav_f32(main_wav.string(), bed, 48000).has_value());
    REQUIRE(iclforge::ac3::io::write_wav_f32(description_wav.string(), voice, 48000).has_value());
    const auto stream = dir / (name + ".ec3");
    REQUIRE(run_cli("eac3-encode \"" + main_wav.string() + "\" \"" + stream.string() +
                        "\" 448 none 51 off programme2=\"" + description_wav.string() +
                        "\" programme2-layout=mono programme2-bitrate=96" + description_options,
                    dir / (name + "_make.log")) == 0);
    return stream;
}

}  // namespace

TEST_CASE("devices enumerates or explains itself, and never does neither",
          "[cli][audio-io][concurrency]") {
    const auto log = scratch_dir() / "devices.log";
    const auto rc = run_cli("devices", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    // Whichever branch ran, it named what it was talking about: either the
    // "no active capture endpoints found" line, the table's own header, or
    // the platform's reason for having no capture capability at all.
    CHECK((out.find("capture") != std::string::npos || out.find("idx") != std::string::npos ||
           out.find("unavailable") != std::string::npos));
}

TEST_CASE("outputs enumerates or explains itself, and points at the spdif substitute",
          "[cli][audio-io][concurrency]") {
    const auto log = scratch_dir() / "outputs.log";
    const auto rc = run_cli("outputs", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    // main.cpp's Needs::kPassthrough branch is the one refusal in this CLI
    // that offers a portable alternative rather than just saying no; when it
    // is the branch that ran, the offer has to actually be there.
    if (out.find("is unavailable on this platform") != std::string::npos) {
        CHECK(out.find("forge spdif") != std::string::npos);
    }
}

// 'identify' (apps/forge/cli/src/commands/audio_io.cpp's run_identify) had no test in
// this suite at all until the three below - added to the CLI after
// devices/outputs/record/live/monitor above, so coverage floors never
// reached it either.

TEST_CASE("identify walks an output's speakers, or refuses by name",
          "[cli][audio-io][concurrency]") {
    // Bare invocation: default endpoint, the endpoint's own speakers as the
    // layout (falling back to stereo), a 2 s walk, no patch, -20 dB - the
    // same "runs for real or says why not" contract as every other
    // device-facing case in this file.
    const auto log = scratch_dir() / "identify.log";
    const auto rc = run_cli("identify", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
}

TEST_CASE("identify refuses an out-of-range device index before touching a layout or a patch",
          "[cli][audio-io]") {
    // 99999 can never be a valid index: either enumeration itself fails
    // (reported by iclforge::audio::describe()) or it succeeds with far fewer
    // entries than that (run_identify's own "no render endpoint with index"
    // refusal) - deterministic either way, with or without real render
    // hardware, and reached before layout/routing parsing or any device open.
    const auto log = scratch_dir() / "identify_bad_index.log";
    const auto rc = run_cli("identify 99999", log);
    const auto out = read_log(log);
    INFO(out);
    REQUIRE(rc != 0);
    CHECK(out.find("error") != std::string::npos);
}

TEST_CASE("identify refuses a layout that does not parse, before any device is touched",
          "[cli][audio-io]") {
    // device_index -1 (the default endpoint) skips run_identify's own
    // enumeration checks entirely - they only run for an explicit index >= 0
    // - so layout parsing is reached deterministically regardless of what
    // render hardware, if any, this machine has. A build with no monitor
    // backend at all still refuses at main.cpp's Needs::kMonitor gate first,
    // same caveat as 'play's "too short to hold a syncframe" case.
    const auto log = scratch_dir() / "identify_bad_layout.log";
    const auto rc = run_cli("identify -1 not-a-layout 1", log);
    const auto out = read_log(log);
    INFO(out);
    CHECK(rc != 0);
    if (out.find("is unavailable on this platform") == std::string::npos) {
        CHECK(out.find("is not a layout") != std::string::npos);
    }
}

TEST_CASE("record either captures a real endpoint or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "record.ac3";
    const auto log = dir / "record.log";
    fs::remove(out_path);

    // One second at the default bitrate: long enough to start the capture
    // thread, the ring and the watchdog on a machine that has an endpoint,
    // short enough not to matter to the suite's runtime on one that does not.
    const auto rc = run_cli("record \"" + out_path.string() + "\" 1", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);

    // The invariant that holds on both machines: a refusal leaves nothing
    // behind. A half-written .ac3 from a command that reported failure is
    // exactly the state 'keep-partial' exists to make explicit elsewhere.
    if (rc != 0) {
        CHECK_FALSE(fs::exists(out_path));
    } else {
        CHECK(fs::exists(out_path));
        CHECK(fs::file_size(out_path) > 0);
    }
}

TEST_CASE("live either runs a capture-to-monitor session or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "live.ac3";
    const auto log = dir / "live.log";
    fs::remove(out_path);
    // <out> <capture_device> [seconds] - capture device 0 and a one-second
    // session, the shortest run main.cpp's argument table accepts.
    const auto rc = run_cli("live \"" + out_path.string() + "\" 0 1", log);
    check_spoke_either_way(rc, read_log(log));
    if (rc != 0) {
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("a malformed positions= token is refused, by name whenever a device let it get there",
          "[cli][audio-io][concurrency]") {
    // A bad positions= token is checked well after the capture_device index
    // is (run_live's own order: device range checks, then mode/positions
    // validation, then capture.start()), so on a headless CI container with
    // no capture endpoint at all, device index 0 is out of range FIRST and
    // that is the refusal actually reported - still a refusal, just not
    // about positions=. The unconditional claim is "refused, nothing
    // written"; the positions=-specific claim only holds once a device
    // error is ruled out.
    const auto out_path = scratch_dir() / "positions_malformed.ac3";
    const auto log = scratch_dir() / "positions_malformed.log";
    fs::remove(out_path);
    const auto rc = run_cli(
        "live \"" + out_path.string() + "\" 0 1 192 -2 -2 atmos positions=not-a-real-token", log);
    const auto out = read_log(log);
    REQUIRE(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    if (out.find("capture device index") == std::string::npos) {
        CHECK(out.find("positions=") != std::string::npos);
        CHECK(out.find("scheme") != std::string::npos);
    }
}

TEST_CASE("positions= is refused outright with mode=channels",
          "[cli][audio-io][concurrency]") {
    // Unlike the malformed-token case above, this is a pure input-shape
    // conflict - live_audio.cpp checks it before any device enumeration, so
    // the refusal is unconditional once run_live() actually runs. On a build
    // with no capture backend compiled in at all (CI's no-alsa/posix leg),
    // main.cpp refuses the whole 'live' command one level higher, before
    // run_live() is ever reached, with its own "unavailable on this
    // platform" message - still a refusal, just not this one.
    const auto out_path = scratch_dir() / "positions_channels.ac3";
    const auto log = scratch_dir() / "positions_channels.log";
    fs::remove(out_path);
    const auto rc = run_cli(
        "live \"" + out_path.string() + "\" 0 1 192 -2 -2 channels positions=osc:9000", log);
    const auto out = read_log(log);
    REQUIRE(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    if (out.find("is unavailable on this platform") == std::string::npos) {
        CHECK(out.find("positions=") != std::string::npos);
    }
}

TEST_CASE("live mode=atmos positions=osc either runs a live-driven session or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "live_positions.ec3";
    const auto log = dir / "live_positions.log";
    fs::remove(out_path);
    // port 0 asks the OS for an ephemeral port - this can never collide with
    // anything else running on the machine, the same reason
    // libs/audio/tests/test_live_positions.cpp binds the same way.
    const auto rc =
        run_cli("live \"" + out_path.string() + "\" 0 1 192 -2 -2 atmos positions=osc:local:0",
                log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    if (rc == 0) {
        // A real session ran: the listener status line is unconditional
        // whenever positions= is honoured, whether or not anything was
        // actually sent to it.
        CHECK(out.find("positions: OSC on") != std::string::npos);
        CHECK(fs::exists(out_path));
    } else {
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("live mode=atmos with no positions= runs the built-in orbit, or refuses by name",
          "[cli][audio-io][atmos][concurrency]") {
    // Every mode=atmos case above supplies positions=, so the synthetic
    // orbit run_live falls back to whenever no live position source is bound
    // (its own per-frame angle/height computation) has never run through
    // this suite - only the OSC-driven path has.
    const auto dir = scratch_dir();
    const auto out_path = dir / "live_orbit.ec3";
    const auto log = dir / "live_orbit.log";
    fs::remove(out_path);
    const auto rc = run_cli("live \"" + out_path.string() + "\" 0 1 192 -2 -2 atmos", log);
    check_spoke_either_way(rc, read_log(log));
    if (rc != 0) {
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("monitor describes a stream's object layer the way decode does",
          "[cli][audio-io][atmos][concurrency]") {
    // monitor printed its own copy of decode's object-count line, and the copy
    // kept only the form this project's own streams need - "N dynamic objects
    // + the bed's LFE = M objects" - whatever the program was. decode names a
    // bed program's channels instead, and counts the LFE only when there is
    // one, so monitor misdescribed both programs below. Each stream is built
    // here because AtmosEncoder writes neither, and each is silent, like the
    // cases below, because on a machine with speakers monitor plays it.
    //
    // decode's report is checked on every machine. monitor prints its own only
    // once a render endpoint opens; without one, the check is that it spoke.
    const auto dir = scratch_dir();
    const auto check_both = [&dir](const std::string& name,
                                   const iclforge::objects::oba::Program& program,
                                   std::span<const iclforge::objects::oba::DynamicObject> objects,
                                   const std::string& line) {
        const auto stream = dir / (name + ".ec3");
        write_oamd_stream(stream, program, objects);

        const auto decode_log = dir / (name + "_decode.log");
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" +
                            (dir / (name + ".wav")).string() + "\"",
                        decode_log) == 0);
        const auto decoded = read_log(decode_log);
        INFO("decode:\n" + decoded);
        CHECK(decoded.find(line) != std::string::npos);

        const auto monitor_log = dir / (name + "_monitor.log");
        const auto rc = run_cli("monitor \"" + stream.string() + "\"", monitor_log);
        const auto monitored = read_log(monitor_log);
        INFO("monitor:\n" + monitored);
        check_spoke_either_way(rc, monitored);
        if (rc == 0) {
            CHECK(monitored.find(line) != std::string::npos);
        }
    };
    const std::array<iclforge::objects::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.25, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 0.75, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
    }};

    SECTION("a bed program, as channel-based immersive content is, names its bed") {
        constexpr auto k514 = static_cast<std::uint16_t>(iclforge::objects::oba::bed::k51 |
                                                         iclforge::objects::oba::bed::kTflTfr |
                                                         iclforge::objects::oba::bed::kTblTbr);
        check_both("monitor_bed_program",
                   {.dynamic_only = false, .bed = k514, .dynamic_objects = 2}, objects,
                   "  bed [L R C LFE Ls Rs Tfl Tfr Tbl Tbr] + 2 dynamic objects = 12 objects, "
                   "OAMD present (JOC audio not reconstructed)");
    }
    SECTION("a dynamic-object-only program with no LFE object does not claim one") {
        check_both("monitor_no_lfe", {.dynamic_only = true, .lfe = false, .dynamic_objects = 2},
                   objects,
                   "  2 dynamic objects = 2 objects, OAMD present (JOC audio not reconstructed)");
    }
}

TEST_CASE("spatial reads a legacy-core stream instead of refusing it as plain AC-3",
          "[cli][audio-io][atmos][concurrency]") {
    // run_spatial refused any stream whose first frame was AC-3 (bsid <= 8)
    // before ever checking for an Annex E extension substream behind it -
    // but a §E2.3.1.2 legacy-core delivery's object layer lives in exactly
    // such a dependent (decoder.hpp's DecodedAccessUnit::object_metadata
    // comment), so a real legacy-core Atmos stream was refused outright even
    // though it does carry one. write_legacy_core_oamd_stream above builds
    // exactly that shape.
    //
    // This machine's spatial refusal only fires unconditionally before any
    // device is touched, so its absence is the one assertion every machine
    // can make, headless CI included - same reasoning as check_spoke_either_way,
    // spelled out here because the specific line under test is a refusal
    // this stream must never hit, not merely "some" refusal.
    const auto dir = scratch_dir();
    const auto stream = dir / "spatial_legacy_core.ec3";
    const std::array<iclforge::objects::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.25, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 0.75, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
    }};
    write_legacy_core_oamd_stream(
        stream, {.dynamic_only = true, .lfe = false, .dynamic_objects = 2}, objects);

    const auto log = dir / "spatial_legacy_core.log";
    const auto rc = run_cli("spatial \"" + stream.string() + "\"", log);
    const auto out = read_log(log);
    INFO("spatial:\n" + out);
    check_spoke_either_way(rc, out);
    CHECK(out.find("needs the object layer") == std::string::npos);
    if (rc == 0) {
        // A spatial-capable endpoint opened for real: the final summary line
        // is spatial's own confirmation that decode_access_unit populated
        // object_metadata/object_audio from the dependent and actually
        // played the unit, not just that the refusal above was skipped.
        CHECK(out.find("played") != std::string::npos);
    }
}

TEST_CASE("monitor prints nothing on stdout under quiet, whichever way it goes",
          "[cli][audio-io][quiet][concurrency]") {
    // Three of monitor's status lines went to stdout through plain
    // fmt::println, so quiet did not silence them: the verify-objects
    // summary, printed before any device is touched; the §7.8 fold note,
    // printed when the endpoint has fewer channels than the programme; and
    // the object-count line, printed once the sink has started. A signed
    // object stream reaches the first on any build with a monitor backend,
    // and the object-count line wherever a render endpoint opens - the fold
    // note too when that endpoint is narrower than the 5.1 bed. Silent, like
    // the case below: on a machine with speakers this plays out loud.
    const auto dir = scratch_dir();
    const auto key = dir / "monitor_quiet.key";
    {
        std::ofstream out{key, std::ios::binary};
        REQUIRE(out.is_open());
        out << "not-a-real-key-just-test-material";
    }
    const auto bed = dir / "monitor_quiet_bed.ac3";
    const auto pcm = dir / "monitor_quiet.wav";
    const auto stream = dir / "monitor_quiet.ec3";
    const auto setup = dir / "monitor_quiet_setup.log";
    REQUIRE(run_cli("silence \"" + bed.string() + "\" 1", setup) == 0);
    REQUIRE(run_cli("decode \"" + bed.string() + "\" \"" + pcm.string() + "\"", setup) == 0);
    REQUIRE(run_cli("atmos-encode \"" + pcm.string() + "\" \"" + stream.string() +
                        "\" 448 sign-objects signing-key=\"" + key.string() + "\"",
                    setup) == 0);

    const std::string monitor = "monitor \"" + stream.string() +
                                "\" verify-objects signing-key=\"" + key.string() + "\"";
    const auto out = dir / "monitor_quiet.out";
    const auto err = dir / "monitor_quiet.err";

    // Without quiet first, to show the lines are there to silence. A build
    // with no monitor backend refuses the command before it reads the stream.
    const auto loud_rc = run_cli_split(monitor, out, err);
    const auto loud = read_log(out);
    const auto loud_err = read_log(err);
    INFO("without quiet, stdout:\n" + loud + "\nstderr:\n" + loud_err);
    if (loud_err.find("is unavailable on this platform") == std::string::npos) {
        CHECK(loud.find("object signature") != std::string::npos);
    }
    if (loud_rc == 0) {
        CHECK(loud.find("OAMD present") != std::string::npos);
    }

    const auto rc = run_cli_split(monitor + " quiet", out, err);
    const auto quiet_err = read_log(err);
    INFO("with quiet, stderr:\n" + quiet_err);
    CHECK(read_log(out).empty());
    if (rc != 0) {
        CHECK(quiet_err.find("error") != std::string::npos);
    }
}

TEST_CASE("monitor either plays a stream or refuses by name", "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "monitor_in.ac3";
    const auto log = dir / "monitor.log";

    // A real, decodable stream, so a machine that DOES have a render endpoint
    // exercises the decode-and-play path rather than bailing on a bad input.
    REQUIRE(run_cli("silence \"" + stream.string() + "\" 1", dir / "monitor_silence.log") == 0);
    REQUIRE(fs::exists(stream));

    const auto rc = run_cli("monitor \"" + stream.string() + "\"", log);
    check_spoke_either_way(rc, read_log(log));
}

// 'play' (apps/forge/cli/src/commands/audio_io.cpp's run_play) had no test in this
// suite at all until this one - unlike devices/outputs/record/live/monitor
// above, added when this file was, coverage floors never reached it.

TEST_CASE("play either streams to a device or refuses by name", "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "play_in.ac3";
    REQUIRE(run_cli("silence \"" + stream.string() + "\" 1", dir / "play_silence.log") == 0);
    REQUIRE(fs::exists(stream));

    const auto log = dir / "play.log";
    const auto rc = run_cli("play \"" + stream.string() + "\"", log);
    check_spoke_either_way(rc, read_log(log));
}

TEST_CASE("play refuses a stream too short to hold a syncframe, before any device is touched",
          "[cli][audio-io]") {
    // 3 bytes: not empty (read_elementary_stream's own "nothing at all"
    // refusal is a different, already-covered branch), but short of the 6
    // stream_bsid() needs. run_play reads this off the file before
    // enumerating or opening anything, so this holds identically on a
    // machine with real render hardware and on one with none at all.
    //
    // run_spatial has the identical check (live_audio.cpp's own line, one
    // read_all()/apply_object_verification() call ahead of it) but it is not
    // exercised here: main.cpp's Needs::kSpatial gate refuses the whole
    // 'spatial' command before run_spatial() is ever called on any build
    // without a real spatial backend - "this build has no spatial backend:
    // ISpatialAudioObjectRenderStream is a Windows-only API" (confirmed
    // against this exact build). Every line inside run_spatial() is
    // therefore unreachable through the CLI on the Linux/ALSA build this
    // suite runs on, and on any other non-Windows build - not merely
    // untested here, but dead from this entry point on every platform this
    // repository's CI actually runs a coverage job on. There is no
    // Windows coverage leg to reach it from either.
    const auto path = scratch_dir() / "too_short.ac3";
    write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}});

    const auto log = scratch_dir() / "play_too_short.log";
    const auto rc = run_cli("play \"" + path.string() + "\"", log);
    const auto out = read_log(log);
    INFO(out);
    CHECK(rc != 0);
    // Unlike 'spatial', 'play' can be genuinely available (Needs::kPassthrough
    // - real ALSA/IEC 61937 hardware on this build), in which case run_play
    // does reach its own too-short check first. But a build with no
    // passthrough capability at all (the "no-alsa" CI leg) hits main.cpp's
    // gate before run_play ever runs, same branch the 'outputs' test above
    // already handles - accept either refusal rather than assuming this
    // build always has the capability.
    if (out.find("is unavailable on this platform") != std::string::npos) {
        CHECK(out.find("forge spdif") != std::string::npos);
    } else {
        CHECK(out.find("too short to hold a syncframe") != std::string::npos);
    }
}

TEST_CASE("play refuses a stream that claims E-AC-3/AC-3 but does not split into valid units",
          "[cli][audio-io]") {
    // run_play reads bsid straight off byte 5 to decide which of
    // split_access_units/split_frames to call, then reports whichever of
    // them fails - both checks run well before device enumeration, so
    // neither depends on what render hardware the machine running this test
    // has. A bad sync word (bytes 0-1) is enough to fail either split call
    // regardless of the rest of the header, which is why the two vectors
    // below only need to differ in the one byte (5) that decides bsid.
    SECTION("bsid > 8 (E-AC-3): split_access_units finds no valid access unit") {
        const auto path = scratch_dir() / "play_bad_eac3.ec3";
        write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x50}});
        const auto log = scratch_dir() / "play_bad_eac3.log";
        const auto rc = run_cli("play \"" + path.string() + "\"", log);
        const auto out = read_log(log);
        INFO(out);
        CHECK(rc != 0);
        // See the "too short to hold a syncframe" test above for why both
        // branches are accepted: a no-passthrough-capability build refuses
        // at main.cpp's gate before run_play's own split check ever runs.
        if (out.find("is unavailable on this platform") != std::string::npos) {
            CHECK(out.find("forge spdif") != std::string::npos);
        } else {
            CHECK(out.find("is not a valid E-AC-3 stream") != std::string::npos);
        }
    }

    SECTION("bsid <= 8 (AC-3): split_frames finds no valid frame") {
        const auto path = scratch_dir() / "play_bad_ac3.ac3";
        // byte 4's top two bits (fscod) are 0b11, A/52's own reserved value -
        // syncframe_bytes() refuses it outright rather than looking up a
        // frame size.
        write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0xFF}, std::byte{0x08}});
        const auto log = scratch_dir() / "play_bad_ac3.log";
        const auto rc = run_cli("play \"" + path.string() + "\"", log);
        const auto out = read_log(log);
        INFO(out);
        CHECK(rc != 0);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            CHECK(out.find("forge spdif") != std::string::npos);
        } else {
            CHECK(out.find("is not a valid AC-3 stream") != std::string::npos);
        }
    }
}

TEST_CASE("monitor reports a decode failure by name, distinct from a device refusal",
          "[cli][audio-io]") {
    // A semantically invalid but framing-correct, CRC-correct E-AC-3 access
    // unit - spxbegf placed past spxendf, collapsing the spectral extension
    // region to nothing (see iclforge::ac3::describe(DecodeError::kInvalidStream)) -
    // the exact vector libs/ac3/tests/decoder/test_eac3_decoder.cpp's "the E-AC-3
    // decoder rejects malformed spectral extension streams" test already
    // validates bit-for-bit at the library level, reused here through the
    // CLI. run_monitor decodes its first access unit before ever calling
    // MonitorSink::start() (that only happens once a decode actually
    // succeeds), so unlike every other 'monitor' case in this file, this one
    // never depends on what render hardware is present.
    iclforge::ac3::eac3::AccessUnitEncoder encoder{{.independent = {.bitrate_kbps = 448,
                                                          .acmod = iclforge::ac3::Acmod::k3_2,
                                                          .lfe = true,
                                                          .spx = true,
                                                          .spx_atten = false}}};
    REQUIRE(encoder.channel_count() == 6);
    std::vector<std::vector<float>> pcm(
        6, std::vector<float>(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame)));
    const double tones[6] = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < iclforge::ac3::kSamplesPerFrame; ++i) {
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * tones[ch] * static_cast<double>(i) /
                              48000.0));
        }
    }
    const std::vector<std::span<const float>> views{pcm[0], pcm[1], pcm[2], pcm[3], pcm[4], pcm[5]};
    const auto unit = encoder.encode_access_unit(views);
    REQUIRE(unit.has_value());
    auto broken = unit->bytes;
    // Bit offsets straight from test_eac3_decoder.cpp's own comment: bsi (54
    // bits) + audfrm (85 bits, spx on / attenuation off / nothing else
    // coupled) + block 0's dithflag(5)/dynrnge(1) prefix (6 bits) puts
    // spxinu at bit 145, followed by chinspx[0..4] (5), spxstrtf (2),
    // spxbegf (3), spxendf (3).
    constexpr std::size_t kSpxinuBit = 145;
    constexpr std::size_t kSpxbegfBit = kSpxinuBit + 1 + 5 + 2;
    constexpr std::size_t kSpxendfBit = kSpxbegfBit + 3;
    patch_bits(broken, kSpxbegfBit, 3, 7);  // begin_subbnd = 11
    patch_bits(broken, kSpxendfBit, 3, 0);  // end_subbnd = 5

    const auto path = scratch_dir() / "monitor_decode_fail.ec3";
    write_bytes(path, broken);
    const auto log = scratch_dir() / "monitor_decode_fail.log";
    const auto rc = run_cli("monitor \"" + path.string() + "\"", log);
    const auto out = read_log(log);
    INFO(out);
    CHECK(rc != 0);
    // Same caveat as 'play' above: a build with no monitor capability at all
    // (Needs::kMonitor) refuses at main.cpp's gate before run_monitor's own
    // decode ever runs, rather than reaching the decode-failure path this
    // test is really after.
    if (out.find("is unavailable on this platform") == std::string::npos) {
        CHECK(out.find("error: decode failed:") != std::string::npos);
    }
}

// A stream with a second independent substream (§E2.3.1.2: a second language,
// an audio description) carries an ALTERNATIVE, and its units arrive
// interleaved with the main's, one frame period of each in turn. monitor,
// spatial and play gave their decoder every one of them and no `programme`,
// so on a 5.1 main with a mono description the second unit read channels the
// first unit's width said were there: monitor died with an access violation.
// decode has always chosen one programme; these hold the three live commands
// to the same choice.

TEST_CASE("monitor, spatial and play refuse a programme the stream does not carry, by name",
          "[cli][audio-io][programme]") {
    // The choice is made before any device is looked at, so this holds on a
    // machine with no output at all - which is what lets CI check it.
    const auto stream = write_two_programme_stream("programme_refused");
    for (const std::string command : {"monitor", "spatial", "play"}) {
        CAPTURE(command);
        const auto log = scratch_dir() / ("programme_refused_" + command + ".log");
        const auto rc = run_cli(command + " \"" + stream.string() + "\" -1 programme=5", log);
        const auto out = read_log(log);
        INFO(out);
        CHECK(rc != 0);
        // A build without the command's backend refuses at main.cpp's gate,
        // one level above - the same caveat 'play's refusal tests above give.
        if (out.find("is unavailable on this platform") == std::string::npos) {
            CHECK(rc == 1);
            CHECK(out.find("error: no programme 5 in this stream (it carries 0, 1)") !=
                  std::string::npos);
        }
    }
}

TEST_CASE("monitor plays one programme of a stream that carries two, and says which",
          "[cli][audio-io][programme][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = write_two_programme_stream("programme_monitor");

    SECTION("the first, by default") {
        const auto log = dir / "programme_monitor_default.log";
        const auto rc = run_cli("monitor \"" + stream.string() + "\"", log);
        const auto out = read_log(log);
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        // Said before a device is touched, so on every machine.
        CHECK(out.find("  programme 0 of 2 (0, 1)") != std::string::npos);
        check_spoke_either_way(rc, out);
        if (rc == 0) {
            // One programme's second of audio, not two programmes' units
            // one after the other (64).
            CHECK(out.find("played 32 access units") != std::string::npos);
        }
    }
    SECTION("the one programme= names") {
        const auto log = dir / "programme_monitor_second.log";
        const auto rc = run_cli("monitor \"" + stream.string() + "\" -1 programme=1", log);
        const auto out = read_log(log);
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(out.find("  programme 1 of 2 (0, 1)") != std::string::npos);
        check_spoke_either_way(rc, out);
        if (rc == 0) {
            CHECK(out.find("(1 channels, ") != std::string::npos);
            CHECK(out.find("played 32 access units") != std::string::npos);
        }
    }
}

// §E3.10: monitor mixes an associated service into the programme it plays, as
// decode does, from a second decoder run in step with the first. What is mixed
// is checked sample for sample by iclforge-app-media-tests
// (test_associated_mix.cpp); these hold the command around it, on a machine
// with or without an output.

TEST_CASE("monitor refuses an associated service the stream cannot give, by name, and plays nothing",
          "[cli][audio-io][programme][associated]") {
    // Chosen with the programme, before any device is looked at, so this holds
    // on a machine with no output at all - and in decode's words and with
    // decode's exit code.
    const auto stream = write_two_programme_stream("associated_refused", " programme2-bsmod=vi");
    const auto run = [&](const std::string& options) {
        const auto log = scratch_dir() / "associated_refused.log";
        const auto rc = run_cli("monitor \"" + stream.string() + "\" -1 " + options, log);
        return std::pair{rc, read_log(log)};
    };

    SECTION("a substream the stream does not carry") {
        const auto [rc, out] = run("associated=5");
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(rc == 2);
        CHECK(out.find("error: no programme 5 in this stream (it carries 0, 1)") !=
              std::string::npos);
        CHECK(out.find("played ") == std::string::npos);
    }
    SECTION("the programme being played") {
        const auto [rc, out] = run("associated=0");
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(rc == 2);
        CHECK(out.find("associated=0 is the programme being decoded") != std::string::npos);
    }
    SECTION("a service no programme is") {
        const auto [rc, out] = run("associated=commentary");
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(rc == 2);
        CHECK(out.find("no programme of this stream is that associated service") !=
              std::string::npos);
        // What there was to choose from, by what the stream calls each.
        CHECK(out.find("1: associated service: visually impaired") != std::string::npos);
    }
    SECTION("the main is the second programme and the service is the first") {
        // programme=1 plays the description; the main is then 'the other'.
        const auto [rc, out] = run("programme=1 associated=1");
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(rc == 2);
        CHECK(out.find("associated=1 is the programme being decoded") != std::string::npos);
    }
    SECTION("a gain with nothing to scale is named and ignored") {
        const auto [rc, out] = run("associated-gain=-6");
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(out.find("associated-gain= scales an associated service") != std::string::npos);
        check_spoke_either_way(rc, out);
    }
}

TEST_CASE("monitor mixes an associated service into the programme it plays, and says so",
          "[cli][audio-io][programme][associated][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = write_two_programme_stream("associated_monitor", " programme2-bsmod=vi");

    SECTION("by substream") {
        const auto log = dir / "associated_monitor_number.log";
        const auto rc = run_cli("monitor \"" + stream.string() + "\" -1 associated=1", log);
        const auto out = read_log(log);
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        CHECK(out.find("  programme 0 of 2 (0, 1)") != std::string::npos);
        check_spoke_either_way(rc, out);
        if (rc == 0) {
            // What it mixes, said when playback starts and again with the
            // gains it applied; one programme's second of audio all the way.
            CHECK(out.find("  mixing programme 1 (associated service: visually impaired") !=
                  std::string::npos);
            CHECK(out.find("  mixed programme 1 (associated service: visually impaired") !=
                  std::string::npos);
            CHECK(out.find("into programme 0") != std::string::npos);
            CHECK(out.find("played 32 access units") != std::string::npos);
        }
    }
    SECTION("by what the stream calls it, with the listener's own level") {
        const auto log = dir / "associated_monitor_name.log";
        const auto rc = run_cli("monitor \"" + stream.string() +
                                    "\" -1 associated=visually-impaired associated-gain=-6",
                                log);
        const auto out = read_log(log);
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        check_spoke_either_way(rc, out);
        if (rc == 0) {
            CHECK(out.find("  mixed programme 1 (associated service: visually impaired") !=
                  std::string::npos);
            CHECK(out.find("associated -6.0 dB") != std::string::npos);
        }
    }
    SECTION("folded to the endpoint's width, still with the service in it") {
        const auto log = dir / "associated_monitor_fold.log";
        const auto rc =
            run_cli("monitor \"" + stream.string() + "\" -1 downmix=loro associated=1", log);
        const auto out = read_log(log);
        INFO(out);
        if (out.find("is unavailable on this platform") != std::string::npos) {
            return;
        }
        check_spoke_either_way(rc, out);
        if (rc == 0) {
            CHECK(out.find("(2 channels, ") != std::string::npos);
            CHECK(out.find("  mixed programme 1 ") != std::string::npos);
        }
    }
}

TEST_CASE("monitor names associated= as ignored on a plain AC-3 stream, which has one programme",
          "[cli][audio-io][associated][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "associated_plain.ac3";
    REQUIRE(run_cli("silence \"" + stream.string() + "\" 1", dir / "associated_plain_make.log") ==
            0);
    const auto log = dir / "associated_plain.log";
    const auto rc = run_cli("monitor \"" + stream.string() + "\" -1 associated=1", log);
    const auto out = read_log(log);
    INFO(out);
    if (out.find("is unavailable on this platform") != std::string::npos) {
        return;
    }
    CHECK(out.find("warning: associated= mixes a second programme and") != std::string::npos);
    CHECK(out.find("is plain AC-3, which carries one; ignored") != std::string::npos);
    check_spoke_either_way(rc, out);
}

TEST_CASE("spatial names associated= as ignored, since it places one programme's objects",
          "[cli][audio-io][atmos][programme][associated][concurrency]") {
    const auto stream = write_two_programme_stream("associated_spatial", " programme2-bsmod=vi");
    const auto log = scratch_dir() / "associated_spatial.log";
    const auto rc = run_cli("spatial \"" + stream.string() + "\" -1 associated=1", log);
    const auto out = read_log(log);
    INFO(out);
    if (out.find("is unavailable on this platform") != std::string::npos) {
        return;
    }
    // Said before the spatial endpoint is probed, so on every machine.
    CHECK(out.find("warning: associated= is ignored by spatial") != std::string::npos);
    check_spoke_either_way(rc, out);
}

TEST_CASE("spatial names the programme it renders from a stream that carries two",
          "[cli][audio-io][atmos][programme][concurrency]") {
    const auto stream = write_two_programme_stream("programme_spatial");
    const auto log = scratch_dir() / "programme_spatial.log";
    const auto rc = run_cli("spatial \"" + stream.string() + "\" -1 programme=1", log);
    const auto out = read_log(log);
    INFO(out);
    if (out.find("is unavailable on this platform") != std::string::npos) {
        return;
    }
    // Chosen before the spatial endpoint is probed, so this is there whether
    // the machine can render objects or refuses by name.
    CHECK(out.find("  programme 1 of 2 (0, 1)") != std::string::npos);
    check_spoke_either_way(rc, out);
}
