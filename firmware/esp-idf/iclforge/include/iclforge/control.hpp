#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/hardware_info.hpp"
#include "iclforge/player.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/wall_clock.hpp"

// The control surface: a few HTTP verbs over the player, for the ESP-IDF
// integrator whose controller is not Home Assistant. planning/esp32-player.md
// (Control) says why it is REST here and a media_player entity on ESPHome.
//
//   GET  /            a web page that shows what the player is doing and drives
//                     it through the routes below and nothing else, with its
//                     script at GET /ui.js. Both are sent from flash as the
//                     component embeds them (planning/esp32-device-ui.md).
//   GET  /api         the routes, as text
//   GET  /status      what is playing and how it is going, as JSON
//   GET  /hardware    what this board IS - the chip, its revision, whether it
//                     has an FPU and PSRAM, this sink's own ceiling, and the
//                     firmware running on it - as JSON, gathered once at
//                     startup rather than on every request: unlike /status,
//                     nothing in it changes while the board runs.
//                     planning/esp32-device-ui.md says why this is a route of
//                     its own rather than part of /status.
//   POST /play        body: the location to play - a URL for the HTTP source,
//                     a path for a file source. 202 when accepted (the owner
//                     opens it on its own task; /status says how that went),
//                     409 when the source cannot take a location, 400 when the
//                     body is empty.
//   POST /stop        200
//   POST /volume      body: a number 0.0 to 1.0. 200, or 409 if the sink has
//                     no volume to set.
//   GET  /layout      the output layout, as text: a name or a speaker list
//                     (ac3/render/layout.hpp)
//   PUT  /layout      body: a name or a speaker list. Takes effect at the next
//                     play. 200, 400 when it is not a layout, 409 when the
//                     sink's bus has fewer slots than it needs.
//   GET  /pairing     the servers a Sendspin player is paired with, as JSON:
//                     each one's server_id and name, whether it is connected,
//                     and which last played. 404 with no player.
//   POST /pairing     body: reset, cancel or forget, for a board that is a
//                     Sendspin player; or forget, a space and a server_id
//                     from GET /pairing, for that one server alone. 200, 400
//                     for another body, 404 for a server_id the board has no
//                     pairing with, 409 with no player.
//
// And the board's own settings, which ControlHandlers lists: GET and PUT
// /name, /slot-width and /wiring, PUT /network, GET and PUT /network-preference on
// a board with a cable and a station, and GET and PUT /now-playing,
// /dim-after, /brightness and /clock-format on a board with a screen, and GET and PUT /trim and GET and POST /identify on a
// board with a Sendspin player: the listener's own level and distance for each speaker, and a test tone out of each in turn.
//
// And updates over the network, which Control hands to the owner's Firmware
// (firmware.hpp, planning/esp32-ota.md): GET and PUT /firmware, PUT
// /firmware/mode and /firmware/rollback, and POST /restart. In flash mode
// every route that would play something or change a setting answers 409.
//
// Every handler below runs on esp_http_server's task. Nothing here touches a
// Player: the callbacks hand the request to whichever task owns the player -
// a queue, in the streaming example - and answer from what that task
// publishes. The one exception is `stats`, which is a snapshot the Player
// already makes safe from any task.

namespace iclforge {

class Firmware;
class FirmwarePull;

// GET /status's "sendspin" object, for a board that is a Sendspin player
// (sendspin_host.hpp, burst_player.hpp). Plain values, so that the control
// surface carries no part of the player: the owner fills it in.
struct ControlSendspin {
    // The server this board is admitted to, and how.
    std::string server;
    std::string server_id;
    std::string dialect;
    std::string psk;
    std::string activity;
    std::string role;
    bool clock_converged = false;
    long long clock_error_us = 0;
    // Clock bursts taken by the filter, and left out for their delay.
    unsigned long clock_updates = 0;
    unsigned long clock_rejected = 0;
    unsigned connections = 0;
    // This board's client_id, and the servers it is paired with.
    std::string client_id;
    unsigned paired = 0;
    // The dynamic pairing code while an attempt shows one; whether an
    // attempt waits for the operator; rounds since the last code that
    // matched; how the last attempt ended; and whether a server holds a
    // pairing this board has lost.
    std::string pairing_code;
    bool pairing_held = false;
    unsigned pairing_rounds = 0;
    std::string pairing_outcome;
    bool lost_pairing = false;
    // The stream: "bursts", "pcm" or "idle", and its counters.
    std::string stream;
    unsigned long long bursts = 0;
    unsigned long long underruns = 0;
    unsigned long long late = 0;
    unsigned long long dropped = 0;
    unsigned long long invalid = 0;
    unsigned resyncs = 0;
    // When frames played against when they should have, positive when late.
    long long error_us = 0;
    long long worst_error_us = 0;
    // The newest frame played with a known time: its place in the stream, and
    // when it played on the server's clock; with that, when the stream's first
    // frame played by the same measure, which two boards in a group compare.
    std::optional<unsigned long long> play_frame;
    std::optional<long long> play_server_us;
    std::optional<long long> origin_server_us;
    // Per output over the last 100 ms, in dB, and the stream's RMS scaled by a
    // million, as the console prints it.
    std::vector<float> peak_db;
    std::vector<float> rms_db;
    std::vector<unsigned long> stream_rms;
    unsigned burst_us = 0;
    unsigned worst_burst_us = 0;
    unsigned long decode_stack_free = 0;
    unsigned long server_stack_free = 0;
    long long settings_revision = 0;
    bool identifying = false;
};

// GET /pairing: the servers a Sendspin player is paired with, the most
// recently used first (sendspin_store.hpp keeps them in that order). Plain
// values, which the owner fills in.
struct ControlPairing {
    // The server's key, base64url: what POST /pairing's forget takes.
    std::string server_id;
    // What the server's hello called it; empty for a pairing made before the
    // board kept names, until that server connects again.
    std::string name;
    // A connection from it, on this pairing, is open.
    bool connected = false;
    // It was the last server to play here.
    bool last_playback = false;
    // It has connected, or paired, since the board started.
    bool seen = false;
};

struct ControlPairings {
    // How many pairings the board keeps before it forgets the least recently
    // used one.
    unsigned capacity = 0;
    std::vector<ControlPairing> servers;
};

// GET /status's "time" object: what a board that keeps the time says about it, for a page that offers to set the
// zone and the server and for someone asking why a clock shows nothing.
// PUT /layout's answer from the owner: whether the layout was taken, and the sentence that goes with it (see
// ControlHandlers::set_layout). An empty message on a refusal leaves the route's own text.
struct ControlLayoutAnswer {
    bool accepted = false;
    std::string message;
};

struct ControlTime {
    // The board keeps its time with a time server (PUT /time-sync).
    bool sync = false;
    // The SNTP client is started, and a time server has answered since it was.
    bool asking = false;
    bool synced = false;
    // The time to the second in the board's zone ("2026-10-21 08:09:44"); empty until a server has answered.
    std::string local;
    // The zone as set, or "none" (the time is UTC), the name the person chose it by (iclforge/time_zones.hpp,
    // "Australia/Melbourne"), or "none" when they typed a string or set none, and the server a person named, or "none".
    std::string zone;
    std::string zone_name;
    // The place in the table the zone is (the name chosen, or the table's place for a string set with none), or "none"
    // for a zone in no table; and what a person reads for the zone ("Melbourne (Australia)", "UTC",
    // "UTC+10:00 (custom)", never the string: iclforge::wallclock::zone_readable). A page shows these, not `zone`.
    std::string zone_place;
    std::string zone_label;
    std::string server;
    // The servers lwIP is set to ask, as text, and how many answers there have been since boot.
    std::string servers;
    unsigned answers = 0;
};

// GET /status's "network" object: what the board is joined to, for a page
// that offers to move it to another network and for someone asking why its
// stream stutters. Plain values, which the owner fills in.
struct ControlNetwork {
    // The interface the board is using: "wifi", or "ethernet" (a cable, or the
    // MAC QEMU emulates). While none holds an address, the one it is waiting on.
    std::string kind;
    // The access point's SSID; empty on Ethernet and while the station is not
    // associated.
    std::string ssid;
    // The access point's signal as the station last heard it, in dBm, while
    // associated.
    std::optional<int> rssi_dbm;
    // The board's IPv4 address as text; empty while it holds none.
    std::string address;

    // A board with both a cable and a station reports the rest; one with a single
    // network leaves `preference` empty and the interfaces absent, and GET /status
    // has them as null.
    //
    // Which interface the board uses when both could be used: "auto" (the cable,
    // else the station), "ethernet" or "wifi" (that one alone).
    std::string preference;
    // How each interface stands. Never the passphrase.
    struct Interface {
        // Not switched off by the preference.
        bool enabled = false;
        // Wifi: associated. Ethernet: a cable with a partner on it.
        bool link = false;
        // As text; empty while the interface holds none.
        std::string address;
        // As text ("e8:f6:0a:e7:53:fb"), or empty before the interface is up.
        std::string mac;
        // Wifi only: a network is stored or built in. Absent from Ethernet's.
        std::optional<bool> configured;
        // Wifi only, while associated.
        std::string ssid;
        std::optional<int> rssi_dbm;
        // Ethernet only, while there is a link.
        std::optional<int> speed_mbps;
        std::optional<bool> full_duplex;
    };
    std::optional<Interface> wifi;
    std::optional<Interface> ethernet;
};

// Why a handler refused a request that a route can only pass on: the words to say, and which of the three
// ways to say no it is (400 the request is wrong, 409 the board cannot now, 404 there is nothing here).
struct ControlRefusal {
    enum class Kind : std::uint8_t { kBadRequest, kConflict, kMissing };
    Kind kind = Kind::kConflict;
    std::string why;
};

struct ControlHandlers {
    // GET /hardware's "profile" and "profile_hash": which locked build configuration this image
    // was built as, and a hash of what that configuration locks (firmware/hearth-sink/
    // profiles.json). The owner knows; left empty they report empty strings, which is what a
    // build not made as a profile says.
    std::string profile;
    std::string profile_hash;
    // POST /play. False means refused (the source cannot take a location, or
    // the location was not acceptable); the reply says so.
    std::function<bool(std::string_view location)> play;
    // POST /stop.
    std::function<void()> stop;
    // POST /volume. False means this sink has nothing to set.
    std::function<bool(float volume)> set_volume;
    std::function<float()> volume;
    // GET and PUT /layout. set_layout is the owner's decision on a text: not a layout, or one the board cannot carry
    // (iclforge/layout_options.hpp, the same verdict GET /layout-options and the Preferences list give), is refused
    // with the reason in `message`, which the route sends as the body of its 409; an accepted layout may carry a
    // note in `message` (a limit the layout sits under), which the route appends to its 200. The owner applies an
    // accepted layout as it can (a stream that plays takes it at its next burst, a play from POST /play at its next
    // play), and `layout` reports whatever is in force or next.
    std::function<std::string()> layout;
    std::function<ControlLayoutAnswer(std::string_view text)> set_layout;

    // GET and PUT /slot-width, in bits. How wide a slot the sink's bus
    // carries is a property of the DACs a board is wired to, so it is a
    // setting rather than a build choice; set_slot_bits returns false for a
    // width the sink does not have, and the owner applies it at its next
    // play. It moves sink_slots below with it - an I2S line carries 128 bits
    // a frame either way, so the same wiring reaches sixteen slots at 16 bits
    // and eight at 32 - which can leave a layout already set too wide to
    // play; the owner says so at the next play rather than here.
    std::function<int()> slot_bits;
    std::function<bool(int bits)> set_slot_bits;

    // GET and PUT /name: what the board calls itself, on the network and in a
    // server's list of sinks. Stored on the board (its settings), so it
    // survives a reflash.
    std::function<std::string()> name;
    std::function<bool(std::string_view text)> set_name;

    // GET and PUT /wiring: whether the second I2S line is connected to
    // anything. It moves sink_slots with it, the same way the slot width
    // does, because two lines carry twice one line's slots. An owner whose
    // sink can never have a second line leaves both empty, so that /status
    // has no wiring to offer a choice about.
    std::function<bool()> second_line;
    std::function<bool(bool wired)> set_second_line;

    // PUT /network: the access point the board joins, as an SSID and a
    // passphrase. Improv over the serial port is the other way in
    // (the board's own provisioning); this is the one the page offers to
    // someone already on the network who wants to move the board to another.
    // Takes effect at the next boot: the station is already associated.
    std::function<bool(std::string_view ssid, std::string_view password)> set_network;

    // GET and PUT /network-preference: which of its networks a board with a
    // cable and a WiFi station uses, "auto", "ethernet" or "wifi" (ControlNetwork).
    // A board with one network leaves both empty, and the route answers that it
    // has none to choose between. set_network_preference is handed one of those
    // three words; false means the board refused it (the station has no network
    // to join, or the setting could not be stored). It acts at once, with no
    // restart, but not before the reply is sent: the reply may have come over the
    // interface the change switches off.
    std::function<std::string()> network_preference;
    std::function<bool(std::string_view preference)> set_network_preference;

    // GET and PUT /now-playing and /dim-after: what a board with a screen shows
    // while a song with no lyrics plays, and when that screen dims
    // (iclforge/now_view.hpp, planning/p4x-now-playing.md). A board with no
    // screen leaves all four empty, and the routes answer that it has none.
    // set_now_playing is handed one of "speakers", "title", "clock" or "off";
    // false means this build cannot show that view (or the setting could not
    // be stored). set_dim_after is handed minutes, 1 to 240, or 0 for never.
    // Both act at once, with no restart. now_playing and dim_after answer
    // with the words a PUT takes: a view, and "never" or the minutes.
    std::function<std::string()> now_playing;
    std::function<bool(std::string_view view)> set_now_playing;
    std::function<std::string()> dim_after;
    std::function<bool(int minutes)> set_dim_after;
    // GET and PUT /speakers-style: how that screen's speakers show a level, "glow" or "pulse"
    // (iclforge/now_view.hpp, planning/p4x-speakers-options.md). Empty on a board with no screen, as the
    // four above. set_speakers_style is handed one of the two words; false means it could not be stored.
    // It acts at once, with no restart.
    std::function<std::string()> speakers_style;
    std::function<bool(std::string_view style)> set_speakers_style;

    // What a person chose for the screen of a board that has one (planning/p4x-preferences.md): GET and PUT
    // /brightness, the backlight's lit level in percent (iclforge/now_view.hpp: 10 to 100, handed to the setter
    // already read), and GET and PUT /clock-format, 24 or 12 hours with or without seconds (iclforge/wall_clock.hpp).
    // A board with no screen leaves all four empty and the routes answer that it has none. set_brightness is false
    // when the glass has no backlight that can be dimmed or the setting could not be stored. Both act at once.
    std::function<int()> brightness;
    std::function<bool(int percent)> set_brightness;
    std::function<wallclock::ClockFormat()> clock_format;
    std::function<bool(wallclock::ClockFormat format)> set_clock_format;
    // The night schedule (iclforge/now_view.hpp): GET and PUT /night-schedule, the two times by the board's own time
    // and whether it is on (`22:00-07:00 on`; PUT takes the same, or `on`, or `off`), and GET and PUT /night-idle, on or
    // off, whether it also covers the idle screen. set_night is handed the whole schedule, already read over the one
    // `night` returns. A board with no screen leaves both empty and the routes answer that it has none.
    std::function<nowview::NightSchedule()> night;
    std::function<bool(const nowview::NightSchedule& night)> set_night;

    // GET and PUT /dynamic-range: how much of a Dolby stream's dynamic range the listener wants
    // (iclforge/base/dynamic_range.hpp, planning/hearth-late-night.md). dynamic_range answers with
    // the words a PUT takes, "standard" or "late-night"; set_dynamic_range is handed one of them
    // and stores it, and the decoder follows from the next stream's start. A board that decodes
    // none of the Dolby codecs leaves both empty, and the routes answer that it has nothing to
    // choose.
    std::function<std::string()> dynamic_range;
    std::function<bool(std::string_view range)> set_dynamic_range;

    // The board's time of day (iclforge/wall_clock.hpp, planning/p4x-now-playing.md, stage 3), on every board that
    // keeps one: GET and PUT /timezone, /ntp-server and /time-sync, GET /time-zones and GET /status's "time". A board
    // that keeps no time (no network, or QEMU's) leaves them all empty, and the routes answer that it has none. Each
    // setter is handed what the route has already read as the setting (wallclock::parse_zone_choice and the like: an
    // empty string is `none`); false means the board could not take it (its C library reads a zone differently,
    // or it could not be stored). They act at once, with no restart. The getters answer with the words a PUT
    // takes: a zone or a server or `none`, and `on` or `off`. set_time_zone is handed the POSIX string and the
    // name the person chose it by (iclforge/time_zones.hpp), empty for a string they typed or none.
    std::function<std::string()> time_zone;
    std::function<bool(std::string_view zone, std::string_view name)> set_time_zone;
    std::function<std::string()> ntp_server;
    std::function<bool(std::string_view server)> set_ntp_server;
    std::function<std::string()> time_sync;
    std::function<bool(bool on)> set_time_sync;
    // GET /status's "time": see ControlTime.
    std::function<std::optional<ControlTime>()> time;

    // GET and PUT /trim: the listener's level (+-12 dB) and distance (0.1 to 10 m) for each speaker of the layout
    // in force (iclforge/render/speaker_trim.hpp), as JSON, and a body of statements that changes them
    // ("L level=-1.5 distance=2.4; reset"). `trim` is nothing, and the route answers 404, on a board with no
    // player to apply it. set_trim answers nothing when the table was taken, stored and put in force at once,
    // and why not otherwise. A board whose build holds no delay lines takes levels and says distance is
    // unavailable (the JSON's "delay" object).
    std::function<std::optional<std::string>()> trim;
    std::function<std::optional<ControlRefusal>(std::string_view body)> set_trim;
    // GET and POST /identify: what the board's own test tone is doing, as JSON, and a request for it: off, all,
    // a speaker's name, or a name and "loop" (iclforge/render/identify_request.hpp). Refused while a stream
    // plays; stops at once when one begins; ends by its own limits in any case.
    std::function<std::optional<std::string>()> identify;
    std::function<std::optional<ControlRefusal>(std::string_view body)> set_identify;
    // GET /panel-stats: what the screen costs, as JSON (iclforge/panel_stats.hpp): the panel's refresh times, the heap, and the Meters view's rate,
    // timings and misses. Unset on a board with no screen, which answers that it has none; nothing while the panel is not running.
    std::function<std::optional<std::string>()> panel_stats;

    // GET /status draws on these. Any may be left empty; the field is then
    // omitted or reported as null.
    std::function<PlayerStats()> stats;
    std::function<std::optional<StreamInfo>()> stream;
    std::function<std::string()> location;  // what is, or was last, playing
    std::function<const char*()> source_name;
    std::function<const char*()> sink_name;
    // The slots the sink's bus has, and so the widest layout set_layout can
    // accept; reported beside the sink's name.
    std::function<int()> sink_slots;
    // GET /hardware's own slot count: this sink's ceiling at ANY setting it
    // takes, not just the one in force - player::sink_max_slots() beside
    // sink_open() and sink_slots() (main/audio_sink.hpp). Left empty reports
    // nothing, the same rule every optional handler here follows.
    std::function<int()> sink_max_slots;
    // The slot width sink_max_slots's figure was reached at (16 or 32) -
    // player::sink_max_slots_bit_width() beside it. Left empty (the default)
    // reports the slot count alone, with no width qualifier - the right
    // default for a sink whose ceiling is not a function of slot width at
    // all (a capture/null sink has no TDM frame to divide).
    std::function<int()> sink_max_slots_bits;
    // "playing", "stopped", "finished", "failed" - the owner knows.
    std::function<const char*()> state;

    // GET /status's "sendspin" object; null in the reply when this returns
    // nothing, and left out when the handler is empty.
    std::function<std::optional<ControlSendspin>()> sendspin;
    // POST /pairing: body "reset" (the dynamic code's round limit, the
    // operator's action pairing.md asks for), "cancel" (the attempt in
    // progress) or "forget" (every pairing, and a new identity). False for
    // anything else, or with no player.
    std::function<bool(std::string_view action)> pairing;
    // GET /pairing; nothing with no player running, and the route answers 404
    // then, as it does when this is left empty.
    std::function<std::optional<ControlPairings>()> pairings;
    // POST /pairing's "forget " and a server_id: that server's pairing alone.
    // False when the board has none with it; nothing with no player.
    std::function<std::optional<bool>(std::string_view server_id)> forget_server;

    // GET /status's "network" object; null in the reply when this returns
    // nothing (a build with no network), and left out when the handler is
    // empty. Called for every GET /status, on the server's task.
    std::function<std::optional<ControlNetwork>()> network;

    // GET /preferences: every preference the board has (iclforge/preferences.hpp), as this board sees it - its value,
    // whether it can use it and, if not, why. Read-only: a change goes through the entry's own route. The owner
    // keeps the backend alive for as long as Control runs; null leaves the route answering that there are none.
    iclforge::prefs::Backend* preferences = nullptr;

    // POST /reset-preferences: the choices a person made once (what the screen shows, when it dims, the
    // time zone, ...) go back to the image's defaults, at once; the name, the network, the wiring and
    // the pairings stay (iclforge/settings_keys.hpp says which is which). Nothing, when it is done;
    // otherwise why not, in words for the reply.
    std::function<std::optional<std::string>()> reset_preferences;
    // POST /factory-reset, body "erase everything": the pairings, the name, the network and every setting
    // are erased, and the board restarts as a new one. Called only for a request that named this board by
    // its own address or name. Nothing once it has erased and the restart is under way; otherwise why not.
    std::function<std::optional<std::string>()> factory_reset;

    // The firmware routes (firmware.hpp), and flash mode's refusals. Null
    // leaves the routes answering that the board takes no updates. The owner
    // keeps it alive for as long as Control runs.
    Firmware* firmware = nullptr;
    // The routes of an update from GitHub releases (firmware_pull.hpp, planning/esp32-ota.md, O10):
    // GET /firmware/available, POST /firmware/check, PUT /firmware/from-release and PUT
    // /firmware/channel. Null answers that the board takes no firmware updates.
    FirmwarePull* firmware_pull = nullptr;
};

// What this board is - chip, revision, cores, FPU, PSRAM, and the sink ceiling the caller states - gathered from
// the SDK (GET /hardware reports the same; the board's capability description, iclforge/board_caps.hpp, builds on it).
[[nodiscard]] HardwareFacts hardware_facts(int sink_max_slots, int sink_max_slots_bits);

class Control {
   public:
    // The server task's stack, from internal RAM. Every callback above runs
    // on it, so it is sized for the owner's work as much as the server's: in
    // the streaming example PUT /layout parses the layout there, and under
    // QEMU on 2026-09-11 the task's deepest use was 4,596 bytes. At
    // esp_http_server's default of 4,096 that request overflowed the stack
    // without tripping its canary, and the part panicked later in FreeRTOS's
    // list code. An owner whose callbacks do more passes more.
    static constexpr std::size_t kDefaultStackBytes = 6144;

    Control() = default;
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // Starts the server on `port`. False, having said why, if it could not.
    [[nodiscard]] bool start(const ControlHandlers& handlers, std::uint16_t port = 80,
                             std::size_t stack_bytes = kDefaultStackBytes);
    void stop();

   private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace iclforge
