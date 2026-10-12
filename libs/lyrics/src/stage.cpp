#include "iclforge/lyrics/stage.hpp"

#include <algorithm>

namespace iclforge::lyrics {

namespace {

[[nodiscard]] bool has_text(const LyricLine& line) { return !line.text.empty(); }

// The lines of text among lines [0, limit), the last `want` of them, oldest first.
void collect_before(Stage& stage, const Lyrics& lyrics, std::size_t limit, std::size_t want) {
    const std::size_t take = std::min(want, kStageMaxContext);
    std::array<std::size_t, kStageMaxContext> newest_first{};
    std::size_t found = 0;
    for (std::size_t i = std::min(limit, lyrics.lines.size()); i > 0 && found < take; --i) {
        if (has_text(lyrics.lines[i - 1])) {
            newest_first[found++] = i - 1;
        }
    }
    stage.before_count = found;
    for (std::size_t k = 0; k < found; ++k) {
        stage.before[k] = newest_first[found - 1 - k];
    }
}

// The first `want` lines of text from line `from` on, soonest first.
void collect_after(Stage& stage, const Lyrics& lyrics, std::size_t from, std::size_t want) {
    const std::size_t take = std::min(want, kStageMaxContext);
    std::size_t found = 0;
    for (std::size_t i = from; i < lyrics.lines.size() && found < take; ++i) {
        if (has_text(lyrics.lines[i])) {
            stage.after[found++] = i;
        }
    }
    stage.after_count = found;
}

// The first line of text at or after `from`, or lines.size() when there is none.
[[nodiscard]] std::size_t next_text_line(const Lyrics& lyrics, std::size_t from) {
    while (from < lyrics.lines.size() && !has_text(lyrics.lines[from])) {
        ++from;
    }
    return from;
}

}  // namespace

Stage make_stage(const Lyrics* lyrics, std::optional<std::int64_t> position_ms, std::int64_t delay_ms,
                 const StageOptions& options) {
    Stage stage;
    if (lyrics == nullptr) {
        return stage;
    }
    if (!lyrics->timed) {
        stage.mode = StageMode::kPlain;
        return stage;
    }
    if (!position_ms) {
        stage.mode = StageMode::kWaiting;
        collect_after(stage, *lyrics, 0, options.lines_after);
        return stage;
    }

    const LyricPosition at = lyric_position(*lyrics, *position_ms, delay_ms);
    stage.position_ms = at.position_ms;

    if (at.line && at.in_line && has_text(lyrics->lines[*at.line])) {
        const LyricLine& line = lyrics->lines[*at.line];
        stage.mode = StageMode::kLine;
        stage.line = *at.line;
        stage.singer = line.singer;
        stage.word_timed = line.word_timed();
        if (stage.word_timed) {
            stage.words_sung = static_cast<std::size_t>(std::count_if(
                line.words.begin(), line.words.end(), [&](const LyricWord& word) {
                    return word_state(word, at.position_ms) == WordState::kSung;
                }));
            stage.word = at.word;
            stage.fraction = at.word ? at.fraction : 0.0;
        }
        collect_before(stage, *lyrics, *at.line, options.lines_before);
        collect_after(stage, *lyrics, *at.line + 1, options.lines_after);
        return stage;
    }

    // Nothing is being sung. The line most recently started (if any) has ended, or only marks a time.
    stage.mode = StageMode::kGap;
    const std::size_t next_from = at.line ? *at.line + 1 : 0;
    collect_before(stage, *lyrics, at.line ? *at.line + 1 : 0, options.lines_before);
    collect_after(stage, *lyrics, next_from, options.lines_after);
    if (at.line) {
        stage.line = *at.line;
    }

    std::int64_t gap_from = 0;
    if (at.line) {
        const LyricLine& last = lyrics->lines[*at.line];
        gap_from = has_text(last) ? last.end_ms : last.start_ms;
    }
    const std::size_t next = next_text_line(*lyrics, next_from);
    if (next < lyrics->lines.size()) {
        const std::int64_t start = lyrics->lines[next].start_ms;
        stage.next_line_in_ms = std::max<std::int64_t>(0, start - at.position_ms);
        stage.gap_ms = std::max<std::int64_t>(0, start - gap_from);
        stage.instrumental = *stage.gap_ms >= options.instrumental_min_ms;
        if (stage.instrumental && options.count_in_dots > 0) {
            const std::int64_t seconds = (*stage.next_line_in_ms + 999) / 1000;
            stage.count_in = static_cast<std::size_t>(
                std::min<std::int64_t>(seconds, static_cast<std::int64_t>(options.count_in_dots)));
        }
    }
    return stage;
}

namespace {

// The next code point of `text` from `at`, and its length in bytes; a byte that does not begin a
// well-formed sequence is returned as itself, one byte long, and is not strong.
[[nodiscard]] char32_t next_code_point(std::string_view text, std::size_t at, std::size_t& length) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    length = 1;
    if (lead < 0x80) {
        return lead;
    }
    std::size_t extra = 0;
    char32_t value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        return 0xFFFD;
    }
    if (at + extra >= text.size()) {
        return 0xFFFD;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        value = (value << 6) | (byte(at + i) & 0x3F);
    }
    length = extra + 1;
    return value;
}

[[nodiscard]] bool is_strong_right_to_left(char32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
           (c >= 0x1E800 && c <= 0x1EFFF);
}

[[nodiscard]] bool is_strong_left_to_right(char32_t c) {
    if (c < 0x80) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    if (c < 0xC0) {
        return c == 0xAA || c == 0xB5 || c == 0xBA;
    }
    // Punctuation and symbol blocks, CJK punctuation, variation selectors, fullwidth forms' punctuation.
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        c == 0xD7 || c == 0xF7 || c == 0xFFFD) {
        return false;
    }
    return !is_strong_right_to_left(c);
}

}  // namespace

bool text_is_right_to_left(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        std::size_t length = 1;
        const char32_t c = next_code_point(text, at, length);
        if (is_strong_right_to_left(c)) {
            return true;
        }
        if (is_strong_left_to_right(c)) {
            return false;
        }
        at += length;
    }
    return false;
}

}  // namespace iclforge::lyrics
