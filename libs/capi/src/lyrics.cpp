#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "iclforge/lyrics/lyrics.hpp"
#include "internal.hpp"

// The C face of iclforge::lyrics (libs/lyrics): a lyrics file read into lines of timed words, and
// the stateless cursor that says which line and word a playing position is in. The reader itself
// is compiled into this library from libs/lyrics/src (libs/capi/CMakeLists.txt says why it is not
// linked: that library is internal and never installed, and an installed archive cannot name it).

using iclforge_c::guard;

struct iclforge_lyrics {
    iclforge::lyrics::Lyrics data;
};

namespace {

// The sentence of the last refusal on this thread: what iclforge_lyrics_error_message() returns,
// which is why it outlives the call that set it.
thread_local std::string g_error;

const iclforge::lyrics::LyricLine* line_of(const iclforge_lyrics_t* lyrics, std::size_t line) {
    if (lyrics == nullptr || line >= lyrics->data.lines.size()) {
        return nullptr;
    }
    return &lyrics->data.lines[line];
}

const iclforge::lyrics::LyricWord* word_of(const iclforge_lyrics_t* lyrics, std::size_t line,
                                           std::size_t word) {
    const auto* found = line_of(lyrics, line);
    if (found == nullptr || word >= found->words.size()) {
        return nullptr;
    }
    return &found->words[word];
}

const char* text_or_empty(const iclforge_lyrics_t* lyrics, const std::string iclforge::lyrics::Lyrics::*field) {
    return lyrics == nullptr ? "" : (lyrics->data.*field).c_str();
}

}  // namespace

extern "C" {

iclforge_status_t iclforge_lyrics_parse(const uint8_t* data, size_t size, int64_t duration_ms,
                                         iclforge_lyrics_t** out_lyrics) {
    if (out_lyrics == nullptr || (data == nullptr && size != 0)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> iclforge_status_t {
        g_error.clear();
        const std::string_view text{reinterpret_cast<const char*>(data), size};
        const std::optional<std::int64_t> duration =
            duration_ms >= 0 ? std::optional<std::int64_t>{duration_ms} : std::nullopt;
        auto parsed = iclforge::lyrics::parse_timed_text(text, duration);
        if (!parsed) {
            g_error = std::move(parsed.error());
            return ICLFORGE_ERROR_LYRICS_UNREADABLE;
        }
        auto owned = std::make_unique<iclforge_lyrics>();
        owned->data = std::move(*parsed);
        *out_lyrics = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_lyrics_destroy(iclforge_lyrics_t* lyrics) { delete lyrics; }

const char* iclforge_lyrics_error_message(void) { return g_error.c_str(); }

const char* iclforge_lyrics_title(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::title);
}

const char* iclforge_lyrics_artist(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::artist);
}

const char* iclforge_lyrics_album(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::album);
}

const char* iclforge_lyrics_author(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::author);
}

const char* iclforge_lyrics_first_singer_name(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::first_singer_name);
}

const char* iclforge_lyrics_second_singer_name(const iclforge_lyrics_t* lyrics) {
    return text_or_empty(lyrics, &iclforge::lyrics::Lyrics::second_singer_name);
}

int iclforge_lyrics_length_ms(const iclforge_lyrics_t* lyrics, int64_t* out_length_ms) {
    if (lyrics == nullptr || !lyrics->data.length_ms.has_value()) {
        return 0;
    }
    if (out_length_ms != nullptr) {
        *out_length_ms = *lyrics->data.length_ms;
    }
    return 1;
}

int64_t iclforge_lyrics_offset_ms(const iclforge_lyrics_t* lyrics) {
    return lyrics == nullptr ? 0 : lyrics->data.offset_ms;
}

int iclforge_lyrics_timed(const iclforge_lyrics_t* lyrics) {
    return lyrics != nullptr && lyrics->data.timed ? 1 : 0;
}

int iclforge_lyrics_has_singers(const iclforge_lyrics_t* lyrics) {
    return lyrics != nullptr && iclforge::lyrics::has_singers(lyrics->data) ? 1 : 0;
}

size_t iclforge_lyrics_line_count(const iclforge_lyrics_t* lyrics) {
    return lyrics == nullptr ? 0 : lyrics->data.lines.size();
}

int64_t iclforge_lyrics_line_start_ms(const iclforge_lyrics_t* lyrics, size_t line) {
    const auto* found = line_of(lyrics, line);
    return found == nullptr ? 0 : found->start_ms;
}

int64_t iclforge_lyrics_line_end_ms(const iclforge_lyrics_t* lyrics, size_t line) {
    const auto* found = line_of(lyrics, line);
    return found == nullptr ? 0 : found->end_ms;
}

const char* iclforge_lyrics_line_text(const iclforge_lyrics_t* lyrics, size_t line) {
    const auto* found = line_of(lyrics, line);
    return found == nullptr ? "" : found->text.c_str();
}

iclforge_lyrics_singer_t iclforge_lyrics_line_singer(const iclforge_lyrics_t* lyrics,
                                                      size_t line) {
    const auto* found = line_of(lyrics, line);
    return found == nullptr ? ICLFORGE_LYRICS_SINGER_UNMARKED
                            : static_cast<iclforge_lyrics_singer_t>(found->singer);
}

size_t iclforge_lyrics_line_word_count(const iclforge_lyrics_t* lyrics, size_t line) {
    const auto* found = line_of(lyrics, line);
    return found == nullptr ? 0 : found->words.size();
}

int64_t iclforge_lyrics_word_start_ms(const iclforge_lyrics_t* lyrics, size_t line, size_t word) {
    const auto* found = word_of(lyrics, line, word);
    return found == nullptr ? 0 : found->start_ms;
}

int64_t iclforge_lyrics_word_end_ms(const iclforge_lyrics_t* lyrics, size_t line, size_t word) {
    const auto* found = word_of(lyrics, line, word);
    return found == nullptr ? 0 : found->end_ms;
}

const char* iclforge_lyrics_word_text(const iclforge_lyrics_t* lyrics, size_t line, size_t word) {
    const auto* found = word_of(lyrics, line, word);
    return found == nullptr ? "" : found->text.c_str();
}

void iclforge_lyrics_position(const iclforge_lyrics_t* lyrics, int64_t position_ms,
                              int64_t delay_ms, iclforge_lyrics_position_t* out) {
    if (lyrics == nullptr || out == nullptr) {
        return;
    }
    const auto position = iclforge::lyrics::lyric_position(lyrics->data, position_ms, delay_ms);
    out->has_line = position.line.has_value() ? 1 : 0;
    out->line = position.line.value_or(0);
    out->in_line = position.in_line ? 1 : 0;
    out->has_word = position.word.has_value() ? 1 : 0;
    out->word = position.word.value_or(0);
    out->fraction = position.fraction;
    out->position_ms = position.position_ms;
}

iclforge_lyrics_word_state_t iclforge_lyrics_word_state(const iclforge_lyrics_t* lyrics,
                                                         size_t line, size_t word,
                                                         int64_t position_ms) {
    const auto* found = word_of(lyrics, line, word);
    if (found == nullptr) {
        return ICLFORGE_LYRICS_WORD_NOT_YET_SUNG;
    }
    return static_cast<iclforge_lyrics_word_state_t>(
        iclforge::lyrics::word_state(*found, position_ms));
}

}  // extern "C"

static_assert(static_cast<int>(iclforge::lyrics::Singer::kUnmarked) ==
              ICLFORGE_LYRICS_SINGER_UNMARKED);
static_assert(static_cast<int>(iclforge::lyrics::Singer::kFirst) == ICLFORGE_LYRICS_SINGER_FIRST);
static_assert(static_cast<int>(iclforge::lyrics::Singer::kSecond) == ICLFORGE_LYRICS_SINGER_SECOND);
static_assert(static_cast<int>(iclforge::lyrics::Singer::kBoth) == ICLFORGE_LYRICS_SINGER_BOTH);
static_assert(static_cast<int>(iclforge::lyrics::WordState::kSung) == ICLFORGE_LYRICS_WORD_SUNG);
static_assert(static_cast<int>(iclforge::lyrics::WordState::kBeingSung) ==
              ICLFORGE_LYRICS_WORD_BEING_SUNG);
static_assert(static_cast<int>(iclforge::lyrics::WordState::kNotYetSung) ==
              ICLFORGE_LYRICS_WORD_NOT_YET_SUNG);
