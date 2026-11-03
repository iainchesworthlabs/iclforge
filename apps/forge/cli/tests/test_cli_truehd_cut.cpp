#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "platform/process.hpp"

// `forge cut` and `forge cat` of TrueHD and MLP streams, over the FFmpeg fixtures of
// testdata/truehd. A cut starts at a unit with a major sync and keeps whole units; a join is whole
// units back to back. The proof needs no FFmpeg: the bytes are the input's units, and the decode of
// the output (forge's own, which the codec matrix holds to FFmpeg's) is the decode of the input,
// sliced or joined. A cut that began anywhere but a major sync would decode to something shorter.
//
// ICLFORGE_CLI_EXE and ICLFORGE_TRUEHD_FIXTURE_DIR come from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

namespace th = iclforge::truehd;

constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;
constexpr std::size_t kUnitSamples = 40;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_cut_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string arg(const fs::path& path) {
    return "\"" + path.string() + "\" ";
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void write_bytes(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::string text;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            continue;
        }
        text.push_back(raw[i]);
    }
    return text;
}

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / file;
}

struct UnitRef {
    std::size_t offset = 0;
    std::size_t length = 0;
    bool major_sync = false;
};

std::vector<UnitRef> units_of(const std::vector<std::byte>& bytes) {
    std::vector<UnitRef> units;
    th::ElementaryReader reader(bytes);
    while (const auto unit = reader.next()) {
        units.push_back(UnitRef{.offset = static_cast<std::size_t>(unit->data() - bytes.data()),
                                .length = unit->size(),
                                .major_sync = th::starts_with_major_sync(unit->subspan(4))});
    }
    return units;
}

// The bytes of units [first, last) of `bytes`.
std::vector<std::byte> units_bytes(const std::vector<std::byte>& bytes,
                                   const std::vector<UnitRef>& units, std::size_t first,
                                   std::size_t last) {
    std::vector<std::byte> out;
    for (std::size_t i = first; i < last; ++i) {
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(units[i].offset),
                   bytes.begin() + static_cast<std::ptrdiff_t>(units[i].offset + units[i].length));
    }
    return out;
}

// The samples of a decode: the data chunk of the 68-byte header's WAV.
std::vector<std::byte> decoded_samples(const fs::path& stream, const fs::path& dir,
                                       const std::string& options = {}) {
    const fs::path wav = dir / "decoded.wav";
    REQUIRE(run_cli("decode " + arg(stream) + arg(wav) + options, dir / "decode.log") == 0);
    const std::vector<std::byte> bytes = read_bytes(wav);
    REQUIRE(bytes.size() >= 68);
    return {bytes.begin() + 68, bytes.end()};
}

}  // namespace

TEST_CASE("cut starts a TrueHD stream at the first major sync at or after the time asked",
          "[forge-cli][truehd][cut]") {
    const fs::path dir = scratch_dir() / "cut";
    fs::create_directories(dir);
    const fs::path log = dir / "cut.log";
    const fs::path out = dir / "cut.thd";
    const fs::path in_path = fixture("truehd_m51_s24.thd");
    const std::vector<std::byte> input = read_bytes(in_path);
    const std::vector<UnitRef> units = units_of(input);
    REQUIRE(units.size() == 300);

    // 0.05 s is unit 60; the major syncs are not at every unit, so the cut starts later.
    constexpr std::size_t kWanted = 60;
    std::size_t first = kWanted;
    while (!units[first].major_sync) {
        ++first;
    }
    REQUIRE(first > kWanted);
    constexpr std::size_t kCount = 120;  // 0.1 s of 40-sample units at 48 kHz

    SECTION("whole units, from the major sync, for the duration asked") {
        REQUIRE(run_cli("cut " + arg(in_path) + arg(out) + "0.05 0.1", log) == 0);
        CHECK(read_bytes(out) == units_bytes(input, units, first, first + kCount));
        const std::string text = read_text(log);
        CHECK(text.find("cut 120 access units of 300") != std::string::npos);
        CHECK(text.find("started at the first major sync at or after 0.050 s: unit " +
                        std::to_string(first)) != std::string::npos);
    }
    SECTION("the output decodes to the matching slice of the input's decode") {
        REQUIRE(run_cli("cut " + arg(in_path) + arg(out) + "0.05 0.1", log) == 0);
        const auto all = decoded_samples(in_path, dir);
        const auto slice = decoded_samples(out, dir);
        constexpr std::size_t kBytesPerSample = 3;
        constexpr std::size_t kChannels = 6;
        const std::size_t per_unit = kUnitSamples * kChannels * kBytesPerSample;
        REQUIRE(slice.size() == kCount * per_unit);
        REQUIRE(all.size() >= (first + kCount) * per_unit);
        CHECK(std::equal(slice.begin(), slice.end(),
                         all.begin() + static_cast<std::ptrdiff_t>(first * per_unit)));
    }
    SECTION("a time that is a major sync's own is not moved") {
        // Unit `first` starts at first * 40 / 48000 s; five decimals are short of it, so ask for more.
        const double seconds = static_cast<double>(first * kUnitSamples) / 48000.0;
        REQUIRE(run_cli("cut " + arg(in_path) + arg(out) + std::to_string(seconds) + " 0.01", log) ==
                0);
        CHECK(read_text(log).find("started at unit " + std::to_string(first) +
                                  ", which carries a major sync") != std::string::npos);
    }
    SECTION("no start and no duration is the whole stream") {
        REQUIRE(run_cli("cut " + arg(in_path) + arg(out), log) == 0);
        CHECK(read_bytes(out) == input);
    }
    SECTION("a duration shorter than a unit gives one unit") {
        REQUIRE(run_cli("cut " + arg(in_path) + arg(out) + "0 0.0001", log) == 0);
        CHECK(read_bytes(out) == units_bytes(input, units, 0, 1));
    }
    SECTION("MLP is cut the same way") {
        const fs::path mlp = fixture("mlp_st48_s24.mlp");
        const std::vector<std::byte> bytes = read_bytes(mlp);
        const std::vector<UnitRef> mlp_units = units_of(bytes);
        REQUIRE(run_cli("cut " + arg(mlp) + arg(out) + "0.1 0.05", log) == 0);
        std::size_t start = 120;  // 0.1 s
        while (!mlp_units[start].major_sync) {
            ++start;
        }
        CHECK(read_bytes(out) == units_bytes(bytes, mlp_units, start, start + 60));
        CHECK(read_text(log).find("MLP") != std::string::npos);
    }
}

TEST_CASE("cut refuses what it cannot do, by name, and writes nothing", "[forge-cli][truehd][cut]") {
    const fs::path dir = scratch_dir() / "cut_refused";
    fs::create_directories(dir);
    const fs::path log = dir / "cut.log";
    const fs::path out = dir / "cut.thd";
    // 168 units with a major sync at unit 0 and unit 128 (0.1067 s): nothing to start at after that.
    const std::string gap = arg(fixture("truehd_st48_s24_gap128.thd"));

    SECTION("a start after the last major sync") {
        CHECK(run_cli("cut " + gap + arg(out) + "0.12", log) == kExitUsage);
        const std::string text = read_text(log);
        CHECK(text.find("no access unit with a major sync at or after 0.120 s") !=
              std::string::npos);
        CHECK(text.find("0.107 s") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("a start after the end") {
        CHECK(run_cli("cut " + gap + arg(out) + "5", log) == kExitUsage);
        CHECK(read_text(log).find("is past the end") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("a negative start and a duration that is not positive") {
        CHECK(run_cli("cut " + gap + arg(out) + "-1", log) == kExitUsage);
        CHECK(run_cli("cut " + gap + arg(out) + "0 0", log) == kExitUsage);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("an option a cut does not apply") {
        CHECK(run_cli("cut " + gap + arg(out) + "truehd-presentation=2ch", log) == kExitUsage);
        CHECK(read_text(log).find("unknown option 'truehd-presentation=2ch'") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("a stream with bytes between its units that do not read as one") {
        std::vector<std::byte> bytes = read_bytes(fixture("truehd_m51_s24.thd"));
        const std::vector<UnitRef> units = units_of(bytes);
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(units[10].offset),
                     std::size_t{6}, std::byte{0x55});
        const fs::path damaged = dir / "damaged.thd";
        write_bytes(damaged, bytes);
        CHECK(run_cli("cut " + arg(damaged) + arg(out) + "0 0.05", log) == kExitInput);
        CHECK(read_text(log).find("forge probe") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
}

TEST_CASE("cat joins TrueHD streams at their major syncs", "[forge-cli][truehd][cat]") {
    const fs::path dir = scratch_dir() / "cat";
    fs::create_directories(dir);
    const fs::path log = dir / "cat.log";
    const fs::path out = dir / "joined.thd";
    const fs::path a = fixture("truehd_m51_s24.thd");
    const std::vector<std::byte> input = read_bytes(a);

    SECTION("two copies are the input twice, and decode to its decode twice") {
        REQUIRE(run_cli("cat " + arg(out) + arg(a) + arg(a), log) == 0);
        std::vector<std::byte> twice = input;
        twice.insert(twice.end(), input.begin(), input.end());
        CHECK(read_bytes(out) == twice);
        const std::string text = read_text(log);
        CHECK(text.find("joined 2 files, 600 TrueHD access units") != std::string::npos);
        CHECK(text.find("0.500 s, 48000 Hz, 6 channel(s)") != std::string::npos);
        auto once = decoded_samples(a, dir);
        // Forge's strict decode stops at the join (ERRATA question 20); FFmpeg's and the lenient one do not.
        const auto joined = decoded_samples(out, dir, "truehd-check=lenient");
        once.insert(once.end(), once.begin(), once.end());
        CHECK(joined == once);
    }
    SECTION("a cut piece joins to the stream it was cut from") {
        const fs::path head = dir / "head.thd";
        const fs::path tail = dir / "tail.thd";
        // Units 0 to 63, then 64 on: unit 64 carries a major sync.
        const std::vector<UnitRef> units = units_of(input);
        REQUIRE(units[64].major_sync);
        REQUIRE(run_cli("cut " + arg(a) + arg(head) + "0 0.0533333333", log) == 0);
        REQUIRE(run_cli("cut " + arg(a) + arg(tail) + "0.0533333333", log) == 0);
        REQUIRE(read_bytes(head).size() == units[64].offset);
        REQUIRE(run_cli("cat " + arg(out) + arg(head) + arg(tail), log) == 0);
        CHECK(read_bytes(out) == input);
    }
    SECTION("an input that differs in shape is refused, with the difference named") {
        struct Case {
            std::string file;
            std::string what;
        };
        for (const Case& item : {Case{"truehd_st48_s24.thd", "substream count"},
                                 Case{"truehd_c50_s16.thd", "channels per substream"}}) {
            DYNAMIC_SECTION(item.file) {
                fs::remove(out);
                CHECK(run_cli("cat " + arg(out) + arg(a) + arg(fixture(item.file)), log) ==
                      kExitUsage);
                CHECK(read_text(log).find("in " + item.what) != std::string::npos);
                CHECK_FALSE(fs::exists(out));
            }
        }
        SECTION("a sample rate, and TrueHD against MLP") {
            CHECK(run_cli("cat " + arg(out) + arg(fixture("truehd_st48_s24.thd")) +
                              arg(fixture("truehd_st44_s24.thd")),
                          log) == kExitUsage);
            CHECK(read_text(log).find("in sample rate") != std::string::npos);
            CHECK(run_cli("cat " + arg(out) + arg(fixture("truehd_st48_s24.thd")) +
                              arg(fixture("mlp_st48_s24.mlp")),
                          log) == kExitUsage);
            CHECK(read_text(log).find("in stream type") != std::string::npos);
            CHECK_FALSE(fs::exists(out));
        }
    }
    SECTION("an input that does not open with a major sync is refused") {
        const std::vector<UnitRef> units = units_of(input);
        // The stream without its first unit: its first major sync is the next restart's, behind
        // units a decoder cannot start from.
        REQUIRE(units[1].offset > 0);
        REQUIRE_FALSE(units[1].major_sync);
        const fs::path late = dir / "late.thd";
        write_bytes(late, units_bytes(input, units, 1, units.size()));
        CHECK(run_cli("cat " + arg(out) + arg(a) + arg(late), log) == kExitUsage);
        CHECK(read_text(log).find("does not open with an access unit that carries a major sync") !=
              std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("an input that is not a TrueHD stream, and an output that is an input") {
        CHECK(run_cli("cat " + arg(out) + arg(a) + arg(fixture("manifest.json")), log) ==
              kExitUsage);
        CHECK(read_text(log).find("is not a TrueHD or MLP stream") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
        const fs::path copy = dir / "copy.thd";
        fs::copy_file(a, copy, fs::copy_options::overwrite_existing);
        CHECK(run_cli("cat " + arg(copy) + arg(a) + arg(copy), log) == kExitUsage);
        CHECK(read_text(log).find("is both an input and the output") != std::string::npos);
        CHECK(read_bytes(copy) == input);
    }
    SECTION("an option a join does not apply") {
        CHECK(run_cli("cat " + arg(out) + arg(a) + arg(a) + "truehd-check=lenient", log) ==
              kExitUsage);
        CHECK(read_text(log).find("unknown option 'truehd-check=lenient'") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
}
