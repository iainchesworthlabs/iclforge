#include "optional_modules.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/lyrics/lyrics.hpp"

#include "binding_support.hpp"

// The variant of the `iclforge.lyrics` submodule compiled when iclforge::lyrics is in this build:
// timed lyrics for a karaoke stream. An LRC file (UTF-8, or UTF-16 with a byte-order mark), a
// SubRip `.srt`, an Advanced SubStation Alpha `.ass` and a MIDI karaoke `.kar` are read into lines
// of timed words, and a stateless cursor says which line and word a playing position is in - the
// reader Hearth, Crucible and the sink firmware use, bound as it is.
//
// See optional_modules.hpp for the present/absent pair and bindings/python/CMakeLists.txt for the
// selection.

namespace iclforge::python {

namespace py = pybind11;
using detail::to_bytes;

void register_lyrics(py::module_& m) {
    namespace lyr = iclforge::lyrics;

    auto lyrics = m.def_submodule(
        "lyrics",
        "Timed lyrics for a karaoke stream: parse a LRC/SubRip/ASS/MIDI-karaoke file into lines "
        "of timed words and ask where a playing position is in them.");

    py::enum_<lyr::Singer>(lyrics, "Singer",
                           "Which singer of a duet sings a line (its V1:, V2: or V1+V2: marker).")
        .value("kUnmarked", lyr::Singer::kUnmarked,
               "A line that says nothing, which a solo's every line is")
        .value("kFirst", lyr::Singer::kFirst, "V1")
        .value("kSecond", lyr::Singer::kSecond, "V2")
        .value("kBoth", lyr::Singer::kBoth, "V1+V2");

    py::enum_<lyr::WordState>(lyrics, "WordState", "A word against a position.")
        .value("kSung", lyr::WordState::kSung, "From the word's end")
        .value("kBeingSung", lyr::WordState::kBeingSung, "From the word's start")
        .value("kNotYetSung", lyr::WordState::kNotYetSung, "Before it");

    py::class_<lyr::LyricWord>(lyrics, "Word",
                               "One word of a line. Its text keeps the spacing written after it.")
        .def_readonly("start_ms", &lyr::LyricWord::start_ms)
        .def_readonly("end_ms", &lyr::LyricWord::end_ms)
        .def_readonly("text", &lyr::LyricWord::text);

    py::class_<lyr::LyricLine>(lyrics, "Line", "One line of lyrics, with its words when it has them.")
        .def_readonly("start_ms", &lyr::LyricLine::start_ms)
        .def_readonly("end_ms", &lyr::LyricLine::end_ms,
                      "The next line's start; for the last line the item's duration, its [length:] "
                      "or five seconds after it starts")
        .def_readonly("text", &lyr::LyricLine::text,
                      "The line without its tags; empty for a line that only marks a time")
        .def_readonly("words", &lyr::LyricLine::words,
                      "Empty unless the line carried word tags: no word times are invented")
        .def_readonly("singer", &lyr::LyricLine::singer)
        .def_property_readonly("word_timed", &lyr::LyricLine::word_timed);

    py::class_<lyr::LyricPosition>(lyrics, "Position",
                                   "Where in the lyrics a playing position is. Stateless: a seek, a "
                                   "pause and a stop are only a different position.")
        .def_readonly("line", &lyr::LyricPosition::line,
                      "The line most recently started; None before the first")
        .def_readonly("in_line", &lyr::LyricPosition::in_line,
                      "Whether the position is before that line's end: False after the last line")
        .def_readonly("word", &lyr::LyricPosition::word,
                      "The word being sung, when that line is word-timed and one word spans the "
                      "position")
        .def_readonly("fraction", &lyr::LyricPosition::fraction,
                      "How far through that word, from 0 up to but not including 1, by time")
        .def_readonly("position_ms", &lyr::LyricPosition::position_ms,
                      "The position the answer is for: the one asked, less the delay");

    py::class_<lyr::Lyrics>(lyrics, "Lyrics", "A lyrics file read into lines of timed words.")
        .def_readonly("title", &lyr::Lyrics::title)
        .def_readonly("artist", &lyr::Lyrics::artist)
        .def_readonly("album", &lyr::Lyrics::album)
        .def_readonly("author", &lyr::Lyrics::author)
        .def_readonly("first_singer_name", &lyr::Lyrics::first_singer_name,
                      "What the file calls its first singer ([v1:name]); empty when it does not")
        .def_readonly("second_singer_name", &lyr::Lyrics::second_singer_name)
        .def_readonly("length_ms", &lyr::Lyrics::length_ms,
                      "The file's [length:], in milliseconds, or None")
        .def_readonly("offset_ms", &lyr::Lyrics::offset_ms,
                      "The file's [offset:], already applied to every time")
        .def_readonly("timed", &lyr::Lyrics::timed,
                      "False for plain text with no times at all: not followed")
        .def_readonly("lines", &lyr::Lyrics::lines, "In time order, stable among equal times")
        .def_property_readonly(
            "has_singers", [](const lyr::Lyrics& self) { return lyr::has_singers(self); },
            "Whether any line has a singer: the file is a duet")
        .def(
            "position",
            [](const lyr::Lyrics& self, std::int64_t position_ms, std::int64_t delay_ms) {
                return lyr::lyric_position(self, position_ms, delay_ms);
            },
            py::arg("position_ms"), py::arg("delay_ms") = 0,
            "Where `position_ms` is in the lyrics. `delay_ms` is the singer's nudge: positive "
            "shows the words later, so the answer is for an earlier position.")
        .def(
            "word_state",
            [](const lyr::Lyrics& self, std::size_t line, std::size_t word,
               std::int64_t position_ms) {
                if (line >= self.lines.size() || word >= self.lines[line].words.size()) {
                    return lyr::WordState::kNotYetSung;
                }
                return lyr::word_state(self.lines[line].words[word], position_ms);
            },
            py::arg("line"), py::arg("word"), py::arg("position_ms"),
            "A word against a position; a word that is not there is not yet sung.");

    lyrics.def(
        "parse",
        [](const py::buffer& content, std::optional<std::int64_t> duration_ms) {
            const auto bytes = to_bytes(content);
            const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            auto parsed = lyr::parse_timed_text(text, duration_ms);
            if (!parsed) {
                throw py::value_error(parsed.error());
            }
            return std::move(*parsed);
        },
        py::arg("content"), py::arg("duration_ms") = py::none(),
        "Read the bytes of a lyrics file: LRC (UTF-8, or UTF-16 with a byte-order mark), SubRip, "
        "Advanced SubStation Alpha or MIDI karaoke, told by what it holds and not its name. "
        "`duration_ms` is the item's length, for the end of the last line. Raises ValueError, "
        "with the reason, for bytes that are empty, in an encoding that is not read or without "
        "lyrics.");

    // The same for text already in a str (UTF-8 on the way in).
    lyrics.def(
        "parse",
        [](const std::string& text, std::optional<std::int64_t> duration_ms) {
            auto parsed = lyr::parse_timed_text(text, duration_ms);
            if (!parsed) {
                throw py::value_error(parsed.error());
            }
            return std::move(*parsed);
        },
        py::arg("content"), py::arg("duration_ms") = py::none(),
        "As parse(bytes), for lyrics already held as a str.");

    lyrics.def(
        "read",
        [](const std::string& path, std::optional<std::int64_t> duration_ms) {
            auto parsed = lyr::read_lyrics(path, duration_ms);
            if (!parsed) {
                throw py::value_error(parsed.error());
            }
            return std::move(*parsed);
        },
        py::arg("path"), py::arg("duration_ms") = py::none(),
        "parse() of a file's bytes. A file over four MiB, which no lyrics file is, is refused.");

    lyrics.def(
        "find_file",
        [](const std::string& item_path) -> std::optional<std::string> {
            const auto found = lyr::find_lyrics_file(item_path);
            if (!found) {
                return std::nullopt;
            }
            return found->string();
        },
        py::arg("item_path"),
        "The lyrics file of a media item: `name.lrc` in the item's folder, the extension in any "
        "case, and where there is none `name.srt`, then `name.ass`, then `name.kar`; None when "
        "there is not one.");

    lyrics.def(
        "singer_marker",
        [](lyr::Singer singer) { return std::string{lyr::singer_marker(singer)}; },
        py::arg("singer"),
        "The marker a line of that singer is written with: 'V1:', 'V2:' or 'V1+V2:'; empty for "
        "kUnmarked.");
}

}  // namespace iclforge::python
