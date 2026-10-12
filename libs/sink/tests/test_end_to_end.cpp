#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/sink/loopback_harness.hpp"

// AR1 (planning/hearth-android.md): the bytes the pass-through output hands its link are the burst the
// host wrote, for every fixture, through the transport, the sink and the output. The harness
// (iclforge/sink/loopback_harness.hpp) packs each stream as Hearth's server does, sends it over a
// loopback WebSocket to a sink whose extension-role output is the PassthroughOutput over a model of
// the Shield's device, and compares what the device was handed with what the packer wrote.
//
// Real time: a fixture is about two to three seconds and a second and a half of lead, so a case is a
// few seconds. ICLFORGE_FIXTURE_ROOT names the directory holding external-baseline/ and
// object-fixture/ when the binary runs somewhere the compiled-in path does not exist (a device).

namespace {

namespace fs = std::filesystem;

fs::path fixture_root() {
    if (const char* root = std::getenv("ICLFORGE_FIXTURE_ROOT"); root != nullptr && *root != '\0') {
        return fs::path{root};
    }
    return fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR}.parent_path();
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    std::transform(chars.begin(), chars.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

fs::path scratch(const std::string& leaf) {
    fs::path base{ICLFORGE_TEST_SCRATCH_DIR};
    if (const char* dir = std::getenv("ICLFORGE_TEST_SCRATCH"); dir != nullptr && *dir != '\0') {
        base = fs::path{dir};
    }
    const fs::path dir = base / ("iclforge_sink_end_to_end_" + leaf);
    fs::remove_all(dir);
    return dir;
}

// One fixture through the path, and what is required of it.
void check_fixture(const std::string& fixture) {
    INFO(fixture);
    const std::vector<std::byte> stream = read_file(fixture_root() / fixture);
    iclforge::sink::harness::Options options;
    options.state_directory = scratch(std::to_string(std::hash<std::string>{}(fixture)));
    // About three seconds of each: a longer clip is a longer run in real time and no more of the path.
    options.max_bursts = 100;
    const auto began = std::chrono::steady_clock::now();
    const iclforge::sink::harness::Result result = iclforge::sink::harness::play_through(stream, options);
    const auto whole_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();

    INFO("sent " << result.sent << ", written " << result.written << ", nulls " << result.nulls << ", first difference "
                 << result.first_difference << ", late " << result.counts.late << ", dropped " << result.counts.dropped
                 << ", underruns " << result.counts.underruns << "; setup " << result.setup_us / 1000 << " ms, play "
                 << result.play_us / 1000 << " ms, teardown " << result.teardown_us / 1000 << " ms; the call took "
                 << whole_ms << " ms");
    REQUIRE(result.error.empty());
    CHECK(result.sent >= 10);
    CHECK(result.written == result.sent);
    CHECK(result.equal);
    CHECK(result.written_hash == result.sent_hash);
    CHECK(result.counts.late == 0);
    CHECK(result.counts.dropped == 0);
    CHECK(result.counts.underruns == 0);
    // The device was kept fed between and around them.
    CHECK(result.nulls >= 8);
    // Closing is quick: a run that spends seconds there has a thread that does not stop.
    CHECK(result.teardown_us < 3'000'000);
}

}  // namespace

// The Dolby encoder's AC-3 at 5.1 and at stereo, its E-AC-3 5.1, and two of its three Atmos (JOC)
// streams: 5.1.4 and 9.1.6, the narrowest and the widest of the bed layouts the plan lists.
TEST_CASE("end to end: AC-3 5.1 at 448 kbit/s", "[sink][end-to-end][websocket]") {
    check_fixture("external-baseline/ac3-51-448/dee.ac3");
}
TEST_CASE("end to end: AC-3 stereo at 192 kbit/s", "[sink][end-to-end][websocket]") {
    check_fixture("external-baseline/ac3-music-stereo-192/dee.ac3");
}
TEST_CASE("end to end: E-AC-3 5.1 at 256 kbit/s", "[sink][end-to-end][websocket]") {
    check_fixture("external-baseline/eac3-51-256/dee.ec3");
}
TEST_CASE("end to end: E-AC-3 JOC 5.1.4", "[sink][end-to-end][websocket]") {
    check_fixture("object-fixture/dee_joc_514.ec3");
}
TEST_CASE("end to end: E-AC-3 JOC 9.1.6", "[sink][end-to-end][websocket]") {
    check_fixture("object-fixture/dee_joc_916.ec3");
}
