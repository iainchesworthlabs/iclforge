// The keys of a board's own settings and what Reset preferences does to each, tested on the host - see
// iclforge/settings_keys.hpp's own header comment. These hold the LIST, which needs no registry: a
// preference that is added later has to be listed, and a setting that is not a preference has to stay.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/settings_keys.hpp"

namespace sk = iclforge::settingskeys;

namespace {

std::set<std::string> names_of(sk::Kind kind) {
    std::set<std::string> out;
    for (const sk::Key& key : sk::kKeys) {
        if (key.kind == kind) {
            out.insert(key.nvs);
        }
    }
    return out;
}

std::set<std::string> preferences() { return names_of(sk::Kind::preference); }

std::set<std::string> everything_else() {
    std::set<std::string> out;
    for (const sk::Key& key : sk::kKeys) {
        if (key.kind != sk::Kind::preference) {
            out.insert(key.nvs);
        }
    }
    return out;
}

// ICLFORGE_REPO_ROOT is the repository's root, given by this binary's CMakeLists.txt; the file is empty when the
// source tree is not there to read (a binary run away from its checkout).
std::string read_file(const std::string& relative) {
    std::ifstream in(std::string(ICLFORGE_REPO_ROOT) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("every key is an NVS key and is listed once", "[settingskeys]") {
    std::set<std::string> seen;
    for (const sk::Key& key : sk::kKeys) {
        const std::string name = key.nvs;
        INFO("key " << name);
        CHECK_FALSE(name.empty());
        CHECK(name.size() <= sk::kMaxKeyChars);
        CHECK(std::all_of(name.begin(), name.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'; }));
        CHECK(seen.insert(name).second);
        CHECK(std::string(key.what).size() > 3);
    }
}

TEST_CASE("Reset preferences keeps what the board is, how it is joined and how it is wired", "[settingskeys]") {
    // The name, the Wi-Fi network and its passphrase, the choice between a cable and Wi-Fi, the DAC wiring and
    // the speaker layout, and the technical time settings: a reset board has to stay named, on its network,
    // in its room and paired.
    const std::set<std::string> kept = everything_else();
    for (const char* name :
         {"name", "ssid", "password", "net_pref", "slot_bits", "second_line", "layout", "speaker_trim", "time_zone",
          "ntp_server", "time_sync"}) {
        INFO("kept " << name);
        CHECK(kept.contains(name));
        CHECK(sk::reset_of(name) == sk::Reset::keep);
        CHECK(sk::find(name) != nullptr);
    }
}

TEST_CASE("Reset preferences erases the choices a person made once", "[settingskeys]") {
    // What the screen shows without lyrics, and when it dims. Later preferences join this list (brightness, the clock's
    // format, the night schedule, the late-night mode, the speakers' style); the test asks only that these are in it, so
    // adding one does not break it. The time zone is not: it is where the room is, and the per-speaker trims and delays
    // are the room's calibration (the owner's call, 2026-10-25), both kept.
    const std::set<std::string> erased = preferences();
    for (const char* name : {"np_view", "np_dim", "np_style"}) {
        INFO("erased " << name);
        CHECK(erased.contains(name));
        CHECK(sk::reset_of(name) == sk::Reset::erase);
    }
    // Nothing is a preference and kept at once.
    for (const std::string& name : erased) {
        CHECK_FALSE(everything_else().contains(name));
    }
}

TEST_CASE("a key the table does not list is kept, and said", "[settingskeys]") {
    CHECK(sk::reset_of("some_future_key") == sk::Reset::keep_unlisted);
    CHECK(sk::find("some_future_key") == nullptr);
    CHECK(sk::reset_of("") == sk::Reset::keep_unlisted);
}

TEST_CASE("the sentences name what a reset changes and what it keeps, each thing once", "[settingskeys]") {
    const std::string reset = sk::reset_words();
    CHECK(reset.find("what the screen shows without lyrics") != std::string::npos);
    CHECK(reset.find("when the screen dims") != std::string::npos);
    CHECK(reset.find("speakers style") != std::string::npos);
    // None of what it keeps is in what it changes.
    CHECK(reset.find("time zone") == std::string::npos);
    CHECK(reset.find("name") == std::string::npos);
    CHECK(reset.find("Wi-Fi") == std::string::npos);

    const std::string kept = sk::kept_words();
    CHECK(kept.find("name") != std::string::npos);
    CHECK(kept.find("Wi-Fi network") != std::string::npos);
    CHECK(kept.find("DAC wiring") != std::string::npos);
    CHECK(kept.find("time zone") != std::string::npos);
    CHECK(kept.find("speaker layout") != std::string::npos);
    CHECK(kept.find("per-speaker trims and delays") != std::string::npos);
    // A phrase two keys share (the SSID and the passphrase) is said once.
    CHECK(kept.find("Wi-Fi network") == kept.rfind("Wi-Fi network"));
    CHECK(kept.find("DAC wiring") == kept.rfind("DAC wiring"));
    CHECK(kept.find(" and ") != std::string::npos);
}

TEST_CASE("a list reads as a sentence", "[settingskeys]") {
    using sk::detail::joined;
    CHECK(joined({}).empty());
    CHECK(joined({"a"}) == "a");
    CHECK(joined({"a", "b"}) == "a and b");
    CHECK(joined({"a", "b", "c"}) == "a, b and c");
}

TEST_CASE("every key the header spells out is in the table", "[settingskeys]") {
    // A key added to the header's list of names without a row of kKeys would be written to NVS and be
    // classified by nobody.
    const std::string header = read_file("libs/device/include/iclforge/settings_keys.hpp");
    if (header.empty()) {
        SKIP("the repository root is not known to this binary (ICLFORGE_REPO_ROOT)");
    }
    std::set<std::string> spelled;
    const std::string marker = "inline constexpr const char* k";
    for (std::size_t at = header.find(marker); at != std::string::npos; at = header.find(marker, at + 1)) {
        const std::size_t quote = header.find('"', at);
        const std::size_t end = header.find('"', quote + 1);
        const std::size_t semicolon = header.find(';', at);
        if (quote == std::string::npos || end == std::string::npos || quote > semicolon) {
            continue;
        }
        spelled.insert(header.substr(quote + 1, end - quote - 1));
    }
    REQUIRE(spelled.size() >= 12);
    std::set<std::string> listed;
    for (const sk::Key& key : sk::kKeys) {
        listed.insert(key.nvs);
    }
    CHECK(spelled == listed);
}

TEST_CASE("settings.cpp takes every key name from the table", "[settingskeys]") {
    const std::string source = read_file("firmware/hearth-sink/main/settings.cpp");
    if (source.empty()) {
        SKIP("the repository root is not known to this binary (ICLFORGE_REPO_ROOT)");
    }
    // A key spelled out in settings.cpp itself (`constexpr const char* kKeyBrightness = "bright";`, as a
    // setting added there is tempted to) is still a key the table has to list: it is read and written all
    // the same, and Reset preferences has to know which kind it is.
    const std::string marker = "constexpr const char* kKey";
    for (std::size_t at = source.find(marker); at != std::string::npos; at = source.find(marker, at + 1)) {
        const std::size_t quote = source.find('"', at);
        const std::size_t end = source.find('"', quote + 1);
        REQUIRE(quote != std::string::npos);
        REQUIRE(end != std::string::npos);
        const std::string key = source.substr(quote + 1, end - quote - 1);
        INFO("a key spelled out in settings.cpp: " << key);
        CHECK(sk::find(key) != nullptr);
    }
    // And nothing reads or writes a key by a literal at the call.
    CHECK(source.find("nvs_get_i32(handle, \"") == std::string::npos);
    CHECK(source.find("nvs_get_str(handle, \"") == std::string::npos);
    CHECK(source.find("write_int(\"") == std::string::npos);
    CHECK(source.find("write_string(\"") == std::string::npos);
    CHECK(source.find("namespace sk = iclforge::settingskeys;") != std::string::npos);
    CHECK(source.find("sk::kName") != std::string::npos);
}
