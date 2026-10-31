// What a Hearth board may be called, tested on the host - see iclforge/device_name.hpp's own header
// comment: the check every route and screen holds a name to, the host name a name answers to, and the
// on-screen keyboard's keys and editor.

#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/device_name.hpp"

using namespace iclforge::devname;

namespace {

// The code points the panel's fonts carry in the Latin range (panel/lvgl/glyphs.cpp, kCovered): Basic
// Latin, then Latin-1 and Latin Extended-A in one run, then the rows the keyboard's extra letters use.
bool font_has(char32_t point) {
    return (point >= 0x20 && point <= 0x7E) || (point >= 0xA0 && point <= 0x17F) || (point >= 0x1FA && point <= 0x21B) ||
           point == 0x1E9E;
}

std::vector<char32_t> points(std::string_view text) {
    std::vector<char32_t> out;
    for (std::size_t at = 0; at < text.size();) {
        out.push_back(next_code_point(text, at));
    }
    return out;
}

}  // namespace

TEST_CASE("a name is 1 to 32 bytes of text with nothing invisible and no space at either end", "[devname]") {
    CHECK(check_name("hearth-a1b2c3").ok());
    CHECK(check_name("Sitting room").ok());
    CHECK(check_name("Dad's study (TV) #2").ok());
    CHECK(check_name("K\xC3\xBC" "che").ok());  // Kuche with an umlaut
    CHECK(check_name(std::string(32, 'x')).ok());

    CHECK(check_name("").problem == Problem::empty);
    CHECK(check_name(std::string(33, 'x')).problem == Problem::too_long);
    CHECK(check_name(" Attic").problem == Problem::edge_space);
    CHECK(check_name("Attic ").problem == Problem::edge_space);
    CHECK(check_name(" ").problem == Problem::edge_space);
    CHECK(check_name("At\ntic").problem == Problem::invisible);
    CHECK(check_name("At\ttic").problem == Problem::invisible);
    CHECK(check_name(std::string("At\0tic", 6)).problem == Problem::invisible);
    CHECK(check_name("At\x7F" "tic").problem == Problem::invisible);
    CHECK(check_name("At\xC2\x85tic").problem == Problem::invisible);          // U+0085, a C1 control
    CHECK(check_name("At\xE2\x80\xA8tic").problem == Problem::invisible);      // U+2028 line separator
    CHECK(check_name("At\xE2\x80\xAEtic").problem == Problem::invisible);      // U+202E right-to-left override
    CHECK(check_name("At\xEF\xBB\xBFtic").problem == Problem::invisible);      // a byte order mark
    CHECK(check_name("At\xE2\x80\x8Btic").problem == Problem::invisible);      // a zero-width space
}

TEST_CASE("a name that is not well-formed UTF-8 is refused", "[devname]") {
    CHECK(check_name("At\x80tic").problem == Problem::bad_utf8);        // a stray continuation byte
    CHECK(check_name("At\xC3").problem == Problem::bad_utf8);           // cut short
    CHECK(check_name("At\xC0\xAFtic").problem == Problem::bad_utf8);    // an overlong slash
    CHECK(check_name("At\xE0\x80\xAFtic").problem == Problem::bad_utf8);  // an overlong, three bytes
    CHECK(check_name("At\xED\xA0\x80tic").problem == Problem::bad_utf8);  // a surrogate
    CHECK(check_name("At\xF4\x90\x80\x80").problem == Problem::bad_utf8);  // past U+10FFFF
    CHECK(check_name("At\xFFtic").problem == Problem::bad_utf8);
    // Four-byte text is well-formed (an emoji is a name a person might give), and counts four bytes.
    CHECK(check_name("Den \xF0\x9F\x8E\xB5").ok());
}

TEST_CASE("the limit is bytes, so an accented letter costs two", "[devname]") {
    // Sixteen two-byte letters fill the 32 bytes exactly.
    std::string full;
    for (int i = 0; i < 16; ++i) {
        full += "\xC3\xA9";
    }
    CHECK(check_name(full).ok());
    CHECK(check_name(full + "x").problem == Problem::too_long);
}

TEST_CASE("every problem has words for the screen and the route", "[devname]") {
    CHECK(std::string(problem_text(Problem::none)).empty());
    for (const Problem problem :
         {Problem::empty, Problem::too_long, Problem::bad_utf8, Problem::invisible, Problem::edge_space}) {
        CHECK(std::string(problem_text(problem)).size() > 10);
    }
}

TEST_CASE("code points round-trip through encode() and next_code_point()", "[devname]") {
    for (const char32_t point : {char32_t{0x41}, char32_t{0xE9}, char32_t{0x17F}, char32_t{0x1E9E}, char32_t{0x1F3B5}}) {
        const std::string text = encode(point);
        std::size_t at = 0;
        CHECK(next_code_point(text, at) == point);
        CHECK(at == text.size());
    }
}

TEST_CASE("the host name is the name in lower case with hyphens between words", "[devname]") {
    // What discovery.cpp has always made of an ASCII name.
    CHECK(mdns_label("hearth-a1b2c3") == "hearth-a1b2c3");
    CHECK(mdns_label("Sitting Room") == "sitting-room");
    CHECK(mdns_label("  Sitting   Room  ") == "sitting-room");
    CHECK(mdns_label("Dad's study (TV) #2") == "dad-s-study-tv-2");
    CHECK(mdns_label("--Attic--") == "attic");
    CHECK(mdns_label("A") == "a");
    CHECK(mdns_label("room 2") == "room-2");
}

TEST_CASE("a Latin letter in the host name is the ASCII it is spelt with", "[devname]") {
    CHECK(mdns_label("Caf\xC3\xA9") == "cafe");
    CHECK(mdns_label("K\xC3\xBC" "che") == "kuche");
    CHECK(mdns_label("Stra\xC3\x9F" "e") == "strasse");
    CHECK(mdns_label("\xC3\x86ther") == "aether");
    CHECK(mdns_label("\xC3\x98" "l") == "ol");
    CHECK(mdns_label("\xC5\x81\xC3\xB3" "d\xC5\xBA") == "lodz");  // Łódź
    CHECK(mdns_label("\xC5\x9E" "ark\xC4\xB1") == "sarki");       // Şarkı
    CHECK(mdns_label("\xC5\x92uvre") == "oeuvre");
    CHECK(mdns_label("\xC3\x9E" "or") == "thor");
}

TEST_CASE("a name with nothing a host name can hold is 'hearth'", "[devname]") {
    CHECK(mdns_label("") == "hearth");
    CHECK(mdns_label("!!!") == "hearth");
    CHECK(mdns_label("\xE2\x99\xAB") == "hearth");  // a music note
    CHECK(mdns_label("\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F") == "hearth");  // Cyrillic: no spelling out here
    CHECK(mdns_label("\xFF\xFE") == "hearth");
}

TEST_CASE("the host name keeps words apart where the name has a letter the label cannot hold", "[devname]") {
    CHECK(mdns_label("Den \xE2\x99\xAB Room") == "den-room");
    CHECK(mdns_label("a\xC3\x97" "b") == "a-b");  // a multiplication sign
}

TEST_CASE("a host name is at most a label long", "[devname]") {
    const std::string label = mdns_label(std::string(200, 'a'));
    CHECK(label.size() == 63);
    // Cut at a hyphen, the hyphen goes too.
    const std::string cut = mdns_label(std::string(62, 'a') + " b");
    CHECK(cut == std::string(62, 'a'));
}

TEST_CASE("every key the keyboard offers is a name on its own and a letter the fonts carry", "[devname]") {
    std::set<std::string> seen;
    for (const Page page : {Page::lower, Page::upper, Page::symbols, Page::accents_latin1, Page::accents_extended}) {
        for (const bool shifted : {false, true}) {
            for (const std::string& key : page_keys(page, shifted)) {
                INFO("key " << key);
                CHECK(check_name(key).ok());
                CHECK(is_offered(key));
                const std::vector<char32_t> p = points(key);
                REQUIRE(p.size() == 1);
                CHECK(font_has(p[0]));
                seen.insert(key);
            }
        }
    }
    // 26 + 26 + 10 + 11 symbols + 31 + 29 accents in lower case, and the upper forms the pairs add.
    CHECK(seen.size() >= 26 + 26 + 10 + 11 + 31 + 29);
}

TEST_CASE("an accent key's two cases are the right pair", "[devname]") {
    // Each upper form is one code point from the same row of the font; lower and upper differ.
    for (const AccentPair& pair : kAccentedLatin1) {
        CHECK(std::string(pair.lower) != std::string(pair.upper));
        CHECK(points(pair.lower).size() == 1);
        CHECK(points(pair.upper).size() == 1);
    }
    for (const AccentPair& pair : kAccentedExtended) {
        CHECK(points(pair.lower).size() == 1);
        CHECK(points(pair.upper).size() == 1);
    }
    // Spot checks: e acute / E acute, y diaeresis / Y diaeresis, sharp s / capital sharp s.
    CHECK(std::string(kAccentedLatin1[9].lower) == "\xC3\xA9");
    CHECK(std::string(kAccentedLatin1[9].upper) == "\xC3\x89");
    CHECK(std::string(kAccentedLatin1[30].lower) == "\xC3\xBF");
    CHECK(std::string(kAccentedLatin1[30].upper) == "\xC5\xB8");
    CHECK(std::string(kAccentedExtended[27].lower) == "\xC3\x9F");
    CHECK(std::string(kAccentedExtended[27].upper) == "\xE1\xBA\x9E");
}

TEST_CASE("a key the keyboard does not offer is not offered", "[devname]") {
    CHECK_FALSE(is_offered(""));
    CHECK_FALSE(is_offered("ab"));
    CHECK_FALSE(is_offered("\n"));
    CHECK_FALSE(is_offered("\""));
    CHECK_FALSE(is_offered("\\"));
    CHECK_FALSE(is_offered("/"));
    CHECK_FALSE(is_offered("\xC3\xB7"));          // a division sign
    CHECK_FALSE(is_offered("\xD0\x96"));          // Cyrillic
    CHECK_FALSE(is_offered("\xE2\x99\xAB"));      // a music note
    CHECK(is_offered(" "));
    CHECK(is_offered("-"));
    CHECK(is_offered("\xC3\xA9"));
}

TEST_CASE("the editor starts on the name now, or empty when that is not a name", "[devname]") {
    CHECK(Editor("Attic").text() == "Attic");
    CHECK(Editor("").text().empty());
    CHECK(Editor(" Attic").text().empty());
    CHECK(Editor("At\xFFtic").text().empty());
    // Capitals first on an empty name, lower case after one.
    CHECK(Editor("").page() == Page::upper);
    CHECK(Editor("Attic").page() == Page::lower);
}

TEST_CASE("typing a capital returns the keyboard to lower case", "[devname]") {
    Editor editor("");
    REQUIRE(editor.page() == Page::upper);
    CHECK(editor.press("K"));
    CHECK(editor.page() == Page::lower);
    CHECK(editor.press("i"));
    CHECK(editor.press("t"));
    CHECK(editor.text() == "Kit");
    // Shift is a one-shot: one capital and back.
    editor.shift();
    CHECK(editor.page() == Page::upper);
    CHECK(editor.shifted());
    CHECK(editor.press("C"));
    CHECK(editor.page() == Page::lower);
    CHECK(editor.text() == "KitC");
    // Shift again on the upper page without typing gives it back.
    editor.shift();
    editor.shift();
    CHECK(editor.page() == Page::lower);
}

TEST_CASE("the editor refuses keys that would not fit or are not offered", "[devname]") {
    Editor editor("");
    CHECK_FALSE(editor.press(" "));  // nothing to put a space after
    CHECK_FALSE(editor.press("\n"));
    CHECK_FALSE(editor.press("ab"));
    CHECK_FALSE(editor.press(""));
    CHECK(editor.text().empty());

    for (int i = 0; i < 32; ++i) {
        CHECK(editor.press("a"));
    }
    CHECK(editor.bytes_left() == 0);
    CHECK_FALSE(editor.press("a"));
    CHECK_FALSE(editor.can_press("\xC3\xA9"));
    CHECK(editor.text().size() == 32);

    // One byte left cannot take a two-byte letter, but takes an ASCII one.
    Editor nearly(std::string(31, 'a'));
    CHECK(nearly.bytes_left() == 1);
    CHECK_FALSE(nearly.can_press("\xC3\xA9"));
    CHECK(nearly.can_press("b"));
}

TEST_CASE("backspace removes a whole letter", "[devname]") {
    Editor editor("Caf\xC3\xA9");
    editor.backspace();
    CHECK(editor.text() == "Caf");
    editor.backspace();
    editor.backspace();
    editor.backspace();
    editor.backspace();  // nothing left: no harm
    CHECK(editor.text().empty());

    Editor emoji("A\xF0\x9F\x8E\xB5");
    emoji.backspace();
    CHECK(emoji.text() == "A");
}

TEST_CASE("OK stores the name without the spaces typed after it, and only when it changed", "[devname]") {
    Editor editor("Attic");
    CHECK_FALSE(editor.can_accept());  // unchanged
    CHECK(editor.press(" "));
    CHECK(editor.text() == "Attic ");
    CHECK(editor.result() == "Attic");
    CHECK_FALSE(editor.can_accept());  // still the name it began as
    CHECK(editor.press("2"));
    CHECK(editor.can_accept());
    CHECK(editor.result() == "Attic 2");

    Editor empty("Attic");
    empty.clear();
    CHECK_FALSE(empty.can_accept());
    CHECK(empty.check().problem == Problem::empty);
}

TEST_CASE("the accent pages swap case and stay swapped", "[devname]") {
    Editor editor("a");
    editor.show(Page::accents_latin1);
    CHECK_FALSE(editor.shifted());
    editor.shift();
    CHECK(editor.shifted());
    CHECK(editor.page() == Page::accents_latin1);
    CHECK(editor.press("\xC3\x89"));  // E acute, from the shifted page
    CHECK(editor.shifted());          // an accent page is not a one-shot
    editor.shift();
    CHECK_FALSE(editor.shifted());
}
