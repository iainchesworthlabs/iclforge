#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What a Hearth board may be called, and what a rename changes (planning/p4x-preferences.md, E4).
//
// THE NAME. One text of 1 to kMaxBytes bytes of UTF-8: what the board calls itself on the network (its
// mDNS instance name and the `name` in its _sendspin._tcp record), in the hello it says to a Sendspin
// server (the name a server lists the player by), on the web page's title and heading, in Improv's
// device name and on the screen's footer. The route (PUT /name), the web page and the on-screen
// keyboard all hold a name to check_name(), so that a name one of them stores is a name the others can
// show.
//
// WHAT A RENAME CHANGES (the firmware's side is hearth_sink.cpp, set_name, and discovery.cpp):
//   - the NVS key `name` and settings().name: at once;
//   - the mDNS host name (`<label>.local`, mdns_label() below), the instance name and the
//     _sendspin._tcp record's instance and `name` text: at once, with no restart;
//   - the hello a Sendspin server was given: the player's config is rebuilt, and a connection that
//     said hello with the old name is told to go (client/goodbye restart) and dialled again, which
//     the server does at once. While a stream is playing that is held back until it ends, so that a
//     rename never cuts the music;
//   - the web page's title and heading, the screen's footer, Improv's device_info: read from
//     settings().name when they next draw;
//   - the names the board answers to for the firmware PUTs (discovery_host_name(): the Host check),
//     which follow the mDNS host name in the same step.
// What it does NOT change: the pairings. A pairing is the server's key and the PSK, the board's identity
// is its key (client_id), and nothing in the handshake is the name, so every server stays paired.
// Neither does a rename change the name a server has shown as ITS OWN label for the player: Music
// Assistant keeps a name a person gave the player there.
//
// Free of ESP-IDF and of LVGL, like dev_view.hpp: libs/device/tests/test_device_name.cpp holds it to
// the letter.

namespace iclforge::devname {

// mDNS's own limit on a label is 63 octets (RFC 6763 section 4.1.1); this is the shorter thing a person
// types, and what NVS and the Sendspin hello are sized for.
inline constexpr std::size_t kMaxBytes = 32;

// --- UTF-8 -----------------------------------------------------------------------------------------

inline constexpr char32_t kInvalid = 0xFFFFFFFF;

// The code point at `at` in `text`, and `at` moved past it. kInvalid, having moved one byte, for a byte
// that is not the start of a well-formed sequence (a stray continuation byte, an overlong form, a
// surrogate, a point past U+10FFFF, a sequence cut short).
[[nodiscard]] inline char32_t next_code_point(std::string_view text, std::size_t& at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    std::size_t length = 0;
    char32_t point = 0;
    char32_t least = 0;
    if (lead < 0x80) {
        ++at;
        return lead;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        point = lead & 0x1Fu;
        least = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        point = lead & 0x0Fu;
        least = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        point = lead & 0x07u;
        least = 0x10000;
    } else {
        ++at;
        return kInvalid;
    }
    if (at + length > text.size()) {
        ++at;
        return kInvalid;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0u) != 0x80u) {
            ++at;
            return kInvalid;
        }
        point = (point << 6) | (byte(at + i) & 0x3Fu);
    }
    if (point < least || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
        ++at;
        return kInvalid;
    }
    at += length;
    return point;
}

// The UTF-8 of one code point, which must be valid.
[[nodiscard]] inline std::string encode(char32_t point) {
    std::string out;
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xC0 | (point >> 6));
        out += static_cast<char>(0x80 | (point & 0x3F));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xE0 | (point >> 12));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (point >> 18));
        out += static_cast<char>(0x80 | ((point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (point & 0x3F));
    }
    return out;
}

// --- the check -------------------------------------------------------------------------------------

enum class Problem : std::uint8_t {
    none,
    empty,       // no characters
    too_long,    // more than kMaxBytes bytes
    bad_utf8,    // not well-formed UTF-8
    invisible,   // a control character, a line or paragraph separator, a bidirectional control, a BOM
    edge_space,  // a space at either end
};

struct Check {
    Problem problem = Problem::none;
    [[nodiscard]] bool ok() const { return problem == Problem::none; }
};

// A code point no name may hold: it draws as nothing, or moves the text about.
[[nodiscard]] constexpr bool is_invisible(char32_t point) {
    return point < 0x20 || (point >= 0x7F && point <= 0x9F) || point == 0x2028 || point == 0x2029 ||
           (point >= 0x200B && point <= 0x200F) || (point >= 0x202A && point <= 0x202E) ||
           (point >= 0x2066 && point <= 0x2069) || point == 0xFEFF;
}

[[nodiscard]] inline Check check_name(std::string_view name) {
    if (name.empty()) {
        return {Problem::empty};
    }
    if (name.size() > kMaxBytes) {
        return {Problem::too_long};
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return {Problem::edge_space};
    }
    for (std::size_t at = 0; at < name.size();) {
        const char32_t point = next_code_point(name, at);
        if (point == kInvalid) {
            return {Problem::bad_utf8};
        }
        if (is_invisible(point)) {
            return {Problem::invisible};
        }
    }
    return {};
}

// What the screen and the route say of a name that does not pass, in the words of the page.
[[nodiscard]] inline const char* problem_text(Problem problem) {
    switch (problem) {
        case Problem::none:
            return "";
        case Problem::empty:
            return "A name needs at least one character.";
        case Problem::too_long:
            return "A name can be 32 bytes at most; a letter with an accent is two.";
        case Problem::bad_utf8:
            return "A name has to be text (UTF-8).";
        case Problem::invisible:
            return "A name cannot hold a control character.";
        case Problem::edge_space:
            return "A name cannot start or end with a space.";
    }
    return "";
}

// --- the mDNS host name ----------------------------------------------------------------------------

namespace detail {

// The Latin-1 Supplement's letters (U+00C0 to U+00FF) as the ASCII a host name can hold: a letter
// without its accent, or its usual spelling out (AE, TH, ss). Empty for the two signs, which separate.
inline constexpr std::array<const char*, 64> kLatin1 = {
    "a",  "a",  "a",  "a",  "a",  "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // C0
    "d",  "n",  "o",  "o",  "o",  "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "ss",  // D0
    "a",  "a",  "a",  "a",  "a",  "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // E0
    "d",  "n",  "o",  "o",  "o",  "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "y",   // F0
};

// Latin Extended-A (U+0100 to U+017F), the same way.
inline constexpr std::array<const char*, 128> kExtendedA = {
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",    // 100
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",    // 110
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",    // 120
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",  // 130
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",    // 140
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",  // 150
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",    // 160
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",    // 170
};

// The ASCII a code point stands for in a host name: its own lower-case letter or digit, the spelling of
// a Latin letter, or nothing (a separator).
inline void append_label_letters(char32_t point, std::string& out) {
    if (point >= 'a' && point <= 'z') {
        out += static_cast<char>(point);
    } else if (point >= 'A' && point <= 'Z') {
        out += static_cast<char>(point - 'A' + 'a');
    } else if (point >= '0' && point <= '9') {
        out += static_cast<char>(point);
    } else if (point >= 0xC0 && point <= 0xFF) {
        out += kLatin1[point - 0xC0];
    } else if (point >= 0x100 && point <= 0x17F) {
        out += kExtendedA[point - 0x100];
    }
}

}  // namespace detail

// The label before ".local" that a name answers to: letters and digits in lower case, a Latin letter
// as the ASCII it is spelt with ("Café" is "cafe"), anything else a hyphen between words, none at either
// end. "hearth" when nothing is left. The same rule as the board has always had for ASCII; the
// letters of the keyboard (kAccentedLatin1 and kAccentedExtended) are the new ones.
[[nodiscard]] inline std::string mdns_label(std::string_view name) {
    std::string out;
    bool separator = false;
    for (std::size_t at = 0; at < name.size();) {
        const char32_t point = next_code_point(name, at);
        std::string letters;
        if (point != kInvalid) {
            detail::append_label_letters(point, letters);
        }
        if (letters.empty()) {
            separator = !out.empty();
            continue;
        }
        if (separator) {
            out += '-';
            separator = false;
        }
        out += letters;
    }
    constexpr std::size_t kLabelBytes = 63;
    if (out.size() > kLabelBytes) {
        out.resize(kLabelBytes);
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
    }
    return out.empty() ? std::string("hearth") : out;
}

// --- the keyboard ----------------------------------------------------------------------------------

// What the on-screen keyboard offers, as the text each key adds. Every key passes check_name() on its
// own and is a letter the panel's fonts carry (panel/lvgl/glyphs.cpp: the Latin subset of Archivo,
// U+0020 to U+007E and U+00A0 to U+017F, and U+1E9E), which firmware checks against is_drawable() when
// the editor opens. Names are counted in bytes, so an accented letter costs two.

inline constexpr std::string_view kLowerLetters = "abcdefghijklmnopqrstuvwxyz";
inline constexpr std::string_view kUpperLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
inline constexpr std::string_view kDigits = "0123456789";
// Punctuation a room's name uses: Dad's room, Kitchen (TV), Study #2, Bar & grill, living-room.
inline constexpr std::string_view kSymbols = "-_.,'&()#+!";

// One accented letter in both cases: the lower-case key, and what shift gives.
struct AccentPair {
    const char* lower;
    const char* upper;
};

// The Latin-1 Supplement's letters (U+00E0 to U+00FF without the division sign).
inline constexpr std::array<AccentPair, 31> kAccentedLatin1 = {{
    {"\xC3\xA0", "\xC3\x80"}, {"\xC3\xA1", "\xC3\x81"}, {"\xC3\xA2", "\xC3\x82"}, {"\xC3\xA3", "\xC3\x83"},  // a grave, acute, circumflex, tilde
    {"\xC3\xA4", "\xC3\x84"}, {"\xC3\xA5", "\xC3\x85"}, {"\xC3\xA6", "\xC3\x86"}, {"\xC3\xA7", "\xC3\x87"},  // a diaeresis, a ring, ae, c cedilla
    {"\xC3\xA8", "\xC3\x88"}, {"\xC3\xA9", "\xC3\x89"}, {"\xC3\xAA", "\xC3\x8A"}, {"\xC3\xAB", "\xC3\x8B"},  // e grave, acute, circumflex, diaeresis
    {"\xC3\xAC", "\xC3\x8C"}, {"\xC3\xAD", "\xC3\x8D"}, {"\xC3\xAE", "\xC3\x8E"}, {"\xC3\xAF", "\xC3\x8F"},  // i grave, acute, circumflex, diaeresis
    {"\xC3\xB0", "\xC3\x90"}, {"\xC3\xB1", "\xC3\x91"}, {"\xC3\xB2", "\xC3\x92"}, {"\xC3\xB3", "\xC3\x93"},  // eth, n tilde, o grave, acute
    {"\xC3\xB4", "\xC3\x94"}, {"\xC3\xB5", "\xC3\x95"}, {"\xC3\xB6", "\xC3\x96"}, {"\xC3\xB8", "\xC3\x98"},  // o circumflex, tilde, diaeresis, stroke
    {"\xC3\xB9", "\xC3\x99"}, {"\xC3\xBA", "\xC3\x9A"}, {"\xC3\xBB", "\xC3\x9B"}, {"\xC3\xBC", "\xC3\x9C"},  // u grave, acute, circumflex, diaeresis
    {"\xC3\xBD", "\xC3\x9D"}, {"\xC3\xBE", "\xC3\x9E"}, {"\xC3\xBF", "\xC5\xB8"},                            // y acute, thorn, y diaeresis
}};

// The letters of Latin Extended-A and the Romanian pair that Polish, Czech, Slovak, Hungarian, Turkish,
// Romanian, Croatian and the Baltic languages add.
inline constexpr std::array<AccentPair, 29> kAccentedExtended = {{
    {"\xC4\x85", "\xC4\x84"}, {"\xC4\x87", "\xC4\x86"}, {"\xC4\x8D", "\xC4\x8C"}, {"\xC4\x8F", "\xC4\x8E"},  // a ogonek, c acute, c caron, d caron
    {"\xC4\x91", "\xC4\x90"}, {"\xC4\x99", "\xC4\x98"}, {"\xC4\x9B", "\xC4\x9A"}, {"\xC4\x9F", "\xC4\x9E"},  // d stroke, e ogonek, e caron, g breve
    {"\xC5\x82", "\xC5\x81"}, {"\xC5\x84", "\xC5\x83"}, {"\xC5\x88", "\xC5\x87"}, {"\xC5\x91", "\xC5\x90"},  // l stroke, n acute, n caron, o double acute
    {"\xC5\x99", "\xC5\x98"}, {"\xC5\x9B", "\xC5\x9A"}, {"\xC5\xA1", "\xC5\xA0"}, {"\xC5\x9F", "\xC5\x9E"},  // r caron, s acute, s caron, s cedilla
    {"\xC5\xA5", "\xC5\xA4"}, {"\xC5\xAF", "\xC5\xAE"}, {"\xC5\xB1", "\xC5\xB0"}, {"\xC5\xBA", "\xC5\xB9"},  // t caron, u ring, u double acute, z acute
    {"\xC5\xBC", "\xC5\xBB"}, {"\xC5\xBE", "\xC5\xBD"}, {"\xC4\x83", "\xC4\x82"}, {"\xC5\x93", "\xC5\x92"},  // z dot, z caron, a breve, oe
    {"\xC8\x99", "\xC8\x98"}, {"\xC8\x9B", "\xC8\x9A"}, {"\xC4\xB1", "I"},        {"\xC3\x9F", "\xE1\xBA\x9E"},  // s comma, t comma, dotless i, sharp s
    {"\xC4\x8B", "\xC4\x8A"},
}};

enum class Page : std::uint8_t { lower, upper, symbols, accents_latin1, accents_extended };

// The keys of a page, each the text it adds. `shifted` is for the accent pages (the upper-case forms);
// the letter pages are their own shift.
[[nodiscard]] inline std::vector<std::string> page_keys(Page page, bool shifted = false) {
    std::vector<std::string> keys;
    const auto characters = [&](std::string_view letters) {
        for (const char c : letters) {
            keys.emplace_back(1, c);
        }
    };
    const auto pairs = [&](std::span<const AccentPair> accents) {
        for (const AccentPair& pair : accents) {
            keys.emplace_back(shifted ? pair.upper : pair.lower);
        }
    };
    switch (page) {
        case Page::lower:
            characters(kLowerLetters);
            break;
        case Page::upper:
            characters(kUpperLetters);
            break;
        case Page::symbols:
            characters(kDigits);
            characters(kSymbols);
            break;
        case Page::accents_latin1:
            pairs(kAccentedLatin1);
            break;
        case Page::accents_extended:
            pairs(kAccentedExtended);
            break;
    }
    return keys;
}

// Whether `key` is one the keyboard offers (on any page, in either case), or the space bar.
[[nodiscard]] inline bool is_offered(std::string_view key) {
    if (key.size() == 1) {
        return key == " " || kLowerLetters.find(key) != std::string_view::npos ||
               kUpperLetters.find(key) != std::string_view::npos || kDigits.find(key) != std::string_view::npos ||
               kSymbols.find(key) != std::string_view::npos;
    }
    const auto among = [&](std::span<const AccentPair> accents) {
        for (const AccentPair& pair : accents) {
            if (key == pair.lower || key == pair.upper) {
                return true;
            }
        }
        return false;
    };
    return among(kAccentedLatin1) || among(kAccentedExtended);
}

// The name under edit: what the keys have typed so far, and what is left of the byte budget. The screen
// draws it and asks it; it does not store anything.
class Editor {
   public:
    // `initial` is the name now, shown to be changed; a name that fails check_name() starts empty.
    explicit Editor(std::string_view initial = {}) : initial_(initial) {
        if (check_name(initial).ok()) {
            text_ = std::string(initial);
        }
        page_ = text_.empty() ? Page::upper : Page::lower;
    }

    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] Page page() const { return page_; }
    [[nodiscard]] std::size_t bytes_left() const { return text_.size() >= kMaxBytes ? 0 : kMaxBytes - text_.size(); }

    // What a tap on OK stores: the text without the spaces typed at its end.
    [[nodiscard]] std::string result() const {
        std::string out = text_;
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }

    // Whether `result()` is a name, and if it is not, why not.
    [[nodiscard]] Check check() const { return check_name(result()); }

    // Whether OK does anything: a valid name that is not the one it began as.
    [[nodiscard]] bool can_accept() const { return check().ok() && result() != initial_; }

    // Whether a tap on `key` would add it: it is one the keyboard offers, the budget has room for its
    // bytes, and it is not a space with nothing before it.
    [[nodiscard]] bool can_press(std::string_view key) const {
        if (!is_offered(key) || text_.size() + key.size() > kMaxBytes) {
            return false;
        }
        return key != " " || !text_.empty();
    }

    // A tap on a key. False, and nothing typed, when can_press() says no. A capital typed on a one-shot
    // shift returns to the lower-case page, as a phone's keyboard does.
    bool press(std::string_view key) {
        if (!can_press(key)) {
            return false;
        }
        text_ += key;
        if (page_ == Page::upper && shift_once_) {
            page_ = Page::lower;
        }
        shift_once_ = false;
        return true;
    }

    // Removes the last character (not the last byte: an accented letter goes whole).
    void backspace() {
        if (text_.empty()) {
            return;
        }
        std::size_t cut = text_.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text_[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        text_.erase(cut);
    }

    void clear() { text_.clear(); }

    // The shift key: the letters swap case for one letter; on an accent page the forms swap and stay.
    void shift() {
        if (page_ == Page::lower) {
            page_ = Page::upper;
            shift_once_ = true;
        } else if (page_ == Page::upper) {
            page_ = Page::lower;
            shift_once_ = false;
        } else {
            accents_shifted_ = !accents_shifted_;
        }
    }
    [[nodiscard]] bool shifted() const {
        return page_ == Page::upper || ((page_ == Page::accents_latin1 || page_ == Page::accents_extended) && accents_shifted_);
    }
    void show(Page page) {
        page_ = page;
        shift_once_ = false;
    }

   private:
    std::string initial_;
    std::string text_;
    Page page_ = Page::lower;
    bool shift_once_ = true;
    bool accents_shifted_ = false;
};

}  // namespace iclforge::devname
