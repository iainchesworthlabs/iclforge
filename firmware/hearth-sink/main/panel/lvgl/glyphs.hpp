#pragma once

#include <string>
#include <string_view>

// What the panel's fonts can draw (planning/karaoke-p4x-panel.md, D3). A line with a letter they do not
// have would be drawn as one missing-glyph box a letter; the screen shows a sentence in its place
// instead (the sentence the Hearth family's shared sink-screen spec pins), a title in such a script is
// left out, and a music note, which none of the fonts has, is dropped from a line.
//
// Plain C++ with nothing of LVGL or ESP-IDF in it, so that the preview builds it.

namespace player::panel {

// The sentence that stands for a line the fonts cannot draw.
inline constexpr std::string_view kUndrawableLine = "This line is in a script this screen cannot show.";

// `text` as a label should show it: without its music notes (and the spaces they leave at an end), or,
// when it has a letter the fonts do not carry, kUndrawableLine; `*replaced` says which.
[[nodiscard]] std::string drawable(std::string_view text, bool* replaced = nullptr);

// A title: the same, and empty where it cannot be drawn.
[[nodiscard]] std::string drawable_title(std::string_view text);

// Whether `text` is drawable as it stands (with its notes dropped).
[[nodiscard]] bool is_drawable(std::string_view text);

}  // namespace player::panel
