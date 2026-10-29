#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "platform/process.hpp"

// The channel meaning of a TrueHD stream (TH spec §14.1): the fields `truehd-encode` writes and
// `forge probe` shows, `forge metadata` and `forge normalize` rewrite, and `forge qc` compares with
// the measurement. The real binary, run as a subprocess, over the FFmpeg fixtures of
// testdata/truehd.
//
// Provisional, no oracle until the DEE set (libs/truehd/ERRATA.md, questions 51 to 55): FFmpeg's
// streams hold zeros, so these tests hold the layout of the specification, that every major sync
// carries the values and a good CRC, and that the audio is the same.
//
// ICLFORGE_CLI_EXE and ICLFORGE_TRUEHD_FIXTURE_DIR come from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

namespace th = iclforge::truehd;

constexpr int kExitUsage = 1;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_meta_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
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

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / file;
}

std::string cli_arg(const fs::path& path) {
    return "\"" + path.string() + "\" ";
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// The channel meaning of every major sync of the stream, each read with its CRC checked.
std::vector<th::ChannelMeaning> meanings_of(const fs::path& stream) {
    const std::vector<std::byte> bytes = read_bytes(stream);
    std::vector<th::ChannelMeaning> out;
    th::ElementaryReader reader(bytes);
    while (const auto unit = reader.next()) {
        if (unit->size() >= 8 && th::starts_with_major_sync(unit->subspan(4))) {
            const auto sync = th::parse_major_sync(unit->subspan(4), true);
            REQUIRE(sync.has_value());
            out.push_back(sync->channel_meaning);
        }
    }
    REQUIRE(reader.skipped_bytes() == 0);
    return out;
}

// The 5.1 fixture as a 24-bit WAV, a source for `truehd-encode`.
fs::path source_wav(const fs::path& dir) {
    static fs::path made;
    if (made.empty() || !fs::exists(made)) {
        made = dir / "source_51.wav";
        REQUIRE(run_cli("decode " + cli_arg(fixture("truehd_m51_s24.thd")) + cli_arg(made),
                        dir / "source.log") == 0);
    }
    return made;
}

// Every option of the channel meaning with a value of its own.
const char* const kEveryOption =
    "truehd-dialnorm-2ch=40 truehd-dialnorm-6ch=27 truehd-dialnorm-8ch=24 truehd-mix-level-2ch=33 "
    "truehd-mix-level-6ch=17 truehd-mix-level-8ch=49 truehd-source-format-6ch=5 "
    "truehd-source-format-8ch=38 truehd-control-2ch=on truehd-control-6ch=off "
    "truehd-control-8ch=on truehd-heavy-drc-start-up-gain=-9 truehd-drc-start-up-gain=21";

th::ChannelMeaning every_option_meaning() {
    th::ChannelMeaning meaning;
    meaning.twoch_dialogue_norm = 40;
    meaning.sixch_dialogue_norm = 27;
    meaning.eightch_dialogue_norm = 24;
    meaning.twoch_mix_level = 33;
    meaning.sixch_mix_level = 17;
    meaning.eightch_mix_level = 49;
    meaning.sixch_source_format = 5;
    meaning.eightch_source_format = 38;
    meaning.twoch_control_enabled = true;
    meaning.sixch_control_enabled = false;
    meaning.eightch_control_enabled = true;
    meaning.heavy_drc_start_up_gain = -9;
    meaning.drc_start_up_gain = 21;
    return meaning;
}

}  // namespace

TEST_CASE("truehd-encode writes the channel meaning into every major sync",
          "[forge-cli][truehd][truehd-meta][truehd-encode]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "encode.log";
    const fs::path source = source_wav(dir);

    SECTION("by default it writes zeros, as FFmpeg's streams hold") {
        const fs::path stream = dir / "default.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) + "truehd-restart=8",
                        log) == 0);
        CHECK_FALSE(contains(read_text(log), "channel meaning written"));
        const auto meanings = meanings_of(stream);
        REQUIRE(meanings.size() > 3);
        for (const th::ChannelMeaning& meaning : meanings) {
            CHECK(meaning == th::ChannelMeaning{});
        }
        // Every field at its zero is the same stream.
        const fs::path zeros = dir / "zeros.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(zeros) +
                            "truehd-restart=8 truehd-dialnorm-2ch=0 truehd-dialnorm-6ch=0 "
                            "truehd-mix-level-8ch=0 truehd-control-6ch=off "
                            "truehd-drc-start-up-gain=0",
                        log) == 0);
        CHECK(read_bytes(zeros) == read_bytes(stream));
    }

    SECTION("every option sets its field, in every major sync, and the audio is the same") {
        const fs::path plain = dir / "plain.thd";
        const fs::path stream = dir / "every.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(plain) + "truehd-restart=8",
                        log) == 0);
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) +
                            "truehd-restart=8 verify " + kEveryOption,
                        log) == 0);
        const std::string text = read_text(log);
        CHECK(contains(text, "channel meaning written (provisional, no oracle until the DEE set)"));
        CHECK(contains(text,
                       "6ch  control off, dialogue norm 27 (-27 dBFS), mix level 17, "
                       "source format 5"));
        CHECK(contains(text, "drc start-up  heavy -9, standard 21"));
        const auto meanings = meanings_of(stream);
        REQUIRE(meanings.size() == meanings_of(plain).size());
        REQUIRE(meanings.size() > 3);
        for (const th::ChannelMeaning& meaning : meanings) {
            CHECK(meaning == every_option_meaning());
        }
        const fs::path a = dir / "plain.wav";
        const fs::path b = dir / "every.wav";
        REQUIRE(run_cli("decode " + cli_arg(plain) + cli_arg(a), log) == 0);
        REQUIRE(run_cli("decode " + cli_arg(stream) + cli_arg(b), log) == 0);
        CHECK(read_bytes(a) == read_bytes(b));
    }

    SECTION("only the options named are set") {
        const fs::path stream = dir / "one.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) +
                            "truehd-restart=8 truehd-dialnorm-6ch=31 truehd-control-6ch=on",
                        log) == 0);
        th::ChannelMeaning expected;
        expected.sixch_dialogue_norm = 31;
        expected.sixch_control_enabled = true;
        for (const th::ChannelMeaning& meaning : meanings_of(stream)) {
            CHECK(meaning == expected);
        }
    }

    SECTION("a container carries the stream whole") {
        const fs::path mkv = dir / "every.mkv";
        const fs::path back = dir / "every_demux.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(mkv) + kEveryOption, log) ==
                0);
        REQUIRE(run_cli("demux " + cli_arg(mkv) + cli_arg(back), log) == 0);
        for (const th::ChannelMeaning& meaning : meanings_of(back)) {
            CHECK(meaning == every_option_meaning());
        }
    }
}

TEST_CASE("a channel meaning option out of its field's range is refused by name",
          "[forge-cli][truehd][truehd-meta][truehd-encode]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "refused.log";
    const fs::path source = source_wav(dir);
    const fs::path stream = dir / "refused.thd";
    struct Case {
        std::string option;
        std::string range;
    };
    // One past either end of each field's width: TH spec §12.3, §12.5.
    const std::vector<Case> cases{
        {"truehd-dialnorm-2ch=64", "0 to 63"},
        {"truehd-dialnorm-6ch=32", "0 to 31"},
        {"truehd-dialnorm-8ch=32", "0 to 31"},
        {"truehd-dialnorm-6ch=-1", "0 to 31"},
        {"truehd-mix-level-2ch=64", "0 to 63"},
        {"truehd-mix-level-6ch=64", "0 to 63"},
        {"truehd-mix-level-8ch=64", "0 to 63"},
        {"truehd-source-format-6ch=32", "0 to 31"},
        {"truehd-source-format-8ch=64", "0 to 63"},
        {"truehd-heavy-drc-start-up-gain=32", "-32 to 31"},
        {"truehd-heavy-drc-start-up-gain=-33", "-32 to 31"},
        {"truehd-drc-start-up-gain=64", "-64 to 63"},
        {"truehd-drc-start-up-gain=-65", "-64 to 63"},
    };
    for (const Case& item : cases) {
        DYNAMIC_SECTION(item.option) {
            fs::remove(stream);
            CHECK(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) + item.option,
                          log) == kExitUsage);
            const std::string text = read_text(log);
            CHECK(contains(text, item.option.substr(0, item.option.find('='))));
            CHECK(contains(text, item.range));
            CHECK_FALSE(fs::exists(stream));
        }
    }
    SECTION("the ends of each range are written") {
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) +
                            "truehd-dialnorm-2ch=63 truehd-dialnorm-8ch=31 "
                            "truehd-source-format-8ch=63 truehd-heavy-drc-start-up-gain=-32 "
                            "truehd-drc-start-up-gain=63",
                        log) == 0);
        const th::ChannelMeaning meaning = meanings_of(stream).front();
        CHECK(meaning.twoch_dialogue_norm == 63);
        CHECK(meaning.eightch_dialogue_norm == 31);
        CHECK(meaning.eightch_source_format == 63);
        CHECK(meaning.heavy_drc_start_up_gain == -32);
        CHECK(meaning.drc_start_up_gain == 63);
    }
    SECTION("not a number, not on or off, and an option the field does not have") {
        for (const char* option :
             {"truehd-dialnorm-6ch=loud", "truehd-dialnorm-6ch=", "truehd-dialnorm-6ch=27x",
              "truehd-control-6ch=1", "truehd-control-6ch=yes", "truehd-source-format-2ch=1",
              "truehd-dialnorm=27"}) {
            DYNAMIC_SECTION(option) {
                fs::remove(stream);
                CHECK(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) + option, log) ==
                      kExitUsage);
                CHECK_FALSE(fs::exists(stream));
            }
        }
    }
    SECTION("E-AC-3's own options are still refused by name, and these are TrueHD's alone") {
        CHECK(run_cli("truehd-encode " + cli_arg(source) + cli_arg(stream) + "dialnorm=27", log) ==
              kExitUsage);
        CHECK(contains(read_text(log), "does not apply dialnorm=27"));
        CHECK(run_cli("eac3-encode " + cli_arg(source) + cli_arg(dir / "x.ec3") +
                          "truehd-dialnorm-6ch=27",
                      log) == kExitUsage);
        CHECK(run_cli("decode " + cli_arg(fixture("truehd_m51_s24.thd")) + cli_arg(dir / "x.wav") +
                          "truehd-dialnorm-6ch=27",
                      log) == kExitUsage);
    }
}
