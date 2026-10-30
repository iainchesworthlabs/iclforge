#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/adm/ac3adm.hpp"
#include "iclforge/adm/bridge.hpp"
#include "iclforge/objects/motion.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/evolution.hpp"
#include "platform/process.hpp"

// `forge decode ... adm_out` on TrueHD (libs/truehd/ERRATA.md entry 61): the real binary over the
// hand-built streams of testdata/truehd/handbuilt, the ADM master read back with iclforge::adm. It
// needs forge built with ICLFORGE_BUILD_ADM, like test_cli_decode_adm.cpp, which this follows.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_adm_" + iclforge::test::platform::process_id());
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

// The object audio metadata of the two evolution frames of hb_atmos_objects, as the library reads
// them.
std::vector<iclforge::objects::oba::DecodedProgram> oamd_programs(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    namespace th = iclforge::truehd;
    th::ElementaryReader reader(bytes);
    std::vector<iclforge::objects::oba::DecodedProgram> programs;
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
            programs.push_back(*program);
        }
    }
    return programs;
}

}  // namespace

TEST_CASE("decode writes the beds and objects of a TrueHD Atmos stream as an ADM master",
          "[forge-cli][truehd][atmos][adm]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = fixture("hb_atmos_objects.thd");
    const fs::path adm = dir / "atmos_master.wav";
    fs::remove(adm);
    const fs::path log = dir / "atmos_master.log";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(dir / "atmos_master_all.wav") +
                        " \"\" " + quoted(adm) + " truehd-presentation=all",
                    log) == 0);
    CHECK(read_text(log).find("wrote ADM master (12 channels)") != std::string::npos);
    REQUIRE(fs::exists(adm));

    const auto parsed = iclforge::adm::parse_bw64(adm.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 12);
    std::size_t lfe = 0;
    std::size_t dynamic = 0;
    std::size_t lfe_at = 12;
    for (std::size_t i = 0; i < 12; ++i) {
        if (bridged->is_lfe[i]) {
            ++lfe;
            lfe_at = i;
        } else if (!bridged->is_bed[i]) {
            ++dynamic;
        }
    }
    CHECK(lfe == 1);
    CHECK(dynamic == 11);
    CHECK(lfe_at == 0);  // the LFE is channel 0 of the substream, object 0 of the programme

    // The positions are the metadata's: the first frame's at the start, the second's once its ramp
    // (from sample 640, over 1536) is done.
    const auto programs = oamd_programs(stream);
    REQUIRE(programs.size() == 2);
    const auto at_start = iclforge::objects::oba::evaluate_placements(bridged->paths, 0.0);
    const auto at_end =
        iclforge::objects::oba::evaluate_placements(bridged->paths, 1279.0 / 48000.0);
    REQUIRE(at_start.size() == 12);
    for (std::size_t i = 1; i < 12; ++i) {
        const auto& first = programs[0].blocks.front().objects[i - 1].position;
        const auto& second = programs[1].blocks.front().objects[i - 1].position;
        CHECK(std::abs(at_start[i].position.x - first.x) < 0.02);
        CHECK(std::abs(at_start[i].position.y - first.y) < 0.02);
        // The objects move 0.1 to the right between the frames, so the ramp is under way at the
        // end of the file and not finished; the position has left the first frame's.
        CHECK(at_end[i].position.x > first.x + 0.02);
        CHECK(at_end[i].position.x < second.x + 0.02);
    }
}

TEST_CASE("decode writes a TrueHD presentation's bed alone as DirectSpeakers channels",
          "[forge-cli][truehd][atmos][adm]") {
    const fs::path dir = scratch_dir();
    const fs::path adm = dir / "bed_master.wav";
    fs::remove(adm);
    const fs::path log = dir / "bed_master.log";
    REQUIRE(run_cli("decode " + quoted(fixture("hb_eight_channels.thd")) + " " +
                        quoted(dir / "bed_out.wav") + " \"\" " + quoted(adm),
                    log) == 0);
    CHECK(read_text(log).find("wrote ADM master (bed only, 8 channels)") != std::string::npos);
    const auto parsed = iclforge::adm::parse_bw64(adm.string());
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 8);
    std::size_t lfe = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(bridged->is_bed[i]);
        lfe += bridged->is_lfe[i] ? 1U : 0U;
    }
    CHECK(lfe == 1);
}

TEST_CASE("decode does not write an ADM master where the channels cannot be named",
          "[forge-cli][truehd][atmos][adm]") {
    const fs::path dir = scratch_dir();
    const fs::path adm = dir / "no_master.wav";
    fs::remove(adm);
    const fs::path log = dir / "no_master.log";
    // The twelve channels of this stream have no object audio metadata to name them.
    REQUIRE(run_cli("decode " + quoted(fixture("hb_substream3_fine_matrices.thd")) + " " +
                        quoted(dir / "no_master_all.wav") + " \"\" " + quoted(adm) +
                        " truehd-presentation=all",
                    log) == 0);
    CHECK(read_text(log).find("no object audio metadata") != std::string::npos);
    CHECK(read_text(log).find("the stream carries no object audio metadata") != std::string::npos);
    CHECK_FALSE(fs::exists(adm));
}
