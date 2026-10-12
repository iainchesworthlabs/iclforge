#include "code_pages.hpp"

#include <array>
#include <cstddef>

#include "code_page_convert.hpp"

namespace iclforge::apps {

namespace {

// Name, language group, the page's own name, the Windows number, iconv's name, aliases. The group
// is English and translated by whoever shows it; the title is not. Adding a page is a line here
// and an entry in the test's list of known conversions.
constexpr std::array kPages{
    CodePage{"windows-1252", "Western European", "Windows-1252", 1252, "CP1252", "cp1252 ansi"},
    CodePage{"iso-8859-1", "Western European", "ISO 8859-1", 28591, "ISO-8859-1",
             "latin1 latin-1 iso8859-1 l1"},
    CodePage{"iso-8859-15", "Western European", "ISO 8859-15", 28605, "ISO-8859-15",
             "latin9 latin-9 iso8859-15"},
    CodePage{"cp850", "Western European", "DOS CP850", 850, "CP850", "ibm850 850"},
    CodePage{"cp437", "Western European", "DOS CP437", 437, "CP437", "ibm437 437"},
    CodePage{"windows-1250", "Central European", "Windows-1250", 1250, "CP1250", "cp1250"},
    CodePage{"iso-8859-2", "Central European", "ISO 8859-2", 28592, "ISO-8859-2",
             "latin2 latin-2 iso8859-2"},
    CodePage{"windows-1251", "Cyrillic", "Windows-1251", 1251, "CP1251", "cp1251"},
    CodePage{"koi8-r", "Cyrillic", "KOI8-R", 20866, "KOI8-R", "koi8r"},
    CodePage{"koi8-u", "Cyrillic", "KOI8-U", 21866, "KOI8-U", "koi8u"},
    CodePage{"windows-1253", "Greek", "Windows-1253", 1253, "CP1253", "cp1253"},
    CodePage{"windows-1254", "Turkish", "Windows-1254", 1254, "CP1254", "cp1254"},
    CodePage{"windows-1255", "Hebrew", "Windows-1255", 1255, "CP1255", "cp1255"},
    CodePage{"windows-1256", "Arabic", "Windows-1256", 1256, "CP1256", "cp1256"},
    CodePage{"windows-1257", "Baltic", "Windows-1257", 1257, "CP1257", "cp1257"},
    CodePage{"windows-1258", "Vietnamese", "Windows-1258", 1258, "CP1258", "cp1258"},
    CodePage{"windows-874", "Thai", "Windows-874", 874, "CP874", "cp874 tis-620 tis620"},
    CodePage{"shift-jis", "Japanese", "Shift-JIS", 932, "CP932", "sjis cp932 windows-31j ms932"},
    CodePage{"euc-jp", "Japanese", "EUC-JP", 20932, "EUC-JP", "eucjp"},
    CodePage{"gbk", "Simplified Chinese", "GBK", 936, "CP936", "cp936 gb2312 windows-936"},
    CodePage{"gb18030", "Simplified Chinese", "GB18030", 54936, "GB18030", ""},
    CodePage{"big5", "Traditional Chinese", "Big5", 950, "CP950", "cp950 big-5"},
    CodePage{"euc-kr", "Korean", "EUC-KR", 949, "CP949", "cp949 uhc windows-949"},
};

// ASCII lower case, with `_` and a space read as `-`: the form `name` and `aliases` are written in.
std::string normalised(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == '_' || c == ' ') {
            out.push_back('-');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool has_alias(std::string_view aliases, std::string_view wanted) {
    while (!aliases.empty()) {
        const std::size_t space = aliases.find(' ');
        const std::string_view token = aliases.substr(0, space);
        if (token == wanted) {
            return true;
        }
        if (space == std::string_view::npos) {
            break;
        }
        aliases.remove_prefix(space + 1);
    }
    return false;
}

bool decode_with_page(void* context, std::string_view bytes, std::string& utf8) {
    return code_page_to_utf8(*static_cast<const CodePage*>(context), bytes, utf8);
}

}  // namespace

std::span<const CodePage> code_pages() noexcept {
    return kPages;
}

const CodePage* find_code_page(std::string_view name) noexcept {
    // A name is never long; one that is cannot be a page, and normalising it would allocate.
    if (name.empty() || name.size() > 64) {
        return nullptr;
    }
    const std::string wanted = normalised(name);
    for (const CodePage& page : kPages) {
        if (page.name == wanted || has_alias(page.aliases, wanted)) {
            return &page;
        }
    }
    return nullptr;
}

std::string code_page_names() {
    std::string names;
    for (const CodePage& page : kPages) {
        if (!names.empty()) {
            names += ", ";
        }
        names += page.name;
    }
    return names;
}

bool code_page_available(const CodePage& page) {
    return platform::code_page_installed(page);
}

bool code_page_to_utf8(const CodePage& page, std::string_view bytes, std::string& utf8) {
    return platform::convert_to_utf8(page, bytes, utf8);
}

iclforge::lyrics::TextEncoding lyrics_encoding(const CodePage& page) noexcept {
    // The context is the table entry, which is const; the decoder reads it as const again.
    return iclforge::lyrics::TextEncoding{&decode_with_page, const_cast<CodePage*>(&page)};
}

}  // namespace iclforge::apps
