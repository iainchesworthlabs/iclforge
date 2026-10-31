#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The ESP32-P4X panel's developer view (planning/karaoke-p4x-panel.md, "The developer view"): the
// facts a developer wants of a board, as one plain struct, and the words and numbers the screen
// shows for them. Three taps on the board's name in the footer open it, a tap closes it.
//
// Free of ESP-IDF and of LVGL, like hardware_info.hpp and log_ring.hpp beside it: the firmware
// fills `Facts` from what it already knows (panel.cpp), the preview fills it from a fixture, and
// everything between a number and a label - the thousands separators, "3 h 12 min", which cell
// turns the accent colour, which words the confirming step says - is here, where
// libs/device/tests/test_dev_view.cpp holds it to the letter.
//
// What the view never carries: the Wi-Fi passphrase and the Sendspin pairing token. Facts has no
// field for either, and log_tail() hides a console line that names one (the console prints a token
// when it is asked to, and the log ring keeps what the console printed).

namespace iclforge::devview {

// --- the triple tap ------------------------------------------------------------------------------

// Counts taps that follow each other closely enough, near enough to each other, to be one streak.
// LVGL keeps a streak of its own on the pointer, but with a 400 ms gap and 10 pixels of wander,
// which on this panel (0.15 mm a pixel) is a finger that must not move 1.5 mm between taps; this
// one is the decision the panel makes, and a person's three taps at a label pass it.
class TapStreak {
   public:
    static constexpr int kTaps = 3;
    // From the end of one tap to the end of the next.
    static constexpr std::uint32_t kGapMs = 600;
    // From one tap's place to the next one's.
    static constexpr int kRadiusPx = 64;

    // A tap that ended at (x, y) at `now_ms`, on any steady millisecond clock that may wrap. True
    // for the tap that completes a streak, which starts the next one over: a fourth tap is the first
    // of another.
    bool tap(int x, int y, std::uint32_t now_ms) {
        const std::int64_t dx = x - last_x_;
        const std::int64_t dy = y - last_y_;
        const bool follows = count_ > 0 && static_cast<std::uint32_t>(now_ms - last_ms_) <= kGapMs &&
                             dx * dx + dy * dy <= static_cast<std::int64_t>(kRadiusPx) * kRadiusPx;
        count_ = follows ? count_ + 1 : 1;
        last_x_ = x;
        last_y_ = y;
        last_ms_ = now_ms;
        if (count_ >= kTaps) {
            count_ = 0;
            return true;
        }
        return false;
    }

    // Taps in the streak so far.
    [[nodiscard]] int count() const { return count_; }
    void reset() { count_ = 0; }

   private:
    int count_ = 0;
    int last_x_ = 0;
    int last_y_ = 0;
    std::uint32_t last_ms_ = 0;
};

// --- numbers as words ----------------------------------------------------------------------------

// 1234567 as "1,234,567".
[[nodiscard]] inline std::string grouped(unsigned long long value) {
    std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

// How long the board has been up: "7 s", "12 min 03 s", "3 h 12 min", "2 d 4 h".
[[nodiscard]] inline std::string duration_text(std::uint64_t ms) {
    const std::uint64_t seconds = ms / 1000;
    char buffer[40];
    if (seconds < 60) {
        std::snprintf(buffer, sizeof buffer, "%llu s", static_cast<unsigned long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(buffer, sizeof buffer, "%llu min %02llu s", static_cast<unsigned long long>(seconds / 60),
                      static_cast<unsigned long long>(seconds % 60));
    } else if (seconds < 86400) {
        std::snprintf(buffer, sizeof buffer, "%llu h %02llu min", static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>(seconds % 3600 / 60));
    } else {
        std::snprintf(buffer, sizeof buffer, "%llu d %llu h", static_cast<unsigned long long>(seconds / 86400),
                      static_cast<unsigned long long>(seconds % 86400 / 3600));
    }
    return buffer;
}

// A byte count in the unit it reads best in: "512 B", "98.4 KiB", "28.4 MiB", and "32 MiB" where the
// figure is whole.
[[nodiscard]] inline std::string bytes_text(std::uint64_t bytes) {
    char buffer[32];
    constexpr std::uint64_t kKiB = 1024;
    constexpr std::uint64_t kMiB = 1024 * 1024;
    if (bytes < kKiB) {
        std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < kMiB) {
        if (bytes % kKiB == 0) {
            std::snprintf(buffer, sizeof buffer, "%llu KiB", static_cast<unsigned long long>(bytes / kKiB));
        } else {
            std::snprintf(buffer, sizeof buffer, "%.1f KiB", static_cast<double>(bytes) / static_cast<double>(kKiB));
        }
    } else if (bytes % kMiB == 0) {
        std::snprintf(buffer, sizeof buffer, "%llu MiB", static_cast<unsigned long long>(bytes / kMiB));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f MiB", static_cast<double>(bytes) / static_cast<double>(kMiB));
    }
    return buffer;
}

// Microseconds as the unit that reads best: "412 us", "6.1 ms", "1.20 s". `with_sign` puts a + before
// a positive figure, for an error that can go either way.
[[nodiscard]] inline std::string micros_text(long long us, bool with_sign = false) {
    char buffer[32];
    const char* const sign = us < 0 ? "-" : (with_sign && us > 0 ? "+" : "");
    const double magnitude = std::fabs(static_cast<double>(us));
    if (magnitude < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%s%.0f us", sign, magnitude);
    } else if (magnitude < 1'000'000.0) {
        std::snprintf(buffer, sizeof buffer, "%s%.1f ms", sign, magnitude / 1000.0);
    } else {
        std::snprintf(buffer, sizeof buffer, "%s%.2f s", sign, magnitude / 1'000'000.0);
    }
    return buffer;
}

// The height of a level meter, 0 to 1, for a level in dB of full scale: -60 dB and below is empty.
[[nodiscard]] inline float level_fraction(float db) {
    if (!(db > -60.0F)) {
        return 0.0F;
    }
    return std::min(1.0F, (db + 60.0F) / 60.0F);
}

// --- the facts -----------------------------------------------------------------------------------

// One network interface, as network.hpp's InterfaceStatus has it.
struct NetInterface {
    // The build drives this interface at all.
    bool present = false;
    // The preference has not switched it off.
    bool enabled = false;
    // Wi-Fi: associated. Ethernet: a cable with a link partner on it.
    bool link = false;
    std::string address;
    std::string mac;
    // Wi-Fi only: a network is stored for the station to join.
    bool configured = false;
    std::string ssid;
    std::optional<int> rssi_dbm;
    // Ethernet only, while there is a link.
    std::optional<int> speed_mbps;
    std::optional<bool> full_duplex;
};

// What a change of the network preference is doing, asked from the panel's controls.
enum class NetChange : std::uint8_t { idle, working, done, refused };

struct Facts {
    // --- the network
    // "ethernet" or "wifi": the interface the board is using, empty while it has none.
    std::string active;
    // "auto", "ethernet" or "wifi"; empty on a board with one network, which has no choice to make.
    std::string preference;
    NetInterface ethernet;
    NetInterface wifi;
    // What the board calls itself, and the mDNS name it answers to.
    std::string name;
    std::string mdns;
    // The address the web page is at, without the scheme or the port.
    std::string web_address;
    unsigned web_port = 80;
    NetChange net_change = NetChange::idle;
    // The word the change was asked for.
    std::string net_change_word;

    // --- the configuration
    std::string profile;
    std::string profile_hash;
    std::string firmware;
    // The application slot the board runs from ("ota_0").
    std::string slot;
    std::string idf;
    std::string chip;
    int revision_major = 0;
    int revision_minor = 0;
    int cores = 0;
    int cpu_mhz = 0;
    int psram_mhz = 0;
    std::uint64_t psram_bytes = 0;
    // The output layout, as the layout text has it ("5.1"), what the sink is and how wide its bus is.
    std::string layout;
    std::string sink;
    int slots = 0;
    int slot_bits = 0;
    // A core dump of the last crash is in flash; its size when it is.
    bool core_dump = false;
    std::uint64_t core_dump_bytes = 0;
    std::uint64_t uptime_ms = 0;
    std::string reset_reason;

    // --- the stream (GET /status, "sendspin")
    // The build has a Sendspin player.
    bool player = false;
    std::string server;
    unsigned connections = 0;
    unsigned paired = 0;
    bool clock_converged = false;
    long long clock_error_us = 0;
    // "bursts", "pcm" or "idle".
    std::string stream;
    unsigned long long bursts = 0;
    unsigned long long underruns = 0;
    unsigned long long late = 0;
    unsigned long long dropped = 0;
    unsigned long long invalid = 0;
    unsigned resyncs = 0;
    long long error_us = 0;
    long long worst_error_us = 0;
    unsigned burst_us = 0;
    unsigned worst_burst_us = 0;
    // Per output, the last 100 ms in dB of full scale.
    std::vector<float> peak_db;

    // --- the play from POST /play (GET /status's figures): zero until one has run
    bool played = false;
    unsigned long long us_per_frame = 0;
    unsigned long long worst_frame_us = 0;
    unsigned long long sink_us_per_frame = 0;
    unsigned long long render_us_per_frame = 0;

    // --- performance
    std::uint64_t internal_free = 0;
    std::uint64_t internal_low = 0;
    std::uint64_t internal_largest = 0;
    std::uint64_t psram_free = 0;
    std::uint64_t psram_low = 0;
    // The panel's own drawing.
    unsigned long refreshes = 0;
    unsigned long render_us_mean = 0;
    unsigned long render_us_max = 0;
    unsigned long late_cycles = 0;
    unsigned long cycle_us_max = 0;
    // The touch controller's reads, the ones that failed, and the touches it reported.
    unsigned long touch_reads = 0;
    unsigned long touch_failures = 0;
    unsigned long touch_points = 0;
    // The Meters view, once it has been opened (planning/p4x-meters-view.md; GET /panel-stats has every figure): the rate it holds, the shape
    // it takes ("grid 12": the representation and the outputs), the means of what an update costs, and what it missed.
    unsigned long meters_opens = 0;
    bool meters_showing = false;
    int meters_rate_hz = 0;
    std::string meters_shape;
    unsigned long meters_analysis_us = 0;
    unsigned long meters_widgets_us = 0;
    unsigned long meters_render_us = 0;
    unsigned long meters_late = 0;
    unsigned long meters_dropped = 0;
    unsigned long meters_tap_us = 0;  // what the audio path pays for a burst of 1,536 frames
};

// --- the rows --------------------------------------------------------------------------------------

enum class Tone : std::uint8_t {
    text,       // the colour of a value
    muted,      // a value that is absent or quiet
    attention,  // a value that is wrong, or worth a look
};

struct Cell {
    std::string key;
    std::string value;
    Tone tone = Tone::text;
};

// A row is one cell across the column, or two side by side.
struct Row {
    std::array<Cell, 2> cells;
    int used = 1;
};

struct Section {
    std::string title;
    std::vector<Row> rows;
};

struct Page {
    Section network;
    Section configuration;
    Section streaming;
    Section performance;
};

namespace detail {

inline Row one(std::string key, std::string value, Tone tone = Tone::text) {
    Row row;
    row.cells[0] = {std::move(key), std::move(value), tone};
    return row;
}

inline Row two(Cell first, Cell second) {
    Row row;
    row.cells[0] = std::move(first);
    row.cells[1] = std::move(second);
    row.used = 2;
    return row;
}

// A counter: attention when it is not zero.
inline Cell counter(std::string key, unsigned long long value) {
    return {std::move(key), grouped(value), value > 0 ? Tone::attention : Tone::text};
}

inline std::string join(std::initializer_list<std::string_view> parts, std::string_view with = " \xC2\xB7 ") {
    std::string out;
    for (const std::string_view part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += with;
        }
        out += part;
    }
    return out;
}

}  // namespace detail

// What an interface says of itself in one line: the link, then the signal or the speed.
[[nodiscard]] inline std::string link_text(const NetInterface& net, bool is_wifi) {
    if (!net.enabled) {
        return "off (the preference leaves it out)";
    }
    if (is_wifi) {
        if (net.link) {
            std::string out = net.ssid.empty() ? std::string("joined") : net.ssid;
            if (net.rssi_dbm) {
                out += " \xC2\xB7 " + std::to_string(*net.rssi_dbm) + " dBm";
            }
            return out;
        }
        return net.configured ? "not joined" : "no network stored";
    }
    if (!net.link) {
        return "no link";
    }
    std::string out = "up";
    if (net.speed_mbps) {
        out += " " + std::to_string(*net.speed_mbps) + " Mb/s";
        if (net.full_duplex) {
            out += *net.full_duplex ? " full duplex" : " half duplex";
        }
    }
    return out;
}

// The address and the MAC of an interface on one line.
[[nodiscard]] inline std::string address_text(const NetInterface& net) {
    std::string address = net.address.empty() ? (net.enabled && net.link ? "no address yet" : "no address") : net.address;
    return detail::join({address, net.mac});
}

// The version the chip reports: "v3.2".
[[nodiscard]] inline std::string revision_text(int major, int minor) {
    return "v" + std::to_string(major) + "." + std::to_string(minor);
}

[[nodiscard]] inline std::string web_url(const Facts& facts) {
    if (facts.web_address.empty()) {
        return "no address yet";
    }
    std::string url = "http://" + facts.web_address;
    if (facts.web_port != 80) {
        url += ":" + std::to_string(facts.web_port);
    }
    return url + "/";
}

// A reset that was a fault, in the words GET /firmware gives for the reason of the boot
// ("poweron", "sw", "panic", "int_wdt", ...). "wdt" is not one: it is the RTC watchdog, which is how a
// P4 with its radio on a companion chip restarts the whole system (system_reset.cpp).
[[nodiscard]] inline bool reset_was_a_fault(std::string_view reason) {
    for (const std::string_view fault : {"panic", "int_wdt", "task_wdt", "brownout", "glitch", "lockup"}) {
        if (reason == fault) {
            return true;
        }
    }
    return false;
}

// The signal below which the Wi-Fi row turns to the attention colour.
inline constexpr int kWeakSignalDbm = -75;

// What the screen shows, as rows of cells.
[[nodiscard]] inline Page describe(const Facts& facts) {
    using detail::join;
    using detail::one;
    using detail::two;
    Page page;

    // --- network
    Section& net = page.network;
    net.title = "NETWORK";
    const auto interface_rows = [&](const char* key, const char* kind, const NetInterface& in, bool is_wifi) {
        if (!in.present) {
            return;
        }
        const bool in_use = facts.active == kind;
        std::string state = link_text(in, is_wifi);
        if (in_use) {
            state = "in use \xC2\xB7 " + state;
        }
        // A radio that hears its access point faintly is worth a look: below -75 dBm streams stutter.
        const bool weak = is_wifi && in.link && in.rssi_dbm && *in.rssi_dbm < kWeakSignalDbm;
        const Tone tone = !in.enabled || !in.link ? Tone::muted : (weak ? Tone::attention : Tone::text);
        net.rows.push_back(one(key, state, tone));
        const Tone address_tone = in.address.empty() ? Tone::muted : Tone::text;
        net.rows.push_back(one("", address_text(in), address_tone));
    };
    interface_rows("Wired", "ethernet", facts.ethernet, false);
    interface_rows("Wi-Fi", "wifi", facts.wifi, true);
    if (!facts.ethernet.present && !facts.wifi.present) {
        net.rows.push_back(one("Network", facts.web_address.empty() ? "none" : facts.web_address,
                               facts.web_address.empty() ? Tone::attention : Tone::text));
    }
    net.rows.push_back(one("Name", join({facts.name, facts.mdns}), Tone::text));
    net.rows.push_back(one("Web page", web_url(facts), facts.web_address.empty() ? Tone::attention : Tone::text));

    // --- configuration
    Section& config = page.configuration;
    config.title = "CONFIGURATION";
    config.rows.push_back(one("Profile", facts.profile.empty() ? std::string("none") : join({facts.profile, facts.profile_hash}),
                              facts.profile.empty() ? Tone::muted : Tone::text));
    config.rows.push_back(one("Firmware", join({facts.firmware, facts.slot})));
    config.rows.push_back(one("Chip", join({facts.chip + (facts.chip.empty() ? "" : " " + revision_text(facts.revision_major, facts.revision_minor)),
                                            facts.idf.empty() ? std::string() : "IDF " + facts.idf})));
    {
        std::string clocks;
        if (facts.cores > 0 && facts.cpu_mhz > 0) {
            clocks = std::to_string(facts.cores) + " \xC3\x97 " + std::to_string(facts.cpu_mhz) + " MHz";
        }
        std::string psram;
        if (facts.psram_bytes > 0) {
            psram = "PSRAM " + bytes_text(facts.psram_bytes) +
                    (facts.psram_mhz > 0 ? " at " + std::to_string(facts.psram_mhz) + " MHz" : std::string());
        }
        config.rows.push_back(one("Clocks", join({clocks, psram})));
    }
    {
        std::string slots;
        if (facts.slots > 0) {
            slots = std::to_string(facts.slots) + " slots";
            if (facts.slot_bits > 0) {
                slots += " \xC3\x97 " + std::to_string(facts.slot_bits) + " bit";
            }
        }
        config.rows.push_back(one("Output", join({facts.layout, slots, facts.sink})));
    }
    config.rows.push_back(one("Run", join({"up " + duration_text(facts.uptime_ms),
                                           facts.reset_reason.empty() ? std::string() : "reset: " + facts.reset_reason}),
                              reset_was_a_fault(facts.reset_reason) ? Tone::attention : Tone::text));
    config.rows.push_back(one("Crash", facts.core_dump ? "core dump stored, " + bytes_text(facts.core_dump_bytes)
                                                       : std::string("no core dump stored"),
                              facts.core_dump ? Tone::attention : Tone::muted));

    // --- streaming
    Section& stream = page.streaming;
    stream.title = "STREAMING";
    if (!facts.player) {
        stream.rows.push_back(one("Player", "no Sendspin player in this build", Tone::muted));
    } else {
        const bool playing = !facts.stream.empty() && facts.stream != "idle";
        std::string server = facts.server.empty() ? std::string("none") : facts.server;
        // A server's name may be long (Music Assistant gives its id with it): it has the row to itself.
        stream.rows.push_back(one("Server", server, facts.server.empty() ? Tone::muted : Tone::text));
        std::string clock = facts.clock_converged ? "converged \xC2\xB7 error " + micros_text(facts.clock_error_us)
                                                  : std::string("not converged");
        stream.rows.push_back(one("Clock", clock, facts.clock_converged ? Tone::text : (playing ? Tone::attention : Tone::muted)));
        stream.rows.push_back(one("Stream", join({facts.stream.empty() ? std::string("idle") : facts.stream,
                                                  facts.peak_db.empty() ? std::string()
                                                                         : std::to_string(facts.peak_db.size()) + " outputs",
                                                  std::to_string(facts.connections) + " conn",
                                                  std::to_string(facts.paired) + " paired"}),
                                  playing ? Tone::text : Tone::muted));
        stream.rows.push_back(two({"Bursts", grouped(facts.bursts), Tone::text}, detail::counter("Underruns", facts.underruns)));
        stream.rows.push_back(two(detail::counter("Late", facts.late), detail::counter("Dropped", facts.dropped)));
        stream.rows.push_back(two(detail::counter("Resyncs", facts.resyncs), detail::counter("Invalid", facts.invalid)));
        // How far the sound plays from where the server's clock puts it, now and at its worst.
        stream.rows.push_back(one("Offset", join({micros_text(facts.error_us, true),
                                                  "worst " + micros_text(facts.worst_error_us, true)})));
        stream.rows.push_back(one("Burst", join({micros_text(facts.burst_us), "worst " + micros_text(facts.worst_burst_us)})));
    }
    // The figures of a play from POST /play (GET /status): dashes until one has run.
    const Tone play_tone = facts.played ? Tone::text : Tone::muted;
    const auto per_frame = [&](unsigned long long us) {
        return facts.played ? micros_text(static_cast<long long>(us)) + "/frame" : std::string("-");
    };
    stream.rows.push_back(one("Play", facts.played ? join({per_frame(facts.us_per_frame),
                                                           "worst " + micros_text(static_cast<long long>(facts.worst_frame_us))})
                                                   : std::string("-"),
                              play_tone));
    stream.rows.push_back(two({"Sink", per_frame(facts.sink_us_per_frame), play_tone},
                              {"Render", per_frame(facts.render_us_per_frame), play_tone}));

    // --- performance
    Section& perf = page.performance;
    perf.title = "PERFORMANCE";
    const bool low_internal = facts.internal_low > 0 && facts.internal_low < 16 * 1024;
    perf.rows.push_back(one("Heap", join({bytes_text(facts.internal_free) + " free", "low " + bytes_text(facts.internal_low),
                                          "block " + bytes_text(facts.internal_largest)}),
                            low_internal ? Tone::attention : Tone::text));
    perf.rows.push_back(one("PSRAM", join({bytes_text(facts.psram_free) + " free", "low " + bytes_text(facts.psram_low)})));
    perf.rows.push_back(one("Panel", join({"render " + micros_text(static_cast<long long>(facts.render_us_mean)) + " (max " +
                                               micros_text(static_cast<long long>(facts.render_us_max)) + ")",
                                           std::to_string(facts.late_cycles) + " late"}),
                            Tone::text));
    perf.rows.push_back(one("Touch", join({grouped(facts.touch_reads) + " reads", std::to_string(facts.touch_failures) + " failed",
                                           grouped(facts.touch_points) + " touches"}),
                            facts.touch_failures > 0 ? Tone::attention : Tone::text));
    if (facts.meters_opens > 0) {
        // Ten a second is the governor's answer to a task that could not keep up: worth a look while the view shows.
        const bool slow = facts.meters_showing && facts.meters_rate_hz < 20;
        perf.rows.push_back(one("Meters", join({std::to_string(facts.meters_rate_hz) + " Hz", facts.meters_shape,
                                                facts.meters_showing ? "showing" : "closed"}),
                                slow ? Tone::attention : (facts.meters_showing ? Tone::text : Tone::muted)));
        const auto us = [](unsigned long v) { return micros_text(static_cast<long long>(v)); };
        perf.rows.push_back(one("Update", join({"analysis " + us(facts.meters_analysis_us), "widgets " + us(facts.meters_widgets_us),
                                                "render " + us(facts.meters_render_us)}),
                                facts.meters_showing ? Tone::text : Tone::muted));
        perf.rows.push_back(one("Missed", join({std::to_string(facts.meters_late) + " late", std::to_string(facts.meters_dropped) + " dropped",
                                                "tap " + us(facts.meters_tap_us) + "/burst"}),
                                facts.meters_dropped > 0 ? Tone::attention : (facts.meters_showing ? Tone::text : Tone::muted)));
    }
    return page;
}

// --- the network preference control -------------------------------------------------------------

enum class Preference : std::uint8_t { automatic, ethernet, wifi };

[[nodiscard]] inline const char* preference_word(Preference preference) {
    switch (preference) {
        case Preference::ethernet:
            return "ethernet";
        case Preference::wifi:
            return "wifi";
        case Preference::automatic:
            break;
    }
    return "auto";
}

[[nodiscard]] inline std::optional<Preference> parse_preference(std::string_view word) {
    if (word == "auto") {
        return Preference::automatic;
    }
    if (word == "ethernet") {
        return Preference::ethernet;
    }
    if (word == "wifi") {
        return Preference::wifi;
    }
    return std::nullopt;
}

// What the confirming step says before the panel asks for a change of preference. `blocked` is a
// change the board is certain to refuse (the interface it names holds no address, so the board
// would be cut off): the step then offers no way to go on.
struct Confirm {
    std::string headline;
    std::string detail;
    Tone tone = Tone::text;
    bool blocked = false;
};

[[nodiscard]] inline Confirm confirm_text(const Facts& facts, Preference target) {
    Confirm confirm;
    const auto where_now = [&](const NetInterface& to) {
        return " The board is " + std::string(facts.active == "ethernet" ? "on wired" : "on Wi-Fi") +
               " now: this page's address changes" + (to.address.empty() ? "." : " to " + to.address + ".");
    };
    switch (target) {
        case Preference::automatic:
            confirm.headline = "Use automatic?";
            confirm.detail = "Wired when it is linked, Wi-Fi when it is not. Both stay on.";
            return confirm;
        // The board keeps a pin only once the interface it names holds an address, and brings it up
        // itself if it is off now (a pin to the other one switches it off): so an interface that is
        // off is not a reason to refuse, and a switched-on one with nothing to answer it is.
        case Preference::ethernet:
            confirm.headline = "Use wired only?";
            confirm.detail = "Wi-Fi is switched off.";
            if (facts.ethernet.enabled && !facts.ethernet.link) {
                confirm.tone = Tone::attention;
                confirm.blocked = true;
                confirm.detail = "Wired has no link, so the board would be cut off: it refuses.";
            } else if (!facts.ethernet.enabled) {
                confirm.detail += " Wired is brought up first, and Wi-Fi stays on until it holds an address.";
            } else if (facts.active != "ethernet") {
                confirm.detail += where_now(facts.ethernet);
            }
            return confirm;
        case Preference::wifi:
            confirm.headline = "Use Wi-Fi only?";
            confirm.detail = "Wired is switched off.";
            if (!facts.wifi.configured) {
                confirm.tone = Tone::attention;
                confirm.blocked = true;
                confirm.detail = "No Wi-Fi network is stored, so the board would be cut off: it refuses.";
            } else if (!facts.wifi.enabled) {
                confirm.detail += " Wi-Fi is joined first, and wired stays on until it holds an address.";
            } else if (facts.wifi.address.empty()) {
                confirm.detail += " Wi-Fi has no address yet: if it cannot join, the board keeps wired.";
            } else if (facts.active != "wifi") {
                confirm.detail += where_now(facts.wifi);
            }
            return confirm;
    }
    return confirm;
}

// What the control says while a change is under way and after it.
[[nodiscard]] inline std::string net_change_text(NetChange state, std::string_view word) {
    switch (state) {
        case NetChange::working:
            return "Switching to " + std::string(word == "wifi" ? "Wi-Fi" : word == "ethernet" ? "wired" : "automatic") + "...";
        case NetChange::done:
            return "Done.";
        case NetChange::refused:
            return "The board refused the change.";
        case NetChange::idle:
            break;
    }
    return {};
}

// --- the console log -------------------------------------------------------------------------------

// The last `max_lines` lines of the console, ready for a label: escape sequences (colours) removed,
// a letter the panel's font may lack shown as ?, a line too long cut with ..., an empty line left
// out. A line that names the Wi-Fi passphrase or the Sendspin pairing token is replaced by a
// sentence that says so: the log ring keeps whatever the console printed, and the screen is read
// by whoever is in the room.
[[nodiscard]] inline std::vector<std::string> log_tail(std::string_view text, std::size_t max_lines,
                                                       std::size_t max_chars) {
    std::vector<std::string> lines;
    std::string line;
    const auto keep = [&] {
        // Only spaces: nothing to show.
        if (line.find_first_not_of(' ') == std::string::npos) {
            line.clear();
            return;
        }
        std::string lowered = line;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.find("passphrase") != std::string::npos || lowered.find("password") != std::string::npos ||
            lowered.find("pairing token") != std::string::npos) {
            line = "(a line that names a secret is not shown)";
        }
        if (max_chars > 3 && line.size() > max_chars) {
            line.resize(max_chars - 3);
            line += "...";
        }
        lines.push_back(std::move(line));
        line.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            keep();
        } else if (c == 0x1B) {
            // ESC [ ... final letter: a colour or a cursor move.
            if (i + 1 < text.size() && text[i + 1] == '[') {
                i += 2;
                while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) {
                    ++i;
                }
            }
        } else if (c == '\t') {
            line += ' ';
        } else if (c >= 0x80) {
            // One ? a letter: a lead byte stands for it, its continuation bytes are dropped.
            if ((c & 0xC0) != 0x80) {
                line += '?';
            }
        } else if (c >= 0x20 && c != 0x7F) {
            line += static_cast<char>(c);
        }
    }
    if (!line.empty()) {
        keep();
    }
    if (lines.size() > max_lines) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(max_lines));
    }
    return lines;
}

}  // namespace iclforge::devview
