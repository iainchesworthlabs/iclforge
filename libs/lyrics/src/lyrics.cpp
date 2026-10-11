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
    Singer singer = Singer::kUnmarked;
    std::string text{};
    std::vector<WordProto> words{};
    // Where the last word ends, from the line's start, when a closing tag said.
    std::optional<std::int64_t> end_offset{};
};

struct Segment {
    std::optional<std::int64_t> tag{};
    std::string text{};
};

// A singer marker at the start of `text`: `V1:`, `V2:`, `V1+V2:` or `V2+V1:`, in either case,
// with spaces round the `+` and after the colon. It may come after a first word tag
// (`<00:12.00>V1: Leave`), which is kept in front of what is left. Nothing is taken from a text
// that does not start with one.
struct Marked {
    Singer singer = Singer::kUnmarked;
    std::string rest{};
};

[[nodiscard]] Marked take_singer(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size() && is_space(text[at])) {
        ++at;
    }
    std::string_view tag;
    if (at < text.size() && text[at] == '<') {
        const std::size_t close = text.find('>', at);
        if (close != std::string_view::npos && parse_time(text.substr(at + 1, close - at - 1))) {
            tag = text.substr(at, close - at + 1);
            at = close + 1;
            while (at < text.size() && is_space(text[at])) {
                ++at;
            }
        }
    }
    const auto upper = [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    };
    const auto digit_after_v = [&](std::size_t i) -> int {
        if (i + 1 < text.size() && upper(text[i]) == 'V' && (text[i + 1] == '1' || text[i + 1] == '2')) {
            return text[i + 1] - '0';
        }
        return 0;
    };
    const int first = digit_after_v(at);
    if (first == 0) {
        return {.singer = Singer::kUnmarked, .rest = std::string{text}};
    }
    std::size_t i = at + 2;
    Singer singer = first == 1 ? Singer::kFirst : Singer::kSecond;
    // `+Vn` with the other digit, spaces allowed round the plus.
    std::size_t j = i;
    while (j < text.size() && is_space(text[j])) {
        ++j;
    }
    if (j < text.size() && text[j] == '+') {
        ++j;
        while (j < text.size() && is_space(text[j])) {
            ++j;
        }
        const int second = digit_after_v(j);
        if (second == 0 || second == first) {
            return {.singer = Singer::kUnmarked, .rest = std::string{text}};
        }
        singer = Singer::kBoth;
        i = j + 2;
    }
    while (i < text.size() && is_space(text[i])) {
        ++i;
    }
    if (i >= text.size() || text[i] != ':') {
        return {.singer = Singer::kUnmarked, .rest = std::string{text}};
    }
    ++i;
    while (i < text.size() && is_space(text[i])) {
        ++i;
    }
    std::string rest{tag};
    rest += text.substr(i);
    return {.singer = singer, .rest = std::move(rest)};
}

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

}  // namespace

std::expected<Lyrics, std::string> parse_lyrics(std::string_view text,
                                                std::optional<std::int64_t> duration_ms) {
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

    Lyrics lyrics;
    std::vector<LineProto> timed;
    std::vector<std::string> plain;
    std::vector<Singer> plain_singers;

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
                // A letter, then letters or digits: `ti`, `ar`, and `v1` and `v2`.
                if (colon != std::string_view::npos && colon > 0 &&
                    std::isalpha(static_cast<unsigned char>(inner[0])) != 0 &&
                    std::all_of(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(colon),
                                [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; })) {
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
                    } else if (name == "v1") {
                        lyrics.first_singer_name = value;
                    } else if (name == "v2") {
                        lyrics.second_singer_name = value;
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
            // A plain line may carry the marker too, so that a duet's text can be marked
            // before it is timed.
            Marked marked = take_singer(line);
            plain.emplace_back(std::move(marked.rest));
            plain_singers.push_back(marked.singer);
            return;
        }
        const Marked marked = take_singer(line.substr(at));
        const std::vector<Segment> segments = split_words(marked.rest);
        LineProto shape;
        shape.singer = marked.singer;
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
        for (std::size_t i = 0; i < plain.size(); ++i) {
            lyrics.lines.push_back({.start_ms = 0,
                                    .end_ms = 0,
                                    .text = std::move(plain[i]),
                                    .words = {},
                                    .singer = plain_singers[i]});
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
        line.singer = proto.singer;
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
    return parse_lyrics(bytes, duration_ms);
}

std::optional<std::filesystem::path> find_lyrics_file(const std::string& item_path) {
    const std::filesystem::path media{item_path};
    const std::filesystem::path folder = media.parent_path();
    const std::string stem = media.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    // The usual spelling first, which is also the only one on a case-
    // insensitive file system.
    const std::filesystem::path usual = folder / (stem + ".lrc");
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
            lowercase(it->path().extension().string()) == ".lrc") {
            return it->path();
        }
        it.increment(error);
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

bool has_singers(const Lyrics& lyrics) {
    return std::any_of(lyrics.lines.begin(), lyrics.lines.end(),
                       [](const LyricLine& line) { return line.singer != Singer::kUnmarked; });
}

std::string_view singer_marker(Singer singer) {
    switch (singer) {
        case Singer::kFirst: return "V1:";
        case Singer::kSecond: return "V2:";
        case Singer::kBoth: return "V1+V2:";
        case Singer::kUnmarked: break;
    }
    return {};
}

WordState word_state(const LyricWord& word, std::int64_t position_ms) {
    if (position_ms >= word.end_ms) {
        return WordState::kSung;
    }
    return position_ms >= word.start_ms ? WordState::kBeingSung : WordState::kNotYetSung;
}

}  // namespace iclforge::lyrics
