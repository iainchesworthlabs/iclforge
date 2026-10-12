// Timed lyrics for the browser decode demo: the same reader Hearth, Crucible and the sink firmware
// use (libs/lyrics), compiled into the demo's module so that the page reads an LRC, SubRip, ASS or
// MIDI karaoke file with the one implementation and not a second one in JavaScript.
//
//   parseLyrics(bytes, durationMs)  -> { ok: true, document } | { ok: false, error }
//   document.describe()             -> the lines and their words, as plain objects
//   document.position(ms, delayMs)  -> which line and word a playing position is in
//   document.wordState(line, word, ms)
//
// The document is an Embind class so that it is parsed once and asked each animation frame
// ("stateless cursor": a seek, a pause and a stop are only another position); the page deletes it
// when it loads another file (`document.delete()`).
//
// The reader is standard library only, and takes UTF-8, UTF-16 with a byte-order mark, SubRip, ASS
// and MIDI karaoke as bytes, told by what they hold. A file in a code page is refused with a
// sentence that says so, which the page shows.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "iclforge/lyrics/lyrics.hpp"

namespace {

namespace lyr = iclforge::lyrics;

const char* singer_name(lyr::Singer singer) {
    switch (singer) {
        case lyr::Singer::kFirst: return "v1";
        case lyr::Singer::kSecond: return "v2";
        case lyr::Singer::kBoth: return "both";
        case lyr::Singer::kUnmarked: break;
    }
    return "";
}

class LyricsDocument {
   public:
    explicit LyricsDocument(lyr::Lyrics lyrics) : lyrics_(std::move(lyrics)) {}

    // The whole file as plain objects, once: the page holds this and draws from it.
    emscripten::val describe() const {
        auto out = emscripten::val::object();
        out.set("title", lyrics_.title);
        out.set("artist", lyrics_.artist);
        out.set("album", lyrics_.album);
        out.set("author", lyrics_.author);
        out.set("firstSingerName", lyrics_.first_singer_name);
        out.set("secondSingerName", lyrics_.second_singer_name);
        out.set("lengthMs", lyrics_.length_ms ? emscripten::val(static_cast<double>(*lyrics_.length_ms))
                                              : emscripten::val::null());
        out.set("offsetMs", static_cast<double>(lyrics_.offset_ms));
        out.set("timed", lyrics_.timed);
        out.set("hasSingers", lyr::has_singers(lyrics_));

        auto lines = emscripten::val::array();
        for (std::size_t i = 0; i < lyrics_.lines.size(); ++i) {
            const lyr::LyricLine& line = lyrics_.lines[i];
            auto entry = emscripten::val::object();
            entry.set("startMs", static_cast<double>(line.start_ms));
            entry.set("endMs", static_cast<double>(line.end_ms));
            entry.set("text", line.text);
            entry.set("singer", std::string(singer_name(line.singer)));
            auto words = emscripten::val::array();
            for (std::size_t w = 0; w < line.words.size(); ++w) {
                const lyr::LyricWord& word = line.words[w];
                auto item = emscripten::val::object();
                item.set("startMs", static_cast<double>(word.start_ms));
                item.set("endMs", static_cast<double>(word.end_ms));
                item.set("text", word.text);
                words.set(static_cast<unsigned>(w), item);
            }
            entry.set("words", words);
            lines.set(static_cast<unsigned>(i), entry);
        }
        out.set("lines", lines);
        return out;
    }

    // Where `position_ms` is, less the singer's nudge. `line` and `word` are null where there is
    // none (before the first line; a line with no word under the position).
    emscripten::val position(double position_ms, double delay_ms) const {
        const lyr::LyricPosition at = lyr::lyric_position(
            lyrics_, static_cast<std::int64_t>(position_ms), static_cast<std::int64_t>(delay_ms));
        auto out = emscripten::val::object();
        out.set("line", at.line ? emscripten::val(static_cast<double>(*at.line)) : emscripten::val::null());
        out.set("inLine", at.in_line);
        out.set("word", at.word ? emscripten::val(static_cast<double>(*at.word)) : emscripten::val::null());
        out.set("fraction", at.fraction);
        out.set("positionMs", static_cast<double>(at.position_ms));
        return out;
    }

    // 0 sung, 1 being sung, 2 not yet sung - a word that is not there is not yet sung. The
    // position is as lyric_position() takes it: the caller has taken the delay off.
    int wordState(int line, int word, double position_ms) const {
        if (line < 0 || word < 0 || static_cast<std::size_t>(line) >= lyrics_.lines.size() ||
            static_cast<std::size_t>(word) >= lyrics_.lines[static_cast<std::size_t>(line)].words.size()) {
            return static_cast<int>(lyr::WordState::kNotYetSung);
        }
        return static_cast<int>(lyr::word_state(
            lyrics_.lines[static_cast<std::size_t>(line)].words[static_cast<std::size_t>(word)],
            static_cast<std::int64_t>(position_ms)));
    }

   private:
    lyr::Lyrics lyrics_;
};

// The bytes of a lyrics file. `duration_ms` is the item's length, which the last line's end is
// taken from when the file does not say; negative says it is not known.
emscripten::val parseLyrics(const emscripten::val& js_bytes, double duration_ms) {
    const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
    const std::string_view text{reinterpret_cast<const char*>(raw.data()), raw.size()};
    const std::optional<std::int64_t> duration =
        duration_ms >= 0.0 ? std::optional<std::int64_t>{static_cast<std::int64_t>(duration_ms)}
                           : std::nullopt;

    auto result = emscripten::val::object();
    auto parsed = lyr::parse_timed_text(text, duration);
    if (!parsed) {
        result.set("ok", false);
        result.set("error", parsed.error());
        return result;
    }
    result.set("ok", true);
    result.set("document", emscripten::val(LyricsDocument{std::move(*parsed)}));
    return result;
}

}  // namespace

EMSCRIPTEN_BINDINGS(iclforge_wasm_lyrics) {
    emscripten::function("parseLyrics", &parseLyrics);
    emscripten::class_<LyricsDocument>("LyricsDocument")
        .function("describe", &LyricsDocument::describe)
        .function("position", &LyricsDocument::position)
        .function("wordState", &LyricsDocument::wordState);
}
