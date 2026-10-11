#include "iclforge/lyrics/timing.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace iclforge::lyrics {

namespace {

// As the reader's own: how long the last line lasts when nothing says.
constexpr std::int64_t kLastLineMs = 5000;

// The nearest hundredth of a second, never negative: the file's own resolution, so that what is
// held is what is written and a file read back is the one that was edited.
[[nodiscard]] std::int64_t to_hundredth(std::int64_t ms) {
    return (std::max<std::int64_t>(ms, 0) + 5) / 10 * 10;
}

// "mm:ss.xx", to the nearest hundredth, never negative.
[[nodiscard]] std::string stamp(std::int64_t ms) {
    const std::int64_t clamped = std::max<std::int64_t>(ms, 0);
    const std::int64_t centis = (clamped + 5) / 10;
    char text[48];
    std::snprintf(text, sizeof text, "%02lld:%02lld.%02lld", static_cast<long long>(centis / 6000),
                  static_cast<long long>((centis / 100) % 60), static_cast<long long>(centis % 100));
    return text;
}

[[nodiscard]] bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] bool is_ascii_punctuation(unsigned char c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

// A code point that is not a letter of the word: the combining diacritics, the Hebrew points and
// cantillation (U+0591 to U+05BD, U+05BF, U+05C1, U+05C2, U+05C4, U+05C5, U+05C7), the maqaf,
// paseq and sof pasuq (U+05BE, U+05C0, U+05C3) and the Hebrew punctuation marks geresh and
// gershayim (U+05F3, U+05F4). Yiddish uses the same points, so a vocalised word weighs as it would
// without them.
[[nodiscard]] bool is_not_a_letter(std::uint32_t code) {
    return (code >= 0x0300 && code <= 0x036F) || (code >= 0x0591 && code <= 0x05C7) ||
           code == 0x05F3 || code == 0x05F4;
}

// The letters of a UTF-8 string: its code points that are not ASCII spaces or punctuation or one of
// the marks above. A byte that is not a well-formed lead counts as one letter, so that nothing the
// reader let through can weigh nothing.
[[nodiscard]] std::size_t letters_of(std::string_view s) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) {
            if (!is_ascii_space(s[i]) && !is_ascii_punctuation(lead)) {
                ++count;
            }
            ++i;
            continue;
        }
        std::size_t length = 1;
        std::uint32_t code = 0;
        if (lead >= 0xF0) {
            length = 4;
            code = lead & 0x07U;
        } else if (lead >= 0xE0) {
            length = 3;
            code = lead & 0x0FU;
        } else if (lead >= 0xC0) {
            length = 2;
            code = lead & 0x1FU;
        }
        if (length == 1 || i + length > s.size()) {
            ++count;
            ++i;
            continue;
        }
        for (std::size_t k = 1; k < length; ++k) {
            code = (code << 6U) | (static_cast<unsigned char>(s[i + k]) & 0x3FU);
        }
        if (!is_not_a_letter(code)) {
            ++count;
        }
        i += length;
    }
    return count;
}

// A line's end for estimating from, before the ends are settled: its own when it is after its start,
// else a gap on.
[[nodiscard]] std::int64_t usable_end(const LyricLine& line) {
    return line.end_ms > line.start_ms ? line.end_ms : line.start_ms + kLastLineMs;
}

}  // namespace

std::string write_lrc(const Lyrics& lyrics) {
    std::string out;
    const auto tag = [&out](const char* name, const std::string& value) {
        if (!value.empty()) {
            out += '[';
            out += name;
            out += ':';
            out += value;
            out += "]\n";
        }
    };
    tag("ti", lyrics.title);
    tag("ar", lyrics.artist);
    tag("al", lyrics.album);
    tag("by", lyrics.author);
    if (lyrics.length_ms) {
        const std::int64_t seconds = std::max<std::int64_t>(*lyrics.length_ms, 0) / 1000;
        char text[40];
        std::snprintf(text, sizeof text, "%02lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
        tag("length", text);
    }
    for (const LyricLine& line : lyrics.lines) {
        if (lyrics.timed) {
            out += '[';
            out += stamp(line.start_ms);
            out += ']';
            if (line.word_timed()) {
                for (const LyricWord& word : line.words) {
                    out += '<';
                    out += stamp(word.start_ms);
                    out += '>';
                    out += word.text;
                }
            } else {
                out += line.text;
            }
        } else {
            out += line.text;
        }
        out += '\n';
    }
    return out;
}

std::vector<LyricWord> estimate_words(const LyricLine& line) {
    std::vector<LyricWord> words;
    if (line.text.empty()) {
        return words;
    }
    // Each word keeps the spaces after it; any before the first word go with it.
    const std::string_view text = line.text;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = at;
        while (end < text.size() && is_ascii_space(text[end])) {
            ++end;
        }
        while (end < text.size() && !is_ascii_space(text[end])) {
            ++end;
        }
        while (end < text.size() && is_ascii_space(text[end])) {
            ++end;
        }
        words.push_back(LyricWord{.text = std::string(text.substr(at, end - at))});
        at = end;
    }

    std::vector<std::size_t> weight;
    weight.reserve(words.size());
    std::size_t total = 0;
    for (const LyricWord& word : words) {
        weight.push_back(std::max<std::size_t>(letters_of(word.text), 1));
        total += weight.back();
    }
    const std::int64_t start = line.start_ms;
    const std::int64_t span = std::max<std::int64_t>(usable_end(line) - start, 0);
    std::size_t before = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto offset = static_cast<std::int64_t>(
            (static_cast<long double>(span) * static_cast<long double>(before)) /
                static_cast<long double>(total) + 0.5L);
        words[i].start_ms = to_hundredth(start + offset);
        before += weight[i];
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i].end_ms = i + 1 < words.size() ? words[i + 1].start_ms : start + span;
    }
    return words;
}

bool set_line_start(Lyrics& lyrics, std::size_t line, std::int64_t start_ms) {
    if (line >= lyrics.lines.size()) {
        return false;
    }
    std::int64_t start = to_hundredth(start_ms);
    if (line > 0) {
        start = std::max(start, lyrics.lines[line - 1].start_ms);
    }
    LyricLine& target = lyrics.lines[line];
    const std::int64_t delta = start - target.start_ms;
    target.start_ms = start;
    for (LyricWord& word : target.words) {
        word.start_ms = std::max(word.start_ms + delta, start);
        word.end_ms = std::max(word.end_ms + delta, word.start_ms);
    }
    // The lines after it stay in order: only those that would now be earlier move, and to the
    // line before them, which keeps a later line later and an equal one equal.
    for (std::size_t i = line + 1; i < lyrics.lines.size(); ++i) {
        LyricLine& next = lyrics.lines[i];
        const std::int64_t floor = lyrics.lines[i - 1].start_ms;
        if (next.start_ms >= floor) {
            break;
        }
        const std::int64_t by = floor - next.start_ms;
        next.start_ms = floor;
        for (LyricWord& word : next.words) {
            word.start_ms += by;
            word.end_ms += by;
        }
    }
    lyrics.timed = true;
    // This line's end and the one before's, for what a caller reads before it settles them all.
    if (line > 0) {
        lyrics.lines[line - 1].end_ms = start;
    }
    if (line + 1 < lyrics.lines.size()) {
        target.end_ms = lyrics.lines[line + 1].start_ms;
    } else {
        target.end_ms = std::max(target.end_ms, start);
    }
    return true;
}

bool set_word_start(Lyrics& lyrics, std::size_t line, std::size_t word, std::int64_t start_ms) {
    if (line >= lyrics.lines.size()) {
        return false;
    }
    LyricLine& target = lyrics.lines[line];
    if (word >= target.words.size()) {
        return false;
    }
    const std::int64_t low = word > 0 ? target.words[word - 1].start_ms : target.start_ms;
    const std::int64_t line_end = std::max(usable_end(target), low);
    const std::int64_t high = word + 1 < target.words.size() ? target.words[word + 1].start_ms : line_end;
    const std::int64_t start = std::clamp(to_hundredth(start_ms), low, std::max(high, low));
    target.words[word].start_ms = start;
    if (word > 0) {
        target.words[word - 1].end_ms = start;
    }
    target.words[word].end_ms =
        word + 1 < target.words.size() ? target.words[word + 1].start_ms : line_end;
    return true;
}

bool estimate_line(Lyrics& lyrics, std::size_t line) {
    if (line >= lyrics.lines.size()) {
        return false;
    }
    LyricLine& target = lyrics.lines[line];
    if (target.text.empty() || !target.words.empty()) {
        return false;
    }
    target.words = estimate_words(target);
    return !target.words.empty();
}

void settle_ends(Lyrics& lyrics, std::optional<std::int64_t> duration_ms) {
    for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
        LyricLine& line = lyrics.lines[i];
        if (i + 1 < lyrics.lines.size()) {
            line.end_ms = std::max(lyrics.lines[i + 1].start_ms, line.start_ms);
        } else if (duration_ms && *duration_ms > line.start_ms) {
            line.end_ms = *duration_ms;
        } else if (lyrics.length_ms && *lyrics.length_ms > line.start_ms) {
            line.end_ms = *lyrics.length_ms;
        } else {
            line.end_ms = line.start_ms + kLastLineMs;
        }
        for (std::size_t w = 0; w < line.words.size(); ++w) {
            LyricWord& word = line.words[w];
            word.start_ms = std::clamp(word.start_ms, line.start_ms, line.end_ms);
            if (w > 0) {
                word.start_ms = std::max(word.start_ms, line.words[w - 1].start_ms);
            }
        }
        for (std::size_t w = 0; w < line.words.size(); ++w) {
            line.words[w].end_ms =
                w + 1 < line.words.size() ? line.words[w + 1].start_ms : line.end_ms;
        }
    }
}

}  // namespace iclforge::lyrics
