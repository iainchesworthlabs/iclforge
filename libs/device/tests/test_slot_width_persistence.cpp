// A speaker layout set on a board outlives a restart only if the slot width it needs does too: a line of the S3 and
// C6 sink carries 4 slots at 32-bit and 8 at 16-bit, and boot keeps a stored layout only when the sink carries it
// (firmware/hearth-sink/main/hearth_sink.cpp). These hold the two halves of that to the source, as
// test_settings_keys.cpp does for the keys: PUT /slot-width stores the width, and boot applies the stored width
// before it judges the stored layout. A 5.1.2 set on an S3 came back as 2.0 after every update while the width
// was only held in RAM.

#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {

std::string read_file(const std::string& relative) {
    std::ifstream in(std::string(ICLFORGE_REPO_ROOT) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

constexpr const char* kSink = "firmware/hearth-sink/main/hearth_sink.cpp";

}  // namespace

TEST_CASE("PUT /slot-width stores the width it changed", "[slotwidth]") {
    const std::string source = read_file(kSink);
    if (source.empty()) {
        SKIP("the repository root is not known to this binary (ICLFORGE_REPO_ROOT)");
    }
    const std::size_t handler = source.find("h.set_slot_bits = ");
    REQUIRE(handler != std::string::npos);
    const std::size_t changed = source.find("player::sink_set_slot_bits(bits)", handler);
    const std::size_t stored = source.find("player::settings_set_slot_bits(bits)", handler);
    const std::size_t next_handler = source.find("h.stats = ", handler);
    REQUIRE(changed != std::string::npos);
    REQUIRE(stored != std::string::npos);
    CHECK(changed < stored);       // stored only after the sink took the width
    CHECK(stored < next_handler);  // inside this handler
}

TEST_CASE("boot applies the stored width before it judges the stored layout", "[slotwidth]") {
    const std::string source = read_file(kSink);
    if (source.empty()) {
        SKIP("the repository root is not known to this binary (ICLFORGE_REPO_ROOT)");
    }
    const std::size_t width = source.find("player::sink_set_slot_bits(player::settings().slot_bits)");
    const std::size_t layout = source.find("player::settings().layout[0] != '\0'");
    REQUIRE(width != std::string::npos);
    REQUIRE(layout != std::string::npos);
    CHECK(width < layout);
}
