#include "iclforge/sink/lyric_frame.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace iclforge::sink {

namespace {

[[nodiscard]] LyricRow row_of(const lyrics::Lyrics& lyrics, std::size_t index) {
    const lyrics::LyricLine& line = lyrics.lines[index];
    return {.text = line.text, .singer = line.singer, .right_to_left = lyrics::text_is_right_to_left(line.text)};
}

[[nodiscard]] std::string_view mode_name(lyrics::StageMode mode) {
    switch (mode) {
        case lyrics::StageMode::kNoLyrics:
            return "none";
        case lyrics::StageMode::kPlain:
            return "plain";
        case lyrics::StageMode::kWaiting:
            return "waiting";
        case lyrics::StageMode::kGap:
            return "gap";
        case lyrics::StageMode::kLine:
            return "line";
    }
    return "none";
}

// A JSON string, RFC 8259: the quote, the backslash and the controls are escaped, and everything
// else (UTF-8 included) is passed through, which JSON allows.
void put_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    std::array<char, 8> escape{};
                    std::snprintf(escape.data(), escape.size(), "\\u%04x", static_cast<unsigned>(byte));
                    out += escape.data();
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void put_key(std::string& out, std::string_view key) {
    if (out.back() != '{') {
        out.push_back(',');
    }
    put_string(out, key);
    out.push_back(':');
}

void put_row(std::string& out, const LyricRow& row) {
    out += "{\"text\":";
    put_string(out, row.text);
    out += ",\"singer\":" + std::to_string(static_cast<int>(row.singer));
    out += std::string(",\"rtl\":") + (row.right_to_left ? "true" : "false") + "}";
}

void put_rows(std::string& out, std::string_view key, const std::vector<LyricRow>& rows) {
    put_key(out, key);
    out.push_back('[');
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        put_row(out, rows[i]);
    }
    out.push_back(']');
}

}  // namespace

LyricFrame make_lyric_frame(std::optional<std::int64_t> item, const lyrics::Lyrics* lyrics,
                            const lyrics::Stage& stage, bool streaming) {
    LyricFrame frame;
    frame.item = item.value_or(-1);
    frame.streaming = streaming;
    frame.mode = lyrics != nullptr ? stage.mode : lyrics::StageMode::kNoLyrics;
    if (lyrics == nullptr) {
        return frame;
    }
    frame.position_ms = stage.position_ms;
    frame.title = lyrics->title;
    frame.artist = lyrics->artist;
    frame.album = lyrics->album;
    frame.first_singer = lyrics->first_singer_name;
    frame.second_singer = lyrics->second_singer_name;
    frame.duet = lyrics::has_singers(*lyrics);
    frame.timed = lyrics->timed;
    frame.length_ms = lyrics->length_ms;
    frame.line_count = lyrics::text_line_count(*lyrics);

    if (stage.mode == lyrics::StageMode::kPlain) {
        for (std::size_t i = 0; i < lyrics->lines.size(); ++i) {
            if (!lyrics->lines[i].text.empty()) {
                frame.plain.push_back(row_of(*lyrics, i));
            }
        }
        return frame;
    }

    for (std::size_t i = 0; i < stage.before_count; ++i) {
        frame.before.push_back(row_of(*lyrics, stage.before[i]));
    }
    for (std::size_t i = 0; i < stage.after_count; ++i) {
        frame.after.push_back(row_of(*lyrics, stage.after[i]));
    }

    if (stage.mode == lyrics::StageMode::kLine && stage.line < lyrics->lines.size()) {
        const lyrics::LyricLine& line = lyrics->lines[stage.line];
        frame.current = row_of(*lyrics, stage.line);
        frame.word_timed = stage.word_timed;
        if (stage.word_timed) {
            for (const lyrics::LyricWord& word : line.words) {
                frame.words.push_back(word.text);
            }
            frame.words_sung = stage.words_sung;
            frame.word = stage.word;
            frame.fraction = stage.fraction;
        }
        frame.line_number = lyrics::text_line_number(*lyrics, stage.line);
    }

    if (stage.mode == lyrics::StageMode::kGap) {
        frame.next_line_in_ms = stage.next_line_in_ms;
        frame.gap_ms = stage.gap_ms;
        frame.instrumental = stage.instrumental;
        frame.count_in = stage.count_in;
    }
    return frame;
}

std::string json_quote(std::string_view text) {
    std::string out;
    put_string(out, text);
    return out;
}

std::string lyric_frame_json(const LyricFrame& frame) {
    std::string out = "{";
    put_key(out, "item");
    out += std::to_string(frame.item);
    put_key(out, "streaming");
    out += frame.streaming ? "true" : "false";
    put_key(out, "mode");
    put_string(out, mode_name(frame.mode));
    put_key(out, "position_ms");
    out += std::to_string(frame.position_ms);
    put_key(out, "title");
    put_string(out, frame.title);
    put_key(out, "artist");
    put_string(out, frame.artist);
    put_key(out, "album");
    put_string(out, frame.album);
    put_key(out, "first_singer");
    put_string(out, frame.first_singer);
    put_key(out, "second_singer");
    put_string(out, frame.second_singer);
    put_key(out, "duet");
    out += frame.duet ? "true" : "false";
    put_key(out, "timed");
    out += frame.timed ? "true" : "false";
    put_key(out, "length_ms");
    out += frame.length_ms ? std::to_string(*frame.length_ms) : "null";

    put_key(out, "current");
    put_row(out, frame.current);
    put_key(out, "word_timed");
    out += frame.word_timed ? "true" : "false";
    put_key(out, "words");
    out.push_back('[');
    for (std::size_t i = 0; i < frame.words.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        put_string(out, frame.words[i]);
    }
    out.push_back(']');
    put_key(out, "words_sung");
    out += std::to_string(frame.words_sung);
    put_key(out, "word");
    out += frame.word ? std::to_string(*frame.word) : "null";
    put_key(out, "fraction");
    std::array<char, 32> fraction{};
    std::snprintf(fraction.data(), fraction.size(), "%.4f", frame.fraction);
    out += fraction.data();

    put_rows(out, "before", frame.before);
    put_rows(out, "after", frame.after);
    put_rows(out, "plain", frame.plain);

    put_key(out, "next_line_in_ms");
    out += frame.next_line_in_ms ? std::to_string(*frame.next_line_in_ms) : "null";
    put_key(out, "gap_ms");
    out += frame.gap_ms ? std::to_string(*frame.gap_ms) : "null";
    put_key(out, "instrumental");
    out += frame.instrumental ? "true" : "false";
    put_key(out, "count_in");
    out += std::to_string(frame.count_in);
    put_key(out, "line_number");
    out += std::to_string(frame.line_number);
    put_key(out, "line_count");
    out += std::to_string(frame.line_count);
    out.push_back('}');
    return out;
}

}  // namespace iclforge::sink
