// The P4X panel's developer view, tested on the host - see iclforge/dev_view.hpp's own header
// comment: the triple tap, the numbers as words, the rows of the page, the confirming step of the
// network preference and the console log's tail.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/dev_view.hpp"

using namespace iclforge::devview;

namespace {

// A board with both interfaces up, the cable in use and a stream playing cleanly: the picture the
// design sheet shows.
Facts healthy() {
    Facts f;
    f.active = "ethernet";
    f.preference = "auto";
    // Member by member: the CI's compilers refuse a designated initializer that leaves a member out.
    f.ethernet.present = true;
    f.ethernet.enabled = true;
    f.ethernet.link = true;
    f.ethernet.address = "192.168.1.163";
    f.ethernet.mac = "e8:f6:0a:e7:53:fb";
    f.ethernet.speed_mbps = 100;
    f.ethernet.full_duplex = true;
    f.wifi.present = true;
    f.wifi.enabled = true;
    f.wifi.link = true;
    f.wifi.address = "192.168.1.251";
    f.wifi.mac = "e8:f6:0a:e7:53:f9";
    f.wifi.configured = true;
    f.wifi.ssid = "HomeNet";
    f.wifi.rssi_dbm = -52;
    f.name = "hearth-e753f9";
    f.mdns = "hearth-e753f9.local";
    f.web_address = "192.168.1.163";
    f.profile = "p4-rev3";
    f.profile_hash = "ff9b27a6c4fd";
    f.firmware = "0.10.0-beta.1-5127";
    f.slot = "ota_0";
    f.idf = "v6.1.0";
    f.chip = "ESP32-P4";
    f.revision_major = 3;
    f.revision_minor = 2;
    f.cores = 2;
    f.cpu_mhz = 360;
    f.psram_mhz = 200;
    f.psram_bytes = 32ULL * 1024 * 1024;
    f.layout = "5.1";
    f.sink = "i2s-tdm";
    f.slots = 16;
    f.slot_bits = 32;
    f.uptime_ms = (3ULL * 3600 + 12 * 60) * 1000;
    f.reset_reason = "sw";
    f.player = true;
    f.server = "Hearth on STUDIO-PC";
    f.connections = 1;
    f.paired = 2;
    f.clock_converged = true;
    f.clock_error_us = 120;
    f.stream = "bursts";
    f.bursts = 1234;
    f.peak_db = {-12.0F, -14.5F, -30.0F, -20.0F, -80.0F, -18.0F};
    f.internal_free = 100'762;
    f.internal_low = 62'464;
    f.internal_largest = 53'248;
    f.psram_free = 29'800'000;
    f.psram_low = 29'000'000;
    f.render_us_mean = 6100;
    f.render_us_max = 20'000;
    f.touch_reads = 12'345;
    f.touch_points = 41;
    return f;
}

const Cell* find(const Section& section, const std::string& key) {
    for (const Row& row : section.rows) {
        for (int i = 0; i < row.used; ++i) {
            if (row.cells[static_cast<std::size_t>(i)].key == key) {
                return &row.cells[static_cast<std::size_t>(i)];
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("three taps close together at one place are a triple tap", "[device][dev_view]") {
    TapStreak streak;
    CHECK_FALSE(streak.tap(100, 570, 1000));
    CHECK(streak.count() == 1);
    CHECK_FALSE(streak.tap(104, 566, 1250));
    CHECK(streak.count() == 2);
    CHECK(streak.tap(98, 574, 1500));
    // The streak starts over: a fourth tap is the first of the next.
    CHECK(streak.count() == 0);
    CHECK_FALSE(streak.tap(100, 570, 1700));
    CHECK(streak.count() == 1);
}

TEST_CASE("a slow tap or a far one breaks the streak", "[device][dev_view]") {
    TapStreak streak;
    CHECK_FALSE(streak.tap(100, 570, 0));
    CHECK_FALSE(streak.tap(100, 570, 300));
    // 700 ms after the last: too slow, so this is the first of a new streak.
    CHECK_FALSE(streak.tap(100, 570, 1000));
    CHECK(streak.count() == 1);
    CHECK_FALSE(streak.tap(100, 570, 1200));
    // 200 px away: another place, another streak.
    CHECK_FALSE(streak.tap(300, 570, 1400));
    CHECK(streak.count() == 1);
    CHECK_FALSE(streak.tap(300, 570, 1600));
    CHECK(streak.tap(300, 570, 1800));
}

TEST_CASE("the gap and the wander are as wide as a finger needs", "[device][dev_view]") {
    TapStreak streak;
    // Exactly the gap, and exactly the radius, still count.
    CHECK_FALSE(streak.tap(100, 570, 0));
    CHECK_FALSE(streak.tap(100 + TapStreak::kRadiusPx, 570, TapStreak::kGapMs));
    CHECK(streak.count() == 2);
    // One more millisecond, or one more pixel, does not.
    CHECK_FALSE(streak.tap(100 + TapStreak::kRadiusPx, 570, 2 * TapStreak::kGapMs + 1));
    CHECK(streak.count() == 1);
    CHECK_FALSE(streak.tap(100 + TapStreak::kRadiusPx + TapStreak::kRadiusPx + 1, 570, 2 * TapStreak::kGapMs + 100));
    CHECK(streak.count() == 1);
}

TEST_CASE("a streak survives the millisecond clock wrapping", "[device][dev_view]") {
    TapStreak streak;
    const std::uint32_t near_end = 0xFFFFFF00U;
    CHECK_FALSE(streak.tap(50, 580, near_end));
    CHECK_FALSE(streak.tap(50, 580, near_end + 200));  // wraps to 0x000000C8
    CHECK(streak.tap(50, 580, near_end + 400));
}

TEST_CASE("a streak can be cleared", "[device][dev_view]") {
    TapStreak streak;
    CHECK_FALSE(streak.tap(1, 1, 0));
    CHECK_FALSE(streak.tap(1, 1, 100));
    streak.reset();
    CHECK_FALSE(streak.tap(1, 1, 200));
    CHECK(streak.count() == 1);
}

TEST_CASE("numbers are written for a person", "[device][dev_view]") {
    CHECK(grouped(0) == "0");
    CHECK(grouped(999) == "999");
    CHECK(grouped(1000) == "1,000");
    CHECK(grouped(1234567) == "1,234,567");
    CHECK(grouped(1000000000ULL) == "1,000,000,000");

    CHECK(duration_text(0) == "0 s");
    CHECK(duration_text(7'400) == "7 s");
    CHECK(duration_text(123'000) == "2 min 03 s");
    CHECK(duration_text((3 * 3600 + 12 * 60) * 1000ULL) == "3 h 12 min");
    CHECK(duration_text((2 * 86400 + 4 * 3600 + 59 * 60) * 1000ULL) == "2 d 4 h");

    CHECK(bytes_text(512) == "512 B");
    CHECK(bytes_text(1024) == "1 KiB");
    CHECK(bytes_text(100'762) == "98.4 KiB");
    CHECK(bytes_text(32ULL * 1024 * 1024) == "32 MiB");
    CHECK(bytes_text(29'800'000) == "28.4 MiB");

    CHECK(micros_text(412) == "412 us");
    CHECK(micros_text(6100) == "6.1 ms");
    CHECK(micros_text(1'200'000) == "1.20 s");
    CHECK(micros_text(400, true) == "+400 us");
    CHECK(micros_text(-2100, true) == "-2.1 ms");
    CHECK(micros_text(0, true) == "0 us");

    CHECK(level_fraction(0.0F) == 1.0F);
    CHECK(level_fraction(-30.0F) == 0.5F);
    CHECK(level_fraction(-60.0F) == 0.0F);
    CHECK(level_fraction(-200.0F) == 0.0F);
    CHECK(level_fraction(3.0F) == 1.0F);
}

TEST_CASE("the network section says which interface is in use and where each one is", "[device][dev_view]") {
    const Page page = describe(healthy());
    const Section& net = page.network;
    REQUIRE(net.rows.size() == 6);
    CHECK(net.rows[0].cells[0].key == "Wired");
    CHECK(net.rows[0].cells[0].value == "in use \xC2\xB7 up 100 Mb/s full duplex");
    CHECK(net.rows[1].cells[0].value == "192.168.1.163 \xC2\xB7 e8:f6:0a:e7:53:fb");
    CHECK(net.rows[2].cells[0].key == "Wi-Fi");
    CHECK(net.rows[2].cells[0].value == "HomeNet \xC2\xB7 -52 dBm");
    CHECK(net.rows[2].cells[0].tone == Tone::text);
    CHECK(net.rows[3].cells[0].value == "192.168.1.251 \xC2\xB7 e8:f6:0a:e7:53:f9");
    CHECK(find(net, "Name")->value == "hearth-e753f9 \xC2\xB7 hearth-e753f9.local");
    CHECK(find(net, "Web page")->value == "http://192.168.1.163/");
}

TEST_CASE("a cable pulled out reads as no link, and the radio is in use", "[device][dev_view]") {
    Facts f = healthy();
    f.active = "wifi";
    f.ethernet.link = false;
    f.ethernet.address.clear();
    f.ethernet.speed_mbps.reset();
    f.web_address = "192.168.1.251";
    const Section net = describe(f).network;
    CHECK(net.rows[0].cells[0].value == "no link");
    CHECK(net.rows[0].cells[0].tone == Tone::muted);
    CHECK(net.rows[1].cells[0].value == "no address \xC2\xB7 e8:f6:0a:e7:53:fb");
    CHECK(net.rows[2].cells[0].value == "in use \xC2\xB7 HomeNet \xC2\xB7 -52 dBm");
    CHECK(find(net, "Web page")->value == "http://192.168.1.251/");
}

TEST_CASE("an interface the preference leaves out says so; a link without an address is waiting for one", "[device][dev_view]") {
    Facts f = healthy();
    f.preference = "ethernet";
    f.wifi.enabled = false;
    f.wifi.link = false;
    f.wifi.address.clear();
    f.wifi.ssid.clear();
    f.wifi.rssi_dbm.reset();
    Section net = describe(f).network;
    CHECK(net.rows[2].cells[0].value == "off (the preference leaves it out)");
    CHECK(net.rows[2].cells[0].tone == Tone::muted);

    f = healthy();
    f.ethernet.address.clear();
    net = describe(f).network;
    CHECK(net.rows[1].cells[0].value == "no address yet \xC2\xB7 e8:f6:0a:e7:53:fb");
    CHECK(net.rows[1].cells[0].tone == Tone::muted);
}

TEST_CASE("a faint radio signal turns the Wi-Fi row to the attention colour", "[device][dev_view]") {
    Facts f = healthy();
    f.wifi.rssi_dbm = -75;
    CHECK(describe(f).network.rows[2].cells[0].tone == Tone::text);
    f.wifi.rssi_dbm = -76;
    CHECK(describe(f).network.rows[2].cells[0].tone == Tone::attention);
    // The cable's row has no signal to be weak.
    CHECK(describe(f).network.rows[0].cells[0].tone == Tone::text);
    // Not joined: nothing to hear.
    f.wifi.link = false;
    CHECK(describe(f).network.rows[2].cells[0].tone == Tone::muted);
}

TEST_CASE("a Wi-Fi station with no network stored, or not joined, says which", "[device][dev_view]") {
    Facts f = healthy();
    f.wifi.link = false;
    f.wifi.address.clear();
    f.wifi.ssid.clear();
    f.wifi.rssi_dbm.reset();
    CHECK(link_text(f.wifi, true) == "not joined");
    f.wifi.configured = false;
    CHECK(link_text(f.wifi, true) == "no network stored");
}

TEST_CASE("a build with no second interface shows one row for the network", "[device][dev_view]") {
    Facts f;
    f.web_address = "10.0.2.15";
    f.name = "hearth-aabbcc";
    const Section net = describe(f).network;
    CHECK(find(net, "Network")->value == "10.0.2.15");
    CHECK(find(net, "Wired") == nullptr);
    f.web_address.clear();
    CHECK(find(describe(f).network, "Network")->tone == Tone::attention);
    CHECK(web_url(f) == "no address yet");
    f.web_address = "10.0.2.15";
    f.web_port = 8080;
    CHECK(web_url(f) == "http://10.0.2.15:8080/");
}

TEST_CASE("the configuration section is what the board is", "[device][dev_view]") {
    const Section config = describe(healthy()).configuration;
    CHECK(find(config, "Profile")->value == "p4-rev3 \xC2\xB7 ff9b27a6c4fd");
    CHECK(find(config, "Firmware")->value == "0.10.0-beta.1-5127 \xC2\xB7 ota_0");
    CHECK(find(config, "Chip")->value == "ESP32-P4 v3.2 \xC2\xB7 IDF v6.1.0");
    CHECK(find(config, "Clocks")->value == "2 \xC3\x97 360 MHz \xC2\xB7 PSRAM 32 MiB at 200 MHz");
    CHECK(find(config, "Output")->value == "5.1 \xC2\xB7 16 slots \xC3\x97 32 bit \xC2\xB7 i2s-tdm");
    CHECK(find(config, "Run")->value == "up 3 h 12 min \xC2\xB7 reset: sw");
    CHECK(find(config, "Run")->tone == Tone::text);
    CHECK(find(config, "Crash")->value == "no core dump stored");
    CHECK(find(config, "Crash")->tone == Tone::muted);
}

TEST_CASE("a panic, a stored core dump and a profile that is not there are noticed", "[device][dev_view]") {
    Facts f = healthy();
    f.reset_reason = "panic";
    f.core_dump = true;
    f.core_dump_bytes = 262'144;
    f.profile.clear();
    const Section config = describe(f).configuration;
    CHECK(find(config, "Run")->tone == Tone::attention);
    CHECK(find(config, "Crash")->value == "core dump stored, 256 KiB");
    CHECK(find(config, "Crash")->tone == Tone::attention);
    CHECK(find(config, "Profile")->value == "none");

    // The RTC watchdog is how this board restarts (system_reset.cpp): not a fault.
    CHECK_FALSE(reset_was_a_fault("wdt"));
    CHECK_FALSE(reset_was_a_fault("sw"));
    CHECK_FALSE(reset_was_a_fault("poweron"));
    CHECK(reset_was_a_fault("task_wdt"));
    CHECK(reset_was_a_fault("int_wdt"));
    CHECK(reset_was_a_fault("brownout"));
}

TEST_CASE("the streaming section reads clean when nothing has gone wrong", "[device][dev_view]") {
    const Section stream = describe(healthy()).streaming;
    CHECK(find(stream, "Server")->value == "Hearth on STUDIO-PC");
    CHECK(find(stream, "Clock")->value == "converged \xC2\xB7 error 120 us");
    CHECK(find(stream, "Stream")->value == "bursts \xC2\xB7 6 outputs \xC2\xB7 1 conn \xC2\xB7 2 paired");
    CHECK(find(stream, "Bursts")->value == "1,234");
    for (const char* key : {"Underruns", "Late", "Dropped", "Resyncs", "Invalid"}) {
        REQUIRE(find(stream, key) != nullptr);
        CHECK(find(stream, key)->value == "0");
        CHECK(find(stream, key)->tone == Tone::text);
    }
    // No play from POST /play: its figures are dashes, in the quiet colour.
    CHECK(find(stream, "Sink")->value == "-");
    CHECK(find(stream, "Sink")->tone == Tone::muted);
}

TEST_CASE("every counter that is not zero turns to the attention colour", "[device][dev_view]") {
    Facts f = healthy();
    f.underruns = 3;
    f.late = 1200;
    f.dropped = 2;
    f.resyncs = 1;
    f.invalid = 7;
    const Section stream = describe(f).streaming;
    CHECK(find(stream, "Underruns")->tone == Tone::attention);
    CHECK(find(stream, "Late")->value == "1,200");
    CHECK(find(stream, "Late")->tone == Tone::attention);
    CHECK(find(stream, "Dropped")->tone == Tone::attention);
    CHECK(find(stream, "Resyncs")->tone == Tone::attention);
    CHECK(find(stream, "Invalid")->tone == Tone::attention);
    CHECK(find(stream, "Bursts")->tone == Tone::text);
}

TEST_CASE("a clock that has not converged matters only while something plays", "[device][dev_view]") {
    Facts f = healthy();
    f.clock_converged = false;
    CHECK(find(describe(f).streaming, "Clock")->value == "not converged");
    CHECK(find(describe(f).streaming, "Clock")->tone == Tone::attention);
    f.stream = "idle";
    f.peak_db.clear();
    CHECK(find(describe(f).streaming, "Clock")->tone == Tone::muted);
    CHECK(find(describe(f).streaming, "Stream")->value == "idle \xC2\xB7 1 conn \xC2\xB7 2 paired");
    f.server.clear();
    f.connections = 0;
    CHECK(find(describe(f).streaming, "Server")->value == "none");
    CHECK(find(describe(f).streaming, "Server")->tone == Tone::muted);
}

TEST_CASE("the play figures appear once a play has run", "[device][dev_view]") {
    Facts f = healthy();
    f.played = true;
    f.us_per_frame = 412;
    f.worst_frame_us = 1120;
    f.sink_us_per_frame = 250;
    f.render_us_per_frame = 80;
    const Section stream = describe(f).streaming;
    CHECK(find(stream, "Play")->value == "412 us/frame \xC2\xB7 worst 1.1 ms");
    CHECK(find(stream, "Play")->tone == Tone::text);
    CHECK(find(stream, "Sink")->value == "250 us/frame");
    CHECK(find(stream, "Render")->value == "80 us/frame");
    // The playout's offset and the burst time say their worst beside them.
    CHECK(find(stream, "Offset")->value == "0 us \xC2\xB7 worst 0 us");
    f.error_us = 400;
    f.worst_error_us = -2100;
    f.burst_us = 410;
    f.worst_burst_us = 1120;
    CHECK(find(describe(f).streaming, "Offset")->value == "+400 us \xC2\xB7 worst -2.1 ms");
    CHECK(find(describe(f).streaming, "Burst")->value == "410 us \xC2\xB7 worst 1.1 ms");
}

TEST_CASE("a build without a Sendspin player says so, and keeps the play figures", "[device][dev_view]") {
    Facts f = healthy();
    f.player = false;
    const Section stream = describe(f).streaming;
    CHECK(find(stream, "Player")->value == "no Sendspin player in this build");
    CHECK(find(stream, "Underruns") == nullptr);
    CHECK(find(stream, "Play") != nullptr);
}

TEST_CASE("the performance section watches the heap and the panel's own cost", "[device][dev_view]") {
    Facts f = healthy();
    Section perf = describe(f).performance;
    CHECK(find(perf, "Heap")->value == "98.4 KiB free \xC2\xB7 low 61 KiB \xC2\xB7 block 52 KiB");
    CHECK(find(perf, "Heap")->tone == Tone::text);
    CHECK(find(perf, "PSRAM")->value == "28.4 MiB free \xC2\xB7 low 27.7 MiB");
    CHECK(find(perf, "Panel")->value == "render 6.1 ms (max 20.0 ms) \xC2\xB7 0 late");
    CHECK(find(perf, "Touch")->value == "12,345 reads \xC2\xB7 0 failed \xC2\xB7 41 touches");
    f.internal_low = 9000;
    f.late_cycles = 4;
    f.touch_failures = 2;
    perf = describe(f).performance;
    CHECK(find(perf, "Heap")->tone == Tone::attention);
    // The panel's own late cycles are not a fault: the task is the lowest, and the view's own
    // refreshes make some.
    CHECK(find(perf, "Panel")->value == "render 6.1 ms (max 20.0 ms) \xC2\xB7 4 late");
    CHECK(find(perf, "Panel")->tone == Tone::text);
    CHECK(find(perf, "Touch")->tone == Tone::attention);
}

TEST_CASE("the performance section shows the Meters view's rate and costs once it has been opened", "[device][dev_view]") {
    Facts f = healthy();
    CHECK(find(describe(f).performance, "Meters") == nullptr);
    f.meters_opens = 2;
    f.meters_showing = true;
    f.meters_rate_hz = 20;
    f.meters_shape = "grid 12";
    f.meters_analysis_us = 8200;
    f.meters_widgets_us = 1100;
    f.meters_render_us = 12000;
    f.meters_late = 3;
    f.meters_tap_us = 21;
    Section perf = describe(f).performance;
    CHECK(find(perf, "Meters")->value == "20 Hz \xC2\xB7 grid 12 \xC2\xB7 showing");
    CHECK(find(perf, "Meters")->tone == Tone::text);
    CHECK(find(perf, "Update")->value == "analysis 8.2 ms \xC2\xB7 widgets 1.1 ms \xC2\xB7 render 12.0 ms");
    CHECK(find(perf, "Missed")->value == "3 late \xC2\xB7 0 dropped \xC2\xB7 tap 21 us/burst");
    CHECK(find(perf, "Missed")->tone == Tone::text);
    // The governor's ten a second, and a dropped step, are worth a look; a closed view's figures are quiet.
    f.meters_rate_hz = 10;
    f.meters_dropped = 1;
    perf = describe(f).performance;
    CHECK(find(perf, "Meters")->tone == Tone::attention);
    CHECK(find(perf, "Missed")->tone == Tone::attention);
    f.meters_showing = false;
    f.meters_dropped = 0;
    perf = describe(f).performance;
    CHECK(find(perf, "Meters")->value == "10 Hz \xC2\xB7 grid 12 \xC2\xB7 closed");
    CHECK(find(perf, "Meters")->tone == Tone::muted);
    CHECK(find(perf, "Update")->tone == Tone::muted);
}

TEST_CASE("the preference words round-trip", "[device][dev_view]") {
    for (const Preference p : {Preference::automatic, Preference::ethernet, Preference::wifi}) {
        const auto parsed = parse_preference(preference_word(p));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == p);
    }
    CHECK_FALSE(parse_preference("").has_value());
    CHECK_FALSE(parse_preference("Wi-Fi").has_value());
}

TEST_CASE("the confirming step names what is about to be switched off", "[device][dev_view]") {
    Facts f = healthy();
    Confirm c = confirm_text(f, Preference::automatic);
    CHECK(c.headline == "Use automatic?");
    CHECK_FALSE(c.blocked);

    c = confirm_text(f, Preference::wifi);
    CHECK(c.headline == "Use Wi-Fi only?");
    CHECK(c.detail.starts_with("Wired is switched off."));
    // On the cable now: the page the person is reading from moves.
    CHECK(c.detail.find("The board is on wired now") != std::string::npos);
    CHECK(c.detail.find("192.168.1.251") != std::string::npos);
    CHECK_FALSE(c.blocked);

    f.active = "wifi";
    c = confirm_text(f, Preference::ethernet);
    CHECK(c.headline == "Use wired only?");
    CHECK(c.detail.find("The board is on Wi-Fi now") != std::string::npos);
    CHECK(c.detail.find("192.168.1.163") != std::string::npos);

    f.active = "ethernet";
    c = confirm_text(f, Preference::ethernet);
    CHECK(c.detail == "Wi-Fi is switched off.");
}

TEST_CASE("a change the board would refuse is blocked before it is asked", "[device][dev_view]") {
    // The cable is on and has no link: nothing would answer a pin to it.
    Facts f = healthy();
    f.ethernet.link = false;
    f.ethernet.address.clear();
    Confirm c = confirm_text(f, Preference::ethernet);
    CHECK(c.blocked);
    CHECK(c.tone == Tone::attention);
    CHECK(c.detail.find("Wired has no link") != std::string::npos);

    // No Wi-Fi network is stored: the board answers PUT with a refusal.
    f = healthy();
    f.wifi.configured = false;
    c = confirm_text(f, Preference::wifi);
    CHECK(c.blocked);
    CHECK(c.detail.find("No Wi-Fi network is stored") != std::string::npos);

    // Auto never cuts the board off.
    CHECK_FALSE(confirm_text(f, Preference::automatic).blocked);
}

TEST_CASE("an interface the current pin has switched off can be pinned to: the board brings it up first", "[device][dev_view]") {
    // Pinned to the cable: the station is off, has no address, and is still the way back to Wi-Fi.
    Facts f = healthy();
    f.preference = "ethernet";
    f.wifi.enabled = false;
    f.wifi.link = false;
    f.wifi.address.clear();
    f.wifi.ssid.clear();
    f.wifi.rssi_dbm.reset();
    Confirm c = confirm_text(f, Preference::wifi);
    CHECK_FALSE(c.blocked);
    CHECK(c.detail.find("Wi-Fi is joined first") != std::string::npos);

    // The same the other way round: the cable's MAC is stopped, so its link reads as down.
    f = healthy();
    f.active = "wifi";
    f.preference = "wifi";
    f.ethernet.enabled = false;
    f.ethernet.link = false;
    f.ethernet.address.clear();
    c = confirm_text(f, Preference::ethernet);
    CHECK_FALSE(c.blocked);
    CHECK(c.detail.find("Wired is brought up first") != std::string::npos);

    // A station that is on and has not joined yet: the board keeps the cable if it cannot.
    f = healthy();
    f.wifi.link = false;
    f.wifi.address.clear();
    c = confirm_text(f, Preference::wifi);
    CHECK_FALSE(c.blocked);
    CHECK(c.detail.find("keeps wired") != std::string::npos);
}

TEST_CASE("a change in progress is described", "[device][dev_view]") {
    CHECK(net_change_text(NetChange::idle, "wifi").empty());
    CHECK(net_change_text(NetChange::working, "wifi") == "Switching to Wi-Fi...");
    CHECK(net_change_text(NetChange::working, "ethernet") == "Switching to wired...");
    CHECK(net_change_text(NetChange::working, "auto") == "Switching to automatic...");
    CHECK(net_change_text(NetChange::done, "wifi") == "Done.");
    CHECK(net_change_text(NetChange::refused, "wifi") == "The board refused the change.");
}

TEST_CASE("the log's tail is the last lines, cleaned for a label", "[device][dev_view]") {
    const std::string text =
        "I (100) boot: one\n"
        "\n"
        "   \n"
        "I (200) net: two\r\n"
        "\x1b[0;32mI (300) net: three\x1b[0m\n"
        "I (400) tab:\tfour\n"
        "panel: five";
    auto lines = log_tail(text, 3, 80);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "I (300) net: three");
    CHECK(lines[1] == "I (400) tab: four");
    CHECK(lines[2] == "panel: five");

    lines = log_tail(text, 10, 80);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "I (100) boot: one");
    CHECK(lines[1] == "I (200) net: two");
    CHECK(log_tail("", 5, 80).empty());
}

TEST_CASE("a long log line is cut, and a letter the font may lack becomes a question mark", "[device][dev_view]") {
    const auto lines = log_tail("0123456789abcdefghij\nsta: joined caf\xC3\xA9 \xE2\x99\xAA net\n", 5, 12);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "012345678...");
    CHECK(lines[1] == "sta: join...");
    const auto wide = log_tail("joined caf\xC3\xA9 \xE2\x99\xAA net\n", 5, 80);
    CHECK(wide[0] == "joined caf? ? net");
}

TEST_CASE("a log line that names a secret is not shown", "[device][dev_view]") {
    const auto lines = log_tail(
        "I (1) net: joined\n"
        "sendspin: pairing token abcDEF123\n"
        "wifi: Password for HomeNet is hunter2\n"
        "provision: PASSPHRASE set\n"
        "I (2) net: up\n",
        10, 80);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "I (1) net: joined");
    for (std::size_t i : {1U, 2U, 3U}) {
        CHECK(lines[i] == "(a line that names a secret is not shown)");
    }
    CHECK(lines[4] == "I (2) net: up");
}
