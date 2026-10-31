#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The keys of a Hearth board's own settings in NVS (firmware/hearth-sink/main/settings.hpp) and which
// of them "Reset preferences" puts back to the image's defaults (planning/p4x-preferences.md, E4).
//
// A board's settings are of two kinds. The ones a person chooses once and leaves - what the screen shows,
// when it dims, the clock - are PREFERENCES, and Reset preferences returns them to how a new board
// behaved. The rest say what this board IS or how it is joined to the world - its name, the Wi-Fi
// network and which of its networks it uses, how its DACs are wired, the speaker layout, the time
// server - and Reset preferences leaves them alone: a board that is reset to its defaults has to stay
// on its network, in its room and paired. (The pairings are not here at all: they are the Sendspin
// store's, in a namespace of its own, and only a factory reset touches them.) FACTORY RESET erases every
// key of the namespace, the preferences and the rest, whatever this table says.
//
// Every key the firmware stores is listed here with its class, so that a test (test_settings_keys.cpp)
// can hold the list to what Reset preferences and the factory reset mean, and so that a setting added
// later cannot arrive without someone saying which kind it is: settings.cpp takes its key names from
// here, and the host test fails on a key it spells that this table does not list. A key's name is an NVS
// key: at most 15 characters.
//
// ADDING A SETTING: add its name below, add a row to kKeys with its class and the words the screen
// uses for it (`what`), and, if it is a preference, a registry entry (preferences.hpp) that says
// `on_reset = kReset`, so Reset preferences puts it back through its own setter and the running board
// follows. Never rename or renumber a key that has been stored: a board in the field has
// it under that name.

namespace iclforge::settingskeys {

// The keys, spelled once. settings.cpp opens NVS with these.
inline constexpr const char* kName = "name";
inline constexpr const char* kSsid = "ssid";
inline constexpr const char* kPassword = "password";
inline constexpr const char* kSlotBits = "slot_bits";
inline constexpr const char* kSecondLine = "second_line";
inline constexpr const char* kNetworkPreference = "net_pref";
inline constexpr const char* kNowView = "np_view";
inline constexpr const char* kDimAfter = "np_dim";
inline constexpr const char* kSpeakersStyle = "np_style";
inline constexpr const char* kTimeZone = "time_zone";
inline constexpr const char* kTimeZoneName = "tz_name";
inline constexpr const char* kBrightness = "bright";
inline constexpr const char* kClockFormat = "clock_fmt";
inline constexpr const char* kNight = "night";
inline constexpr const char* kNtpServer = "ntp_server";
inline constexpr const char* kTimeSync = "time_sync";
inline constexpr const char* kLayout = "layout";
inline constexpr const char* kSpeakerTrim = "speaker_trim";
inline constexpr const char* kDynamicRange = "dyn_range";

// The longest an NVS key may be.
inline constexpr std::size_t kMaxKeyChars = 15;

enum class Kind : std::uint8_t {
    // A choice made once and left: Reset preferences returns it to the default.
    preference,
    // What the board is called: kept by Reset preferences.
    identity,
    // How the board is joined to its network: kept by Reset preferences.
    network,
    // How the board's outputs are wired, what room they serve and where that room is (its time zone): kept by Reset
    // preferences. A person set these up for the room, not for taste.
    setup,
    // A technical setting that stays out of Preferences (planning/p4x-preferences.md): kept by Reset
    // preferences.
    technical,
};

struct Key {
    const char* nvs;
    Kind kind;
    // What a person calls it, for the sentences that say what a reset changes and what it keeps.
    const char* what;
};

inline constexpr std::array kKeys = {
    Key{kName, Kind::identity, "name"},
    Key{kSsid, Kind::network, "Wi-Fi network"},
    Key{kPassword, Kind::network, "Wi-Fi network"},
    Key{kNetworkPreference, Kind::network, "wired or Wi-Fi choice"},
    Key{kSlotBits, Kind::setup, "DAC wiring"},
    Key{kSecondLine, Kind::setup, "DAC wiring"},
    Key{kLayout, Kind::setup, "speaker layout"},
    Key{kSpeakerTrim, Kind::setup, "per-speaker trims and delays"},
    Key{kTimeZone, Kind::setup, "time zone"},
    Key{kNtpServer, Kind::technical, "time server"},
    Key{kTimeSync, Kind::technical, "time sync"},
    Key{kNowView, Kind::preference, "what the screen shows without lyrics"},
    Key{kDimAfter, Kind::preference, "when the screen dims"},
    Key{kSpeakersStyle, Kind::preference, "speakers style"},
    // The name a person chose the zone by: kept by Reset preferences, with the zone it names (the owner, 2026-10-25:
    // Reset preferences keeps the time zone).
    Key{kTimeZoneName, Kind::setup, "time zone"},
    Key{kBrightness, Kind::preference, "brightness"},
    Key{kClockFormat, Kind::preference, "clock format"},
    Key{kNight, Kind::preference, "night schedule"},
    Key{kDynamicRange, Kind::preference, "late-night mode"},
};

[[nodiscard]] constexpr bool is_preference(Kind kind) { return kind == Kind::preference; }

// The key's entry, or null for a key the table does not list.
[[nodiscard]] inline const Key* find(std::string_view nvs) {
    for (const Key& key : kKeys) {
        if (nvs == key.nvs) {
            return &key;
        }
    }
    return nullptr;
}

// What Reset preferences does to a key found in the namespace: erase it (a preference), keep it, or
// keep it and say that nothing classified it.
enum class Reset : std::uint8_t { erase, keep, keep_unlisted };

[[nodiscard]] inline Reset reset_of(std::string_view nvs) {
    const Key* const key = find(nvs);
    if (key == nullptr) {
        return Reset::keep_unlisted;
    }
    return is_preference(key->kind) ? Reset::erase : Reset::keep;
}

namespace detail {

// The distinct `what` words of the keys of the given kinds, in table order.
inline std::vector<std::string> words(bool preferences) {
    std::vector<std::string> out;
    for (const Key& key : kKeys) {
        if (is_preference(key.kind) != preferences) {
            continue;
        }
        const std::string what = key.what;
        bool seen = false;
        for (const std::string& have : out) {
            seen = seen || have == what;
        }
        if (!seen) {
            out.push_back(what);
        }
    }
    return out;
}

inline std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ");
        out += parts[i];
    }
    return out;
}

}  // namespace detail

// A list as a sentence's words: "a", "a and b", "a, b and c".
[[nodiscard]] inline std::string joined(const std::vector<std::string>& parts) { return detail::joined(parts); }

// "what the screen shows without lyrics and when the screen dims": what Reset preferences
// changes, from the table, for the confirming step.
[[nodiscard]] inline std::string reset_words() { return detail::joined(detail::words(true)); }

// "name, Wi-Fi network, ...": what it keeps.
[[nodiscard]] inline std::string kept_words() { return detail::joined(detail::words(false)); }

}  // namespace iclforge::settingskeys
