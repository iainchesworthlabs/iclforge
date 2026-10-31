// The REST control surface, and the web UI it serves. See
// ../include/iclforge/control.hpp and planning/esp32-device-ui.md.

#include "iclforge/control.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_psram.h"

#include "iclforge/base/dynamic_range.hpp"
#include "iclforge/firmware.hpp"
#include "iclforge/firmware_pull.hpp"
#include "iclforge/hardware_info.hpp"
#include "iclforge/layout_options.hpp"
#include "iclforge/log.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/time_zones.hpp"
#include "iclforge/wall_clock.hpp"

// The web UI's two files, gzipped. CMakeLists.txt writes them (tools/gzip_asset.py) and embeds them
// (target_add_binary_data) and they stay in flash. ESP-IDF names each symbol after the file's base name, which is why
// the files carry the component's name: a firmware that embeds an index.html of
// its own would otherwise have two definitions of one symbol.
extern const char iclforge_ui_html_start[] asm("_binary_iclforge_ui_html_gz_start");
extern const char iclforge_ui_html_end[] asm("_binary_iclforge_ui_html_gz_end");
extern const char iclforge_ui_js_start[] asm("_binary_iclforge_ui_js_gz_start");
extern const char iclforge_ui_js_end[] asm("_binary_iclforge_ui_js_gz_end");

namespace iclforge {
namespace {

// The most of the log one GET /log sends: its copy is on the server's heap
// while it goes, and a client following the log asks again for the rest.
constexpr std::size_t kLogReplyBytes = 4096;

// The body of a small POST or PUT, as a string. Empty on a read error or an
// empty body - the two are the same to a handler that needs a location.
std::string read_body(httpd_req_t* req) {
    std::string body;
    if (req->content_len <= 0 || req->content_len > 2048) {
        return body;
    }
    body.resize(static_cast<std::size_t>(req->content_len));
    std::size_t got = 0;
    while (got < body.size()) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) {
            return std::string{};
        }
        got += static_cast<std::size_t>(n);
    }
    // Trim the whitespace a shell puts on the end of `-d`.
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
        body.pop_back();
    }
    return body;
}

// JSON string escaping for the two characters that can break a document; the
// strings here are URLs, paths, layouts and this code's own constants.
void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            continue;
        }
        out += c;
    }
    out += '"';
}

void append_key(std::string& out, const char* key) {
    if (out.back() != '{') {
        out += ',';
    }
    append_json_string(out, key);
    out += ':';
}

void append_number(std::string& out, const char* key, unsigned long long value) {
    append_key(out, key);
    out += std::to_string(value);
}

void append_signed(std::string& out, const char* key, long long value) {
    append_key(out, key);
    out += std::to_string(value);
}

void append_bool(std::string& out, const char* key, bool value) {
    append_key(out, key);
    out += value ? "true" : "false";
}

void append_string(std::string& out, const char* key, std::string_view value) {
    append_key(out, key);
    append_json_string(out, value);
}

// A value, or null.
void append_optional(std::string& out, const char* key, const std::optional<long long>& value) {
    append_key(out, key);
    out += value ? std::to_string(*value) : "null";
}

// Levels in dB to a tenth, as the page shows them.
void append_levels(std::string& out, const char* key, const std::vector<float>& values) {
    append_key(out, key);
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "%s%.1f", i == 0 ? "" : ",", static_cast<double>(values[i]));
        out += buf.data();
    }
    out += ']';
}

// A JSON array of strings: GET /hardware's "capabilities" and "notices".
// Placed here, ABOVE the sendspin object's own appender (which follows
// straight after) rather than below it or between on_status and on_play
// further down: apps/demos/wasm/tests/device-ui/contract.spec.js extracts each of
// those two functions' own keys by searching this file's plain text between
// their names, so anything of this feature's sitting inside either span
// would read as one of THEIR fields. Everything GET /hardware writes stays
// entirely above both spans instead.
void append_strings(std::string& out, const char* key, const std::vector<std::string>& values) {
    append_key(out, key);
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        append_json_string(out, values[i]);
    }
    out += ']';
}

// One of GET /status's network.ethernet and network.wifi (ControlNetwork::Interface). Here, above
// both spans, for the reason append_strings gives: contract.spec.js reads the keys of on_status
// from the text between its name and on_play's, and a nested object's keys written there would
// read as the board's.
void append_interface(std::string& out, const char* key, const ControlNetwork::Interface& i) {
    append_key(out, key);
    out += '{';
    append_bool(out, "enabled", i.enabled);
    append_bool(out, "link", i.link);
    append_string(out, "address", i.address);
    append_string(out, "mac", i.mac);
    if (i.configured) {
        // The station's: whether it has a network to join, and, while it has one, which.
        append_bool(out, "configured", *i.configured);
        append_string(out, "ssid", i.ssid);
        append_optional(out, "rssi_dbm", i.rssi_dbm ? std::optional<long long>(*i.rssi_dbm) : std::nullopt);
    } else {
        // The cable's: what was negotiated, while there is a partner.
        append_optional(out, "speed_mbps",
                        i.speed_mbps ? std::optional<long long>(*i.speed_mbps) : std::nullopt);
        append_key(out, "full_duplex");
        out += i.full_duplex ? (*i.full_duplex ? "true" : "false") : "null";
    }
    out += '}';
}

// esp_chip_info()'s model, named - provision.cpp's own device_info answer
// names four of these; this covers every model ESP-IDF v6.1 knows, since a
// self-report is exactly the place a fifth one should not silently read
// "unknown" for want of a case added there too.
const char* chip_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32C2:
            return "ESP32-C2";
        case CHIP_ESP32C6:
            return "ESP32-C6";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        case CHIP_ESP32P4:
            return "ESP32-P4";
        case CHIP_ESP32C61:
            return "ESP32-C61";
        case CHIP_ESP32C5:
            return "ESP32-C5";
        case CHIP_ESP32H21:
            return "ESP32-H21";
        case CHIP_ESP32H4:
            return "ESP32-H4";
        case CHIP_ESP32S31:
            return "ESP32-S31";
        default:
            return "unknown";
    }
}

// Gathered once, in Control::start: nothing here changes while the board
// runs, unlike everything /status reports. CONFIG_IDF_TARGET and
// CONFIG_SOC_CPU_HAS_FPU are this build's own, always-defined macros;
// esp_chip_info and esp_psram_get_size read the silicon actually under it.
HardwareFacts gather_hardware_facts(int sink_max_slots, int sink_max_slots_bits) {
    HardwareFacts facts;
    facts.target = CONFIG_IDF_TARGET;
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    facts.chip = chip_name(chip.model);
    facts.revision_major = chip.revision / 100;
    facts.revision_minor = chip.revision % 100;
    facts.cores = chip.cores;
#if CONFIG_SOC_CPU_HAS_FPU
    facts.fpu = true;
#endif
    facts.cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    // Guarded on the Kconfig symbol, not just called unconditionally: a
    // target with no PSRAM bus at all (the ESP32-C6 sink) never exposes
    // esp_psram_get_size() to the linker in the first place, unlike a target
    // where CONFIG_SPIRAM is merely off by choice - CONFIG_SPIRAM reads as
    // unset either way, so this one guard covers both without needing to
    // tell them apart. Still reports the real runtime figure, not just
    // whether Kconfig asked for PSRAM, on every target where the option
    // exists at all (this component's own CMakeLists.txt comment).
#if CONFIG_SPIRAM
    facts.psram_bytes = esp_psram_get_size();
#endif
    facts.sink_max_slots = sink_max_slots;
    facts.sink_max_slots_bits = sink_max_slots_bits;
// The ESP32-P4's own pre-production-silicon accommodation
// (docs/platforms/bare-metal/esp32-p4.md, "The chip revision, and what it
// blocks"): a build that lowers the bootloader's revision floor below the
// default v3.1 to admit pre-production silicon can end up running on
// silicon that actually reaches v3.0 - the one direction noticeable post-
// boot, since anything genuinely below whatever floor THIS build set never
// reaches this line at all, refused by the bootloader itself before
// app_main runs. 300 (v3.0), not this build's own lowered floor, because
// v3.0 is where TWO things this build's own Kconfig chose conservatively
// both change: hal/i2s_ll.h's own I2S_LL_DEFAULT_CLK_SRC switches from
// I2S_CLK_SRC_XTAL to I2S_CLK_SRC_PLL_160M, and esp_system's own
// Kconfig.cpu only offers 400 MHz (esp32p4/Kconfig.cpu) once
// ESP32P4_SELECTS_REV_LESS_V3 is unset - both facts worth a chip actually
// there knowing about, which nothing below v3.0 (this board's own v1.3
// included) unlocks regardless of how low this build's floor goes.
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    facts.revision_notice_at = 300;  // v3.0
    facts.revision_floor_cost =
        "This build was compiled to also accept older, pre-production silicon: it runs the "
        "CPU at " + std::to_string(facts.cpu_freq_mhz) + " MHz rather than 400 (esp32p4/"
        "Kconfig.cpu), and falls back to the 40 MHz crystal for I2S rather than the 160 MHz "
        "PLL v3.0+ silicon supports (I2S_CLK_SRC_XTAL/PLL_160M, hal/i2s_ll.h) - a build that "
        "required v3.0 or newer could reach both.";
    // The opposite direction from the notice above, and a hard limit rather
    // than a cost: below v3.0 there is no PLL clock source for I2S at all,
    // and switching to APLL (SOC_I2S_SUPPORTS_APLL) does not rescue it
    // either - its own CLK_LL_APLL_MAX_HZ (125 MHz) ceiling falls short of
    // the ~147 MHz a full-width TDM frame needs on this part. Since a TDM
    // line always opens at its full configured width regardless of the
    // layout actually in use (sink/i2s_wide's own header comment), this
    // isn't "fewer slots than the ceiling above claims" - it's TDM failing
    // to open at ANY channel count above 2 on this exact silicon, proven on
    // a real board (esp32p4-hearth-sink-tdm-clock-ceiling), not a
    // theoretical gap. Unconditional on the detected revision alone, unlike
    // revision_notice_at above: a chip this old can only ever be running a
    // build lenient enough to accept it, so there is no build-flag case
    // where the limit does not apply.
    facts.revision_hard_limit_below = kEsp32p4TdmClockRevision;  // v3.0, which the wide sink reads too
    facts.revision_hard_limit_cost =
        "This chip has no PLL clock source for I2S (hal/i2s_ll.h), and the APLL fallback's own "
        "125 MHz ceiling falls short of what a full-width TDM frame needs: TDM output cannot "
        "open at any channel count above 2 on this board, regardless of layout - only standard "
        "1-2 channel I2S is reachable.";
#endif
    return facts;
}

// One of esp_app_desc_t's fixed-size text fields, up to its terminator: the
// build fills them from the project and checks each fits, but a field that
// exactly fills its array has no terminator to stop at.
template <std::size_t N>
std::string_view app_field(const char (&field)[N]) {
    return {field, static_cast<std::size_t>(std::find(field, field + N, '\0') - field)};
}

std::string build_hardware_json(const ControlHandlers& handlers) {
    const HardwareFacts facts = gather_hardware_facts(handlers.sink_max_slots ? handlers.sink_max_slots() : 0,
                                                      handlers.sink_max_slots_bits ? handlers.sink_max_slots_bits() : 0);
    const HardwareReport report = describe_hardware(facts);
    // The firmware on the board, from the description the build stamps into
    // the image: the project's name, its version (PROJECT_VER, or `git
    // describe` of the tree it was built from) and the ESP-IDF it was built
    // with.
    const esp_app_desc_t* app = esp_app_get_description();
    std::string out = "{";
    append_key(out, "target");
    append_json_string(out, facts.target);
    append_key(out, "chip");
    append_json_string(out, facts.chip);
    append_key(out, "revision");
    append_json_string(out, std::to_string(facts.revision_major) + "." +
                                std::to_string(facts.revision_minor));
    append_number(out, "cores", static_cast<unsigned long long>(facts.cores));
    append_bool(out, "fpu", facts.fpu);
    append_number(out, "cpu_freq_mhz", static_cast<unsigned long long>(facts.cpu_freq_mhz));
    append_number(out, "psram_bytes", static_cast<unsigned long long>(facts.psram_bytes));
    if (facts.sink_max_slots > 0) {
        append_number(out, "sink_max_slots", static_cast<unsigned long long>(facts.sink_max_slots));
        if (facts.sink_max_slots_bits > 0) {
            append_number(out, "sink_max_slots_bits",
                          static_cast<unsigned long long>(facts.sink_max_slots_bits));
        }
    }
    append_string(out, "project", app_field(app->project_name));
    append_string(out, "version", app_field(app->version));
    append_string(out, "idf_version", app_field(app->idf_ver));
    append_string(out, "profile", handlers.profile);
    append_string(out, "profile_hash", handlers.profile_hash);
    append_strings(out, "capabilities", report.capabilities);
    append_strings(out, "notices", report.notices);
    out += "}\n";
    return out;
}

// The "sendspin" object: see ControlSendspin.
void append_sendspin(std::string& out, const ControlSendspin& s) {
    out += '{';
    append_string(out, "server", s.server);
    append_string(out, "server_id", s.server_id);
    append_string(out, "dialect", s.dialect);
    append_string(out, "psk", s.psk);
    append_string(out, "activity", s.activity);
    append_string(out, "role", s.role);
    append_bool(out, "clock_converged", s.clock_converged);
    append_signed(out, "clock_error_us", s.clock_error_us);
    append_number(out, "clock_updates", s.clock_updates);
    append_number(out, "clock_rejected", s.clock_rejected);
    append_number(out, "connections", s.connections);
    append_string(out, "client_id", s.client_id);
    append_number(out, "paired", s.paired);
    append_string(out, "pairing_code", s.pairing_code);
    append_bool(out, "pairing_held", s.pairing_held);
    append_number(out, "pairing_rounds", s.pairing_rounds);
    append_string(out, "pairing_outcome", s.pairing_outcome);
    append_bool(out, "lost_pairing", s.lost_pairing);
    append_string(out, "playing", s.stream);
    append_number(out, "bursts", s.bursts);
    append_number(out, "underruns", s.underruns);
    append_number(out, "late", s.late);
    append_number(out, "dropped", s.dropped);
    append_number(out, "invalid", s.invalid);
    append_number(out, "resyncs", s.resyncs);
    append_signed(out, "error_us", s.error_us);
    append_signed(out, "worst_error_us", s.worst_error_us);
    append_optional(out, "play_frame",
                    s.play_frame ? std::optional<long long>(static_cast<long long>(*s.play_frame)) : std::nullopt);
    append_optional(out, "play_server_us", s.play_server_us);
    append_optional(out, "origin_server_us", s.origin_server_us);
    append_levels(out, "peak_db", s.peak_db);
    append_levels(out, "rms_db", s.rms_db);
    append_key(out, "stream_rms");
    out += '[';
    for (std::size_t i = 0; i < s.stream_rms.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += std::to_string(s.stream_rms[i]);
    }
    out += ']';
    append_number(out, "burst_us", s.burst_us);
    append_number(out, "worst_burst_us", s.worst_burst_us);
    append_number(out, "decode_stack_free", s.decode_stack_free);
    append_number(out, "server_stack_free", s.server_stack_free);
    append_signed(out, "settings_revision", s.settings_revision);
    append_bool(out, "identifying", s.identifying);
    out += '}';
}

esp_err_t send_text(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

// A file of the web UI, sent from where the linker put it: httpd_resp_send
// builds the headers in a small buffer of its own and sends the body from this
// pointer, so no part of the file is copied to the heap. no-cache has a browser
// ask again rather than keep a script from before a firmware update. The
// policy lets the page run script from the device alone and style from its own
// <style> element, and allows the empty data: icon the page declares so that a
// browser does not ask for /favicon.ico. The files are gzip (69 KB as written, 22 KB sent), always: a browser asks for
// gzip, and a client that does not (curl without --compressed) is sent the bytes it can unzip.
esp_err_t send_file(httpd_req_t* req, const char* type, const char* begin, const char* end) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src data:; "
                       "frame-ancestors 'none'");
    return httpd_resp_send(req, begin, static_cast<ssize_t>(end - begin));
}

}  // namespace

HardwareFacts hardware_facts(int sink_max_slots, int sink_max_slots_bits) {
    return gather_hardware_facts(sink_max_slots, sink_max_slots_bits);
}

struct Control::Impl {
    ControlHandlers handlers;
    httpd_handle_t server = nullptr;
    // Built once in Control::start, from the handlers given then: nothing in
    // it changes while the board runs, so on_hardware has nothing to compute
    // and no player to read.
    std::string hardware_json;

    static Impl* self(httpd_req_t* req) { return static_cast<Impl*>(req->user_ctx); }

    // Flash mode (planning/esp32-ota.md): nothing plays and no setting
    // changes until the board restarts. True, having answered, when the
    // request is refused for it.
    static bool refused_in_flash_mode(httpd_req_t* req) {
        const Firmware* firmware = self(req)->handlers.firmware;
        if (firmware == nullptr || !firmware->flash_mode()) {
            return false;
        }
        (void)send_text(req, "409 Conflict",
                        "the board is in flash mode: nothing plays and no setting changes until it "
                        "restarts\n");
        return true;
    }

    // The web UI: a page and its script, which read /status and drive the
    // routes below like any other client.
    static esp_err_t on_page(httpd_req_t* req) {
        return send_file(req, "text/html; charset=utf-8", iclforge_ui_html_start,
                         iclforge_ui_html_end);
    }

    static esp_err_t on_script(httpd_req_t* req) {
        return send_file(req, "text/javascript; charset=utf-8", iclforge_ui_js_start,
                         iclforge_ui_js_end);
    }

    static esp_err_t on_hardware(httpd_req_t* req) {
        httpd_resp_set_type(req, "application/json");
        const std::string& body = self(req)->hardware_json;
        return httpd_resp_send(req, body.c_str(), static_cast<ssize_t>(body.size()));
    }

    static esp_err_t on_api(httpd_req_t* req) {
        return send_text(
            req, "200 OK",
            "iclforge player\n"
            "GET  /              a web page that shows and drives the player\n"
            "GET  /api           this list\n"
            "GET  /status        what is playing, as JSON\n"
            "GET  /hardware      what this board is - chip, revision, FPU, PSRAM, "
            "and this sink's own ceiling - as JSON, once at startup\n"
            "POST /play          body: a URL or path to play\n"
            "POST /stop\n"
            "POST /volume        body: 0.0 to 1.0\n"
            "GET  /layout        the output layout\n"
            "PUT  /layout        body: a name (5.1.4) or a speaker list; next play\n"
            "GET  /layout-options  the layouts to choose from, each supported, with limits or "
            "unavailable on this board and why, as JSON\n"
            "GET  /name          this board's name; PUT one to change it\n"
            "GET  /slot-width    16 or 32; PUT one to change it at the next play\n"
            "GET  /wiring        1 when a second I2S line is wired; PUT 1 or 0\n"
            "PUT  /network       body: an SSID, a newline, a passphrase; next boot\n"
            "GET  /network-preference  auto, ethernet or wifi on a board with wired and WiFi; "
            "PUT one "
            "to change it\n"
            "GET  /now-playing   speakers, title, clock, meters, off or dark on a board with a screen; PUT "
            "one to change it\n"
            "GET  /dim-after     never, or minutes until that screen dims, on a board with a "
            "screen; PUT one\n"
            "GET  /speakers-style  glow or pulse on a board with a screen: how its speakers show a level; "
            "PUT one\n"
            "GET  /brightness    10 to 100: the percent the backlight is at while the screen is lit, "
            "on a board with a screen; PUT one\n"
            "GET  /clock-format  24 or 12 hours, then seconds when they are shown (12 seconds), on a "
            "board with a screen; PUT one\n"
            "GET  /night-schedule  the two times the screen goes dark between, by the board's own time, "
            "and on or off (22:00-07:00 on), on a board with a screen; PUT one, or on, or off\n"
            "GET  /night-idle   on or off: whether the night schedule darkens the idle screen too; "
            "PUT one\n"
            "GET  /dynamic-range standard or late-night: how much of an AC-3, E-AC-3 or AC-4 stream's "
            "dynamic range is kept; PUT one to change it (from the next stream)\n"
            "GET  /preferences   every preference this board has, whether it can use it and why not, "
            "as JSON; change one through its own route\n"
            "GET  /timezone      the POSIX time zone the board's time is shown in, or none (UTC); "
            "PUT one, or a name from /time-zones\n"
            "GET  /time-zones    the names of the time zones PUT /timezone takes, one a line "
            "(Australia/Melbourne)\n"
            "GET  /ntp-server    the time server a person named, or none (the router's, then the "
            "pool); PUT one\n"
            "GET  /time-sync     on or off: whether the board keeps its time with a time server; "
            "PUT one\n"
            "GET  /trim          each speaker's level and distance, as JSON, on a board with a "
            "Sendspin player\n"
            "PUT  /trim          body: statements such as L level=-1.5 distance=2.4, or reset\n"
            "GET  /identify      the test tone: what it is doing and its limits, as JSON\n"
            "POST /identify      body: off, all, a speaker such as Ls or LFE, or a speaker and loop\n"
            "GET  /panel-stats   what the panel and the Meters view cost, as JSON, on a board with a screen\n"
            "GET  /pairing       the servers this board is paired with, as JSON\n"
            "POST /pairing       body: reset, cancel, forget, or forget and a server_id "
            "(Sendspin pairing)\n"
            "GET  /firmware      both app slots, a trial, an upload and the last "
            "update, as JSON\n"
            "PUT  /firmware      body: an app image; flash mode, then a restart into it\n"
            "PUT  /firmware/mode body: flash, or normal (a restart)\n"
            "PUT  /firmware/rollback  the image before this one boots next\n"
            "GET  /firmware/available  an update from GitHub releases: the state of the flow\n"
            "POST /firmware/check  look for a newer release (the result is on "
            "/firmware/available)\n"
            "PUT  /firmware/from-release  body: the version a check offered; install it\n"
            "PUT  /firmware/channel  body: stable or beta\n"
            "GET  /firmware/coredump  the last crash's core dump, as it lies in flash\n"
            "DELETE /firmware/coredump  erase it\n"
            "POST /restart       restart into the image that runs now\n"
            "POST /reset-preferences  the screen's choices back to the defaults; the name, network, "
            "wiring and pairings stay\n"
            "POST /factory-reset body: erase everything; erases the name, network, pairings and every "
            "setting, then restarts\n"
            "GET  /log           recent console output; ?from=N for what came after byte N\n");
    }

    static esp_err_t on_status(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        std::string out = "{";
        append_key(out, "state");
        append_json_string(out, h.state ? h.state() : "unknown");
        if (h.location) {
            append_key(out, "location");
            append_json_string(out, h.location());
        }
        if (h.source_name) {
            append_key(out, "source");
            append_json_string(out, h.source_name());
        }
        if (h.sink_name) {
            append_key(out, "sink");
            append_json_string(out, h.sink_name());
        }
        if (h.sink_slots) {
            append_number(out, "sink_slots", static_cast<unsigned long long>(h.sink_slots()));
        }
        if (h.slot_bits) {
            append_number(out, "slot_bits", static_cast<unsigned long long>(h.slot_bits()));
        }
        if (h.second_line) {
            append_bool(out, "second_line", h.second_line());
        }
        if (h.name) {
            append_key(out, "name");
            append_json_string(out, h.name());
        }
        if (h.layout) {
            append_key(out, "layout");
            append_json_string(out, h.layout());
        }
        if (h.volume) {
            append_key(out, "volume");
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "%.3f", static_cast<double>(h.volume()));
            out += buf.data();
        }
        if (h.stream) {
            append_key(out, "stream");
            if (const auto info = h.stream()) {
                out += '{';
                append_key(out, "codec");
#if CONFIG_ICLFORGE_AC4
                append_json_string(out, info->ac4 ? "AC-4" : (info->eac3 ? "E-AC-3" : "AC-3"));
#else
                append_json_string(out, info->eac3 ? "E-AC-3" : "AC-3");
#endif
                append_number(out, "acmod", static_cast<unsigned long long>(info->acmod));
                append_number(out, "channels", static_cast<unsigned long long>(info->channels));
                append_number(out, "substreams", static_cast<unsigned long long>(info->substreams));
                append_key(out, "dialnorm");
                out += '-';
                out += std::to_string(info->dialnorm);
                append_bool(out, "objects", info->objects);
                append_bool(out, "objects_rendered", info->objects_rendered);
                append_number(out, "slots", static_cast<unsigned long long>(info->slots));
                // How this play serves the layout (planning/esp32-device-ui.md,
                // "The output layout").
                append_key(out, "layout");
                append_json_string(out, info->layout.data());
                append_key(out, "render");
                append_json_string(out, info->render);
                append_key(out, "coded");
                append_json_string(out, info->coded.data());
                append_key(out, "silent");
                append_json_string(out, info->silent.data());
                out += '}';
            } else {
                out += "null";
            }
        }
        if (h.stats) {
            const PlayerStats s = h.stats();
            append_number(out, "frames", s.frames_played);
            append_number(out, "held", s.frames_held);
            append_number(out, "us_per_frame",
                          s.frames_played > 0 ? s.decode_us / s.frames_played : 0);
            append_number(out, "worst_frame_us", s.worst_frame_us);
            append_number(out, "render_us_per_frame",
                          s.frames_played > 0 ? s.render_us / s.frames_played : 0);
            append_number(out, "sink_us_per_frame",
                          s.frames_played > 0 ? s.sink_us / s.frames_played : 0);
            append_number(out, "realtime_permille",
                          s.frames_played > 0 ? (s.decode_us * 1000) / (32000ULL * s.frames_played)
                                              : 0);
            append_number(out, "resync_bytes", s.resync_bytes);
            append_number(out, "fetched_bytes", s.fetched_bytes);
            append_key(out, "ring_low");
            out += s.ring_low_valid ? std::to_string(s.ring_low_water) : "null";
            append_number(out, "passes", s.passes);
            append_number(out, "layout_mismatches", s.layout_mismatches);
            append_bool(out, "finished", s.finished);
            append_bool(out, "failed", s.failed);
            append_key(out, "why");
            append_json_string(out, s.failure);
            append_number(out, "error", static_cast<unsigned long long>(s.error));
        }
        if (h.sendspin) {
            append_key(out, "sendspin");
            if (const std::optional<ControlSendspin> sendspin = h.sendspin()) {
                append_sendspin(out, *sendspin);
            } else {
                out += "null";
            }
        }
        // What the board is joined to: see ControlNetwork.
        if (h.network) {
            append_key(out, "network");
            if (const std::optional<ControlNetwork> network = h.network()) {
                out += '{';
                append_string(out, "kind", network->kind);
                append_string(out, "ssid", network->ssid);
                append_optional(out, "rssi_dbm", network->rssi_dbm ? std::optional<long long>(*network->rssi_dbm)
                                                                   : std::nullopt);
                append_string(out, "address", network->address);
                // A board with a cable and a station says which it prefers and how each stands;
                // a board with one network has both null.
                append_key(out, "preference");
                if (network->preference.empty()) {
                    out += "null";
                } else {
                    append_json_string(out, network->preference);
                }
                append_key(out, "interfaces");
                if (network->ethernet || network->wifi) {
                    out += '{';
                    if (network->ethernet) {
                        append_interface(out, "ethernet", *network->ethernet);
                    }
                    if (network->wifi) {
                        append_interface(out, "wifi", *network->wifi);
                    }
                    out += '}';
                } else {
                    out += "null";
                }
                out += '}';
            } else {
                out += "null";
            }
        }
        // What the board says the time is, on a board that keeps one: see ControlTime.
        if (h.time) {
            append_key(out, "time");
            if (const std::optional<ControlTime> time = h.time()) {
                out += '{';
                append_bool(out, "sync", time->sync);
                append_bool(out, "asking", time->asking);
                append_bool(out, "synced", time->synced);
                append_key(out, "local");
                if (time->local.empty()) {
                    out += "null";
                } else {
                    append_json_string(out, time->local);
                }
                append_string(out, "zone", time->zone);
                append_string(out, "zone_name", time->zone_name);
                append_string(out, "zone_place", time->zone_place);
                append_string(out, "zone_label", time->zone_label);
                append_string(out, "server", time->server);
                append_string(out, "servers", time->servers);
                append_number(out, "answers", time->answers);
                out += '}';
            } else {
                out += "null";
            }
        }
        out += "}\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out.c_str(), static_cast<ssize_t>(out.size()));
    }

    static esp_err_t on_play(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string location = read_body(req);
        if (location.empty()) {
            return send_text(req, "400 Bad Request", "POST /play wants the location as the body\n");
        }
        if (!h.play || !h.play(location)) {
            return send_text(req, "409 Conflict", "this source cannot play that\n");
        }
        // Accepted, not yet playing: the owner opens the source on its own
        // task, and GET /status says how that went.
        return send_text(req, "202 Accepted", "accepted\n");
    }

    static esp_err_t on_stop(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (h.stop) {
            h.stop();
        }
        return send_text(req, "200 OK", "stopped\n");
    }

    static esp_err_t on_volume(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        char* end = nullptr;
        const double value = body.empty() ? -1.0 : std::strtod(body.c_str(), &end);
        if (body.empty() || end == body.c_str() || value < 0.0 || value > 1.0) {
            return send_text(req, "400 Bad Request", "POST /volume wants a number 0.0 to 1.0\n");
        }
        if (!h.set_volume || !h.set_volume(static_cast<float>(value))) {
            return send_text(req, "409 Conflict", "this sink has no volume to set\n");
        }
        return send_text(req, "200 OK", "ok\n");
    }

    static esp_err_t on_layout_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.layout) {
            return send_text(req, "404 Not Found", "this player has no layout to report\n");
        }
        std::string text = h.layout();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_layout_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (body.empty()) {
            return send_text(req, "400 Bad Request",
                             "PUT /layout wants a name (5.1.4) or a speaker list (L,R,C,LFE,Ls,Rs)\n");
        }
        if (!h.set_layout) {
            return send_text(req, "409 Conflict", "this player's layout is fixed\n");
        }
        const ControlLayoutAnswer answer = h.set_layout(body);
        if (!answer.accepted) {
            // The reason the board cannot carry it, from the verdict GET /layout-options and the Preferences list
            // give; the route's own words when the owner has none (text that is not a layout).
            if (answer.message.empty()) {
                return send_text(req, "409 Conflict",
                                 "not a layout this player can play: check the name or the list, and "
                                 "that it has no more slots than the sink\n");
            }
            return send_text(req, "409 Conflict", (answer.message + '\n').c_str());
        }
        if (answer.message.empty()) {
            return send_text(req, "200 OK", "ok; takes effect at the next play\n");
        }
        // Taken, with a limit on it: said, so that a person is not told "ok" about a layout that plays narrower.
        return send_text(req, "200 OK",
                         ("ok; takes effect at the next play. Note: " + answer.message + '\n').c_str());
    }

    // GET /layout-options: every layout the chooser lists, each judged against this board's capabilities
    // (iclforge/layout_options.hpp, the verdict PUT /layout and the Preferences list ask too). Read-only. Sent
    // one option at a time: the whole list is some 3 KB, and the server's task has 6 KB of stack and no heap to
    // spare on a small part.
    static esp_err_t on_layout_options_get(httpd_req_t* req) {
        const ControlHandlers& h = self(req)->handlers;
        if (h.preferences == nullptr || !h.layout) {
            return send_text(req, "404 Not Found", "this player has no layout to choose\n");
        }
        const BoardCaps caps = h.preferences->caps();
        const std::string head = layout_options_head_json(caps, h.layout());
        httpd_resp_set_type(req, "application/json");
        if (httpd_resp_send_chunk(req, head.c_str(), HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
        bool first = true;
        for (const CatalogLayout& entry : kLayoutCatalog) {
            const std::string item = layout_option_json(evaluate_layout(caps, entry.name, entry.slots), first);
            if (httpd_resp_send_chunk(req, item.c_str(), HTTPD_RESP_USE_STRLEN) != ESP_OK) {
                return ESP_FAIL;
            }
            first = false;
        }
        if (httpd_resp_send_chunk(req, "]}\n", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    static esp_err_t on_name_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.name) {
            return send_text(req, "404 Not Found", "this player has no name to report\n");
        }
        std::string text = h.name();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_name_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (body.empty()) {
            return send_text(req, "400 Bad Request", "PUT /name wants a name\n");
        }
        if (!h.set_name) {
            return send_text(req, "409 Conflict", "this player's name is fixed\n");
        }
        if (!h.set_name(body)) {
            return send_text(req, "409 Conflict",
                             "not a name this player can hold: 1 to 32 bytes of text, with no control "
                             "character in it and no space at either end, or it could not be stored\n");
        }
        return send_text(req, "200 OK", "ok\n");
    }

    static esp_err_t on_wiring_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.second_line) {
            return send_text(req, "404 Not Found", "this player reports no wiring\n");
        }
        return send_text(req, "200 OK", h.second_line() ? "1\n" : "0\n");
    }

    static esp_err_t on_wiring_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (body.empty() || (body[0] != '0' && body[0] != '1')) {
            return send_text(req, "400 Bad Request",
                             "PUT /wiring wants 1 (a second I2S line is wired) or 0\n");
        }
        if (!h.set_second_line) {
            return send_text(req, "409 Conflict", "this player's wiring is fixed\n");
        }
        if (!h.set_second_line(body[0] == '1')) {
            return send_text(req, "409 Conflict",
                             "the wiring could not be stored, or a play is running\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at the next play\n");
    }

    static esp_err_t on_network_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        // "ssid\npassword", the passphrase being whatever is left after the
        // first newline: an SSID may contain anything but a newline, and a
        // passphrase may contain anything at all.
        const std::string body = read_body(req);
        const std::size_t split = body.find('\n');
        const std::string_view ssid = std::string_view(body).substr(0, split);
        const std::string_view password =
            split == std::string::npos ? std::string_view{} : std::string_view(body).substr(split + 1);
        if (ssid.empty()) {
            return send_text(req, "400 Bad Request",
                             "PUT /network wants an SSID, a newline, and a passphrase\n");
        }
        if (!h.set_network) {
            return send_text(req, "409 Conflict", "this player's network is fixed\n");
        }
        if (!h.set_network(ssid, password)) {
            return send_text(req, "409 Conflict",
                             "that network could not be stored: the SSID or the passphrase is "
                             "longer than the board holds\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at the next boot\n");
    }

    static esp_err_t on_network_preference_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.network_preference) {
            return send_text(req, "404 Not Found", "this player has one network, so no preference between networks\n");
        }
        std::string text = h.network_preference();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_network_preference_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        // One of three words, as a body or with a line ending after it.
        std::string body = read_body(req);
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
            body.pop_back();
        }
        if (body != "auto" && body != "ethernet" && body != "wifi") {
            return send_text(req, "400 Bad Request", "PUT /network-preference wants auto, ethernet or wifi\n");
        }
        if (!h.set_network_preference) {
            return send_text(req, "409 Conflict", "this player has one network, so no preference between networks\n");
        }
        if (!h.set_network_preference(body)) {
            return send_text(req, "409 Conflict",
                             "that preference could not be set: wifi needs a WiFi network stored, "
                             "and the board may be busy joining one\n");
        }
        return send_text(req, "200 OK",
                         "ok; takes effect in a moment, and is not kept if the interface it names "
                         "does not come up\n");
    }

    static esp_err_t on_now_playing_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.now_playing) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        std::string text = h.now_playing();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_now_playing_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (!nowview::parse_view(body)) {
            return send_text(req, "400 Bad Request", "PUT /now-playing wants speakers, title, clock, meters, off or dark\n");
        }
        if (!h.set_now_playing) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        const std::string_view word = nowview::detail::trimmed(body);
        if (!h.set_now_playing(word)) {
            return send_text(req, "409 Conflict",
                             "that view is not in this build, or the setting could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; the screen follows at once\n");
    }

    static esp_err_t on_dynamic_range_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.dynamic_range) {
            return send_text(req, "404 Not Found", "this board decodes no Dolby stream, so it has no dynamic range to choose\n");
        }
        std::string text = h.dynamic_range();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_dynamic_range_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        const std::optional<base::DynamicRange> range = base::parse_dynamic_range(body);
        if (!range) {
            return send_text(req, "400 Bad Request", "PUT /dynamic-range wants standard or late-night\n");
        }
        if (!h.set_dynamic_range) {
            return send_text(req, "409 Conflict", "this board decodes no Dolby stream, so it has no dynamic range to choose\n");
        }
        if (!h.set_dynamic_range(base::dynamic_range_name(*range))) {
            return send_text(req, "409 Conflict", "that setting could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect from the next stream\n");
    }

    static esp_err_t on_dim_after_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.dim_after) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        std::string text = h.dim_after();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_dim_after_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<int> minutes = nowview::parse_dim_after(read_body(req));
        if (!minutes) {
            return send_text(req, "400 Bad Request",
                             "PUT /dim-after wants never, or a number of minutes from 1 to 240\n");
        }
        if (!h.set_dim_after) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        if (!h.set_dim_after(*minutes)) {
            return send_text(req, "409 Conflict", "that time could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    static esp_err_t on_speakers_style_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.speakers_style) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        std::string text = h.speakers_style();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_speakers_style_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (!nowview::parse_speakers_style(body)) {
            return send_text(req, "400 Bad Request", "PUT /speakers-style wants glow or pulse\n");
        }
        if (!h.set_speakers_style) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        if (!h.set_speakers_style(nowview::detail::trimmed(body))) {
            return send_text(req, "409 Conflict", "that style could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; the screen follows at once\n");
    }

    // A refusal a handler gave, said the way its kind says no.
    static esp_err_t send_refusal(httpd_req_t* req, const ControlRefusal& refusal) {
        std::string text = refusal.why;
        text += '\n';
        switch (refusal.kind) {
            case ControlRefusal::Kind::kBadRequest:
                return send_text(req, "400 Bad Request", text.c_str());
            case ControlRefusal::Kind::kMissing:
                return send_text(req, "404 Not Found", text.c_str());
            case ControlRefusal::Kind::kConflict:
                break;
        }
        return send_text(req, "409 Conflict", text.c_str());
    }

    static esp_err_t send_json_text(httpd_req_t* req, std::string json) {
        json += '\n';
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
    }

    static esp_err_t on_brightness_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.brightness) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        const std::string text = std::to_string(h.brightness()) + '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_brightness_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<int> percent = nowview::parse_brightness(read_body(req));
        if (!percent) {
            return send_text(req, "400 Bad Request", "PUT /brightness wants a number of percent from 10 to 100\n");
        }
        if (!h.set_brightness) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        if (!h.set_brightness(*percent)) {
            return send_text(req, "409 Conflict",
                             "that brightness could not be set: this screen has no backlight to dim, or the "
                             "setting could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    static esp_err_t on_clock_format_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.clock_format) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        const std::string text = wallclock::clock_format_text(h.clock_format()) + '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_clock_format_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<wallclock::ClockFormat> format = wallclock::parse_clock_format(read_body(req));
        if (!format) {
            return send_text(req, "400 Bad Request",
                             "PUT /clock-format wants 24 or 12, and seconds after it when they are shown: 24, "
                             "12 seconds\n");
        }
        if (!h.set_clock_format) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        if (!h.set_clock_format(*format)) {
            return send_text(req, "409 Conflict", "that format could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    static esp_err_t on_night_schedule_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.night) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        const std::string text = nowview::night_schedule_text(h.night()) + '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_night_schedule_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const nowview::NightSchedule current = h.night ? h.night() : nowview::NightSchedule{};
        const std::optional<nowview::NightSchedule> night = nowview::parse_night_schedule(read_body(req), current);
        if (!night) {
            return send_text(req, "400 Bad Request",
                             "PUT /night-schedule wants two times of day and a state, 22:00-07:00 on, or just on or off\n");
        }
        if (!h.set_night) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        if (!h.set_night(*night)) {
            return send_text(req, "409 Conflict", "that schedule could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    static esp_err_t on_night_idle_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.night) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        const std::string text = std::string(nowview::on_off_text(h.night().idle)) + '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_night_idle_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<bool> on = nowview::parse_on_off(read_body(req));
        if (!on) {
            return send_text(req, "400 Bad Request", "PUT /night-idle wants on or off\n");
        }
        if (!h.night || !h.set_night) {
            return send_text(req, "409 Conflict", "this board has no screen\n");
        }
        nowview::NightSchedule night = h.night();
        night.idle = *on;
        if (!h.set_night(night)) {
            return send_text(req, "409 Conflict", "that setting could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    // The names PUT /timezone takes, one a line. Sent in pieces: the 8 KB of them is never one allocation, on a heap
    // that a decoder leaves little of.
    static esp_err_t on_time_zones_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.time_zone) {
            return send_text(req, "404 Not Found", "this board keeps no time\n");
        }
        httpd_resp_set_type(req, "text/plain");
        char piece[512];
        std::size_t used = 0;
        for (std::size_t i = 0; i < wallclock::time_zone_count(); ++i) {
            const std::string_view name = wallclock::time_zone_at(i).name;
            if (used + name.size() + 1 > sizeof piece) {
                if (httpd_resp_send_chunk(req, piece, static_cast<ssize_t>(used)) != ESP_OK) {
                    return ESP_FAIL;
                }
                used = 0;
            }
            std::memcpy(piece + used, name.data(), name.size());
            used += name.size();
            piece[used++] = '\n';
        }
        if (used > 0 && httpd_resp_send_chunk(req, piece, static_cast<ssize_t>(used)) != ESP_OK) {
            return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    static esp_err_t on_preferences_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (h.preferences == nullptr) {
            return send_text(req, "404 Not Found", "this player has no preferences\n");
        }
        const std::string json = prefs::to_json(*h.preferences);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
    }

    static esp_err_t on_trim_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        std::optional<std::string> json = h.trim ? h.trim() : std::nullopt;
        if (!json) {
            return send_text(req, "404 Not Found", "this board has no player to apply a speaker trim to\n");
        }
        return send_json_text(req, std::move(*json));
    }

    static esp_err_t on_panel_stats_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.panel_stats) {
            return send_text(req, "404 Not Found", "this board has no screen\n");
        }
        std::optional<std::string> json = h.panel_stats();
        if (!json) {
            return send_text(req, "404 Not Found", "the screen is not running\n");
        }
        return send_json_text(req, std::move(*json));
    }

    static esp_err_t on_trim_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        if (!h.set_trim) {
            return send_text(req, "404 Not Found", "this board has no player to apply a speaker trim to\n");
        }
        if (const std::optional<ControlRefusal> refusal = h.set_trim(read_body(req))) {
            return send_refusal(req, *refusal);
        }
        return send_text(req, "200 OK", "ok; in force at once\n");
    }

    static esp_err_t on_identify_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        std::optional<std::string> json = h.identify ? h.identify() : std::nullopt;
        if (!json) {
            return send_text(req, "404 Not Found", "this board has no player to play a test tone through\n");
        }
        return send_json_text(req, std::move(*json));
    }

    static esp_err_t on_identify_post(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        if (!h.set_identify) {
            return send_text(req, "404 Not Found", "this board has no player to play a test tone through\n");
        }
        if (const std::optional<ControlRefusal> refusal = h.set_identify(read_body(req))) {
            return send_refusal(req, *refusal);
        }
        return send_text(req, "200 OK", "ok\n");
    }

    static esp_err_t on_time_zone_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.time_zone) {
            return send_text(req, "404 Not Found", "this board keeps no time\n");
        }
        std::string text = h.time_zone();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_time_zone_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        // A name from GET /time-zones, a POSIX zone string, or `none`, read here so that a refusal says what was wrong
        // with it; the owner is handed the one that parsed, and the name a person chose it by.
        const std::optional<wallclock::ZoneChoice> zone = wallclock::parse_zone_choice(read_body(req));
        if (!zone) {
            return send_text(req, "400 Bad Request",
                             "PUT /timezone wants a name from /time-zones (Australia/Melbourne), a POSIX time "
                             "zone with its daylight saving rules, such as AEST-10AEDT,M10.1.0,M4.1.0/3 (the "
                             "offset is hours WEST of UTC), or none\n");
        }
        if (!h.set_time_zone) {
            return send_text(req, "409 Conflict", "this board keeps no time\n");
        }
        if (!h.set_time_zone(zone->posix, zone->name)) {
            return send_text(req, "409 Conflict",
                             "that zone could not be taken: this board's C library reads it differently, or it "
                             "could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; the clock follows at once\n");
    }

    static esp_err_t on_ntp_server_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.ntp_server) {
            return send_text(req, "404 Not Found", "this board keeps no time\n");
        }
        std::string text = h.ntp_server();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_ntp_server_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<std::string> server = wallclock::parse_ntp_server_setting(read_body(req));
        if (!server) {
            return send_text(req, "400 Bad Request",
                             "PUT /ntp-server wants a host name or an IPv4 address, such as pool.ntp.org or "
                             "192.168.1.5, or none\n");
        }
        if (!h.set_ntp_server) {
            return send_text(req, "409 Conflict", "this board keeps no time\n");
        }
        if (!h.set_ntp_server(*server)) {
            return send_text(req, "409 Conflict", "that server could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; the board asks it at once\n");
    }

    static esp_err_t on_time_sync_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.time_sync) {
            return send_text(req, "404 Not Found", "this board keeps no time\n");
        }
        std::string text = h.time_sync();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_time_sync_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::optional<bool> on = wallclock::parse_time_sync(read_body(req));
        if (!on) {
            return send_text(req, "400 Bad Request", "PUT /time-sync wants on or off\n");
        }
        if (!h.set_time_sync) {
            return send_text(req, "409 Conflict", "this board keeps no time\n");
        }
        if (!h.set_time_sync(*on)) {
            return send_text(req, "409 Conflict", "that setting could not be stored\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at once\n");
    }

    // GET /pairing: see ControlPairings. Here, below on_play, for the reason
    // append_strings gives: contract.spec.js reads the keys GET /status
    // writes from the text between on_status and on_play.
    static esp_err_t on_pairing_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        const std::optional<ControlPairings> p = h.pairings ? h.pairings() : std::nullopt;
        if (!p) {
            return send_text(req, "404 Not Found", "this board is not a Sendspin player\n");
        }
        std::string out = "{";
        append_number(out, "capacity", p->capacity);
        append_key(out, "servers");
        out += '[';
        for (std::size_t i = 0; i < p->servers.size(); ++i) {
            const ControlPairing& s = p->servers[i];
            out += i == 0 ? "{" : ",{";
            append_string(out, "server_id", s.server_id);
            append_string(out, "name", s.name);
            append_bool(out, "connected", s.connected);
            append_bool(out, "last_playback", s.last_playback);
            append_bool(out, "seen", s.seen);
            out += '}';
        }
        out += "]}\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out.c_str(), static_cast<ssize_t>(out.size()));
    }

    static esp_err_t on_pairing(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        constexpr std::string_view kForgetOne = "forget ";
        if (body.starts_with(kForgetOne) && body.size() > kForgetOne.size()) {
            const std::optional<bool> forgotten =
                h.forget_server ? h.forget_server(std::string_view(body).substr(kForgetOne.size())) : std::nullopt;
            if (!forgotten) {
                return send_text(req, "409 Conflict", "this board is not a Sendspin player\n");
            }
            if (!*forgotten) {
                return send_text(req, "404 Not Found", "this board has no pairing with that server\n");
            }
            return send_text(req, "200 OK", "ok\n");
        }
        if (body != "reset" && body != "cancel" && body != "forget") {
            return send_text(req, "400 Bad Request",
                             "POST /pairing wants reset, cancel, forget, or forget and a server_id\n");
        }
        if (!h.pairing || !h.pairing(body)) {
            return send_text(req, "409 Conflict", "this board is not a Sendspin player\n");
        }
        return send_text(req, "200 OK", "ok\n");
    }

    // The reset routes. Here, below on_play, for the reason on_pairing_get gives.
    static esp_err_t on_reset_preferences(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        if (!h.reset_preferences) {
            return send_text(req, "409 Conflict", "this board keeps no preferences to reset\n");
        }
        if (const std::optional<std::string> why = h.reset_preferences()) {
            return send_text(req, "409 Conflict", (*why + "\n").c_str());
        }
        return send_text(req, "200 OK",
                         "ok; the preferences are back to the defaults, and the name, the network, the wiring and the "
                         "pairings are kept\n");
    }

    static esp_err_t on_factory_reset(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        // Taken only by the board's own address or name, as the firmware routes are.
        const Firmware* firmware = h.firmware;
        if (firmware != nullptr && !firmware->host_allowed(req)) {
            return send_text(req, "403 Forbidden",
                             "a factory reset is taken only on the board's own address or name\n");
        }
        if (read_body(req) != "erase everything") {
            return send_text(req, "400 Bad Request", "POST /factory-reset wants the body: erase everything\n");
        }
        if (!h.factory_reset) {
            return send_text(req, "409 Conflict", "this board cannot be reset to its factory state\n");
        }
        if (const std::optional<std::string> why = h.factory_reset()) {
            return send_text(req, "409 Conflict", (*why + "\n").c_str());
        }
        return send_text(req, "200 OK",
                         "erased; the board restarts with no name, network or pairings of its own\n");
    }

    static esp_err_t on_slot_width_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.slot_bits) {
            return send_text(req, "404 Not Found", "this player reports no slot width\n");
        }
        std::string bits = std::to_string(h.slot_bits());
        bits += '\n';
        return send_text(req, "200 OK", bits.c_str());
    }

    static esp_err_t on_slot_width_put(httpd_req_t* req) {
        if (refused_in_flash_mode(req)) {
            return ESP_OK;
        }
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (body.empty()) {
            return send_text(req, "400 Bad Request", "PUT /slot-width wants a width in bits\n");
        }
        int bits = 0;
        const auto* first = body.data();
        const auto* last = body.data() + body.size();
        while (first != last && (*first == ' ' || *first == '\n' || *first == '\r')) {
            ++first;
        }
        const auto parsed = std::from_chars(first, last, bits);
        if (parsed.ec != std::errc{}) {
            return send_text(req, "400 Bad Request", "PUT /slot-width wants a number of bits\n");
        }
        if (!h.set_slot_bits) {
            return send_text(req, "409 Conflict", "this player's slot width is fixed\n");
        }
        if (!h.set_slot_bits(bits)) {
            return send_text(req, "409 Conflict",
                             "not a slot width this player's sink has: 16 or 32 on an I2S bus, "
                             "and a sink with no hardware behind it keeps the one it was built "
                             "for\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at the next play\n");
    }

    // The firmware routes, each answered by the owner's Firmware
    // (firmware.hpp): this surface carries them, and has no part of an
    // update in it.
    static esp_err_t no_firmware(httpd_req_t* req) {
        return send_text(req, "404 Not Found", "this board takes no firmware updates\n");
    }

    static esp_err_t on_firmware_get(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_status(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_put(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_upload(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_available(httpd_req_t* req) {
        FirmwarePull* pull = self(req)->handlers.firmware_pull;
        return pull != nullptr ? pull->on_available(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_check(httpd_req_t* req) {
        FirmwarePull* pull = self(req)->handlers.firmware_pull;
        return pull != nullptr ? pull->on_check(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_from_release(httpd_req_t* req) {
        FirmwarePull* pull = self(req)->handlers.firmware_pull;
        return pull != nullptr ? pull->on_from_release(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_channel(httpd_req_t* req) {
        FirmwarePull* pull = self(req)->handlers.firmware_pull;
        return pull != nullptr ? pull->on_channel(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_mode(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_mode(req) : no_firmware(req);
    }

    static esp_err_t on_firmware_rollback(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_rollback(req) : no_firmware(req);
    }

    static esp_err_t on_restart(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_restart(req) : no_firmware(req);
    }

    static esp_err_t on_coredump_get(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_coredump(req) : no_firmware(req);
    }

    static esp_err_t on_coredump_delete(httpd_req_t* req) {
        Firmware* firmware = self(req)->handlers.firmware;
        return firmware != nullptr ? firmware->on_coredump_erase(req) : no_firmware(req);
    }

    // The console's recent output (log.hpp), from byte `from` of all it has
    // written: a client following it asks from X-Log-Next each time, and
    // X-Log-From says where the text starts when the ring had moved on.
    static esp_err_t on_log(httpd_req_t* req) {
        std::uint64_t from = 0;
        std::array<char, 64> query{};
        std::array<char, 24> value{};
        if (httpd_req_get_url_query_str(req, query.data(), query.size()) == ESP_OK &&
            httpd_query_key_value(query.data(), "from", value.data(), value.size()) == ESP_OK) {
            from = std::strtoull(value.data(), nullptr, 10);
        }
        const std::optional<LogRing::Read> read = log_read(from, kLogReplyBytes);
        if (!read) {
            return send_text(req, "404 Not Found", "this board keeps no log\n");
        }
        const std::string start = std::to_string(read->from);
        const std::string next = std::to_string(read->next);
        httpd_resp_set_hdr(req, "X-Log-From", start.c_str());
        httpd_resp_set_hdr(req, "X-Log-Next", next.c_str());
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, read->text.data(), static_cast<ssize_t>(read->text.size()));
    }
};

Control::~Control() { stop(); }

bool Control::start(const ControlHandlers& handlers, std::uint16_t port, std::size_t stack_bytes) {
    if (impl_ != nullptr) {
        return true;
    }
    impl_ = new Impl{};
    impl_->handlers = handlers;
    impl_->hardware_json = build_hardware_json(handlers);

    // Copied into value-initialised httpd_uri_t below: the SDK's struct has
    // WebSocket members when a project turns WebSocket support on, as the
    // Sendspin player does, and none of these routes is one.
    struct Route {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    };
    const Route routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = &Impl::on_page},
        {.uri = "/ui.js", .method = HTTP_GET, .handler = &Impl::on_script},
        {.uri = "/api", .method = HTTP_GET, .handler = &Impl::on_api},
        {.uri = "/status", .method = HTTP_GET, .handler = &Impl::on_status},
        {.uri = "/hardware", .method = HTTP_GET, .handler = &Impl::on_hardware},
        {.uri = "/play", .method = HTTP_POST, .handler = &Impl::on_play},
        {.uri = "/stop", .method = HTTP_POST, .handler = &Impl::on_stop},
        {.uri = "/volume", .method = HTTP_POST, .handler = &Impl::on_volume},
        {.uri = "/layout", .method = HTTP_GET, .handler = &Impl::on_layout_get},
        {.uri = "/layout", .method = HTTP_PUT, .handler = &Impl::on_layout_put},
        {.uri = "/layout-options", .method = HTTP_GET, .handler = &Impl::on_layout_options_get},
        {.uri = "/slot-width", .method = HTTP_GET, .handler = &Impl::on_slot_width_get},
        {.uri = "/slot-width", .method = HTTP_PUT, .handler = &Impl::on_slot_width_put},
        {.uri = "/name", .method = HTTP_GET, .handler = &Impl::on_name_get},
        {.uri = "/name", .method = HTTP_PUT, .handler = &Impl::on_name_put},
        {.uri = "/wiring", .method = HTTP_GET, .handler = &Impl::on_wiring_get},
        {.uri = "/wiring", .method = HTTP_PUT, .handler = &Impl::on_wiring_put},
        {.uri = "/network", .method = HTTP_PUT, .handler = &Impl::on_network_put},
        {.uri = "/network-preference",
         .method = HTTP_GET,
         .handler = &Impl::on_network_preference_get},
        {.uri = "/network-preference",
         .method = HTTP_PUT,
         .handler = &Impl::on_network_preference_put},
        {.uri = "/now-playing", .method = HTTP_GET, .handler = &Impl::on_now_playing_get},
        {.uri = "/now-playing", .method = HTTP_PUT, .handler = &Impl::on_now_playing_put},
        {.uri = "/dim-after", .method = HTTP_GET, .handler = &Impl::on_dim_after_get},
        {.uri = "/dim-after", .method = HTTP_PUT, .handler = &Impl::on_dim_after_put},
        {.uri = "/speakers-style", .method = HTTP_GET, .handler = &Impl::on_speakers_style_get},
        {.uri = "/speakers-style", .method = HTTP_PUT, .handler = &Impl::on_speakers_style_put},
        {.uri = "/brightness", .method = HTTP_GET, .handler = &Impl::on_brightness_get},
        {.uri = "/brightness", .method = HTTP_PUT, .handler = &Impl::on_brightness_put},
        {.uri = "/clock-format", .method = HTTP_GET, .handler = &Impl::on_clock_format_get},
        {.uri = "/clock-format", .method = HTTP_PUT, .handler = &Impl::on_clock_format_put},
        {.uri = "/night-schedule", .method = HTTP_GET, .handler = &Impl::on_night_schedule_get},
        {.uri = "/night-schedule", .method = HTTP_PUT, .handler = &Impl::on_night_schedule_put},
        {.uri = "/night-idle", .method = HTTP_GET, .handler = &Impl::on_night_idle_get},
        {.uri = "/night-idle", .method = HTTP_PUT, .handler = &Impl::on_night_idle_put},
        {.uri = "/time-zones", .method = HTTP_GET, .handler = &Impl::on_time_zones_get},
        {.uri = "/dynamic-range", .method = HTTP_GET, .handler = &Impl::on_dynamic_range_get},
        {.uri = "/dynamic-range", .method = HTTP_PUT, .handler = &Impl::on_dynamic_range_put},
        {.uri = "/preferences", .method = HTTP_GET, .handler = &Impl::on_preferences_get},
        {.uri = "/timezone", .method = HTTP_GET, .handler = &Impl::on_time_zone_get},
        {.uri = "/timezone", .method = HTTP_PUT, .handler = &Impl::on_time_zone_put},
        {.uri = "/ntp-server", .method = HTTP_GET, .handler = &Impl::on_ntp_server_get},
        {.uri = "/ntp-server", .method = HTTP_PUT, .handler = &Impl::on_ntp_server_put},
        {.uri = "/time-sync", .method = HTTP_GET, .handler = &Impl::on_time_sync_get},
        {.uri = "/time-sync", .method = HTTP_PUT, .handler = &Impl::on_time_sync_put},
        {.uri = "/trim", .method = HTTP_GET, .handler = &Impl::on_trim_get},
        {.uri = "/panel-stats", .method = HTTP_GET, .handler = &Impl::on_panel_stats_get},
        {.uri = "/trim", .method = HTTP_PUT, .handler = &Impl::on_trim_put},
        {.uri = "/identify", .method = HTTP_GET, .handler = &Impl::on_identify_get},
        {.uri = "/identify", .method = HTTP_POST, .handler = &Impl::on_identify_post},
        {.uri = "/pairing", .method = HTTP_GET, .handler = &Impl::on_pairing_get},
        {.uri = "/pairing", .method = HTTP_POST, .handler = &Impl::on_pairing},
        {.uri = "/firmware", .method = HTTP_GET, .handler = &Impl::on_firmware_get},
        {.uri = "/firmware", .method = HTTP_PUT, .handler = &Impl::on_firmware_put},
        {.uri = "/firmware/available", .method = HTTP_GET, .handler = &Impl::on_firmware_available},
        {.uri = "/firmware/check", .method = HTTP_POST, .handler = &Impl::on_firmware_check},
        {.uri = "/firmware/from-release",
         .method = HTTP_PUT,
         .handler = &Impl::on_firmware_from_release},
        {.uri = "/firmware/channel", .method = HTTP_PUT, .handler = &Impl::on_firmware_channel},
        {.uri = "/firmware/mode", .method = HTTP_PUT, .handler = &Impl::on_firmware_mode},
        {.uri = "/firmware/rollback", .method = HTTP_PUT, .handler = &Impl::on_firmware_rollback},
        {.uri = "/firmware/coredump", .method = HTTP_GET, .handler = &Impl::on_coredump_get},
        {.uri = "/firmware/coredump", .method = HTTP_DELETE, .handler = &Impl::on_coredump_delete},
        {.uri = "/restart", .method = HTTP_POST, .handler = &Impl::on_restart},
        {.uri = "/reset-preferences", .method = HTTP_POST, .handler = &Impl::on_reset_preferences},
        {.uri = "/factory-reset", .method = HTTP_POST, .handler = &Impl::on_factory_reset},
        {.uri = "/log", .method = HTTP_GET, .handler = &Impl::on_log},
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // The routes above and one or two clients at a time are the whole job;
    // the defaults size the server for more than that and this part has less.
    // Each handler slot is a pointer the server allocates when it starts, so
    // there is one per route and none spare.
    config.max_uri_handlers = static_cast<decltype(config.max_uri_handlers)>(std::size(routes));
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    // The owner's callbacks run on this task too: see kDefaultStackBytes.
    config.stack_size = stack_bytes;
    if (httpd_start(&impl_->server, &config) != ESP_OK) {
        std::printf("control: could not start the HTTP server on port %u\n",
                    static_cast<unsigned>(port));
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    for (const Route& entry : routes) {
        httpd_uri_t route{};
        route.uri = entry.uri;
        route.method = entry.method;
        route.handler = entry.handler;
        route.user_ctx = impl_;
        if (httpd_register_uri_handler(impl_->server, &route) != ESP_OK) {
            std::printf("control: could not register %s\n", entry.uri);
        }
    }
    std::printf("control: http on port %u - a web page at /, the REST routes listed at /api\n",
                static_cast<unsigned>(port));
    return true;
}

void Control::stop() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->server != nullptr) {
        httpd_stop(impl_->server);
    }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace iclforge
