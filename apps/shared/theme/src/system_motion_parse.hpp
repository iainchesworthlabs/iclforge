#pragma once

#include <string_view>

// What a desktop's own settings store says about animation, read as text.
// Header-only and Qt-free so the parts that are a judgement - which printed
// value means "reduced" - are held by a unit test on every platform, not
// only on the one desktop that produces them. The platform files
// (platform/<os>/system_motion_query.cpp) fetch the text and hand it here.
namespace iclforge::theme {

namespace detail {

[[nodiscard]] constexpr std::string_view trimmed(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

}  // namespace detail

// GNOME's org.gnome.desktop.interface "enable-animations", as `gsettings get`
// prints it: `true` or `false` and a newline. Only a plain `false` is a
// request to cut back; an empty answer (no gsettings, no such schema) and
// anything unrecognised is "not reduced", because guessing that a person
// wants less motion from a missing tool would take animation away from
// someone who never asked.
[[nodiscard]] constexpr bool reduced_from_gsettings_enable_animations(std::string_view printed) noexcept {
    return detail::trimmed(printed) == "false";
}

// KDE's [KDE] AnimationDurationFactor in kdeglobals: the multiplier System
// Settings > Workspace Behavior > General Behavior > Animation speed applies
// to every animation's duration, where 1 is the default and 0 is "instant"
// (the slider's far end), which is KDE's own way of turning animation off.
// Only a factor that is zero - "0", "0.0", "0.00" - is a request to cut
// back; absent or unreadable is "not reduced".
[[nodiscard]] constexpr bool reduced_from_kde_animation_duration_factor(std::string_view value) noexcept {
    const std::string_view text = detail::trimmed(value);
    if (text.empty()) {
        return false;
    }
    bool dot = false;
    for (const char c : text) {
        if (c == '.' && !dot) {
            dot = true;
        } else if (c != '0') {
            return false;
        }
    }
    return text.find('0') != std::string_view::npos;
}

}  // namespace iclforge::theme
