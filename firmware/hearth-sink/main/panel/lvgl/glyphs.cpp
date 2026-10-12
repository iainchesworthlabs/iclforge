#include "glyphs.hpp"

#include <array>
#include <span>

#include "iclforge/lyrics/stage.hpp"

namespace player::panel {

namespace {

using iclforge::lyrics::CodePointRange;

// The code points the four font files (Archivo Regular and SemiBold, Noto Sans Hebrew Regular and
// SemiBold) all carry, as ranges: read from their `cmap` tables on 2026-10-12 with fontTools,
//
//   for each file: TTFont(file).getBestCmap(); intersect the two weights of a family; union the
//   families; drop the private-use block and the controls; join adjacent code points.
//
// Archivo is Latin: Basic, Latin-1, Extended-A, most of Extended-B and the Vietnamese block, with the
// punctuation and the currency signs a lyric uses. It has no Cyrillic and no Greek (four letters of
// it). Noto Sans Hebrew adds the Hebrew block and its presentation forms. Change a font file and
// this table is regenerated.
constexpr std::array kCovered = std::to_array<CodePointRange>({
    {0x0020, 0x007E}, {0x00A0, 0x017F}, {0x018F, 0x018F}, {0x0192, 0x0192},
    {0x019D, 0x019D}, {0x01A0, 0x01A1}, {0x01AF, 0x01B0}, {0x01C4, 0x01DC},
    {0x01E6, 0x01E7}, {0x01EA, 0x01EB}, {0x01FA, 0x021B}, {0x022A, 0x022D},
    {0x0230, 0x0233}, {0x0237, 0x0237}, {0x0259, 0x0259}, {0x0272, 0x0272},
    {0x02B9, 0x02BA}, {0x02BC, 0x02BC}, {0x02C6, 0x02C7}, {0x02C9, 0x02C9},
    {0x02D8, 0x02DD}, {0x0300, 0x0304}, {0x0306, 0x030C}, {0x030F, 0x030F},
    {0x0311, 0x0313}, {0x031B, 0x031B}, {0x0323, 0x0324}, {0x0326, 0x0328},
    {0x032E, 0x032E}, {0x0331, 0x0331}, {0x0335, 0x0338}, {0x034F, 0x034F},
    {0x0394, 0x0394}, {0x03A9, 0x03A9}, {0x03BC, 0x03BC}, {0x03C0, 0x03C0},
    {0x0591, 0x05C7}, {0x05D0, 0x05EA}, {0x05EF, 0x05F4}, {0x1E24, 0x1E25},
    {0x1E62, 0x1E63}, {0x1E6C, 0x1E6D}, {0x1E80, 0x1E85}, {0x1E9E, 0x1E9E},
    {0x1EA0, 0x1EF9}, {0x2009, 0x2009}, {0x200C, 0x2011}, {0x2013, 0x2015},
    {0x2017, 0x201A}, {0x201C, 0x201E}, {0x2020, 0x2022}, {0x2026, 0x2026},
    {0x2030, 0x2030}, {0x2032, 0x2033}, {0x2039, 0x203A}, {0x203E, 0x203E},
    {0x2044, 0x2044}, {0x2052, 0x2052}, {0x2070, 0x2070}, {0x2074, 0x2079},
    {0x207F, 0x2089}, {0x2099, 0x2099}, {0x20A1, 0x20A1}, {0x20A3, 0x20A4},
    {0x20A6, 0x20A7}, {0x20A9, 0x20AD}, {0x20B1, 0x20B2}, {0x20B5, 0x20B5},
    {0x20B9, 0x20BA}, {0x20BC, 0x20BD}, {0x2105, 0x2105}, {0x2113, 0x2113},
    {0x2116, 0x2117}, {0x2122, 0x2122}, {0x2126, 0x2126}, {0x212E, 0x212E},
    {0x2190, 0x2195}, {0x21A8, 0x21A8}, {0x2202, 0x2202}, {0x2205, 0x2206},
    {0x220F, 0x220F}, {0x2211, 0x2212}, {0x2215, 0x2215}, {0x2219, 0x221A},
    {0x221E, 0x221F}, {0x2229, 0x2229}, {0x222B, 0x222B}, {0x2248, 0x2248},
    {0x2260, 0x2261}, {0x2264, 0x2265}, {0x2302, 0x2302}, {0x2310, 0x2310},
    {0x2320, 0x2321}, {0x25CA, 0x25CA}, {0x25CC, 0x25CC}, {0x27E8, 0x27E9},
    {0xFB01, 0xFB02}, {0xFB1D, 0xFB36}, {0xFB38, 0xFB3C}, {0xFB3E, 0xFB3E},
    {0xFB40, 0xFB41}, {0xFB43, 0xFB44}, {0xFB46, 0xFB4F}, {0xFEFF, 0xFEFF},
});

// The music notes a lyric file may carry (U+2669 to U+266C), which neither font has, as UTF-8.
[[nodiscard]] bool is_note(std::string_view text, std::size_t at, std::size_t& length) {
    // U+2669 to U+266C encode as E2 99 A9 to E2 99 AC.
    if (at + 2 < text.size() && static_cast<unsigned char>(text[at]) == 0xE2 &&
        static_cast<unsigned char>(text[at + 1]) == 0x99) {
        const auto last = static_cast<unsigned char>(text[at + 2]);
        if (last >= 0xA9 && last <= 0xAC) {
            length = 3;
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string without_notes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        std::size_t length = 1;
        if (is_note(text, at, length)) {
            at += length;
            continue;
        }
        out.push_back(text[at]);
        ++at;
    }
    // The spaces a dropped note leaves at either end.
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

}  // namespace

bool is_drawable(std::string_view text) {
    return iclforge::lyrics::text_is_covered(without_notes(text), std::span<const CodePointRange>(kCovered));
}

std::string drawable(std::string_view text, bool* replaced) {
    std::string clean = without_notes(text);
    const bool covered = iclforge::lyrics::text_is_covered(clean, std::span<const CodePointRange>(kCovered));
    if (replaced != nullptr) {
        *replaced = !covered;
    }
    return covered ? clean : std::string(kUndrawableLine);
}

std::string drawable_title(std::string_view text) {
    std::string clean = without_notes(text);
    return iclforge::lyrics::text_is_covered(clean, std::span<const CodePointRange>(kCovered))
               ? clean
               : std::string();
}

}  // namespace player::panel
