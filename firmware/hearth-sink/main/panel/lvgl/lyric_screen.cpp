// The lyric screen. See lyric_screen.hpp and planning/karaoke-p4x-panel.md.
//
// The rows of a line (the lines before it and after it, the words of the one being sung) are LVGL
// objects that are built when the line changes and left alone while it is sung: a frame changes the
// width of one overlay, and the fill redraws that word's rectangle and nothing else. What is shown,
// and when, is a Stage (libs/lyrics/stage.hpp), which the host tests hold to the arithmetic of the
// format; this file is the drawing of it.

#include "lyric_screen.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"

#include "../lyrics_reader.hpp"
#include "board_info.hpp"
#include "fonts.hpp"
#include "glyphs.hpp"
#include "lv_alloc.hpp"

namespace player::panel {

namespace {

using iclforge::lyrics::Lyrics;
using iclforge::lyrics::Singer;
using iclforge::lyrics::Stage;
using iclforge::lyrics::StageMode;
using iclforge::lyrics::StageOptions;

// --- the design's tokens (planning/karaoke-p4x-panel.md, "The screen"): Hearth's Signature palette
// in its dark mode.
constexpr std::uint32_t kBg = 0x151313;
constexpr std::uint32_t kSurface = 0x1D1A1A;
constexpr std::uint32_t kSurfaceUp = 0x272423;
constexpr std::uint32_t kText = 0xECE7E5;
constexpr std::uint32_t kAccent = 0xFF5C40;
constexpr std::uint32_t kMuted = 0x8B8583;
constexpr std::uint32_t kFaint = 0x4B4645;

constexpr int kMargin = 32;
constexpr int kHeaderHeight = 64;
constexpr int kFooterHeight = 64;
constexpr int kDuetIndent = 184;  // 18 % of 1024, the desktop's rule for the second vocal
// A line already sung is the accent colour at this opacity of 255: 70 %, which is 3.5 : 1 against the
// background (55 % was 2.6 : 1; the shared sink-screen spec asked for the change).
constexpr lv_opa_t kSungOpacity = 180;
constexpr std::int32_t kDelayStepMs = 50;
constexpr std::int32_t kDelayMaxMs = 1000;
constexpr std::uint32_t kSheetIdleMs = 8000;

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

// --- a font, with Greek and Cyrillic and then Hebrew behind it as its fallbacks, at a size that can
// change. Latin letters never leave the first file; a letter it lacks is asked of the next.
struct Face {
    lv_font_t* latin = nullptr;
    lv_font_t* greek_cyrillic = nullptr;
    lv_font_t* hebrew = nullptr;
    int size = 0;
};

bool make_face(Face& face, const FontFile& latin, const FontFile& greek_cyrillic, const FontFile& hebrew,
               int px) {
    if (latin.data == nullptr || greek_cyrillic.data == nullptr || hebrew.data == nullptr) {
        return false;
    }
    face.hebrew = lv_tiny_ttf_create_data(hebrew.data, hebrew.size, px);
    face.greek_cyrillic = lv_tiny_ttf_create_data(greek_cyrillic.data, greek_cyrillic.size, px);
    face.latin = lv_tiny_ttf_create_data(latin.data, latin.size, px);
    if (face.latin == nullptr || face.greek_cyrillic == nullptr || face.hebrew == nullptr) {
        return false;
    }
    face.greek_cyrillic->fallback = face.hebrew;
    face.latin->fallback = face.greek_cyrillic;
    face.size = px;
    return true;
}

void set_size(Face& face, int px) {
    if (face.size == px) {
        return;
    }
    lv_tiny_ttf_set_size(face.latin, px);
    lv_tiny_ttf_set_size(face.greek_cyrillic, px);
    lv_tiny_ttf_set_size(face.hebrew, px);
    face.size = px;
}

// --- what the screen copies from the player, under its lock (sendspin_read_lyrics). The copies are
// in LVGL's heap (PSRAM on the firmware: lv_alloc.hpp says why), not in the internal RAM a decoder
// leaves little of.
struct LineText {
    LvString text;
    Singer singer = Singer::kUnmarked;
    LvVector<LvString> words;
};

class Snapshot final : public LyricsReader {
   public:
    Stage stage{};
    std::int64_t item = -1;
    bool held = false;
    bool timed = false;
    // The lines with text (a line that only marks a time is not one of the song's) and the place of the
    // line being shown among them: what the footer says, "Line 2 of 4". Counted when an item arrives and
    // numbered when its line moves, not on every frame.
    std::size_t text_lines = 0;
    std::size_t text_line_number = 0;
    LvString title;
    LvString artist;
    LvString first_name;
    LvString second_name;
    std::optional<std::int64_t> length_ms;
    LvString plain;
    // Bumped whenever a row's text, the title or the names changed: a rebuild is due.
    std::uint32_t generation = 0;

    [[nodiscard]] const LineText* line(std::size_t index) const {
        const auto found = lines_.find(index);
        return found == lines_.end() ? nullptr : &found->second;
    }

    void on_lyrics(std::int64_t item_id, const Lyrics* lyrics, const Stage& at) override {
        stage = at;
        const bool item_changed = item_id != item;
        item = item_id;
        held = lyrics != nullptr;
        if (lyrics == nullptr) {
            if (!lines_.empty() || !plain.empty() || !title.empty()) {
                lines_.clear();
                plain.clear();
                title.clear();
                artist.clear();
                text_lines = 0;
                text_line_number = 0;
                counted_lines_ = 0;
                numbered_line_ = kNone;
                timed = false;
                length_ms.reset();
                ++generation;
            }
            return;
        }
        timed = lyrics->timed;
        if (item_changed || lyrics->lines.size() != counted_lines_) {
            text_lines = iclforge::lyrics::text_line_count(*lyrics);
            counted_lines_ = lyrics->lines.size();
            numbered_line_ = kNone;
        }
        if (at.mode == StageMode::kLine && at.line != numbered_line_) {
            text_line_number = iclforge::lyrics::text_line_number(*lyrics, at.line);
            numbered_line_ = at.line;
        }
        length_ms = lyrics->length_ms;
        bool changed = item_changed;
        changed |= assign(title, lyrics->title);
        changed |= assign(artist, lyrics->artist);
        changed |= assign(first_name, lyrics->first_singer_name);
        changed |= assign(second_name, lyrics->second_singer_name);

        std::array<std::size_t, 1 + 2 * iclforge::lyrics::kStageMaxContext> wanted{};
        std::size_t count = 0;
        if (at.mode == StageMode::kLine) {
            wanted[count++] = at.line;
        }
        for (std::size_t i = 0; i < at.before_count; ++i) {
            wanted[count++] = at.before[i];
        }
        for (std::size_t i = 0; i < at.after_count; ++i) {
            wanted[count++] = at.after[i];
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t index = wanted[i];
            if (index >= lyrics->lines.size()) {
                continue;
            }
            const iclforge::lyrics::LyricLine& source = lyrics->lines[index];
            LineText& row = lines_[index];
            bool same = std::string_view(row.text) == source.text && row.singer == source.singer &&
                        row.words.size() == source.words.size();
            for (std::size_t w = 0; same && w < source.words.size(); ++w) {
                same = std::string_view(row.words[w]) == source.words[w].text;
            }
            if (!same) {
                row.text.assign(source.text.data(), source.text.size());
                row.singer = source.singer;
                row.words.clear();
                row.words.reserve(source.words.size());
                for (const iclforge::lyrics::LyricWord& word : source.words) {
                    row.words.emplace_back(word.text.data(), word.text.size());
                }
                changed = true;
            }
        }
        // Drop what the stage no longer names.
        for (auto it = lines_.begin(); it != lines_.end();) {
            bool keep = false;
            for (std::size_t i = 0; i < count; ++i) {
                keep |= wanted[i] == it->first;
            }
            it = keep ? std::next(it) : lines_.erase(it);
        }
        if (!lyrics->timed) {
            if (changed || plain.empty()) {
                plain.clear();
                for (const iclforge::lyrics::LyricLine& source : lyrics->lines) {
                    if (plain.size() > 12000) {
                        break;
                    }
                    plain += source.text;
                    plain += '\n';
                }
            }
        } else {
            plain.clear();
        }
        if (changed) {
            ++generation;
        }
    }

   private:
    static bool assign(LvString& to, const std::string& from) {
        if (std::string_view(to) == from) {
            return false;
        }
        to.assign(from.data(), from.size());
        return true;
    }

    LvMap<std::size_t, LineText> lines_;
    std::size_t counted_lines_ = 0;
    std::size_t numbered_line_ = kNone;
};

// --- the rows' identity: what a rebuild is for.
struct Signature {
    StageMode mode = StageMode::kNoLyrics;
    bool idle = true;
    std::int64_t item = -1;
    std::size_t line = 0;
    bool word_timed = false;
    bool instrumental = false;
    std::size_t before_count = 0;
    std::size_t after_count = 0;
    std::array<std::size_t, iclforge::lyrics::kStageMaxContext> before{};
    std::array<std::size_t, iclforge::lyrics::kStageMaxContext> after{};
    std::uint32_t generation = 0;
    int scale = 1;

    bool operator==(const Signature&) const = default;
};

struct WordCell {
    lv_obj_t* cell = nullptr;
    lv_obj_t* base = nullptr;
    lv_obj_t* clip = nullptr;
    lv_obj_t* fill = nullptr;
    lv_obj_t* underline = nullptr;
    int visible_width = 0;
    int fill_width = -1;
    // 0 not yet, 1 sung, 2 being sung: what is painted.
    int painted = -1;
};

enum class Button : int { kAdjust, kDelayLess, kDelayMore, kSmaller, kLarger, kClose };

struct Screen {
    lv_display_t* disp = nullptr;
    int width = 0;
    int height = 0;
    Face current;  // Archivo SemiBold, the line being sung
    Face context;  // Archivo Regular, the lines before and after
    Face small;    // Archivo Regular at 24, the header and the footer
    Face big;      // Archivo SemiBold at 72, the idle screen's name

    lv_obj_t* root = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* clock = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* stage_box = nullptr;
    lv_obj_t* chip_name = nullptr;
    lv_obj_t* chip_line = nullptr;
    lv_obj_t* chip_delay = nullptr;
    lv_obj_t* sheet = nullptr;
    lv_obj_t* sheet_delay = nullptr;
    lv_obj_t* sheet_size = nullptr;

    // The rows of the line on the stage.
    std::vector<WordCell> words;
    std::array<lv_obj_t*, 4> dots{};
    std::size_t dot_count = 0;
    int dots_lit = -1;

    Snapshot snap;
    BoardInfo info;
    Signature sig;
    bool built = false;
    std::uint64_t info_checked_us = 0;
    std::int32_t delay_ms = 0;
    int scale = 1;  // 0 smaller, 1 as chosen, 2 larger
    std::uint32_t sheet_shown_ms = 0;
    bool sheet_visible = false;
    bool filling = false;
    std::string chip_name_text;
    std::string clock_text;
    std::string line_text;
    std::string delay_text;
    std::string size_text;
    int progress_width = -1;

    const char* name = "idle";
    std::uint32_t rebuilds = 0;
    std::uint32_t rebuild_us_max = 0;
};

Screen g;

// ---------------------------------------------------------------------------------------------
// Small helpers

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(time_us() / 1000); }

[[nodiscard]] std::size_t utf8_length(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return count;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

[[nodiscard]] bool had_trailing_space(std::string_view text) {
    return !text.empty() && (text.back() == ' ' || text.back() == '\t');
}

[[nodiscard]] std::string clock_string(std::int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const std::int64_t seconds = ms / 1000;
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%lld:%02lld", static_cast<long long>(seconds / 60),
                  static_cast<long long>(seconds % 60));
    return buffer;
}

[[nodiscard]] std::string delay_string(std::int32_t ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "Delay %+d ms", static_cast<int>(ms));
    return buffer;
}

// The text size step as the sheet says it. The idle screen draws no lyrics to resize, so without
// this the Smaller and Larger buttons showed nothing there (the first run on the kit, 2026-10-20).
[[nodiscard]] const char* size_string(int scale) {
    return scale == 0 ? "Smaller" : scale == 2 ? "Larger" : "Normal";
}

void plain_box(lv_obj_t* object) {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    // The default theme puts a gap between the children of a flex row and column; none is wanted
    // unless a call asks for it.
    lv_obj_set_style_pad_row(object, 0, 0);
    lv_obj_set_style_pad_column(object, 0, 0);
}

lv_obj_t* make_box(lv_obj_t* parent) {
    lv_obj_t* const box = lv_obj_create(parent);
    plain_box(box);
    return box;
}

lv_obj_t* make_label(lv_obj_t* parent, const char* text, const Face& face, std::uint32_t rgb) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, face.latin, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    return label;
}

lv_obj_t* make_label(lv_obj_t* parent, const std::string& text, const Face& face, std::uint32_t rgb) {
    return make_label(parent, text.c_str(), face, rgb);
}

lv_obj_t* make_label(lv_obj_t* parent, const LvString& text, const Face& face, std::uint32_t rgb) {
    return make_label(parent, text.c_str(), face, rgb);
}

[[nodiscard]] int text_width(const std::string& text, const Face& face) {
    lv_point_t size{0, 0};
    lv_text_get_size(&size, text.c_str(), face.latin, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(size.x);
}

// The size the line being sung is drawn at: the largest that keeps it to two rows, then the singer's
// own step on top. A heuristic on the length, because measuring a line at five sizes costs five
// rasterisations of its glyphs for a screen that rebuilds once a few seconds.
[[nodiscard]] int current_size(std::size_t characters, int scale) {
    static constexpr std::array<int, 6> kSizes{72, 64, 56, 48, 40, 34};
    std::size_t index = 1;
    if (characters > 54) {
        index = 4;
    } else if (characters > 44) {
        index = 3;
    } else if (characters > 36) {
        index = 2;
    } else if (characters <= 18) {
        index = 0;
    }
    if (scale == 0) {
        index = std::min<std::size_t>(index + 1, kSizes.size() - 1);
    } else if (scale == 2 && index > 0) {
        --index;
    }
    return kSizes[index];
}

// A singer's name as the file gives it when the fonts can draw it, and V1 or V2 when they cannot (or
// the file gives none).
[[nodiscard]] const char* singer_name(const Snapshot& snap, Singer singer) {
    static std::string first;
    static std::string second;
    static std::string joint;
    first = drawable_title(snap.first_name);
    second = drawable_title(snap.second_name);
    if (first.empty()) {
        first = "V1";
    }
    if (second.empty()) {
        second = "V2";
    }
    switch (singer) {
        case Singer::kFirst:
            return first.c_str();
        case Singer::kSecond:
            return second.c_str();
        case Singer::kBoth:
            joint = first + " + " + second;
            return joint.c_str();
        case Singer::kUnmarked:
            break;
    }
    return "";
}

// ---------------------------------------------------------------------------------------------
// The controls sheet

void on_button(lv_event_t* event) {
    const auto which = static_cast<Button>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
    g.sheet_shown_ms = now_ms();
    switch (which) {
        case Button::kAdjust:
            g.sheet_visible = !g.sheet_visible;
            break;
        case Button::kDelayLess:
            g.delay_ms = std::max(g.delay_ms - kDelayStepMs, -kDelayMaxMs);
            break;
        case Button::kDelayMore:
            g.delay_ms = std::min(g.delay_ms + kDelayStepMs, kDelayMaxMs);
            break;
        case Button::kSmaller:
            g.scale = std::max(g.scale - 1, 0);
            break;
        case Button::kLarger:
            g.scale = std::min(g.scale + 1, 2);
            break;
        case Button::kClose:
            g.sheet_visible = false;
            break;
    }
}

lv_obj_t* make_button(lv_obj_t* parent, const char* text, int w, int h, Button which, bool accent) {
    lv_obj_t* const button = lv_obj_create(parent);
    plain_box(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, color(accent ? kAccent : kSurfaceUp), 0);
    lv_obj_add_event_cb(button, &on_button, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<std::intptr_t>(which)));
    lv_obj_t* const label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, g.small.latin, 0);
    lv_obj_set_style_text_color(label, color(accent ? kBg : kText), 0);
    lv_obj_center(label);
    return button;
}

void build_sheet() {
    // 240 px at the bottom, over the footer and the stage: the targets are 72 px.
    const int sheet_h = 240;
    g.sheet = make_box(g.root);
    lv_obj_set_size(g.sheet, g.width, sheet_h);
    lv_obj_set_pos(g.sheet, 0, g.height - sheet_h);
    lv_obj_set_style_bg_opa(g.sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.sheet, color(kSurface), 0);
    lv_obj_set_style_border_side(g.sheet, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(g.sheet, 2, 0);
    lv_obj_set_style_border_color(g.sheet, color(kAccent), 0);
    lv_obj_add_flag(g.sheet, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* const delay_title = make_label(g.sheet, "Lyric delay", g.small, kMuted);
    lv_obj_set_pos(delay_title, kMargin, 18);
    make_button(g.sheet, "-", 72, 72, Button::kDelayLess, false);
    lv_obj_set_pos(lv_obj_get_child(g.sheet, -1), kMargin, 60);
    g.sheet_delay = make_label(g.sheet, delay_string(g.delay_ms), g.small, kText);
    lv_obj_set_pos(g.sheet_delay, kMargin + 72 + 24, 86);
    make_button(g.sheet, "+", 72, 72, Button::kDelayMore, false);
    lv_obj_set_pos(lv_obj_get_child(g.sheet, -1), kMargin + 72 + 24 + 230, 60);

    lv_obj_t* const size_title = make_label(g.sheet, "Text size", g.small, kMuted);
    lv_obj_set_pos(size_title, 560, 18);
    g.sheet_size = make_label(g.sheet, size_string(g.scale), g.small, kText);
    lv_obj_set_pos(g.sheet_size, 560 + 130, 18);
    make_button(g.sheet, "Smaller", 150, 72, Button::kSmaller, false);
    lv_obj_set_pos(lv_obj_get_child(g.sheet, -1), 560, 60);
    make_button(g.sheet, "Larger", 150, 72, Button::kLarger, false);
    lv_obj_set_pos(lv_obj_get_child(g.sheet, -1), 560 + 150 + 16, 60);

    make_button(g.sheet, "Close", 160, 56, Button::kClose, true);
    lv_obj_set_pos(lv_obj_get_child(g.sheet, -1), g.width - kMargin - 160, sheet_h - 56 - 16);
    lv_obj_add_flag(g.sheet, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------------------------
// The persistent objects

void build_frame() {
    g.root = lv_screen_active();
    plain_box(g.root);
    lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.root, color(kBg), 0);

    // Header: the title at the left, the time at the right, a hairline for the progress under both.
    g.title = make_label(g.root, "", g.small, kText);
    lv_obj_set_width(g.title, g.width - 2 * kMargin - 140);
    lv_obj_set_pos(g.title, kMargin, 18);
    g.clock = make_label(g.root, "", g.small, kMuted);
    lv_obj_set_style_text_align(g.clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(g.clock, 120);
    lv_obj_set_pos(g.clock, g.width - kMargin - 120, 18);
    lv_obj_t* const back = make_box(g.root);
    lv_obj_set_size(back, g.width, 2);
    lv_obj_set_pos(back, 0, kHeaderHeight - 2);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, color(kFaint), 0);
    g.progress = make_box(g.root);
    lv_obj_set_size(g.progress, 0, 2);
    lv_obj_set_pos(g.progress, 0, kHeaderHeight - 2);
    lv_obj_set_style_bg_opa(g.progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.progress, color(kAccent), 0);

    // The stage, which clips what a long line would spill.
    g.stage_box = make_box(g.root);
    lv_obj_set_size(g.stage_box, g.width, g.height - kHeaderHeight - kFooterHeight);
    lv_obj_set_pos(g.stage_box, 0, kHeaderHeight);

    // Footer: the board's name, where in the song, the delay, and the button that opens the sheet.
    lv_obj_t* const footer = make_box(g.root);
    lv_obj_set_size(footer, g.width, kFooterHeight);
    lv_obj_set_pos(footer, 0, g.height - kFooterHeight);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(footer, color(kSurface), 0);
    g.chip_name = make_label(footer, "", g.small, kMuted);
    lv_obj_set_width(g.chip_name, 300);
    lv_obj_set_pos(g.chip_name, kMargin, 18);
    g.chip_line = make_label(footer, "", g.small, kMuted);
    lv_obj_set_width(g.chip_line, 220);
    lv_obj_set_pos(g.chip_line, 360, 18);
    g.chip_delay = make_label(footer, delay_string(g.delay_ms), g.small, kMuted);
    lv_obj_set_width(g.chip_delay, 220);
    lv_obj_set_pos(g.chip_delay, 620, 18);
    make_button(footer, "Adjust", 160, 48, Button::kAdjust, false);
    lv_obj_set_pos(lv_obj_get_child(footer, -1), g.width - kMargin - 160, 8);

    build_sheet();
}

// ---------------------------------------------------------------------------------------------
// The rows

void clear_stage() {
    lv_obj_clean(g.stage_box);
    g.words.clear();
    g.dots.fill(nullptr);
    g.dot_count = 0;
    g.dots_lit = -1;
}

// A context row: a line before or after the current one, with its singer's tag above it when the
// file has singers. `past` fades it out as sung, `next` is the muted colour.
lv_obj_t* make_context_row(lv_obj_t* parent, std::size_t index, bool past, bool duet, int content_w) {
    const LineText* const row = g.snap.line(index);
    if (row == nullptr) {
        return nullptr;
    }
    // A line the fonts cannot draw shows a Latin sentence, which runs left to right whatever it replaces.
    bool undrawable = false;
    const std::string shown = drawable(row->text, &undrawable);
    const bool rtl = !undrawable && iclforge::lyrics::text_is_right_to_left(shown);
    const int indent = (duet && row->singer == Singer::kSecond && !rtl) ? kDuetIndent : 0;
    lv_obj_t* const box = make_box(parent);
    lv_obj_set_width(box, content_w);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(box, indent, 0);
    if (duet && row->singer != Singer::kUnmarked) {
        lv_obj_t* const tag = make_label(box, singer_name(g.snap, row->singer), g.small,
                                         row->singer == Singer::kSecond ? kText : kAccent);
        lv_obj_set_style_text_opa(tag, past ? kSungOpacity : 200, 0);
    }
    lv_obj_t* const label = make_label(box, shown, g.context, past ? kAccent : kMuted);
    if (past) {
        lv_obj_set_style_text_opa(label, kSungOpacity, 0);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, content_w - indent);
    if (rtl) {
        lv_obj_set_style_base_dir(label, LV_BASE_DIR_RTL, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }
    return box;
}

// The words of the line being sung: one cell a word (or a syllable: a file may time the parts of a
// word) in a wrapping row, so the fill can run in the cell's own rectangle. The cells of one word are
// a group the row will not break between. A word's trailing space is the cell's padding, not part of
// its label, so the fill spans the letters and nothing else.
void build_words(lv_obj_t* flow, const LineText& line, bool rtl) {
    const int space = std::max<int>(4, lv_font_get_glyph_width(g.current.latin, ' ', 0));
    g.words.reserve(line.words.size());
    lv_obj_t* group = nullptr;
    for (const LvString& word : line.words) {
        if (group == nullptr) {
            group = make_box(flow);
            lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
            if (rtl) {
                lv_obj_set_style_base_dir(group, LV_BASE_DIR_RTL, 0);
            }
        }
        WordCell cell;
        const std::string visible = drawable(trimmed(word));
        cell.cell = make_box(group);
        lv_obj_set_size(cell.cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        // A part that is only a music note, dropped, has no letters and so no space after it either.
        const bool spaced = had_trailing_space(word) && !visible.empty();
        if (spaced) {
            lv_obj_set_style_pad_right(cell.cell, rtl ? 0 : space, 0);
            lv_obj_set_style_pad_left(cell.cell, rtl ? space : 0, 0);
        }
        cell.base = make_label(cell.cell, visible, g.current, kMuted);
        lv_label_set_long_mode(cell.base, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(cell.base, LV_SIZE_CONTENT);
        cell.visible_width = text_width(visible, g.current);

        // The fill and the underline, made now and not when the word's turn comes: creating them
        // took a frame's time (a measured 80 late cycles in a minute of singing) and a line is
        // rebuilt once. The fill is a clip of no width until its word is being sung.
        const int height = lv_obj_get_height(cell.base);
        cell.clip = make_box(cell.cell);
        lv_obj_set_size(cell.clip, 0, height);
        // LVGL mirrors an alignment under a right-to-left base direction, which the cell has
        // inherited: TOP_LEFT is the word's start in either direction.
        lv_obj_align(cell.clip, LV_ALIGN_TOP_LEFT, 0, 0);
        cell.fill = lv_label_create(cell.clip);
        lv_label_set_text(cell.fill, visible.c_str());
        lv_obj_set_style_text_font(cell.fill, g.current.latin, 0);
        lv_obj_set_style_text_color(cell.fill, color(kAccent), 0);
        lv_label_set_long_mode(cell.fill, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(cell.fill, LV_SIZE_CONTENT);
        lv_obj_align(cell.fill, LV_ALIGN_TOP_LEFT, 0, 0);
        const int thickness = std::max(3, g.current.size / 14);
        cell.underline = make_box(cell.cell);
        lv_obj_set_size(cell.underline, cell.visible_width, thickness);
        lv_obj_set_style_bg_opa(cell.underline, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cell.underline, color(kAccent), 0);
        lv_obj_align(cell.underline, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_flag(cell.underline, LV_OBJ_FLAG_HIDDEN);
        cell.fill_width = 0;

        g.words.push_back(cell);
        if (spaced) {
            group = nullptr;
        }
    }
}

void build_line(const Stage& stage, bool duet, int content_w) {
    const LineText* const line = g.snap.line(stage.line);
    if (line == nullptr) {
        return;
    }
    bool undrawable = false;
    const std::string shown = drawable(line->text, &undrawable);
    const bool rtl = !undrawable && iclforge::lyrics::text_is_right_to_left(shown);
    set_size(g.current, current_size(utf8_length(shown), g.scale));
    const int indent = (duet && line->singer == Singer::kSecond && !rtl) ? kDuetIndent : 0;

    lv_obj_t* const block = make_box(g.stage_box);
    lv_obj_set_width(block, content_w);
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(block, indent, 0);
    lv_obj_set_user_data(block, reinterpret_cast<void*>(1));  // the centre piece

    if (duet && line->singer != Singer::kUnmarked) {
        lv_obj_t* const tag = make_label(block, singer_name(g.snap, line->singer), g.small,
                                         line->singer == Singer::kSecond ? kText : kAccent);
        lv_obj_set_style_text_font(tag, g.small.latin, 0);
    }
    if (stage.word_timed && !line->words.empty() && !undrawable) {
        lv_obj_t* const flow = make_box(block);
        lv_obj_set_width(flow, content_w - indent);
        lv_obj_set_height(flow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(flow, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(flow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(flow, 6, 0);
        if (rtl) {
            lv_obj_set_style_base_dir(flow, LV_BASE_DIR_RTL, 0);
        }
        build_words(flow, *line, rtl);
    } else {
        // Timed as a line only: the whole line, emphasised, and no word invented.
        lv_obj_t* const label = make_label(block, shown, g.current, undrawable ? kMuted : kAccent);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, content_w - indent);
        if (rtl) {
            lv_obj_set_style_base_dir(label, LV_BASE_DIR_RTL, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
}

void build_gap(const Stage& stage, int content_w) {
    lv_obj_t* const block = make_box(g.stage_box);
    lv_obj_set_width(block, content_w);
    lv_obj_set_height(block, 96);
    lv_obj_set_user_data(block, reinterpret_cast<void*>(1));
    if (!stage.instrumental) {
        return;  // a breath between two lines: nothing is drawn where the line was
    }
    // Four dots that go out one a second, so a singer sees the line coming.
    constexpr int kDot = 24;
    constexpr int kSpacing = 20;
    const int row_w = 4 * kDot + 3 * kSpacing;
    const int x0 = (content_w - row_w) / 2;
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        lv_obj_t* const dot = make_box(block);
        lv_obj_set_size(dot, kDot, kDot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, color(kFaint), 0);
        lv_obj_set_pos(dot, x0 + static_cast<int>(i) * (kDot + kSpacing), 12);
        g.dots[i] = dot;
    }
    g.dot_count = g.dots.size();
    lv_obj_t* const label = make_label(block, "Instrumental", g.small, kMuted);
    lv_obj_set_width(label, content_w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, 0, 58);
}

void build_message(const char* first, const char* second, int content_w) {
    lv_obj_t* const block = make_box(g.stage_box);
    lv_obj_set_width(block, content_w);
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 12, 0);
    lv_obj_set_user_data(block, reinterpret_cast<void*>(1));
    set_size(g.current, 56);
    lv_obj_t* const head = make_label(block, first, g.current, kText);
    lv_label_set_long_mode(head, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(head, content_w);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
    if (second != nullptr && second[0] != '\0') {
        lv_obj_t* const tail = make_label(block, second, g.small, kMuted);
        lv_label_set_long_mode(tail, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(tail, content_w);
        lv_obj_set_style_text_align(tail, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void build_idle(int content_w) {
    lv_obj_t* const block = make_box(g.stage_box);
    lv_obj_set_width(block, content_w);
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 16, 0);
    lv_obj_set_user_data(block, reinterpret_cast<void*>(1));

    lv_obj_t* const name = make_label(block, g.info.name.empty() ? "hearth" : g.info.name, g.big, kText);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, content_w);

    std::string where = g.info.address.empty() ? "No network address yet" : g.info.address;
    if (!g.info.address.empty() && g.info.via == "ethernet") {
        where += "   Ethernet";
    } else if (!g.info.network.empty()) {
        where += "   Wi-Fi " + g.info.network;
    }
    make_label(block, where, g.context, kMuted);

    std::string status;
    if (!g.info.pairing_code.empty()) {
        status = "Pairing code " + g.info.pairing_code;
    } else if (!g.info.server.empty() && g.info.connections > 0) {
        status = "Connected to " + g.info.server + ". Waiting for something to play.";
    } else if (g.info.paired > 0) {
        status = "Paired with " + std::to_string(g.info.paired) +
                 (g.info.paired == 1 ? " server" : " servers") + ". Waiting for one to play.";
    } else if (g.info.player) {
        status = "Waiting for a server to pair with.";
    } else {
        status = "No Sendspin player in this build.";
    }
    lv_obj_t* const line = make_label(block, status, g.context, g.info.pairing_code.empty() ? kText : kAccent);
    lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(line, content_w);

    if (!g.info.firmware.empty()) {
        make_label(block, "Firmware " + g.info.firmware, g.small, kFaint);
    }
}

void build_plain(int content_w) {
    lv_obj_t* const scroller = make_box(g.stage_box);
    lv_obj_set_size(scroller, content_w, lv_obj_get_height(g.stage_box) - 16);
    lv_obj_set_pos(scroller, kMargin, 8);
    lv_obj_add_flag(scroller, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroller, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_OFF);
    // A line the fonts cannot draw is the one sentence, as it is elsewhere. The text being laid out is
    // as long as the file's, so it is built in LVGL's heap too.
    LvString shown;
    shown.reserve(g.snap.plain.size());
    for (std::size_t at = 0; at < g.snap.plain.size();) {
        std::size_t end = g.snap.plain.find('\n', at);
        if (end == LvString::npos) {
            end = g.snap.plain.size();
        }
        shown += drawable(std::string_view(g.snap.plain).substr(at, end - at));
        shown += '\n';
        at = end + 1;
    }
    lv_obj_t* const text = make_label(scroller, shown, g.context, kText);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(text, content_w);
    lv_obj_set_user_data(scroller, reinterpret_cast<void*>(2));  // placed already
}

// Lays out what the build made: the centre piece in the middle of the stage, the rows before it
// above and the rows after it below, as the desktop view does.
void place_rows(const std::vector<lv_obj_t*>& past, const std::vector<lv_obj_t*>& next, int content_w) {
    lv_obj_t* centre = nullptr;
    const std::uint32_t children = lv_obj_get_child_count(g.stage_box);
    for (std::uint32_t i = 0; i < children; ++i) {
        lv_obj_t* const child = lv_obj_get_child(g.stage_box, static_cast<int32_t>(i));
        if (reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(child)) == 1) {
            centre = child;
        }
    }
    if (centre == nullptr) {
        return;
    }
    lv_obj_update_layout(g.stage_box);
    const int stage_h = lv_obj_get_height(g.stage_box);
    const int centre_h = lv_obj_get_height(centre);
    const int gap = 18;
    lv_obj_set_pos(centre, kMargin, std::max(0, (stage_h - centre_h) / 2));
    int above = std::max(0, (stage_h - centre_h) / 2) - gap;
    for (auto it = past.rbegin(); it != past.rend(); ++it) {
        lv_obj_set_pos(*it, kMargin, 0);
        lv_obj_update_layout(*it);
        above -= lv_obj_get_height(*it);
        lv_obj_set_y(*it, above);
        above -= 10;
    }
    int below = std::max(0, (stage_h - centre_h) / 2) + centre_h + gap;
    for (lv_obj_t* const row : next) {
        lv_obj_set_pos(row, kMargin, below);
        lv_obj_update_layout(row);
        below += lv_obj_get_height(row) + 10;
    }
    (void)content_w;
}

void rebuild(const Signature& sig) {
    const std::int64_t started = time_us();
    clear_stage();
    const Stage& stage = g.snap.stage;
    const int content_w = g.width - 2 * kMargin;
    const bool duet = g.snap.held && (g.snap.first_name.size() + g.snap.second_name.size() > 0 ||
                                      [&] {
                                          for (std::size_t i = 0; i < stage.before_count; ++i) {
                                              const LineText* const row = g.snap.line(stage.before[i]);
                                              if (row != nullptr && row->singer != Singer::kUnmarked) {
                                                  return true;
                                              }
                                          }
                                          const LineText* const row = g.snap.line(stage.line);
                                          return row != nullptr && row->singer != Singer::kUnmarked;
                                      }());

    std::vector<lv_obj_t*> past;
    std::vector<lv_obj_t*> next;
    const auto context = [&](bool before) {
        const std::size_t count = before ? stage.before_count : stage.after_count;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t index = before ? stage.before[i] : stage.after[i];
            if (lv_obj_t* const row = make_context_row(g.stage_box, index, before, duet, content_w)) {
                (before ? past : next).push_back(row);
            }
        }
    };

    if (sig.idle) {
        g.name = "idle";
        build_idle(content_w);
    } else {
        switch (stage.mode) {
            case StageMode::kLine:
                g.name = "lyrics";
                context(true);
                build_line(stage, duet, content_w);
                context(false);
                break;
            case StageMode::kGap:
                g.name = "gap";
                context(true);
                build_gap(stage, content_w);
                context(false);
                break;
            case StageMode::kWaiting:
                g.name = "waiting";
                build_message("Waiting for the song", "", content_w);
                context(false);
                break;
            case StageMode::kPlain:
                g.name = "plain";
                build_plain(content_w);
                break;
            case StageMode::kNoLyrics:
                g.name = "nolyrics";
                build_message("No lyrics for this item", "The server sent none with it.", content_w);
                break;
        }
    }
    if (std::strcmp(g.name, "plain") != 0) {
        place_rows(past, next, content_w);
    }
    ++g.rebuilds;
    g.rebuild_us_max = std::max<std::uint32_t>(
        g.rebuild_us_max, static_cast<std::uint32_t>(time_us() - started));
}

// ---------------------------------------------------------------------------------------------
// A frame

void paint_word(WordCell& cell, int state, const Stage& stage) {
    if (cell.painted != state) {
        cell.painted = state;
        lv_obj_set_style_text_color(cell.base, color(state == 1 ? kAccent : (state == 2 ? kText : kMuted)), 0);
        if (state == 2) {
            lv_obj_remove_flag(cell.underline, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cell.underline, LV_OBJ_FLAG_HIDDEN);
            if (cell.fill_width != 0) {
                cell.fill_width = 0;
                lv_obj_set_width(cell.clip, 0);
            }
        }
    }
    if (state == 2) {
        const int fill = std::clamp(static_cast<int>(stage.fraction * cell.visible_width + 0.5), 0,
                                    cell.visible_width);
        if (fill != cell.fill_width) {
            cell.fill_width = fill;
            lv_obj_set_width(cell.clip, fill);
        }
    }
}

void update_words(const Stage& stage) {
    g.filling = false;
    for (std::size_t i = 0; i < g.words.size(); ++i) {
        int state = 0;
        if (stage.word && *stage.word == i) {
            state = 2;
            g.filling = true;
        } else if (i < stage.words_sung) {
            state = 1;
        }
        paint_word(g.words[i], state, stage);
    }
}

void update_dots(const Stage& stage) {
    if (g.dot_count == 0) {
        return;
    }
    const int lit = static_cast<int>(stage.count_in);
    if (lit == g.dots_lit) {
        return;
    }
    g.dots_lit = lit;
    // The dots go out from the right as the line approaches.
    for (std::size_t i = 0; i < g.dot_count; ++i) {
        lv_obj_set_style_bg_color(g.dots[i], color(static_cast<int>(i) < lit ? kAccent : kFaint), 0);
    }
}

void update_header_and_footer(const Stage& stage) {
    std::string title = drawable_title(g.snap.title);
    const std::string artist = drawable_title(g.snap.artist);
    if (!artist.empty()) {
        title += title.empty() ? artist : " - " + artist;
    }
    static std::string shown_title = "\x01";
    if (title != shown_title) {
        shown_title = title;
        lv_label_set_text(g.title, title.c_str());
    }
    std::string clock_text;
    if (stage.mode == StageMode::kLine || stage.mode == StageMode::kGap) {
        clock_text = clock_string(stage.position_ms);
        if (g.snap.length_ms) {
            clock_text += " / " + clock_string(*g.snap.length_ms);
        }
    }
    if (clock_text != g.clock_text) {
        g.clock_text = clock_text;
        lv_label_set_text(g.clock, clock_text.c_str());
    }
    int progress = 0;
    if (g.snap.length_ms && *g.snap.length_ms > 0 && (stage.mode == StageMode::kLine || stage.mode == StageMode::kGap)) {
        progress = static_cast<int>(std::clamp<std::int64_t>(
            stage.position_ms * g.width / *g.snap.length_ms, 0, g.width));
    }
    if (progress != g.progress_width) {
        g.progress_width = progress;
        lv_obj_set_width(g.progress, progress);
    }

    std::string line;
    if (stage.mode == StageMode::kLine && g.snap.timed && g.snap.text_line_number > 0) {
        line = "Line " + std::to_string(g.snap.text_line_number) + " of " + std::to_string(g.snap.text_lines);
    }
    if (line != g.line_text) {
        g.line_text = line;
        lv_label_set_text(g.chip_line, line.c_str());
    }
    const std::string delay = delay_string(g.delay_ms);
    if (delay != g.delay_text) {
        g.delay_text = delay;
        lv_label_set_text(g.chip_delay, delay.c_str());
        if (g.sheet_delay != nullptr) {
            lv_label_set_text(g.sheet_delay, delay.c_str());
        }
    }
    const char* const size = size_string(g.scale);
    if (g.size_text != size) {
        g.size_text = size;
        if (g.sheet_size != nullptr) {
            lv_label_set_text(g.sheet_size, size);
        }
    }
}

}  // namespace

bool screen_init(lv_display_t* disp, int width, int height) {
    g.disp = disp;
    g.width = width;
    g.height = height;
    const FontFile regular = archivo_regular();
    const FontFile semibold = archivo_semibold();
    const FontFile greek_regular = noto_sans_greek_cyrillic_regular();
    const FontFile greek_semibold = noto_sans_greek_cyrillic_semibold();
    const FontFile hebrew_regular = noto_sans_hebrew_regular();
    const FontFile hebrew_semibold = noto_sans_hebrew_semibold();
    const bool ok = make_face(g.current, semibold, greek_semibold, hebrew_semibold, 64) &&
                    make_face(g.context, regular, greek_regular, hebrew_regular, 36) &&
                    make_face(g.small, regular, greek_regular, hebrew_regular, 24) &&
                    make_face(g.big, semibold, greek_semibold, hebrew_semibold, 72);
    if (!ok) {
        std::printf("panel: the fonts did not come up\n");
        return false;
    }
    build_frame();
    return true;
}

bool screen_update() {
    // What the player holds, at the delay the singer set here.
    StageOptions options;
    options.lines_before = 1;
    options.lines_after = g.scale == 2 ? 2 : 3;
    const bool player = read_lyrics(g.snap, g.delay_ms, options);
    static_cast<void>(player);

    // The board's own state, twice a second.
    const std::uint64_t now_us = static_cast<std::uint64_t>(time_us());
    if (now_us - g.info_checked_us > 500'000 || !g.built) {
        g.info_checked_us = now_us;
        g.info = read_board_info();
    }

    const Stage& stage = g.snap.stage;
    const bool playing = !g.info.stream.empty() && g.info.stream != "idle";
    // Idle: nothing held, and nothing playing.
    const bool idle = stage.mode == StageMode::kNoLyrics && !playing;

    Signature sig;
    sig.mode = stage.mode;
    sig.idle = idle;
    sig.item = g.snap.item;
    sig.line = stage.mode == StageMode::kLine ? stage.line : 0;
    sig.word_timed = stage.mode == StageMode::kLine && stage.word_timed;
    sig.instrumental = stage.mode == StageMode::kGap && stage.instrumental;
    sig.before_count = stage.before_count;
    sig.after_count = stage.after_count;
    sig.before = stage.before;
    sig.after = stage.after;
    sig.generation = g.snap.generation;
    sig.scale = g.scale;
    // The idle screen says what the board is: a change in it is a rebuild of the idle screen.
    static BoardInfo shown_info;
    const bool info_changed = idle && !(shown_info == g.info);

    if (!g.built || !(sig == g.sig) || info_changed) {
        shown_info = g.info;
        rebuild(sig);
        g.sig = sig;
        g.built = true;
    }

    g.filling = false;
    if (!idle) {
        if (stage.mode == StageMode::kLine && stage.word_timed) {
            update_words(stage);
        } else if (stage.mode == StageMode::kGap) {
            update_dots(stage);
        }
    }
    update_header_and_footer(idle ? Stage{} : stage);
    if (g.chip_name_text != g.info.name) {
        g.chip_name_text = g.info.name;
        lv_label_set_text(g.chip_name, g.chip_name_text.c_str());
    }

    // The sheet: shown on a tap, put away after a few seconds with nothing touched.
    const std::uint32_t now = now_ms();
    if (g.sheet_visible && now - g.sheet_shown_ms > kSheetIdleMs) {
        g.sheet_visible = false;
    }
    if (g.sheet != nullptr) {
        const bool hidden = lv_obj_has_flag(g.sheet, LV_OBJ_FLAG_HIDDEN);
        if (g.sheet_visible && hidden) {
            lv_obj_remove_flag(g.sheet, LV_OBJ_FLAG_HIDDEN);
        } else if (!g.sheet_visible && !hidden) {
            lv_obj_add_flag(g.sheet, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return g.filling;
}

ScreenStats screen_stats() {
    ScreenStats stats;
    stats.name = g.name;
    stats.rebuilds = g.rebuilds;
    stats.rebuild_us_max = g.rebuild_us_max;
    return stats;
}

}  // namespace player::panel
