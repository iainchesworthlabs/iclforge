#include "capture_lyrics.hpp"

#include <cstddef>
#include <utility>

namespace iclforge::crucible::ui {

namespace {

// A day is longer than any song, and keeps the arithmetic far from overflow.
constexpr std::int64_t kMaxMs = 24LL * 60 * 60 * 1000;

[[nodiscard]] bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Digits only, at least one, and not so many that the value could leave an int64's range once it
// is multiplied up to milliseconds.
[[nodiscard]] std::optional<std::int64_t> whole_number(std::string_view text) {
    if (text.empty() || text.size() > 7) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

// `5`, `5.75`, `0:05`, `0:05.75`, `00:05.75`: seconds, or minutes and seconds with the seconds
// under sixty (an .lrc's own form), and an optional fraction of a second.
[[nodiscard]] std::optional<std::int64_t> parse_time_ms(std::string_view text) {
    std::string_view whole = text;
    std::string_view fraction;
    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        whole = text.substr(0, dot);
        fraction = text.substr(dot + 1);
        if (fraction.empty()) {
            return std::nullopt;
        }
        for (const char c : fraction) {
            if (!is_digit(c)) {
                return std::nullopt;
            }
        }
    }
    std::int64_t seconds = 0;
    if (const auto colon = whole.find(':'); colon != std::string_view::npos) {
        const auto minutes = whole_number(whole.substr(0, colon));
        const auto rest = whole_number(whole.substr(colon + 1));
        if (!minutes || !rest || *rest >= 60) {
            return std::nullopt;
        }
        seconds = *minutes * 60 + *rest;
    } else {
        const auto only = whole_number(whole);
        if (!only) {
            return std::nullopt;
        }
        seconds = *only;
    }
    std::int64_t millis = 0;
    std::int64_t place = 100;
    for (std::size_t i = 0; i < fraction.size() && i < 3; ++i, place /= 10) {
        millis += (fraction[i] - '0') * place;
    }
    const std::int64_t total = seconds * 1000 + millis;
    if (total > kMaxMs) {
        return std::nullopt;
    }
    return total;
}

}  // namespace

std::optional<CaptureLyrics> parse_capture_lyrics(std::string_view spec) {
    const auto equals = spec.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    CaptureLyrics out;
    const std::string_view name = trimmed(spec.substr(0, equals));
    std::string_view file = spec.substr(equals + 1);
    if (const auto at = file.rfind('@'); at != std::string_view::npos) {
        if (const auto time = parse_time_ms(trimmed(file.substr(at + 1)))) {
            out.at_ms = *time;
            file = file.substr(0, at);
        }
    }
    file = trimmed(file);
    if (name.empty() || file.empty()) {
        return std::nullopt;
    }
    out.application = std::string(name);
    out.file = std::string(file);
    return out;
}

std::int64_t capture_start_ms(std::int64_t now_ms, std::int64_t at_ms, std::int64_t until_grab_ms) {
    return now_ms + until_grab_ms - at_ms;
}

}  // namespace iclforge::crucible::ui
