# ---------------------------------------------------------------------------
# iclforge::app_text_encoding (planning/karaoke-formats-options.md, D16): the legacy code pages a
# lyrics file can be read in, converted to UTF-8 by the operating system's own conversion, as the
# decoder libs/lyrics asks a program for. Internal: forge's `lyrics-encoding=` and the Forge GUI's
# Lyrics row link it, and it is not installed or packaged.
#
# A library of its own, and not compiled straight into each program as apps/shared/media is, for
# the platform half: the conversion is one src/platform/<os>/code_page_convert.cpp, chosen here
# and not by an #ifdef, and each consumer would otherwise repeat the choice and the iconv link.
# ---------------------------------------------------------------------------
add_library(iclforge_app_text_encoding STATIC src/code_pages.cpp)
add_library(iclforge::app_text_encoding ALIAS iclforge_app_text_encoding)
target_include_directories(iclforge_app_text_encoding PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(iclforge_app_text_encoding PUBLIC cxx_std_23)
target_link_libraries(iclforge_app_text_encoding
    PUBLIC iclforge::lyrics
    PRIVATE iclforge::warnings iclforge::coverage)

if(WIN32)
    # MultiByteToWideChar / WideCharToMultiByte: Windows carries the tables of every page here.
    target_sources(iclforge_app_text_encoding PRIVATE src/platform/windows/code_page_convert.cpp)
    target_compile_definitions(iclforge_app_text_encoding PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(ANDROID)
    # Bionic has iconv from API 28 and the Android builds are for API 26: no conversion, said as such
    # (src/platform/android/code_page_convert.cpp). find_package(Iconv) would find no library.
    target_sources(iclforge_app_text_encoding PRIVATE src/platform/android/code_page_convert.cpp)
else()
    # iconv: in the C library on Linux (glibc, musl) and in libiconv on macOS, which
    # find_package(Iconv) links.
    find_package(Iconv REQUIRED)
    target_sources(iclforge_app_text_encoding PRIVATE src/platform/posix/code_page_convert.cpp)
    target_link_libraries(iclforge_app_text_encoding PRIVATE Iconv::Iconv)
endif()
