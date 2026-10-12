#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "code_pages.hpp"

// apps/shared/text-encoding: the table of code pages, the operating system's conversion of each to
// UTF-8, and a page handed to libs/lyrics as the decoder it asks a program for. The conversion is
// Windows' (MultiByteToWideChar) or iconv's, so what is held here is the same on every leg: known
// bytes in a page become the text a person would write, and bytes the page does not have are
// refused. Bytes are written as numbers and the text as code points, so that this file is ASCII.

namespace {

using iclforge::apps::CodePage;
using iclforge::apps::code_page_available;
using iclforge::apps::code_page_to_utf8;
using iclforge::apps::code_pages;
using iclforge::apps::find_code_page;
using iclforge::apps::lyrics_encoding;

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int value : values) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    return out;
}

// The UTF-8 of a list of code points.
std::string utf8(std::initializer_list<std::uint32_t> points) {
    std::string out;
    for (const std::uint32_t cp : points) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

struct Known {
    std::string_view page;
    std::string input;
    std::string expected;
};

// One conversion a person would recognise, per page of the table: a word, in the page's own
// bytes, and its code points.
const std::vector<Known>& known() {
    static const std::vector<Known> list{
        {"windows-1252", bytes({'c', 'a', 'f', 0xE9, ' ', 0x80}), utf8({'c', 'a', 'f', 0xE9, ' ', 0x20AC})},
        {"iso-8859-1", bytes({'c', 'a', 'f', 0xE9}), utf8({'c', 'a', 'f', 0xE9})},
        {"iso-8859-15", bytes({0xA4, '5'}), utf8({0x20AC, '5'})},
        {"cp850", bytes({0x82, 0x81}), utf8({0xE9, 0xFC})},
        {"cp437", bytes({0x82, 0x81}), utf8({0xE9, 0xFC})},
        {"windows-1250", bytes({0xE8, 0x9A}), utf8({0x10D, 0x161})},
        {"iso-8859-2", bytes({0xE8, 0xB9}), utf8({0x10D, 0x161})},
        {"windows-1251", bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2}),
         utf8({0x41F, 0x440, 0x438, 0x432, 0x435, 0x442})},
        {"koi8-r", bytes({0xF0, 0xD2, 0xC9, 0xD7, 0xC5, 0xD4}),
         utf8({0x41F, 0x440, 0x438, 0x432, 0x435, 0x442})},
        {"koi8-u", bytes({0xA4, 0xA6}), utf8({0x454, 0x456})},
        {"windows-1253", bytes({0xE1, 0xE2, 0xE3}), utf8({0x3B1, 0x3B2, 0x3B3})},
        {"windows-1254", bytes({0xFE, 0xF0}), utf8({0x15F, 0x11F})},
        {"windows-1255", bytes({0xF9, 0xEC, 0xE5, 0xED}), utf8({0x5E9, 0x5DC, 0x5D5, 0x5DD})},
        {"windows-1256", bytes({0xE3, 0xD1, 0xCD, 0xC8, 0xC7}),
         utf8({0x645, 0x631, 0x62D, 0x628, 0x627})},
        {"windows-1257", bytes({0xE8, 0xFE}), utf8({0x10D, 0x17E})},
        {"windows-1258", bytes({0xE0, 0xE3}), utf8({0xE0, 0x103})},
        {"windows-874", bytes({0xCA, 0xC7, 0xD1, 0xCA, 0xB4, 0xD5}),
         utf8({0xE2A, 0xE27, 0xE31, 0xE2A, 0xE14, 0xE35})},
        {"shift-jis", bytes({0x82, 0xA0, 0x82, 0xA2}), utf8({0x3042, 0x3044})},
        {"euc-jp", bytes({0xA4, 0xA2, 0xA4, 0xA4}), utf8({0x3042, 0x3044})},
        {"gbk", bytes({0xC4, 0xE3, 0xBA, 0xC3}), utf8({0x4F60, 0x597D})},
        {"gb18030", bytes({0xC4, 0xE3, 0xBA, 0xC3}), utf8({0x4F60, 0x597D})},
        {"big5", bytes({0xA7, 0x41, 0xA6, 0x6E}), utf8({0x4F60, 0x597D})},
        {"euc-kr", bytes({0xBE, 0xC8, 0xB3, 0xE7}), utf8({0xC548, 0xB155})},
    };
    return list;
}

}  // namespace

TEST_CASE("code pages: every name and alias finds its own page, and only that one", "[text-encoding]") {
    REQUIRE(code_pages().size() > 10);
    std::set<std::string> seen;
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        CHECK(find_code_page(page.name) == &page);
        CHECK(seen.insert(std::string{page.name}).second);
        CHECK_FALSE(page.group.empty());
        CHECK_FALSE(page.title.empty());
        CHECK_FALSE(page.iconv_name.empty());
        CHECK(page.windows_id != 0);
        std::string_view aliases = page.aliases;
        while (!aliases.empty()) {
            const std::size_t space = aliases.find(' ');
            const std::string alias{aliases.substr(0, space)};
            INFO(alias);
            CHECK(find_code_page(alias) == &page);
            CHECK(seen.insert(alias).second);
            if (space == std::string_view::npos) {
                break;
            }
            aliases.remove_prefix(space + 1);
        }
    }
}

TEST_CASE("code pages: a name is read without regard to case, underscores or spaces", "[text-encoding]") {
    const CodePage* sjis = find_code_page("shift-jis");
    REQUIRE(sjis != nullptr);
    CHECK(find_code_page("Shift_JIS") == sjis);
    CHECK(find_code_page("SHIFT JIS") == sjis);
    CHECK(find_code_page("SJIS") == sjis);
    CHECK(find_code_page("CP932") == sjis);

    const CodePage* western = find_code_page("windows-1252");
    REQUIRE(western != nullptr);
    CHECK(find_code_page("Windows_1252") == western);
    CHECK(find_code_page("cp1252") == western);

    CHECK(find_code_page("Latin1") == find_code_page("iso-8859-1"));
    CHECK(find_code_page("GB2312") == find_code_page("gbk"));
}

TEST_CASE("code pages: a name that is no page is not found", "[text-encoding]") {
    CHECK(find_code_page("") == nullptr);
    CHECK(find_code_page("utf-8") == nullptr);
    CHECK(find_code_page("utf-16") == nullptr);
    CHECK(find_code_page("windows-1252x") == nullptr);
    CHECK(find_code_page("shift") == nullptr);
    CHECK(find_code_page(std::string(200, 'a')) == nullptr);
}

TEST_CASE("code pages: the names are listed in the picker's order", "[text-encoding]") {
    const std::string names = iclforge::apps::code_page_names();
    CHECK(names.starts_with("windows-1252, "));
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        CHECK(names.find(page.name) != std::string::npos);
    }
}

TEST_CASE("code pages: every page of the table is carried by this system", "[text-encoding]") {
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        CHECK(code_page_available(page));
    }
}

TEST_CASE("code pages: known bytes become the text a person would write", "[text-encoding]") {
    for (const Known& item : known()) {
        INFO(item.page);
        const CodePage* page = find_code_page(item.page);
        REQUIRE(page != nullptr);
        std::string out = "left over";
        REQUIRE(code_page_to_utf8(*page, item.input, out));
        CHECK(out == item.expected);
    }
}

TEST_CASE("code pages: every page of the table has a known conversion held here", "[text-encoding]") {
    std::set<std::string_view> held;
    for (const Known& item : known()) {
        held.insert(item.page);
    }
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        CHECK(held.count(page.name) == 1);
    }
}

TEST_CASE("code pages: ASCII, and a line end, are the same in every page", "[text-encoding]") {
    const std::string ascii = "[00:12.50]Hello, world 123\r\nSecond line\n";
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        std::string out;
        REQUIRE(code_page_to_utf8(page, ascii, out));
        CHECK(out == ascii);
    }
}

TEST_CASE("code pages: nothing in is nothing out", "[text-encoding]") {
    for (const CodePage& page : code_pages()) {
        INFO(page.name);
        std::string out = "left over";
        REQUIRE(code_page_to_utf8(page, std::string_view{}, out));
        CHECK(out.empty());
    }
}

TEST_CASE("code pages: bytes a page does not have are refused and leave nothing", "[text-encoding]") {
    struct Bad {
        std::string_view page;
        std::string input;
    };
    const Bad cases[] = {
        // A lead byte with its trail missing at the end of the text.
        {"shift-jis", bytes({'a', 0x82})},
        {"gbk", bytes({'a', 0x81})},
        {"big5", bytes({'a', 0xA4})},
        {"euc-kr", bytes({'a', 0xB0})},
        {"euc-jp", bytes({'a', 0xA4})},
        // A lead byte with a trail that is not one.
        {"shift-jis", bytes({'a', 0x82, 0x20})},
        {"gbk", bytes({'a', 0x81, 0x20})},
        {"big5", bytes({'a', 0xA4, 0x20})},
    };
    for (const Bad& item : cases) {
        INFO(item.page);
        const CodePage* page = find_code_page(item.page);
        REQUIRE(page != nullptr);
        std::string out = "left over";
        CHECK_FALSE(code_page_to_utf8(*page, item.input, out));
        CHECK(out.empty());
    }
}

TEST_CASE("code pages: a text longer than any buffer is converted whole", "[text-encoding]") {
    // 20000 two-byte characters, so the conversion fills its buffer many times over.
    std::string input;
    std::string expected;
    for (int i = 0; i < 20000; ++i) {
        input += bytes({0x82, 0xA0});
        expected += utf8({0x3042});
    }
    std::string out;
    REQUIRE(code_page_to_utf8(*find_code_page("shift-jis"), input, out));
    CHECK(out == expected);
}

TEST_CASE("code pages: a page is the decoder libs/lyrics asks for", "[text-encoding]") {
    const CodePage* sjis = find_code_page("shift-jis");
    REQUIRE(sjis != nullptr);
    const auto encoding = lyrics_encoding(*sjis);
    REQUIRE(encoding.decoder != nullptr);

    // A Shift-JIS LRC file: refused without a page, read with one.
    const std::string file = "[ti:" + bytes({0x82, 0xA0}) + "]\n[00:01.00]" + bytes({0x82, 0xA0, 0x82, 0xA2}) +
                             "\n[00:03.00]Hello\n";
    const auto refused = iclforge::lyrics::parse_lyrics(file);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("not UTF-8") != std::string::npos);

    const auto read = iclforge::lyrics::parse_lyrics(file, std::nullopt, encoding);
    REQUIRE(read.has_value());
    REQUIRE(read->lines.size() == 2);
    CHECK(read->lines[0].text == utf8({0x3042, 0x3044}));
    CHECK(read->lines[1].text == "Hello");
}

TEST_CASE("code pages: a file that is UTF-8 is read as UTF-8 whatever page is named", "[text-encoding]") {
    const auto encoding = lyrics_encoding(*find_code_page("windows-1251"));
    const std::string file = "[00:01.00]Caf" + utf8({0xE9}) + "\n";
    const auto read = iclforge::lyrics::parse_lyrics(file, std::nullopt, encoding);
    REQUIRE(read.has_value());
    REQUIRE(read->lines.size() == 1);
    CHECK(read->lines[0].text == "Caf" + utf8({0xE9}));
}

TEST_CASE("code pages: bytes the named page does not have are an error naming the page", "[text-encoding]") {
    const auto encoding = lyrics_encoding(*find_code_page("shift-jis"));
    const std::string file = "[00:01.00]ab" + bytes({0x82}) + "\n";
    const auto read = iclforge::lyrics::parse_lyrics(file, std::nullopt, encoding);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().find("code page") != std::string::npos);
}
