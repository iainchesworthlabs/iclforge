// The board's own settings, in NVS. See settings.hpp for why they are not in
// the image.

#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include "esp_err.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "iclforge/device_name.hpp"
#include "iclforge/settings_keys.hpp"

namespace player {
namespace {

// The keys are libs/device's, spelled once with the class that says what Reset preferences does to each
// (iclforge/settings_keys.hpp): a key written here is a key listed there.
namespace sk = iclforge::settingskeys;

// One namespace, opened per write and closed again: these are written by a
// person pressing something, not in a loop, and a handle held open for the
// life of the run is a handle to leak on any path that fails.
constexpr const char* kNamespace = "hearth_sink";

Settings g_settings;
bool g_loaded = false;

void copy_into(std::span<char> field, std::string_view text) {
    const std::size_t room = field.size() - 1;
    const std::size_t taken = std::min(text.size(), room);
    std::memcpy(field.data(), text.data(), taken);
    field[taken] = '\0';
}

// "hearth-a1b2c3" from the low three bytes of the board's MAC: two boards from
// the same box answer to different names without anyone naming them.
void default_name(std::span<char> field) {
    std::array<std::uint8_t, 6> mac{};
    if (!board_mac(mac)) {
        copy_into(field, "hearth");
        return;
    }
    std::array<char, kMaxNameBytes + 1> text{};
    (void)std::snprintf(text.data(), text.size(), "hearth-%02x%02x%02x", mac[3], mac[4], mac[5]);
    copy_into(field, text.data());
}

// A string from NVS into a fixed field, leaving it alone when the key is
// absent (a board that has never been told) or too long for the field (a
// partition written by a different build).
void read_string(nvs_handle_t handle, const char* key, std::span<char> field) {
    std::size_t length = field.size();
    if (nvs_get_str(handle, key, field.data(), &length) != ESP_OK) {
        return;
    }
    field[field.size() - 1] = '\0';
}

bool write_string(const char* key, std::string_view value, std::span<char> field) {
    if (value.size() > field.size() - 1) {
        std::printf("settings: that value is %u bytes, longer than the %u %s holds\n",
                    static_cast<unsigned>(value.size()),
                    static_cast<unsigned>(field.size() - 1), key);
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to write %s\n", key);
        return false;
    }
    // A fixed-size local rather than the caller's view: the value is not
    // NUL-terminated on its own, and nvs_set_str wants a C string.
    std::array<char, kMaxPasswordBytes + 1> text{};
    const std::size_t taken = std::min(value.size(), text.size() - 1);
    std::memcpy(text.data(), value.data(), taken);
    text[taken] = '\0';
    const esp_err_t err = nvs_set_str(handle, key, text.data());
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
        copy_into(field, std::string_view(text.data(), taken));
    } else {
        std::printf("settings: NVS refused %s (%s)\n", key, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool write_int(const char* key, std::int32_t value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to write %s\n", key);
        return false;
    }
    const esp_err_t err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
    } else {
        std::printf("settings: NVS refused %s (%s)\n", key, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err == ESP_OK;
}

// The image's own answers for every field, so a board with nothing stored behaves
// exactly as it did before there was anywhere to store anything. The
// network is the exception: its Kconfig pair exists only in a build that
// compiled the WiFi seam, so the fallback to it lives there
// (source/http/net/wifi/network.cpp) and an unprovisioned board simply
// has no network here. A whole struct, not the fields that happen to have a
// Kconfig answer: a reload after a factory reset must not keep the network
// the board has just been told to forget.
Settings image_defaults() {
    Settings defaults;
    default_name(defaults.name);
    defaults.slot_bits = CONFIG_ICLFORGE_EXAMPLE_I2S_SLOT_BITS;
    defaults.second_line = CONFIG_ICLFORGE_EXAMPLE_I2S_SECOND_LINE != 0;
    defaults.now_view = iclforge::nowview::kDefaultView;
    defaults.dim_after_minutes = iclforge::nowview::kDefaultDimMinutes;
    defaults.speakers_style = iclforge::nowview::kDefaultSpeakersStyle;
    defaults.brightness_percent = CONFIG_ICLFORGE_EXAMPLE_BRIGHTNESS_PERCENT;
    defaults.clock_format = iclforge::wallclock::clock_format_from_stored(CONFIG_ICLFORGE_EXAMPLE_CLOCK_FORMAT)
                                .value_or(iclforge::wallclock::ClockFormat{});
    defaults.night = iclforge::nowview::parse_night_schedule(CONFIG_ICLFORGE_EXAMPLE_NIGHT_SCHEDULE, {})
                         .value_or(iclforge::nowview::NightSchedule{});
    defaults.night.idle = CONFIG_ICLFORGE_EXAMPLE_NIGHT_IDLE != 0;
    defaults.time_zone = {};
    defaults.time_zone_name = {};
    defaults.ntp_server = {};
    defaults.time_sync = true;
    defaults.layout = {};
    // What the build starts with, when it says (a CI shape, a product's own room): the same statements PUT /trim
    // takes. A string that is wrong is said and leaves every speaker at the default.
    if (const std::string_view built_in = CONFIG_ICLFORGE_EXAMPLE_SPEAKER_TRIM; !built_in.empty()) {
        if (const auto result = defaults.speaker_trim.apply_text(built_in); !result.ok) {
            std::printf("settings: the build's speaker trim is not one this board reads (%s); every speaker at the default\n",
                        result.error);
            defaults.speaker_trim = {};
        }
    }
    defaults.dynamic_range = iclforge::ac3::dynamic_range_from_stored(CONFIG_ICLFORGE_EXAMPLE_DYNAMIC_RANGE)
                                 .value_or(iclforge::ac3::kDefaultDynamicRange);
    return defaults;
}

// The image's answers, then whatever NVS holds over them.
void load_all() {
    g_settings = image_defaults();

    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        // Nothing has ever been written: not a failure, just a new board.
        std::printf("settings: nothing stored; name %s, %d-bit slots, %s line\n",
                    g_settings.name.data(), g_settings.slot_bits,
                    g_settings.second_line ? "a second" : "no second");
        return;
    }
    read_string(handle, sk::kName, g_settings.name);
    read_string(handle, sk::kSsid, g_settings.ssid);
    read_string(handle, sk::kPassword, g_settings.password);
    std::int32_t stored = 0;
    if (nvs_get_i32(handle, sk::kSlotBits, &stored) == ESP_OK) {
        // Checked here as well as where it is written, and said out loud when
        // it fails: a width that is neither 16 nor 32 reaches line_ceiling(),
        // which answers "no such width", and plan_sink then refuses to open
        // the sink at all. On a board that boots unattended that would be
        // silence with the reason on a console nobody is reading, so a bad
        // stored value costs a line and the build's own width stands.
        if (stored == 16 || stored == 32) {
            g_settings.slot_bits = static_cast<int>(stored);
        } else {
            std::printf("settings: stored slot width %ld is not 16 or 32; keeping %d\n",
                        static_cast<long>(stored), g_settings.slot_bits);
        }
    }
    if (nvs_get_i32(handle, sk::kSecondLine, &stored) == ESP_OK) {
        g_settings.second_line = stored != 0;
    }
    if (nvs_get_i32(handle, sk::kNetworkPreference, &stored) == ESP_OK) {
        // Said out loud like the slot width: a stored number this build does not know is
        // a partition written by another build, and the policy stands.
        if (stored >= 0 && stored <= static_cast<std::int32_t>(NetworkPreference::wifi)) {
            g_settings.network_preference = static_cast<NetworkPreference>(stored);
        } else {
            std::printf("settings: stored network preference %ld is not one this build knows; "
                        "keeping auto\n",
                        static_cast<long>(stored));
        }
    }
    if (nvs_get_i32(handle, sk::kNowView, &stored) == ESP_OK) {
        if (const auto view = iclforge::nowview::view_from_stored(stored)) {
            g_settings.now_view = *view;
        } else {
            std::printf("settings: stored screen view %ld is not one this build knows; keeping %s\n",
                        static_cast<long>(stored), iclforge::nowview::view_name(g_settings.now_view));
        }
    }
    if (nvs_get_i32(handle, sk::kDimAfter, &stored) == ESP_OK) {
        if (stored >= iclforge::nowview::kDimNever && stored <= iclforge::nowview::kDimMaxMinutes) {
            g_settings.dim_after_minutes = static_cast<int>(stored);
        } else {
            std::printf("settings: stored dim time %ld is not 0 to %d minutes; keeping %d\n",
                        static_cast<long>(stored), iclforge::nowview::kDimMaxMinutes,
                        g_settings.dim_after_minutes);
        }
    }
    if (nvs_get_i32(handle, sk::kSpeakersStyle, &stored) == ESP_OK) {
        if (const auto style = iclforge::nowview::speakers_style_from_stored(stored)) {
            g_settings.speakers_style = *style;
        } else {
            std::printf("settings: stored speakers style %ld is not one this build knows; keeping %s\n",
                        static_cast<long>(stored), iclforge::nowview::speakers_style_name(g_settings.speakers_style));
        }
    }
    if (nvs_get_i32(handle, sk::kBrightness, &stored) == ESP_OK) {
        if (const auto percent = iclforge::nowview::brightness_from_stored(stored)) {
            g_settings.brightness_percent = *percent;
        } else {
            std::printf("settings: stored brightness %ld is not %d to %d percent; keeping %d\n",
                        static_cast<long>(stored), iclforge::nowview::kBrightnessMin, iclforge::nowview::kBrightnessMax,
                        g_settings.brightness_percent);
        }
    }
    if (nvs_get_i32(handle, sk::kClockFormat, &stored) == ESP_OK) {
        if (const auto format = iclforge::wallclock::clock_format_from_stored(stored)) {
            g_settings.clock_format = *format;
        } else {
            std::printf("settings: stored clock format %ld is not one this build knows; keeping %s\n",
                        static_cast<long>(stored),
                        iclforge::wallclock::clock_format_text(g_settings.clock_format).c_str());
        }
    }
    if (nvs_get_i32(handle, sk::kNight, &stored) == ESP_OK) {
        if (const auto night = iclforge::nowview::night_from_stored(stored)) {
            g_settings.night = *night;
        } else {
            std::printf("settings: stored night schedule %ld is not one this build knows; keeping %s\n",
                        static_cast<long>(stored), iclforge::nowview::night_schedule_text(g_settings.night).c_str());
        }
    }
    // A stored zone that is not one this build reads (a partition written by a later image) is said, and
    // the clock says UTC.
    read_string(handle, sk::kTimeZone, g_settings.time_zone);
    if (g_settings.time_zone[0] != '\0' && !iclforge::wallclock::valid_time_zone(g_settings.time_zone.data())) {
        std::printf("settings: stored time zone '%s' is not one this build reads; the clock says UTC\n",
                    g_settings.time_zone.data());
        g_settings.time_zone[0] = '\0';
    }
    // The name the person chose it by: a name this build's table knows says the zone (so a release that changes a
    // city's rules moves the boards that chose that city), and one it does not know is dropped.
    read_string(handle, sk::kTimeZoneName, g_settings.time_zone_name);
    {
        const iclforge::wallclock::ZoneChoice zone =
            iclforge::wallclock::reconcile_zone(g_settings.time_zone.data(), g_settings.time_zone_name.data());
        if (!zone.name.empty() && zone.posix != g_settings.time_zone.data()) {
            std::printf("settings: time zone %s is now %s (this build's rule for it; stored: '%s')\n",
                        zone.name.c_str(), zone.posix.c_str(), g_settings.time_zone.data());
        }
        copy_into(g_settings.time_zone, zone.posix);
        copy_into(g_settings.time_zone_name, zone.name);
    }
    read_string(handle, sk::kNtpServer, g_settings.ntp_server);
    if (g_settings.ntp_server[0] != '\0' && !iclforge::wallclock::valid_ntp_server(g_settings.ntp_server.data())) {
        std::printf("settings: stored time server '%s' is not one this build reads; the router's and the pool\n",
                    g_settings.ntp_server.data());
        g_settings.ntp_server[0] = '\0';
    }
    if (nvs_get_i32(handle, sk::kTimeSync, &stored) == ESP_OK) {
        g_settings.time_sync = stored != 0;
    }
    read_string(handle, sk::kLayout, g_settings.layout);
    // A stored trim that is not one this build reads (a partition written by a later image, or damaged) is
    // said, and the build's own stands: a speaker at the wrong level is worse than one at none.
    {
        std::array<std::uint8_t, iclforge::render::speaker_trim::SpeakerTrimTable::kBlobBytes> blob{};
        std::size_t blob_length = blob.size();
        if (nvs_get_blob(handle, sk::kSpeakerTrim, blob.data(), &blob_length) == ESP_OK &&
            !g_settings.speaker_trim.from_blob(std::span<const std::uint8_t>(blob.data(), blob_length))) {
            std::printf("settings: the stored speaker trim is not one this build reads; keeping the build's own\n");
        }
    }
    if (nvs_get_i32(handle, sk::kDynamicRange, &stored) == ESP_OK) {
        if (const auto range = iclforge::ac3::dynamic_range_from_stored(stored)) {
            g_settings.dynamic_range = *range;
        } else {
            std::printf("settings: stored dynamic range %ld is not one this build knows; keeping %s\n",
                        static_cast<long>(stored), iclforge::ac3::dynamic_range_name(g_settings.dynamic_range).data());
        }
    }
    nvs_close(handle);

    // The password is never printed, here or anywhere: a console capture goes
    // into CI artifacts and into this repository's issues.
    std::printf("settings: name %s, network '%s'%s, %d-bit slots, %s line, network preference %s, "
                "no-lyrics screen %s (%s), dims after %s, brightness %d, clock %s, night %s%s, time zone %s, "
                "time server %s, time sync %s, dynamic range %s\n",
                g_settings.name.data(), g_settings.ssid.data(),
                g_settings.password[0] != '\0' ? " (password stored)" : "", g_settings.slot_bits,
                g_settings.second_line ? "a second" : "no second",
                network_preference_name(g_settings.network_preference),
                iclforge::nowview::view_name(g_settings.now_view),
                iclforge::nowview::speakers_style_name(g_settings.speakers_style),
                iclforge::nowview::dim_after_text(g_settings.dim_after_minutes).c_str(),
                g_settings.brightness_percent, iclforge::wallclock::clock_format_text(g_settings.clock_format).c_str(),
                iclforge::nowview::night_schedule_text(g_settings.night).c_str(),
                g_settings.night.idle ? " (idle screen too)" : "",
                // As a person reads it (the place, or the offset), not the string the board keeps it by.
                iclforge::wallclock::zone_readable(g_settings.time_zone.data(), g_settings.time_zone_name.data()).c_str(),
                iclforge::wallclock::ntp_server_setting_text(g_settings.ntp_server.data()).c_str(),
                iclforge::wallclock::time_sync_text(g_settings.time_sync),
                iclforge::ac3::dynamic_range_name(g_settings.dynamic_range).data());
}

}  // namespace

void settings_load() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    // WiFi's own calibration data lives in NVS too, and network_up() used to
    // be what initialised it. Doing it here means the settings are readable on
    // a board whose network never comes up at all.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_all();
}

const Settings& settings() { return g_settings; }

bool board_mac(std::span<std::uint8_t, 6> mac) {
    // Ask before reading, not read and fall back: esp_read_mac() logs an error
    // for a MAC type the target has none of, and a P4 would print one on every
    // boot for a MAC it was never going to have. ESP-IDF's MAC table
    // (components/esp_hw_support/mac_addr.c) lists the station MAC only where
    // SOC_WIFI_SUPPORTED is set, and the P4's WiFi is a second chip's;
    // esp_mac_addr_len_get() gives 0 for a type the table lacks, and says
    // nothing. A station MAC is the base MAC unchanged (generate_mac), so the
    // base MAC is what a chip without a radio has in its place.
    const esp_mac_type_t type =
        esp_mac_addr_len_get(ESP_MAC_WIFI_STA) != 0 ? ESP_MAC_WIFI_STA : ESP_MAC_BASE;
    return esp_read_mac(mac.data(), type) == ESP_OK;
}

bool settings_set_name(std::string_view name) {
    // The check every route and the on-screen keyboard hold a name to (iclforge/device_name.hpp): 1 to 32
    // bytes of well-formed text with nothing invisible in it and no space at either end.
    const iclforge::devname::Check check = iclforge::devname::check_name(name);
    if (!check.ok()) {
        std::printf("settings: not a name: %s\n", iclforge::devname::problem_text(check.problem));
        return false;
    }
    return write_string(sk::kName, name, g_settings.name);
}

std::string settings_default_name() {
    std::array<char, kMaxNameBytes + 1> name{};
    default_name(name);
    return name.data();
}

bool settings_set_network(std::string_view ssid, std::string_view password) {
    if (ssid.empty()) {
        std::printf("settings: an SSID cannot be empty\n");
        return false;
    }
    if (!write_string(sk::kSsid, ssid, g_settings.ssid)) {
        return false;
    }
    return write_string(sk::kPassword, password, g_settings.password);
}

bool settings_set_slot_bits(int bits) {
    if (bits != 16 && bits != 32) {
        std::printf("settings: %d-bit slots is not a width (16 or 32)\n", bits);
        return false;
    }
    if (!write_int(sk::kSlotBits, bits)) {
        return false;
    }
    g_settings.slot_bits = bits;
    return true;
}

bool settings_set_second_line(bool wired) {
    if (!write_int(sk::kSecondLine, wired ? 1 : 0)) {
        return false;
    }
    g_settings.second_line = wired;
    return true;
}

bool settings_set_network_preference(NetworkPreference preference) {
    if (!write_int(sk::kNetworkPreference, static_cast<std::int32_t>(preference))) {
        return false;
    }
    g_settings.network_preference = preference;
    return true;
}

bool settings_set_now_view(iclforge::nowview::View view) {
    if (!write_int(sk::kNowView, static_cast<std::int32_t>(view))) {
        return false;
    }
    g_settings.now_view = view;
    return true;
}

bool settings_set_speakers_style(iclforge::nowview::SpeakersStyle style) {
    if (!write_int(sk::kSpeakersStyle, static_cast<std::int32_t>(style))) {
        return false;
    }
    g_settings.speakers_style = style;
    return true;
}

bool settings_set_dim_after(int minutes) {
    if (minutes < iclforge::nowview::kDimNever || minutes > iclforge::nowview::kDimMaxMinutes) {
        std::printf("settings: %d minutes is not a time to dim after\n", minutes);
        return false;
    }
    if (!write_int(sk::kDimAfter, minutes)) {
        return false;
    }
    g_settings.dim_after_minutes = minutes;
    return true;
}

bool settings_set_brightness(int percent) {
    if (!iclforge::nowview::brightness_from_stored(percent)) {
        std::printf("settings: %d percent is not a brightness (%d to %d)\n", percent,
                    iclforge::nowview::kBrightnessMin, iclforge::nowview::kBrightnessMax);
        return false;
    }
    if (!write_int(sk::kBrightness, percent)) {
        return false;
    }
    g_settings.brightness_percent = percent;
    return true;
}

bool settings_set_clock_format(iclforge::wallclock::ClockFormat format) {
    if (!write_int(sk::kClockFormat, iclforge::wallclock::clock_format_stored(format))) {
        return false;
    }
    g_settings.clock_format = format;
    return true;
}

bool settings_set_night(const iclforge::nowview::NightSchedule& night) {
    if (!iclforge::nowview::night_from_stored(iclforge::nowview::night_stored(night))) {
        std::printf("settings: %d and %d are not times of day\n", night.from_min, night.to_min);
        return false;
    }
    if (!write_int(sk::kNight, iclforge::nowview::night_stored(night))) {
        return false;
    }
    g_settings.night = night;
    return true;
}

bool settings_set_layout(std::string_view layout) {
    return write_string(sk::kLayout, layout, g_settings.layout);
}

bool settings_set_speaker_trim(const iclforge::render::speaker_trim::SpeakerTrimTable& table) {
    std::array<std::uint8_t, iclforge::render::speaker_trim::SpeakerTrimTable::kBlobBytes> blob{};
    const std::size_t length = table.to_blob(blob);
    nvs_handle_t handle = 0;
    if (length == 0 || nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to write %s\n", sk::kSpeakerTrim);
        return false;
    }
    const esp_err_t err = nvs_set_blob(handle, sk::kSpeakerTrim, blob.data(), length);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
        g_settings.speaker_trim = table;
    } else {
        std::printf("settings: NVS refused %s (%s)\n", sk::kSpeakerTrim, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool settings_set_time_zone(std::string_view zone, std::string_view name) {
    if (!zone.empty() && !iclforge::wallclock::valid_time_zone(zone)) {
        std::printf("settings: '%.*s' is not a time zone this board reads\n", static_cast<int>(zone.size()),
                    zone.data());
        return false;
    }
    // The name is kept only when the table says it stands for this string: anything else is a string somebody typed.
    std::string_view kept;
    if (!name.empty()) {
        const auto named = iclforge::wallclock::find_time_zone(name);
        if (named && named->posix == zone) {
            kept = named->name;
        } else {
            std::printf("settings: '%.*s' is not the name of that time zone; keeping the string alone\n",
                        static_cast<int>(name.size()), name.data());
        }
    }
    const std::string previous_zone = g_settings.time_zone.data();
    // An empty string is stored as such, and read back as no zone: NVS keeps the key, the clock says UTC.
    if (!write_string(sk::kTimeZone, zone, g_settings.time_zone)) {
        return false;
    }
    // The name is always written, empty for a typed string, so that an old name never stands for a new string. If it
    // cannot be, the string goes back: the two must agree, and a boot reads the name's rule over the string.
    if (!write_string(sk::kTimeZoneName, kept, g_settings.time_zone_name)) {
        (void)write_string(sk::kTimeZone, previous_zone, g_settings.time_zone);
        return false;
    }
    return true;
}

bool settings_set_ntp_server(std::string_view server) {
    if (!server.empty() && !iclforge::wallclock::valid_ntp_server(server)) {
        std::printf("settings: '%.*s' is not a time server this board reads\n", static_cast<int>(server.size()),
                    server.data());
        return false;
    }
    return write_string(sk::kNtpServer, server, g_settings.ntp_server);
}

bool settings_set_time_sync(bool on) {
    if (!write_int(sk::kTimeSync, on ? 1 : 0)) {
        return false;
    }
    g_settings.time_sync = on;
    return true;
}

bool settings_set_dynamic_range(iclforge::ac3::DynamicRange range) {
    if (!write_int(sk::kDynamicRange, static_cast<std::int32_t>(range))) {
        return false;
    }
    g_settings.dynamic_range = range;
    return true;
}

bool settings_forget() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to clear it\n");
        return false;
    }
    const esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        std::printf("settings: NVS refused the erase (%s)\n", esp_err_to_name(err));
        return false;
    }
    // Every field back to the image's, the network and the choice between networks too: the board
    // restarts next, but whatever reads a setting before then must not find the network it was told to forget.
    g_loaded = true;
    load_all();
    return true;
}


}  // namespace player
