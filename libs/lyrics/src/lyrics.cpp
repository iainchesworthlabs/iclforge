#include "iclforge/lyrics/lyrics.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace iclforge::lyrics {

namespace {

// How long the last line lasts when nothing says: the item's duration, a
// `[length:]` tag, and a closing time tag are all better, and are used first.
constexpr std::int64_t kLastLineMs = 5000;
constexpr std::uintmax_t kMaxFileBytes = 4U * 1024U * 1024U;

// ASCII white space only, whatever the locale: a byte of a UTF-8 sequence is not
// white space in any locale's table that matters here, but a Latin-1 locale
// calls 0xA0 one, and 0xA0 ends U+05E0 (nun) and a hundred other Hebrew and
// Yiddish letters. The line's trailing letter must survive being trimmed.
[[nodiscard]] bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::string lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// UTF-16, the bytes after a byte-order mark, as UTF-8; nothing for an odd
// length or a surrogate that is not paired. A noncharacter is as valid as any
// other code point here, as it is in UTF-8's own check below.
[[nodiscard]] std::optional<std::string> utf16_to_utf8(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto unit_at = [&](std::size_t i) {
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]));
        const auto second = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1]));
        return big_endian ? (first << 8) | second : (second << 8) | first;
    };
    std::string out;
    out.reserve(bytes.size());
    const auto append = [&out](std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    };
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const std::uint32_t unit = unit_at(i);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 3 >= bytes.size()) {
                return std::nullopt;
            }
            const std::uint32_t low = unit_at(i + 2);
            if (low < 0xDC00 || low > 0xDFFF) {
                return std::nullopt;
            }
            append(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            i += 2;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return std::nullopt;
        } else {
            append(unit);
        }
    }
    return out;
}

// RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF.
[[nodiscard]] bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
            minimum = 0x80;
            code = lead & 0x1FU;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            minimum = 0x800;
            code = lead & 0x0FU;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            minimum = 0x10000;
            code = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(s[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

[[nodiscard]] bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "m:ss", "m:ss.f", "m:ss.ff", "m:ss.fff" or "m:ss:ff", in milliseconds.
[[nodiscard]] std::optional<std::int64_t> parse_time(std::string_view s) {
    std::size_t i = 0;
    std::int64_t minutes = 0;
    std::size_t minute_digits = 0;
    while (i < s.size() && is_digit(s[i])) {
        if (++minute_digits > 4) {
            return std::nullopt;
        }
        minutes = (minutes * 10) + (s[i] - '0');
        ++i;
    }
    if (minute_digits == 0 || i >= s.size() || s[i] != ':') {
        return std::nullopt;
    }
    ++i;
    std::int64_t seconds = 0;
    std::size_t second_digits = 0;
    while (i < s.size() && is_digit(s[i])) {
        if (++second_digits > 2) {
            return std::nullopt;
        }
        seconds = (seconds * 10) + (s[i] - '0');
        ++i;
    }
    if (second_digits == 0 || seconds > 59) {
        return std::nullopt;
    }
    std::int64_t millis = 0;
    if (i < s.size()) {
        if (s[i] != '.' && s[i] != ':') {
            return std::nullopt;
        }
        ++i;
        std::size_t fraction_digits = 0;
        std::int64_t fraction = 0;
        while (i < s.size() && is_digit(s[i])) {
            if (++fraction_digits > 3) {
                return std::nullopt;
            }
            fraction = (fraction * 10) + (s[i] - '0');
            ++i;
        }
        if (fraction_digits == 0 || i != s.size()) {
            return std::nullopt;
        }
        millis = fraction_digits == 1 ? fraction * 100 : (fraction_digits == 2 ? fraction * 10 : fraction);
    }
    return (((minutes * 60) + seconds) * 1000) + millis;
}

// "+500", "-250" or "500", in milliseconds.
[[nodiscard]] std::optional<std::int64_t> parse_offset(std::string_view s) {
    s = trim(s);
    bool negative = false;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty() || s.size() > 9) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char c : s) {
        if (!is_digit(c)) {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return negative ? -value : value;
}

// A line as read, before the times of its neighbours are known.
struct WordProto {
    // From the line's first time tag.
    std::int64_t offset = 0;
    std::string text{};
};

struct LineProto {
    std::int64_t start = 0;
    std::string text{};
    std::vector<WordProto> words{};
    // Where the last word ends, from the line's start, when a closing tag said.
    std::optional<std::int64_t> end_offset{};
};

struct Segment {
    std::optional<std::int64_t> tag{};
    std::string text{};
};

// A line's text split at its `<mm:ss.xx>` tags. The first segment is whatever
// came before the first tag and has none.
[[nodiscard]] std::vector<Segment> split_words(std::string_view text) {
    std::vector<Segment> segments(1);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const std::size_t close = text.find('>', i);
            if (close != std::string_view::npos) {
                if (const auto tag = parse_time(text.substr(i + 1, close - i - 1))) {
                    segments.push_back(Segment{.tag = tag, .text = {}});
                    i = close + 1;
                    continue;
                }
            }
        }
        segments.back().text.push_back(text[i]);
        ++i;
    }
    return segments;
}

// The words of a line from its segments, with offsets from the line's first
// time tag, `first`; and where a closing tag ended the last word, if one did.
void build_words(const std::vector<Segment>& segments, std::int64_t first, LineProto& line) {
    bool tagged = false;
    for (const Segment& segment : segments) {
        tagged = tagged || segment.tag.has_value();
    }
    if (!tagged) {
        std::string joined;
        for (const Segment& segment : segments) {
            joined += segment.text;
        }
        line.text = std::string{trim(joined)};
        return;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Segment& segment = segments[i];
        const bool last = i + 1 == segments.size();
        if (!segment.tag) {
            if (!trim(segment.text).empty()) {
                line.words.push_back({.offset = 0, .text = segment.text});
            }
            continue;
        }
        const std::int64_t offset = std::max<std::int64_t>(0, *segment.tag - first);
        if (segment.text.empty()) {
            if (last) {
                line.end_offset = offset;
            }
            continue;
        }
        if (trim(segment.text).empty()) {
            // Only spacing after a tag: it belongs to the word before.
            if (!line.words.empty()) {
                line.words.back().text += segment.text;
            }
            continue;
        }
        line.words.push_back({.offset = offset, .text = segment.text});
    }
    // A line shows without the spacing at its ends.
    if (!line.words.empty()) {
        std::string& front = line.words.front().text;
        std::size_t lead = 0;
        while (lead < front.size() && is_space(front[lead])) {
            ++lead;
        }
        front.erase(0, lead);
        std::string& back = line.words.back().text;
        while (!back.empty() && is_space(back.back())) {
            back.pop_back();
        }
    }
    for (const WordProto& word : line.words) {
        line.text += word.text;
    }
}

// The bytes of a file as the UTF-8 they say: UTF-16 with a byte-order mark converted, a UTF-8 mark
// dropped, anything else that is not UTF-8 refused with the reason, for both readers.
[[nodiscard]] std::expected<std::string, std::string> decode_text(std::string_view text) {
    // UTF-32 would begin with the same two bytes as UTF-16's little-endian mark,
    // then two zero bytes; no lyrics file starts with a NUL character.
    if (text.starts_with(std::string_view{"\xFF\xFE\x00\x00", 4}) ||
        text.starts_with(std::string_view{"\x00\x00\xFE\xFF", 4})) {
        return std::unexpected(std::string{"The lyrics file is UTF-32, which is not read. Save it as UTF-8 or UTF-16."});
    }
    std::string converted;
    if (text.starts_with("\xFF\xFE") || text.starts_with("\xFE\xFF")) {
        const bool big_endian = static_cast<unsigned char>(text[0]) == 0xFE;
        auto utf8 = utf16_to_utf8(text.substr(2), big_endian);
        if (!utf8) {
            return std::unexpected(std::string{"The lyrics file is not valid UTF-16."});
        }
        converted = std::move(*utf8);
        text = converted;
    }
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    if (!valid_utf8(text)) {
        return std::unexpected(std::string{"The lyrics file is not UTF-8 or UTF-16."});
    }
    if (trim(text).empty()) {
        return std::unexpected(std::string{"The lyrics file is empty."});
    }
    return std::string{text};
}

// A SubRip time, "hh:mm:ss,mmm" ("." for "," taken too, and the milliseconds as a fraction of a
// second of one to three digits), in milliseconds.
[[nodiscard]] std::optional<std::int64_t> parse_subrip_time(std::string_view s) {
    s = trim(s);
    std::int64_t parts[3] = {0, 0, 0};
    std::size_t at = 0;
    for (int part = 0; part < 3; ++part) {
        std::size_t digits = 0;
        std::int64_t value = 0;
        while (at < s.size() && is_digit(s[at])) {
            if (++digits > 4) {
                return std::nullopt;
            }
            value = (value * 10) + (s[at] - '0');
            ++at;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        parts[part] = value;
        if (part < 2) {
            if (at >= s.size() || s[at] != ':') {
                return std::nullopt;
            }
            ++at;
        }
    }
    std::int64_t millis = 0;
    if (at < s.size()) {
        if (s[at] != ',' && s[at] != '.') {
            return std::nullopt;
        }
        ++at;
        std::size_t digits = 0;
        std::int64_t fraction = 0;
        while (at < s.size() && is_digit(s[at])) {
            if (++digits > 3) {
                return std::nullopt;
            }
            fraction = (fraction * 10) + (s[at] - '0');
            ++at;
        }
        if (digits == 0 || at != s.size()) {
            return std::nullopt;
        }
        millis = digits == 1 ? fraction * 100 : (digits == 2 ? fraction * 10 : fraction);
    } else if (at != s.size()) {
        return std::nullopt;
    }
    if (parts[1] > 59 || parts[2] > 59) {
        return std::nullopt;
    }
    return (((parts[0] * 60) + parts[1]) * 60 + parts[2]) * 1000 + millis;
}

// A cue's start and end from its time line, "start --> end" with anything after the end (the
// position tags `X1:... Y1:...`) ignored.
[[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>> parse_cue_times(std::string_view line) {
    const std::size_t arrow = line.find("-->");
    if (arrow == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = parse_subrip_time(line.substr(0, arrow));
    std::string_view rest = trim(line.substr(arrow + 3));
    const std::size_t space = rest.find_first_of(" \t");
    const auto end = parse_subrip_time(space == std::string_view::npos ? rest : rest.substr(0, space));
    if (!start || !end) {
        return std::nullopt;
    }
    return std::pair{*start, std::max(*end, *start)};
}

// A cue's text without its styling: `<...>` tags that are tags (a letter or a slash after the
// bracket), and `{...}` override blocks; white space folded to single spaces.
[[nodiscard]] std::string clean_cue_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '<' && i + 1 < text.size() &&
            (text[i + 1] == '/' || std::isalpha(static_cast<unsigned char>(text[i + 1])) != 0)) {
            const std::size_t close = text.find('>', i);
            if (close != std::string_view::npos) {
                i = close + 1;
                continue;
            }
        }
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close != std::string_view::npos) {
                i = close + 1;
                continue;
            }
        }
        out.push_back(is_space(text[i]) ? ' ' : text[i]);
        ++i;
    }
    // Fold runs of spaces and trim.
    std::string folded;
    folded.reserve(out.size());
    bool space = true;
    for (const char c : out) {
        if (c == ' ') {
            if (!space) {
                folded.push_back(' ');
            }
            space = true;
        } else {
            folded.push_back(c);
            space = false;
        }
    }
    while (!folded.empty() && folded.back() == ' ') {
        folded.pop_back();
    }
    return folded;
}

// ---- MIDI karaoke -----------------------------------------------------------------------------

// A cursor over the bytes of a Standard MIDI File, which is not valid until it has run off the end.
struct MidiReader {
    std::string_view data;
    std::size_t at = 0;
    bool ok = true;

    [[nodiscard]] bool has(std::size_t count) const { return at + count <= data.size(); }
    std::uint32_t u8() {
        if (!has(1)) {
            ok = false;
            return 0;
        }
        return static_cast<unsigned char>(data[at++]);
    }
    std::uint32_t u16() {
        const std::uint32_t high = u8();
        const std::uint32_t low = u8();
        return (high << 8U) | low;
    }
    std::uint32_t u32() {
        const std::uint32_t high = u16();
        const std::uint32_t low = u16();
        return (high << 16U) | low;
    }
    // A variable-length quantity: seven bits a byte, the high bit set on all but the last, four bytes
    // at most.
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t byte = u8();
            if (!ok) {
                return 0;
            }
            value = (value << 7U) | (byte & 0x7FU);
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        ok = false;
        return 0;
    }
    std::string_view take(std::size_t count) {
        if (!has(count)) {
            ok = false;
            return {};
        }
        const std::string_view out = data.substr(at, count);
        at += count;
        return out;
    }
};

// A Text or Lyric meta event, with the name of the track it is on.
struct MidiText {
    std::uint64_t tick = 0;
    int type = 0;
    std::string text;
    bool on_words_track = false;
};

struct MidiTempo {
    std::uint64_t tick = 0;
    std::uint32_t micros_per_quarter = 500000;
};

// A syllable and where it starts.
struct MidiSyllable {
    std::uint64_t tick = 0;
    std::string text;
};

[[nodiscard]] bool equals_nocase(std::string_view a, std::string_view b) {
    return lowercase(a) == lowercase(b);
}

// ---- Advanced SubStation Alpha ---------------------------------------------------------------

// "h:mm:ss.cc" (hours of one or two digits, hundredths of one to three), in milliseconds.
[[nodiscard]] std::optional<std::int64_t> parse_ass_time(std::string_view s) {
    s = trim(s);
    std::int64_t parts[3] = {0, 0, 0};
    std::size_t at = 0;
    for (int part = 0; part < 3; ++part) {
        std::size_t digits = 0;
        std::int64_t value = 0;
        while (at < s.size() && is_digit(s[at])) {
            if (++digits > 4) {
                return std::nullopt;
            }
            value = (value * 10) + (s[at] - '0');
            ++at;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        parts[part] = value;
        if (part < 2) {
            if (at >= s.size() || s[at] != ':') {
                return std::nullopt;
            }
            ++at;
        }
    }
    std::int64_t millis = 0;
    if (at < s.size()) {
        if (s[at] != '.') {
            return std::nullopt;
        }
        ++at;
        std::size_t digits = 0;
        std::int64_t fraction = 0;
        while (at < s.size() && is_digit(s[at])) {
            if (++digits > 3) {
                return std::nullopt;
            }
            fraction = (fraction * 10) + (s[at] - '0');
            ++at;
        }
        if (digits == 0 || at != s.size()) {
            return std::nullopt;
        }
        millis = digits == 1 ? fraction * 100 : (digits == 2 ? fraction * 10 : fraction);
    } else if (at != s.size()) {
        return std::nullopt;
    }
    if (parts[1] > 59 || parts[2] > 59) {
        return std::nullopt;
    }
    return (((parts[0] * 60) + parts[1]) * 60 + parts[2]) * 1000 + millis;
}

[[nodiscard]] bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// One syllable of an event's text: how long it lasts after the one before (hundredths), and what it
// says, with the spacing written after it.
struct AssSyllable {
    std::int64_t centis = 0;
    std::string text;
};

// An event's text split at its karaoke tags. `karaoke` is whether any was there; `drawing` is
// whether an override block turned drawing mode on, which makes the event not lyrics.
struct AssText {
    std::vector<AssSyllable> syllables;
    bool karaoke = false;
    bool drawing = false;
};

[[nodiscard]] AssText split_ass_text(std::string_view text) {
    AssText out;
    AssSyllable current;
    const auto flush = [&] {
        if (!current.text.empty() || current.centis > 0) {
            out.syllables.push_back(std::move(current));
        }
        current = AssSyllable{};
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close == std::string_view::npos) {
                current.text.push_back(text[i]);
                ++i;
                continue;
            }
            const std::string_view block = text.substr(i + 1, close - i - 1);
            // The tags of the block, each from its backslash to the next.
            std::size_t at = 0;
            while ((at = block.find('\\', at)) != std::string_view::npos) {
                ++at;
                std::size_t end = block.find('\\', at);
                if (end == std::string_view::npos) {
                    end = block.size();
                }
                const std::string_view tag = block.substr(at, end - at);
                // \k, \kf, \K, \ko and then the duration in hundredths of a second.
                std::size_t name = 0;
                if (!tag.empty() && (tag[0] == 'k' || tag[0] == 'K')) {
                    name = 1;
                    if (tag.size() > 1 && (tag[1] == 'f' || tag[1] == 'o')) {
                        name = 2;
                    }
                    std::size_t digits = 0;
                    std::int64_t value = 0;
                    while (name + digits < tag.size() && is_digit(tag[name + digits]) && digits < 6) {
                        value = (value * 10) + (tag[name + digits] - '0');
                        ++digits;
                    }
                    if (digits > 0) {
                        // A tag ends the syllable before it and begins the next.
                        flush();
                        current.centis = value;
                        out.karaoke = true;
                    }
                } else if (tag.size() >= 2 && tag[0] == 'p' && is_digit(tag[1]) && tag[1] != '0') {
                    out.drawing = true;
                }
            }
            i = close + 1;
            continue;
        }
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i + 1] == 'N' || text[i + 1] == 'n' || text[i + 1] == 'h')) {
            current.text.push_back(' ');
            i += 2;
            continue;
        }
        current.text.push_back(is_space(text[i]) ? ' ' : text[i]);
        ++i;
    }
    flush();
    return out;
}

[[nodiscard]] bool looks_like_ass(std::string_view text) {
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
            if (starts_with_nocase(trim(text.substr(begin, i - begin)), "Dialogue:")) {
                return true;
            }
            begin = i + 1;
        }
    }
    return false;
}

// Whether the decoded text has a SubRip cue line: two times either side of an arrow.
[[nodiscard]] bool looks_like_subrip(std::string_view text) {
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
            const std::string_view line = text.substr(begin, i - begin);
            if (line.find("-->") != std::string_view::npos && parse_cue_times(line)) {
                return true;
            }
            begin = i + 1;
        }
    }
    return false;
}

}  // namespace

std::expected<Lyrics, std::string> parse_lyrics(std::string_view text,
                                                std::optional<std::int64_t> duration_ms) {
    const auto decoded = decode_text(text);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    text = *decoded;

    Lyrics lyrics;
    std::vector<LineProto> timed;
    std::vector<std::string> plain;

    const auto read_line = [&](std::string_view raw) {
        const std::string_view line = trim(raw);
        if (line.empty()) {
            return;
        }
        std::vector<std::int64_t> times;
        std::size_t at = 0;
        while (at < line.size() && line[at] == '[') {
            const std::size_t close = line.find(']', at);
            if (close == std::string_view::npos) {
                break;
            }
            const auto time = parse_time(line.substr(at + 1, close - at - 1));
            if (!time) {
                break;
            }
            times.push_back(*time);
            at = close + 1;
        }
        if (times.empty()) {
            // [name:value] is an id tag; anything else is text without a time.
            if (line.front() == '[') {
                const std::size_t close = line.find(']');
                const std::string_view inner =
                    close == std::string_view::npos ? std::string_view{} : line.substr(1, close - 1);
                const std::size_t colon = inner.find(':');
                if (colon != std::string_view::npos && colon > 0 &&
                    std::all_of(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(colon),
                                [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; })) {
                    const std::string name = lowercase(inner.substr(0, colon));
                    const std::string value{trim(inner.substr(colon + 1))};
                    if (name == "ti") {
                        lyrics.title = value;
                    } else if (name == "ar") {
                        lyrics.artist = value;
                    } else if (name == "al") {
                        lyrics.album = value;
                    } else if (name == "by") {
                        lyrics.author = value;
                    } else if (name == "length") {
                        lyrics.length_ms = parse_time(value);
                    } else if (name == "offset") {
                        if (const auto offset = parse_offset(value)) {
                            lyrics.offset_ms = *offset;
                        }
                    }
                    return;
                }
            }
            plain.emplace_back(line);
            return;
        }
        const std::vector<Segment> segments = split_words(line.substr(at));
        LineProto shape;
        build_words(segments, times.front(), shape);
        for (const std::int64_t start : times) {
            LineProto copy = shape;
            copy.start = start;
            timed.push_back(std::move(copy));
        }
    };

    std::size_t begin = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size();
        if (end || text[i] == '\n' || text[i] == '\r') {
            read_line(text.substr(begin, i - begin));
            if (!end && text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            begin = i + 1;
        }
    }

    if (timed.empty()) {
        if (plain.empty()) {
            return std::unexpected(std::string{"The lyrics file has no lyrics in it."});
        }
        for (std::string& line : plain) {
            lyrics.lines.push_back({.start_ms = 0, .end_ms = 0, .text = std::move(line), .words = {}});
        }
        return lyrics;
    }

    // The file's own offset, then time order.
    const auto shifted = [&](std::int64_t value) {
        return std::max<std::int64_t>(0, value - lyrics.offset_ms);
    };
    for (LineProto& line : timed) {
        line.start = shifted(line.start);
    }
    std::stable_sort(timed.begin(), timed.end(),
                     [](const LineProto& a, const LineProto& b) { return a.start < b.start; });

    lyrics.timed = true;
    lyrics.lines.reserve(timed.size());
    for (std::size_t i = 0; i < timed.size(); ++i) {
        const LineProto& proto = timed[i];
        LyricLine line;
        line.start_ms = proto.start;
        line.text = proto.text;
        const bool has_explicit_end = proto.end_offset.has_value();
        std::int64_t explicit_end = 0;
        if (has_explicit_end) {
            explicit_end = proto.start + *proto.end_offset;
        }
        if (i + 1 < timed.size()) {
            line.end_ms = std::max(timed[i + 1].start, line.start_ms);
        } else {
            std::int64_t last = line.start_ms + kLastLineMs;
            if (duration_ms && *duration_ms > line.start_ms) {
                last = *duration_ms;
            } else if (lyrics.length_ms && *lyrics.length_ms > line.start_ms) {
                last = *lyrics.length_ms;
            }
            line.end_ms = std::max({last, explicit_end, line.start_ms});
        }
        // Words: each from its tag (shifted as its line is), never before the
        // line, never going back, and ending where the next begins.
        std::int64_t floor = line.start_ms;
        for (const WordProto& word : proto.words) {
            LyricWord out;
            out.start_ms = std::clamp(std::max(floor, line.start_ms + word.offset), line.start_ms,
                                      line.end_ms);
            out.text = word.text;
            floor = out.start_ms;
            line.words.push_back(std::move(out));
        }
        for (std::size_t w = 0; w < line.words.size(); ++w) {
            if (w + 1 < line.words.size()) {
                line.words[w].end_ms = line.words[w + 1].start_ms;
            } else {
                const std::int64_t end = has_explicit_end ? explicit_end : line.end_ms;
                // Never past the line: the last line's end already holds a
                // closing tag, and another line's is where the next starts.
                line.words[w].end_ms = std::clamp(end, line.words[w].start_ms, line.end_ms);
            }
        }
        lyrics.lines.push_back(std::move(line));
    }
    return lyrics;
}

std::expected<Lyrics, std::string> parse_subrip(std::string_view text,
                                                std::optional<std::int64_t> duration_ms) {
    const auto decoded = decode_text(text);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    struct Cue {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string text;
    };
    std::vector<Cue> cues;
    // The cue being collected: its time line was read and its text lines follow, to a blank line.
    std::optional<Cue> open;
    const auto close_cue = [&] {
        if (open) {
            open->text = clean_cue_text(open->text);
            cues.push_back(std::move(*open));
            open.reset();
        }
    };
    std::size_t begin = 0;
    const std::string_view all = *decoded;
    for (std::size_t i = 0; i <= all.size(); ++i) {
        const bool end = i == all.size();
        if (!end && all[i] != '\n' && all[i] != '\r') {
            continue;
        }
        const std::string_view line = trim(all.substr(begin, i - begin));
        begin = i + 1;
        // CR LF is one line end, not a line and an empty one (which would end every cue).
        if (!end && all[i] == '\r' && i + 1 < all.size() && all[i + 1] == '\n') {
            ++i;
            begin = i + 1;
        }
        if (line.empty()) {
            close_cue();
            continue;
        }
        if (const auto times = parse_cue_times(line)) {
            // A time line closes a cue that had no blank line after it (some tools write none).
            close_cue();
            open = Cue{.start = times->first, .end = times->second, .text = {}};
            continue;
        }
        if (open) {
            if (!open->text.empty()) {
                open->text.push_back(' ');
            }
            open->text.append(line);
        }
        // Anything else outside a cue - the number above one, a stray line - is nothing.
    }
    close_cue();
    if (cues.empty()) {
        return std::unexpected(std::string{"The subtitle file has no cues in it."});
    }
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });

    Lyrics lyrics;
    lyrics.timed = true;
    lyrics.lines.reserve(cues.size());
    for (Cue& cue : cues) {
        LyricLine line;
        line.start_ms = cue.start;
        line.end_ms = cue.end;
        line.text = std::move(cue.text);
        lyrics.lines.push_back(std::move(line));
    }
    // The last cue ends where it says; the item's length only lengthens a cue that ends at its own
    // start, which would otherwise never be "being sung".
    LyricLine& last = lyrics.lines.back();
    if (last.end_ms <= last.start_ms) {
        last.end_ms = (duration_ms && *duration_ms > last.start_ms) ? *duration_ms : last.start_ms + kLastLineMs;
    }
    return lyrics;
}

std::expected<Lyrics, std::string> parse_ass(std::string_view text,
                                             std::optional<std::int64_t> duration_ms) {
    const auto decoded = decode_text(text);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    // The column of Start, End and Text, from the events' Format line; the v4+ order without one.
    std::size_t start_column = 1;
    std::size_t end_column = 2;
    std::size_t text_column = 9;
    std::size_t columns = 10;
    bool in_events = false;
    struct Event {
        std::int64_t start = 0;
        std::int64_t end = 0;
        AssText text;
    };
    std::vector<Event> events;
    std::size_t begin = 0;
    const std::string_view all = *decoded;
    for (std::size_t i = 0; i <= all.size(); ++i) {
        const bool end = i == all.size();
        if (!end && all[i] != '\n' && all[i] != '\r') {
            continue;
        }
        const std::string_view line = trim(all.substr(begin, i - begin));
        begin = i + 1;
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            in_events = starts_with_nocase(line, "[events]");
            continue;
        }
        if (!in_events) {
            continue;
        }
        if (starts_with_nocase(line, "Format:")) {
            std::size_t found = 0;
            std::size_t column = 0;
            std::string_view rest = line.substr(7);
            std::size_t from = 0;
            while (from <= rest.size()) {
                std::size_t comma = rest.find(',', from);
                if (comma == std::string_view::npos) {
                    comma = rest.size();
                }
                const std::string name = lowercase(trim(rest.substr(from, comma - from)));
                if (name == "start") {
                    start_column = column;
                    ++found;
                } else if (name == "end") {
                    end_column = column;
                    ++found;
                } else if (name == "text") {
                    text_column = column;
                    ++found;
                }
                ++column;
                from = comma + 1;
            }
            columns = column;
            static_cast<void>(found);
            continue;
        }
        if (!starts_with_nocase(line, "Dialogue:")) {
            continue;
        }
        // The fields to the Text column; the text is the rest of the line, commas and all.
        std::string_view rest = line.substr(9);
        std::vector<std::string_view> fields;
        std::size_t from = 0;
        while (fields.size() + 1 < columns) {
            const std::size_t comma = rest.find(',', from);
            if (comma == std::string_view::npos) {
                break;
            }
            fields.push_back(rest.substr(from, comma - from));
            from = comma + 1;
        }
        fields.push_back(rest.substr(from));
        if (text_column >= fields.size() || start_column >= fields.size() || end_column >= fields.size()) {
            continue;
        }
        const auto start = parse_ass_time(fields[start_column]);
        const auto stop = parse_ass_time(fields[end_column]);
        if (!start || !stop) {
            continue;
        }
        AssText parsed = split_ass_text(fields[text_column]);
        if (parsed.drawing) {
            continue;
        }
        events.push_back(Event{.start = *start, .end = std::max(*stop, *start), .text = std::move(parsed)});
    }
    if (events.empty()) {
        return std::unexpected(std::string{"The subtitle file has no dialogue in it."});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.start < b.start; });

    Lyrics lyrics;
    lyrics.timed = true;
    lyrics.lines.reserve(events.size());
    for (Event& event : events) {
        LyricLine line;
        line.start_ms = event.start;
        line.end_ms = event.end;
        std::int64_t at = event.start;
        std::string joined;
        for (AssSyllable& syllable : event.text.syllables) {
            if (!syllable.text.empty()) {
                LyricWord word;
                word.start_ms = std::min(at, event.end);
                word.text = syllable.text;
                // The word is as long as its tag says; a word with no length runs to the next.
                word.end_ms = std::min(at + (syllable.centis * 10), event.end);
                if (event.text.karaoke) {
                    line.words.push_back(std::move(word));
                }
                joined += syllable.text;
            }
            at += syllable.centis * 10;
        }
        // A line shows without the spacing at its ends, and so does its first and last word.
        std::string_view shown = trim(joined);
        line.text = std::string{shown};
        if (!line.words.empty()) {
            std::string& first = line.words.front().text;
            std::size_t lead = 0;
            while (lead < first.size() && is_space(first[lead])) {
                ++lead;
            }
            first.erase(0, lead);
            std::string& last = line.words.back().text;
            while (!last.empty() && is_space(last.back())) {
                last.pop_back();
            }
            // Words that came to nothing once trimmed are not words.
            std::erase_if(line.words, [](const LyricWord& word) { return word.text.empty(); });
        }
        // A word of no length ends where the next begins, which is where it began; the last word of
        // no length runs to the line's end, so that the fill has somewhere to go.
        if (!line.words.empty() && line.words.back().end_ms <= line.words.back().start_ms) {
            line.words.back().end_ms = std::max(line.words.back().start_ms, line.end_ms);
        }
        lyrics.lines.push_back(std::move(line));
    }
    static_cast<void>(duration_ms);
    return lyrics;
}

std::expected<Lyrics, std::string> parse_midi_karaoke(std::string_view bytes,
                                                      std::optional<std::int64_t> duration_ms) {
    const std::string damaged = "The MIDI file is damaged.";
    if (!bytes.starts_with("MThd")) {
        return std::unexpected(std::string{"The file is not a MIDI file."});
    }
    MidiReader reader{bytes};
    reader.at = 4;
    const std::uint32_t header_length = reader.u32();
    reader.u16();  // the format, 0, 1 or 2: the tracks are read the same way
    const std::uint32_t track_count = reader.u16();
    const std::uint32_t division = reader.u16();
    if (!reader.ok || header_length < 6) {
        return std::unexpected(damaged);
    }
    if ((division & 0x8000U) != 0) {
        return std::unexpected(std::string{"The MIDI file counts time in SMPTE frames, which is not read."});
    }
    if (division == 0) {
        return std::unexpected(damaged);
    }
    reader.at = 8 + header_length;

    std::vector<MidiTempo> tempos;
    std::vector<MidiText> texts;
    for (std::uint32_t track = 0; track < track_count && reader.has(8); ++track) {
        const std::string_view id = reader.take(4);
        const std::uint32_t length = reader.u32();
        if (!reader.ok) {
            return std::unexpected(damaged);
        }
        const std::size_t end = std::min<std::size_t>(reader.at + length, bytes.size());
        if (id != "MTrk") {
            reader.at = end;
            continue;
        }
        const std::size_t first_text = texts.size();
        std::string name;
        std::uint64_t tick = 0;
        std::uint32_t running = 0;
        while (reader.at < end && reader.ok) {
            tick += reader.vlq();
            if (!reader.ok || reader.at >= end) {
                break;
            }
            std::uint32_t status = static_cast<unsigned char>(bytes[reader.at]);
            if ((status & 0x80U) != 0) {
                ++reader.at;
            } else {
                // Running status: the data bytes follow the status of the event before.
                if (running == 0) {
                    return std::unexpected(damaged);
                }
                status = running;
            }
            if (status == 0xFF) {
                const std::uint32_t type = reader.u8();
                const std::uint32_t meta_length = reader.vlq();
                const std::string_view payload = reader.take(meta_length);
                if (!reader.ok) {
                    return std::unexpected(damaged);
                }
                running = 0;
                if (type == 0x2F) {
                    break;
                }
                if (type == 0x51 && payload.size() == 3) {
                    const auto byte = [&](std::size_t i) {
                        return static_cast<std::uint32_t>(static_cast<unsigned char>(payload[i]));
                    };
                    const std::uint32_t micros = (byte(0) << 16U) | (byte(1) << 8U) | byte(2);
                    if (micros > 0) {
                        tempos.push_back({.tick = tick, .micros_per_quarter = micros});
                    }
                } else if (type == 0x03) {
                    name = std::string{payload};
                } else if (type == 0x01 || type == 0x05) {
                    texts.push_back({.tick = tick, .type = static_cast<int>(type), .text = std::string{payload}});
                }
            } else if (status == 0xF0 || status == 0xF7) {
                const std::uint32_t sysex_length = reader.vlq();
                reader.take(sysex_length);
                running = 0;
            } else {
                // A channel message: one data byte for a program change or a channel pressure, two
                // for the rest. (The first data byte of a running-status event is already at hand.)
                running = status;
                const std::size_t data_bytes = ((status & 0xF0U) == 0xC0U || (status & 0xF0U) == 0xD0U) ? 1 : 2;
                reader.take(data_bytes);
            }
            if (!reader.ok) {
                return std::unexpected(damaged);
            }
        }
        // The track's name is known only at its end: the words of a track called "Words" are marked.
        if (equals_nocase(trim(name), "words")) {
            for (std::size_t i = first_text; i < texts.size(); ++i) {
                texts[i].on_words_track = true;
            }
        }
        reader.at = end;
    }

    // Which events are the words: the Words track's, else the Lyric events, else the Text events.
    const auto any_of_type = [&](int type) {
        return std::any_of(texts.begin(), texts.end(), [type](const MidiText& t) { return t.type == type; });
    };
    const bool words_track = std::any_of(texts.begin(), texts.end(), [](const MidiText& t) { return t.on_words_track; });
    const int wanted_type = any_of_type(5) ? 5 : 1;
    std::vector<MidiText> chosen;
    for (const MidiText& event : texts) {
        if (words_track ? event.on_words_track : event.type == wanted_type) {
            chosen.push_back(event);
        }
    }
    std::stable_sort(chosen.begin(), chosen.end(),
                     [](const MidiText& a, const MidiText& b) { return a.tick < b.tick; });
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const MidiTempo& a, const MidiTempo& b) { return a.tick < b.tick; });

    // Milliseconds from a tick: the time of each stretch at its own tempo.
    const auto to_ms = [&](std::uint64_t tick) -> std::int64_t {
        std::uint64_t micros = 0;
        std::uint64_t previous = 0;
        std::uint64_t tempo = 500000;
        for (const MidiTempo& change : tempos) {
            if (change.tick >= tick) {
                break;
            }
            micros += (change.tick - previous) * tempo / division;
            previous = change.tick;
            tempo = change.micros_per_quarter;
        }
        micros += (tick - previous) * tempo / division;
        return static_cast<std::int64_t>((micros + 500) / 1000);
    };

    Lyrics lyrics;
    std::vector<std::vector<MidiSyllable>> lines(1);
    int titles = 0;
    for (const MidiText& event : chosen) {
        if (!valid_utf8(event.text)) {
            return std::unexpected(std::string{"The words of the MIDI file are not UTF-8: they are probably in a "
                                               "code page, which is not read. Save them as UTF-8."});
        }
        std::string_view text = event.text;
        if (!text.empty() && text.front() == '@') {
            if (text.size() > 2 && (text[1] == 'T' || text[1] == 't')) {
                const std::string value{trim(text.substr(2))};
                if (titles == 0) {
                    lyrics.title = value;
                } else if (titles == 1) {
                    lyrics.artist = value;
                }
                ++titles;
            }
            continue;
        }
        // A `/` or a backslash begins a line wherever it is; so does a line end in the text.
        std::string piece;
        const auto flush = [&] {
            if (!piece.empty()) {
                lines.back().push_back({.tick = event.tick, .text = std::move(piece)});
                piece.clear();
            }
        };
        // (The characters are named, not written as escapes: this source is built by a script.)
        constexpr char kBackslash = static_cast<char>(92);
        constexpr char kCarriageReturn = static_cast<char>(13);
        constexpr char kLineFeed = static_cast<char>(10);
        for (const char c : text) {
            if (c == '/' || c == kBackslash || c == kCarriageReturn || c == kLineFeed) {
                flush();
                if (!lines.back().empty()) {
                    lines.emplace_back();
                }
            } else {
                piece.push_back(c);
            }
        }
        flush();
    }

    // A syllable that begins with a space is the start of a new word: the space goes to the end of the
    // syllable before, where a view expects it, and a line shows without the spacing at its ends.
    for (auto& line : lines) {
        for (std::size_t i = 0; i < line.size(); ++i) {
            std::size_t lead = 0;
            while (lead < line[i].text.size() && is_space(line[i].text[lead])) {
                ++lead;
            }
            if (lead > 0) {
                line[i].text.erase(0, lead);
                if (i > 0 && !line[i - 1].text.empty() && !is_space(line[i - 1].text.back())) {
                    line[i - 1].text.push_back(' ');
                }
            }
        }
        std::erase_if(line, [](const MidiSyllable& s) { return s.text.empty(); });
        if (!line.empty()) {
            while (!line.back().text.empty() && is_space(line.back().text.back())) {
                line.back().text.pop_back();
            }
        }
    }
    std::erase_if(lines, [](const std::vector<MidiSyllable>& line) { return line.empty(); });
    if (lines.empty()) {
        return std::unexpected(std::string{"The MIDI file has no words in it."});
    }

    lyrics.timed = true;
    lyrics.lines.reserve(lines.size());
    for (std::size_t l = 0; l < lines.size(); ++l) {
        LyricLine line;
        line.start_ms = to_ms(lines[l].front().tick);
        if (l + 1 < lines.size()) {
            line.end_ms = std::max(to_ms(lines[l + 1].front().tick), line.start_ms);
        } else {
            const std::int64_t last_start = to_ms(lines[l].back().tick);
            std::int64_t last = last_start + kLastLineMs;
            if (duration_ms && *duration_ms > last_start) {
                last = *duration_ms;
            }
            line.end_ms = std::max(last, line.start_ms);
        }
        for (const MidiSyllable& syllable : lines[l]) {
            LyricWord word;
            word.start_ms = std::clamp(to_ms(syllable.tick), line.start_ms, line.end_ms);
            word.text = syllable.text;
            line.words.push_back(std::move(word));
        }
        for (std::size_t w = 0; w < line.words.size(); ++w) {
            line.words[w].end_ms = w + 1 < line.words.size() ? std::max(line.words[w + 1].start_ms, line.words[w].start_ms)
                                                             : line.end_ms;
            line.text += line.words[w].text;
        }
        lyrics.lines.push_back(std::move(line));
    }
    return lyrics;
}

std::expected<Lyrics, std::string> parse_timed_text(std::string_view text,
                                                    std::optional<std::int64_t> duration_ms) {
    // A MIDI file is bytes, not text: it is known by its header before anything decodes it.
    if (text.starts_with("MThd")) {
        return parse_midi_karaoke(text, duration_ms);
    }
    const auto decoded = decode_text(text);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (looks_like_ass(*decoded)) {
        return parse_ass(*decoded, duration_ms);
    }
    if (looks_like_subrip(*decoded)) {
        return parse_subrip(*decoded, duration_ms);
    }
    return parse_lyrics(*decoded, duration_ms);
}

std::expected<Lyrics, std::string> read_lyrics(const std::filesystem::path& file,
                                               std::optional<std::int64_t> duration_ms) {
    std::error_code sized;
    const std::uintmax_t size = std::filesystem::file_size(file, sized);
    if (sized) {
        return std::unexpected(std::string{"The lyrics file could not be read: "} + sized.message());
    }
    if (size > kMaxFileBytes) {
        return std::unexpected(std::string{"The lyrics file is too large to be lyrics."});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::string{"The lyrics file could not be opened."});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty() && !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(std::string{"The lyrics file could not be read."});
    }
    return parse_timed_text(bytes, duration_ms);
}

std::optional<std::filesystem::path> find_lyrics_file(const std::string& item_path) {
    const std::filesystem::path media{item_path};
    const std::filesystem::path folder = media.parent_path();
    const std::string stem = media.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    // `.lrc` first and `.srt` where there is none: the usual spelling of each, which is also
    // the only one on a case-insensitive file system, then any case.
    for (const std::string_view extension :
         {std::string_view{".lrc"}, std::string_view{".srt"}, std::string_view{".ass"},
          std::string_view{".kar"}}) {
        const std::filesystem::path usual = folder / (stem + std::string{extension});
        if (std::filesystem::is_regular_file(usual, error) && !error) {
            return usual;
        }
        error.clear();
        std::filesystem::directory_iterator it(folder.empty() ? std::filesystem::path{"."} : folder,
                                               std::filesystem::directory_options::skip_permission_denied,
                                               error);
        const std::filesystem::directory_iterator end;
        while (!error && it != end) {
            std::error_code entry_error;
            if (it->is_regular_file(entry_error) && !entry_error &&
                it->path().stem().string() == stem &&
                lowercase(it->path().extension().string()) == extension) {
                return it->path();
            }
            it.increment(error);
        }
        error.clear();
    }
    return std::nullopt;
}

LyricPosition lyric_position(const Lyrics& lyrics, std::int64_t position_ms,
                             std::int64_t delay_ms) {
    LyricPosition out;
    out.position_ms = position_ms - delay_ms;
    if (!lyrics.timed || lyrics.lines.empty()) {
        return out;
    }
    const std::int64_t at = out.position_ms;
    // The last line whose start has been reached.
    const auto after = std::upper_bound(
        lyrics.lines.begin(), lyrics.lines.end(), at,
        [](std::int64_t value, const LyricLine& line) { return value < line.start_ms; });
    if (after == lyrics.lines.begin()) {
        return out;
    }
    const auto index = static_cast<std::size_t>(std::distance(lyrics.lines.begin(), after)) - 1;
    const LyricLine& line = lyrics.lines[index];
    out.line = index;
    out.in_line = at < line.end_ms;
    if (!line.word_timed()) {
        return out;
    }
    const auto past = std::upper_bound(
        line.words.begin(), line.words.end(), at,
        [](std::int64_t value, const LyricWord& word) { return value < word.start_ms; });
    if (past == line.words.begin()) {
        return out;
    }
    const auto word_index = static_cast<std::size_t>(std::distance(line.words.begin(), past)) - 1;
    const LyricWord& word = line.words[word_index];
    if (at < word.end_ms) {
        out.word = word_index;
        out.fraction = static_cast<double>(at - word.start_ms) /
                       static_cast<double>(word.end_ms - word.start_ms);
    }
    return out;
}

WordState word_state(const LyricWord& word, std::int64_t position_ms) {
    if (position_ms >= word.end_ms) {
        return WordState::kSung;
    }
    return position_ms >= word.start_ms ? WordState::kBeingSung : WordState::kNotYetSung;
}

}  // namespace iclforge::lyrics
