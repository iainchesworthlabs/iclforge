#pragma once

#include <cstddef>
#include <cstdint>

// The TrueType files the screen draws with (main/panel/fonts, SIL Open Font License): the firmware
// links them into its image (fonts.cpp), a preview on a computer reads them from the source tree.

namespace player::panel {

struct FontFile {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// Archivo, Latin: the line being sung and the idle screen's name.
[[nodiscard]] FontFile archivo_semibold();
// Archivo Regular: the lines around it, the header and the footer.
[[nodiscard]] FontFile archivo_regular();
// Noto Sans Greek and Cyrillic, at the two weights: a static, subset instance of Noto Sans'
// variable font (tiny_ttf has no axes), behind each Archivo, for Greek, Russian and the other
// Cyrillic languages.
[[nodiscard]] FontFile noto_sans_greek_cyrillic_regular();
[[nodiscard]] FontFile noto_sans_greek_cyrillic_semibold();
// Noto Sans Hebrew, at the two weights, a static instance of the variable font the apps bundle
// (tiny_ttf has no axes): behind those, for Hebrew and Yiddish.
[[nodiscard]] FontFile noto_sans_hebrew_regular();
[[nodiscard]] FontFile noto_sans_hebrew_semibold();

}  // namespace player::panel
