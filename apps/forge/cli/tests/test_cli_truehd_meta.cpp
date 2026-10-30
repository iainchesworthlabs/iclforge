#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
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

// The log of a run, with each line end as LF: a run that writes stdout text on Windows writes CR LF,
// and one that reads stdin writes LF (platform/windows/stdio_binary.cpp).
std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::string text;
    text.reserve(raw.size());
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

// True when `before` and `after` are streams of one length that differ only in the channel meaning
// and the CRC of their major syncs (TH spec 14.1: 8 bytes at 18 from the sync word, and the last
// two).
bool differ_only_in_major_syncs(const fs::path& before, const fs::path& after) {
    const std::vector<std::byte> a = read_bytes(before);
    const std::vector<std::byte> b = read_bytes(after);
    if (a.size() != b.size()) {
        return false;
    }
    std::vector<bool> allowed(a.size(), false);
    th::ElementaryReader reader(a);
    while (const auto unit = reader.next()) {
        if (unit->size() < 8 || !th::starts_with_major_sync(unit->subspan(4))) {
            continue;
        }
        const std::size_t sync = static_cast<std::size_t>(unit->data() - a.data()) + 4;
        const auto parsed = th::parse_major_sync(unit->subspan(4), true);
        if (!parsed) {
            return false;
        }
        for (std::size_t i = 18; i < 26; ++i) {
            allowed[sync + i] = true;
        }
        allowed[sync + parsed->size_bytes - 2] = true;
        allowed[sync + parsed->size_bytes - 1] = true;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i] && !allowed[i]) {
            return false;
        }
    }
    return true;
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

// Whether any object of the document names a key twice.
bool repeats_a_key(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    bool repeated = false;
    const nlohmann::json::parser_callback_t callback =
        [&keys, &repeated](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                    keys.emplace_back();
                    break;
                case nlohmann::json::parse_event_t::key:
                    repeated = !keys.back().insert(parsed.get<std::string>()).second || repeated;
                    break;
                case nlohmann::json::parse_event_t::object_end:
                    keys.pop_back();
                    break;
                default:
                    break;
            }
            return true;
        };
    const nlohmann::json parsed = nlohmann::json::parse(text, callback);
    return repeated || parsed.is_discarded();
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

TEST_CASE("forge probe shows the channel meaning of a TrueHD stream",
          "[forge-cli][truehd][truehd-meta][probe]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "probe_meaning.log";
    const fs::path source = source_wav(dir);
    const fs::path written = dir / "probe_every.thd";
    REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(written) + "truehd-restart=8 " +
                        kEveryOption,
                    log) == 0);

    SECTION("text, in the style of the other lines, marked provisional") {
        REQUIRE(run_cli("probe " + cli_arg(written), log) == 0);
        const std::string text = read_text(log);
        CHECK(contains(text,
                       "channel meaning provisional, no oracle until the DEE set "
                       "(libs/truehd/ERRATA.md, questions 51 to 55)"));
        CHECK(contains(text,
                       "  2ch           control on, dialogue norm 40 (-40 dBFS), mix level 33\n"));
        CHECK(contains(text,
                       "  6ch           control off, dialogue norm 27 (-27 dBFS), mix level 17, "
                       "source format 5\n"));
        CHECK(contains(text, "  drc start-up  heavy -9, standard 21\n"));
        // The 5.1 stream has two substreams, so no 8 channel presentation.
        CHECK(contains(text, "source format 38 (the stream has no such substream)"));
        CHECK_FALSE(contains(text, "different channel meanings"));
    }
    SECTION("a stream FFmpeg wrote shows its zeros") {
        REQUIRE(run_cli("probe " + cli_arg(fixture("truehd_m51_s24.thd")), log) == 0);
        const std::string text = read_text(log);
        CHECK(contains(text, "channel meaning"));
        CHECK(contains(text,
                       "6ch           control off, dialogue norm 0 (0 dBFS), mix level 0, "
                       "source format 0\n"));
    }
    SECTION("JSON, with the raw fields and which presentations the stream has") {
        REQUIRE(run_cli("probe " + cli_arg(written) + "json=1", log) == 0);
        const std::string text = read_text(log);
        CHECK_FALSE(repeats_a_key(text));
        const auto meaning = nlohmann::json::parse(text)["truehd"]["channel_meaning"];
        CHECK(meaning["provisional"] == true);
        CHECK(meaning["question"] == "libs/truehd/ERRATA.md, questions 51 to 55");
        CHECK(meaning["distinct_in_stream"] == 1);
        CHECK(meaning["heavy_drc_start_up_gain"] == -9);
        CHECK(meaning["drc_start_up_gain"] == 21);
        const auto presentations = meaning["presentations"];
        CHECK(presentations["2ch"]["in_stream"] == true);
        CHECK(presentations["2ch"]["control_enabled"] == true);
        CHECK(presentations["2ch"]["dialogue_norm"] == 40);
        CHECK(presentations["2ch"]["dialogue_norm_dbfs"] == -40);
        CHECK(presentations["2ch"]["mix_level"] == 33);
        CHECK_FALSE(presentations["2ch"].contains("source_format"));
        CHECK(presentations["6ch"]["in_stream"] == true);
        CHECK(presentations["6ch"]["control_enabled"] == false);
        CHECK(presentations["6ch"]["dialogue_norm"] == 27);
        CHECK(presentations["6ch"]["mix_level"] == 17);
        CHECK(presentations["6ch"]["source_format"] == 5);
        CHECK(presentations["8ch"]["in_stream"] == false);
        CHECK(presentations["8ch"]["dialogue_norm"] == 24);
        CHECK(presentations["8ch"]["mix_level"] == 49);
        CHECK(presentations["8ch"]["source_format"] == 38);
    }
    SECTION("valid JSON, the same fields, in every container") {
        for (const std::string container : {"mkv", "mp4", "ts"}) {
            DYNAMIC_SECTION(container) {
                const fs::path file = dir / ("probe_every." + container);
                REQUIRE(run_cli(container + " " + cli_arg(written) + cli_arg(file), log) == 0);
                REQUIRE(run_cli("probe " + cli_arg(file) + "json=1", log) == 0);
                const std::string text = read_text(log);
                CHECK_FALSE(repeats_a_key(text));
                const auto document = nlohmann::json::parse(text);
                REQUIRE(document["container"].is_object());
                const auto meaning = document["truehd"]["channel_meaning"];
                CHECK(meaning["presentations"]["6ch"]["dialogue_norm"] == 27);
                CHECK(meaning["drc_start_up_gain"] == 21);
            }
        }
    }
    SECTION("every fixture has the member, MLP has none, and a damaged stream still shows it") {
        for (const char* name : {"truehd_st48_s24.thd", "truehd_mono48_s24.thd",
                                 "truehd_c50_s16.thd", "truehd_st192_s24.thd"}) {
            DYNAMIC_SECTION(name) {
                REQUIRE(run_cli("probe " + cli_arg(fixture(name)) + "json=1", log) == 0);
                const auto document = nlohmann::json::parse(read_text(log));
                CHECK(document["truehd"]["channel_meaning"]["presentations"]["2ch"]["in_stream"] ==
                      true);
            }
        }
        REQUIRE(run_cli("probe " + cli_arg(fixture("mlp_st48_s24.mlp")) + "json=1", log) == 0);
        CHECK(nlohmann::json::parse(read_text(log))["truehd"]["channel_meaning"].is_null());
        REQUIRE(run_cli("probe " + cli_arg(fixture("mlp_st48_s24.mlp")), log) == 0);
        CHECK_FALSE(contains(read_text(log), "channel meaning"));
    }
}

TEST_CASE("forge metadata sets the channel meaning of a TrueHD stream without re-encoding it",
          "[forge-cli][truehd][truehd-meta][metadata]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "metadata.log";

    SECTION("every fixture: every major sync carries the values, nothing else changes") {
        const std::string set = std::string{kEveryOption};
        for (const char* name :
             {"truehd_m51_s24.thd", "truehd_st48_s24.thd", "truehd_mono48_s24.thd",
              "truehd_st192_s24.thd", "truehd_c50_s16.thd", "truehd_st44_s24.thd",
              "truehd_st48_s24_gap8.thd", "truehd_st88_s24.thd"}) {
            DYNAMIC_SECTION(name) {
                const fs::path in = fixture(name);
                const fs::path out = dir / (std::string{"meta_"} + name);
                fs::remove(out);
                REQUIRE(run_cli("metadata " + cli_arg(in) + cli_arg(out) + set, log) == 0);
                const std::string text = read_text(log);
                CHECK(contains(text, "rewrote the channel meaning of "));
                CHECK(contains(text, " major syncs ("));
                CHECK(contains(text, "(audio untouched)"));
                CHECK(contains(text, "6ch dialogue norm"));
                CHECK(contains(text, "0 -> 27"));
                CHECK(contains(text, "2ch control"));
                CHECK(contains(text, "off -> on"));
                CHECK(contains(text, "provisional, no oracle until the DEE set"));
                const auto before = meanings_of(in);
                const auto after = meanings_of(out);  // each major sync read with its CRC checked
                REQUIRE(after.size() == before.size());
                REQUIRE_FALSE(after.empty());
                for (const th::ChannelMeaning& meaning : after) {
                    CHECK(meaning == every_option_meaning());
                }
                CHECK(differ_only_in_major_syncs(in, out));
                // The audio is the same, sample for sample.
                const fs::path a = dir / "meta_a.wav";
                const fs::path b = dir / "meta_b.wav";
                REQUIRE(run_cli("decode " + cli_arg(in) + cli_arg(a), log) == 0);
                REQUIRE(run_cli("decode " + cli_arg(out) + cli_arg(b), log) == 0);
                CHECK(read_bytes(a) == read_bytes(b));
            }
        }
    }

    SECTION("only the fields named change, and probe shows them") {
        const fs::path out = dir / "meta_one.thd";
        REQUIRE(run_cli("metadata " + cli_arg(fixture("truehd_m51_s24.thd")) + cli_arg(out) +
                            "truehd-dialnorm-6ch=24 truehd-mix-level-6ch=9",
                        log) == 0);
        th::ChannelMeaning expected;
        expected.sixch_dialogue_norm = 24;
        expected.sixch_mix_level = 9;
        for (const th::ChannelMeaning& meaning : meanings_of(out)) {
            CHECK(meaning == expected);
        }
        const std::string text = read_text(log);
        CHECK_FALSE(contains(text, "2ch"));
        REQUIRE(run_cli("probe " + cli_arg(out), log) == 0);
        CHECK(contains(read_text(log),
                       "  6ch           control off, dialogue norm 24 (-24 dBFS), mix level 9, "
                       "source format 0\n"));
        // A second edit changes that one field and keeps the first's.
        const fs::path again = dir / "meta_two.thd";
        REQUIRE(run_cli("metadata " + cli_arg(out) + cli_arg(again) + "truehd-control-6ch=on",
                        log) == 0);
        expected.sixch_control_enabled = true;
        for (const th::ChannelMeaning& meaning : meanings_of(again)) {
            CHECK(meaning == expected);
        }
    }

    SECTION("the same values again change no byte, and the input can be the output") {
        const fs::path first = dir / "meta_same_1.thd";
        const fs::path second = dir / "meta_same_2.thd";
        REQUIRE(run_cli("metadata " + cli_arg(fixture("truehd_st48_s24.thd")) + cli_arg(first) +
                            "truehd-dialnorm-2ch=31",
                        log) == 0);
        REQUIRE(run_cli("metadata " + cli_arg(first) + cli_arg(second) + "truehd-dialnorm-2ch=31",
                        log) == 0);
        CHECK(contains(read_text(log), "rewrote the channel meaning of 0 of "));
        CHECK(contains(read_text(log), "the stream already had these values"));
        CHECK(read_bytes(second) == read_bytes(first));
        REQUIRE(run_cli("metadata " + cli_arg(first) + cli_arg(first) + "truehd-dialnorm-2ch=5",
                        log) == 0);
        CHECK(meanings_of(first).front().twoch_dialogue_norm == 5);
    }

    SECTION("the stream on stdout is the file") {
        const fs::path file = dir / "meta_stdout_file.thd";
        const fs::path piped = dir / "meta_stdout.thd";
        REQUIRE(run_cli("metadata " + cli_arg(fixture("truehd_m51_s24.thd")) + cli_arg(file) +
                            "truehd-dialnorm-6ch=20",
                        log) == 0);
        const std::string command = "\"" + std::string(ICLFORGE_CLI_EXE) + "\" metadata " +
                                    cli_arg(fixture("truehd_m51_s24.thd")) +
                                    "- truehd-dialnorm-6ch=20 quiet > \"" + piped.string() + "\"";
        REQUIRE(iclforge::test::platform::run_shell(command) == 0);
        CHECK(read_bytes(piped) == read_bytes(file));
    }

    SECTION("a stream with a different meaning in its major syncs is said so") {
        // Two streams joined: each carries its own, and the edit changes the field in both.
        const fs::path source = source_wav(dir);
        const fs::path a = dir / "meta_join_a.thd";
        const fs::path b = dir / "meta_join_b.thd";
        const fs::path joined = dir / "meta_join.thd";
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(a) +
                            "truehd-restart=8 truehd-dialnorm-6ch=10",
                        log) == 0);
        REQUIRE(run_cli("truehd-encode " + cli_arg(source) + cli_arg(b) +
                            "truehd-restart=8 truehd-dialnorm-6ch=20",
                        log) == 0);
        std::vector<std::byte> bytes = read_bytes(a);
        const std::vector<std::byte> more = read_bytes(b);
        bytes.insert(bytes.end(), more.begin(), more.end());
        {
            std::ofstream out{joined, std::ios::binary};
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        const fs::path out = dir / "meta_join_out.thd";
        REQUIRE(run_cli("metadata " + cli_arg(joined) + cli_arg(out) + "truehd-mix-level-6ch=7",
                        log) == 0);
        CHECK(contains(read_text(log), "the major syncs carried 2 different channel meanings"));
        std::set<unsigned> norms;
        for (const th::ChannelMeaning& meaning : meanings_of(out)) {
            norms.insert(meaning.sixch_dialogue_norm);
            CHECK(meaning.sixch_mix_level == 7);
        }
        CHECK(norms == std::set<unsigned>{10, 20});
    }
}

TEST_CASE("forge metadata of a TrueHD stream refuses what it cannot do, by name",
          "[forge-cli][truehd][truehd-meta][metadata]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "metadata_refused.log";
    const fs::path out = dir / "metadata_refused.thd";
    const std::string in = cli_arg(fixture("truehd_m51_s24.thd"));

    SECTION("nothing to change") {
        fs::remove(out);
        CHECK(run_cli("metadata " + in + cli_arg(out), log) == kExitUsage);
        CHECK(contains(read_text(log), "nothing to change"));
        CHECK(contains(read_text(log), "truehd-dialnorm-2ch="));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("E-AC-3's options are not applied to TrueHD, and say so") {
        for (const char* option : {"dialnorm=27", "dialnorm2=27", "bsmod=2", "dsurmod=1", "insert",
                                   "drc=film-standard"}) {
            DYNAMIC_SECTION(option) {
                fs::remove(out);
                CHECK(run_cli("metadata " + in + cli_arg(out) + "truehd-dialnorm-6ch=27 " + option,
                              log) == kExitUsage);
                const std::string text = read_text(log);
                CHECK(contains(text, std::string{"metadata does not apply "} + option));
                CHECK_FALSE(fs::exists(out));
            }
        }
    }
    SECTION("an option of another command is an unknown one, and nothing is written") {
        for (const char* option : {"compr=0x40", "truehd-restart=8", "truehd-downmix=standard"}) {
            DYNAMIC_SECTION(option) {
                fs::remove(out);
                CHECK(run_cli("metadata " + in + cli_arg(out) + "truehd-dialnorm-6ch=27 " + option,
                              log) == kExitUsage);
                CHECK_FALSE(fs::exists(out));
            }
        }
    }
    SECTION("a value outside its field's width, by name") {
        fs::remove(out);
        CHECK(run_cli("metadata " + in + cli_arg(out) + "truehd-dialnorm-6ch=32", log) ==
              kExitUsage);
        CHECK(contains(read_text(log), "truehd-dialnorm-6ch is a whole number from 0 to 31"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("an MLP stream, whose bytes at that place the specification does not describe") {
        CHECK(run_cli("metadata " + cli_arg(fixture("mlp_st48_s24.mlp")) + cli_arg(out) +
                          "truehd-dialnorm-6ch=27",
                      log) == kExitUsage);
        CHECK(contains(read_text(log), "MLP"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("a stream that is not TrueHD takes none of these options") {
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        REQUIRE(fs::exists(ec3));
        CHECK(run_cli("metadata " + cli_arg(ec3) + cli_arg(dir / "metadata_refused.ec3") +
                          "truehd-dialnorm-6ch=27",
                      log) == kExitUsage);
        CHECK(contains(read_text(log), "is not a TrueHD stream"));
        // and E-AC-3's own metadata is still written
        const fs::path rewritten = dir / "metadata_rewritten.ec3";
        REQUIRE(run_cli("metadata " + cli_arg(ec3) + cli_arg(rewritten) + "dialnorm=20", log) == 0);
        CHECK(contains(read_text(log), "dialnorm 31 -> 20"));
    }
}
