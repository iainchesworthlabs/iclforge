// What this board is, as against what this image is.
//
// A Hearth sink is flashed once and then lives on a shelf: which network it
// joins, what it is called on that network, how wide its DAC's slots are and
// whether a second I2S line is wired are all properties of the BOARD, and
// changing any of them by rebuilding and reflashing is the thing B2 exists to
// stop (planning/hearth-reference-player.md).
//
// So they live in NVS, in one namespace, read once at boot and written when
// something changes them. Kconfig still supplies every default, which is what
// keeps CI working: a QEMU run boots with an empty NVS partition, finds
// nothing, and uses the build's own values exactly as it did before any of
// this existed.
//
// Strings are fixed-size and NUL-terminated rather than std::string: this is
// read on the main task before the decoder has allocated anything, and a
// settings read that fragments the heap it is about to hand over would be a
// poor trade for the convenience.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "iclforge/ac3/decoder/dynamic_range.hpp"
#include "iclforge/device_name.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/wall_clock.hpp"

namespace player {

// The longest a name may be, mDNS's own limit on an instance name's label
// (RFC 6763 §4.1.1 is 63 octets; this is the shorter thing a person types). The rule for what a name
// is - well-formed text, nothing invisible, no space at either end - is iclforge/device_name.hpp's.
inline constexpr std::size_t kMaxNameBytes = iclforge::devname::kMaxBytes;
// WPA2's own limits: 32 octets of SSID, 63 of passphrase.
inline constexpr std::size_t kMaxSsidBytes = 32;
inline constexpr std::size_t kMaxPasswordBytes = 63;

// Which of a board's networks it uses when it has more than one (network.hpp): the cable and
// the WiFi station, on a board with both. `automatic` is the policy - the cable when it has a link
// and an address, the station when only that is up, the cable when both are - and the other two
// pin the board to one interface and leave the other switched off. The stored values are
// these numbers, so they are never renumbered.
enum class NetworkPreference : std::uint8_t { automatic = 0, ethernet = 1, wifi = 2 };

// "auto", "ethernet" or "wifi": the words PUT /network-preference takes and GET /status reports.
[[nodiscard]] constexpr const char* network_preference_name(NetworkPreference preference) {
    switch (preference) {
        case NetworkPreference::ethernet:
            return "ethernet";
        case NetworkPreference::wifi:
            return "wifi";
        case NetworkPreference::automatic:
            break;
    }
    return "auto";
}

struct Settings {
    // What the board calls itself: the mDNS instance name, the name a server
    // shows in a list of sinks. Defaults to "hearth-xxxxxx" from the low three
    // bytes of the board's MAC (board_mac()), so two boards out of the same box
    // differ.
    std::array<char, kMaxNameBytes + 1> name{};
    // The network to join. Empty SSID means "none stored" - the board has
    // never been told, and waits for Improv (B2) to tell it.
    std::array<char, kMaxSsidBytes + 1> ssid{};
    std::array<char, kMaxPasswordBytes + 1> password{};
    // The DAC wiring: how wide a slot the bus carries, and whether the second
    // I2S line is connected to anything. Both are what the board was built
    // with until someone says otherwise.
    int slot_bits = 32;
    bool second_line = false;
    // Which network a board with a cable and a station uses. Read by every build and acted on by
    // the one with both (net/dual/network.cpp).
    NetworkPreference network_preference = NetworkPreference::automatic;
    // What a board with a screen shows while a song with no lyrics plays, and the minutes of nothing
    // touched after which that screen dims (0 is never): planning/p4x-now-playing.md. Read by every
    // build and acted on by the one with a panel (panel/lvgl).
    iclforge::nowview::View now_view = iclforge::nowview::kDefaultView;
    int dim_after_minutes = iclforge::nowview::kDefaultDimMinutes;
    // The time (planning/p4x-now-playing.md, stage 3), on every board that has a network. The zone the time is
    // shown in, as a POSIX string (iclforge/wall_clock.hpp); empty when none is set, and the time is then UTC.
    // A board has no zone database, so this is the zone itself.
    std::array<char, iclforge::wallclock::kTimeZoneMaxBytes + 1> time_zone{};
    // The time server a person named, a host name or an IPv4 address; empty when nobody has, and the board then
    // asks the one the router gave over DHCP and, behind it, pool.ntp.org.
    std::array<char, iclforge::wallclock::kNtpServerMaxBytes + 1> ntp_server{};
    // Whether the board keeps its time with a time server at all. On unless someone turned it off.
    bool time_sync = true;
    // The output layout the board starts with ("5.1", "7.1.4", or a short speaker list), set by PUT /layout
    // and kept over a restart; empty when nobody has set one, and the image's own
    // (CONFIG_ICLFORGE_EXAMPLE_LAYOUT) stands. A server that sends a layout of its own (Hearth does, a
    // Music Assistant does not) replaces it for the stream.
    std::array<char, kMaxPasswordBytes + 1> layout{};
    // How much of a Dolby stream's dynamic range the listener wants (iclforge/ac3/decoder/dynamic_range.hpp,
    // planning/hearth-late-night.md): standard is the decode this board always made, late night is
    // the stream's own compression on top of it. Read by every build and acted on by the one that
    // decodes (the Sendspin player), at the start of the next stream. The image's own answer is
    // CONFIG_ICLFORGE_EXAMPLE_DYNAMIC_RANGE, standard unless a build says otherwise.
    iclforge::ac3::DynamicRange dynamic_range = iclforge::ac3::kDefaultDynamicRange;
};

// Read at boot, before anything else uses them. Initialises NVS if it has to
// (network_up() used to be the only thing that did). Never fails in a way a
// caller can do anything about: a partition that cannot be opened leaves every
// field at its Kconfig default and says so on the console.
void settings_load();

// What was loaded, with whatever has been stored since.
[[nodiscard]] const Settings& settings();

// The board's MAC address, which its default name and the address it gives a
// Sendspin server are both made from: the WiFi station's where the chip has a
// radio, and the chip's own base MAC where it has none - the ESP32-P4, whose
// WiFi is an ESP32-C6 across SDIO, so the address is the P4's and not that of
// the radio it borrows. False, with `mac` unspecified, if ESP-IDF cannot read
// one at all.
[[nodiscard]] bool board_mac(std::span<std::uint8_t, 6> mac);

// Store one field and keep it in `settings()`. Each returns false if the value
// is not one this board can hold - a name that iclforge::devname::check_name()
// refuses, a slot width that is not 16 or 32 - or if NVS refused the write, and
// says which on the console. A refused write leaves the setting as it was.
[[nodiscard]] bool settings_set_name(std::string_view name);
[[nodiscard]] bool settings_set_network(std::string_view ssid, std::string_view password);
[[nodiscard]] bool settings_set_slot_bits(int bits);
[[nodiscard]] bool settings_set_second_line(bool wired);
[[nodiscard]] bool settings_set_network_preference(NetworkPreference preference);
[[nodiscard]] bool settings_set_now_view(iclforge::nowview::View view);
[[nodiscard]] bool settings_set_dim_after(int minutes);
// A POSIX time zone, or an empty string to take the zone away. Refused when the string is not one
// (iclforge::wallclock::parse_time_zone); the C library's own reading of it is the caller's to check.
[[nodiscard]] bool settings_set_time_zone(std::string_view zone);
// A time server (iclforge::wallclock::valid_ntp_server), or an empty string to go back to the router's and the
// pool; and whether to ask one at all.
[[nodiscard]] bool settings_set_ntp_server(std::string_view server);
[[nodiscard]] bool settings_set_time_sync(bool on);
// The stored layout text (already checked as a layout by the caller); an empty string takes it away.
[[nodiscard]] bool settings_set_layout(std::string_view layout);

// Standard or late night. Refused only when NVS refuses the write.
[[nodiscard]] bool settings_set_dynamic_range(iclforge::ac3::DynamicRange range);

// The name a board has until someone names it: "hearth-" and the low three bytes of its MAC.
[[nodiscard]] std::string settings_default_name();

// Everything back to the image's own defaults, for a board being handed on or
// a board whose stored network no longer exists. The name goes back to the one
// derived from the MAC, and the network, the wiring and every preference with it,
// in memory as well as in NVS. This is the factory reset's (the Sendspin pairings
// are the player's, sendspin_pairing("forget"), and the WiFi driver's own copy of the
// network is network_forget_driver_state()).
[[nodiscard]] bool settings_forget();

// Reset preferences: the keys iclforge/settings_keys.hpp lists as preferences are erased and their fields
// go back to the image's defaults, at once. The name, the network, the wiring, the layout and the
// technical settings stay, and so do the pairings. False if NVS refused any erase; the preferences that
// could be reset are.
[[nodiscard]] bool settings_reset_preferences();

}  // namespace player
