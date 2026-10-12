// The fonts main/CMakeLists.txt embeds (target_add_binary_data): the apps' own TrueType files.
// See fonts.hpp.

#include "fonts.hpp"

extern "C" {
extern const std::uint8_t _binary_Archivo_Regular_ttf_start[];
extern const std::uint8_t _binary_Archivo_Regular_ttf_end[];
extern const std::uint8_t _binary_Archivo_SemiBold_ttf_start[];
extern const std::uint8_t _binary_Archivo_SemiBold_ttf_end[];
extern const std::uint8_t _binary_NotoSansGreekCyrillic_Regular_ttf_start[];
extern const std::uint8_t _binary_NotoSansGreekCyrillic_Regular_ttf_end[];
extern const std::uint8_t _binary_NotoSansGreekCyrillic_SemiBold_ttf_start[];
extern const std::uint8_t _binary_NotoSansGreekCyrillic_SemiBold_ttf_end[];
extern const std::uint8_t _binary_NotoSansHebrew_Regular_ttf_start[];
extern const std::uint8_t _binary_NotoSansHebrew_Regular_ttf_end[];
extern const std::uint8_t _binary_NotoSansHebrew_SemiBold_ttf_start[];
extern const std::uint8_t _binary_NotoSansHebrew_SemiBold_ttf_end[];
}

namespace player::panel {

FontFile archivo_regular() {
    return {_binary_Archivo_Regular_ttf_start,
            static_cast<std::size_t>(_binary_Archivo_Regular_ttf_end - _binary_Archivo_Regular_ttf_start)};
}

FontFile archivo_semibold() {
    return {_binary_Archivo_SemiBold_ttf_start,
            static_cast<std::size_t>(_binary_Archivo_SemiBold_ttf_end - _binary_Archivo_SemiBold_ttf_start)};
}

FontFile noto_sans_greek_cyrillic_regular() {
    return {_binary_NotoSansGreekCyrillic_Regular_ttf_start,
            static_cast<std::size_t>(_binary_NotoSansGreekCyrillic_Regular_ttf_end -
                                     _binary_NotoSansGreekCyrillic_Regular_ttf_start)};
}

FontFile noto_sans_greek_cyrillic_semibold() {
    return {_binary_NotoSansGreekCyrillic_SemiBold_ttf_start,
            static_cast<std::size_t>(_binary_NotoSansGreekCyrillic_SemiBold_ttf_end -
                                     _binary_NotoSansGreekCyrillic_SemiBold_ttf_start)};
}

FontFile noto_sans_hebrew_regular() {
    return {_binary_NotoSansHebrew_Regular_ttf_start,
            static_cast<std::size_t>(_binary_NotoSansHebrew_Regular_ttf_end -
                                     _binary_NotoSansHebrew_Regular_ttf_start)};
}

FontFile noto_sans_hebrew_semibold() {
    return {_binary_NotoSansHebrew_SemiBold_ttf_start,
            static_cast<std::size_t>(_binary_NotoSansHebrew_SemiBold_ttf_end -
                                     _binary_NotoSansHebrew_SemiBold_ttf_start)};
}

}  // namespace player::panel
