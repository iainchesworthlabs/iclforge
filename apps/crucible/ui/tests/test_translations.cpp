#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Qt Linguist catalogues of Crucible (apps/crucible/ui/assets/translations/
// crucible_<code>.ts) and of Hearth (apps/hearth/ui/assets/translations/
// hearth_<code>.ts), read as files rather than through Qt: these cases
// ride the plain iclforge-crucible-tests binary, so they run on a developer's own ctest and
// on every CI leg, including the ones that build no Qt at all. What they
// check is what a reader of the catalogue cannot see at a glance and what a
// hand edit or a machine pass gets wrong: a dropped placeholder, a brand name
// translated into something else, one language holding a string the other
// five do not, and an entry left empty.
//
// lupdate writes a fixed shape - one element per line, no attribute order to
// guess at - so the scan below is a plain string walk. Nothing here needs an
// XML parser, and adding one would make the gate depend on a library the
// suite does not otherwise carry.
//
// docs/crucible/localisation.md is the page these rules are written down on.

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 6> kLanguages{"ar", "de", "es", "fr", "he", "yi"};

// Words that name a product, a format or a piece of software. They read the
// same in every language, and a translation that has lost one of them has
// renamed something rather than translated it. German joins a brand into a
// compound with hyphens ("Dolby-Atmos-Szene"), which is that language's own
// orthography, so the check accepts the hyphenated form too.
constexpr std::array<std::string_view, 10> kBrandTerms{
    "ICL Forge", "Crucible", "Dolby Atmos", "E-AC-3", "AC-3",
    "JOC", "PCM", "HDMI", "PipeWire", "WirePlumber"};

// Hearth's window names more formats and one protocol than Crucible's does.
// "Hearth" is the product; Sendspin, Opus, mDNS and WiFi are names of things
// that are written the same way in every language the window ships in.
constexpr std::array<std::string_view, 19> kHearthBrandTerms{
    "Hearth", "Sendspin", "ICL Forge", "iclforge", "Dolby", "Atmos", "AC-3", "E-AC-3",
    "AC-4", "JOC", "PCM", "Opus", "IEC 61937", "Matroska", "MP4", "MPEG-TS", "mDNS",
    "WiFi", "NOTICES.txt"};

// The catalogues are regenerated centrally once the source changes of this
// pass have landed, and the review that fills the new entries follows that
// regeneration (docs/crucible/localisation.md, "Regenerating the
// catalogues"). Until both have happened an unfinished entry is the expected
// state of a freshly extracted string, and a vanished one is the state of a
// string this pass reworded, so the two cases below stand written and idle.
//
// Turned on 2026-09-06, the day the catalogues were refilled: the six
// Crucible files carry 385 messages each and the six forge-gui files 795, none
// of them unfinished and none vanished. Hearth's six were refilled on
// 2026-10-13 (1101 messages each) and are held to the same two rules. From
// here an entry left unfinished, or a dead entry left behind, fails the build.
//
// The translations behind that are machine-made and have not been read by a
// speaker of any of the six languages, which the window says in its own
// language note. This flag asserts that every entry HAS a translation, which
// is a different and smaller claim than that every translation is right; the
// review pass docs/crucible/localisation.md describes is what settles the
// second one, and turning this on does not pre-empt it.
constexpr bool kCatalogueRefilled = true;

// One app's six files and the terms its window must not translate.
struct CatalogueSet {
    std::string_view name;
    std::string_view dir;
    std::string_view prefix;
    std::span<const std::string_view> brand_terms;
};

const CatalogueSet kCrucible{"crucible", ICLFORGE_CRUCIBLE_TS_DIR, "crucible_", kBrandTerms};
const CatalogueSet kHearth{"hearth", ICLFORGE_HEARTH_TS_DIR, "hearth_", kHearthBrandTerms};

struct Message {
    std::string context;
    std::string source;
    // Whatever stood inside the <translation ...> tag, so a case can ask
    // whether the entry was marked unfinished or vanished.
    std::string attributes;
    std::string translation;
    // `<message numerus="yes">`: the translation holds one <numerusform> per
    // plural form the language needs rather than a single text.
    bool numerus = false;
};

[[nodiscard]] fs::path catalogue_path(const CatalogueSet& set, std::string_view code) {
    return fs::path{std::string{set.dir}} / (std::string{set.prefix} + std::string{code} + ".ts");
}

[[nodiscard]] std::string read_catalogue(const CatalogueSet& set, std::string_view code) {
    const auto path = catalogue_path(set, code);
    std::ifstream in(path, std::ios::binary);
    INFO("catalogue " << path.string());
    REQUIRE(in.is_open());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The text between `open` and the matching `close`, starting the search at
// `from`. Returns an empty string when the element is not there, which every
// caller treats as "this message does not carry one".
[[nodiscard]] std::string element(const std::string& text, std::size_t from, std::size_t until,
                                  std::string_view open, std::string_view close) {
    const auto start = text.find(open, from);
    if (start == std::string::npos || start >= until) {
        return {};
    }
    const auto body = start + open.size();
    const auto end = text.find(close, body);
    if (end == std::string::npos || end > until) {
        return {};
    }
    return text.substr(body, end - body);
}

[[nodiscard]] std::vector<Message> parse(const std::string& text) {
    std::vector<Message> messages;
    std::string context;
    std::size_t at = 0;
    while (true) {
        const auto name_at = text.find("<name>", at);
        const auto message_at = text.find("<message", at);
        // A <context> opens with its <name>; every <message> between that and
        // the next one belongs to it.
        const bool name_first = name_at != std::string::npos &&
                                (message_at == std::string::npos || name_at < message_at);
        if (name_first) {
            const auto end = text.find("</name>", name_at);
            if (end == std::string::npos) {
                break;
            }
            context = text.substr(name_at + 6, end - (name_at + 6));
            at = end;
            continue;
        }
        if (message_at == std::string::npos) {
            break;
        }
        const auto message_end = text.find("</message>", message_at);
        if (message_end == std::string::npos) {
            break;
        }
        Message message;
        message.context = context;
        message.source = element(text, message_at, message_end, "<source>", "</source>");
        const auto message_tag_end = text.find('>', message_at);
        if (message_tag_end != std::string::npos && message_tag_end < message_end) {
            message.numerus = text.substr(message_at, message_tag_end - message_at)
                                  .find("numerus=\"yes\"") != std::string::npos;
        }
        // The opening tag first, so an entry marked unfinished or vanished is
        // recognised whether or not it carries any text.
        const auto tag_at = text.find("<translation", message_at);
        if (tag_at != std::string::npos && tag_at < message_end) {
            const auto tag_end = text.find('>', tag_at);
            if (tag_end != std::string::npos && tag_end < message_end) {
                message.attributes = text.substr(tag_at, tag_end - tag_at);
                if (text[tag_end - 1] != '/') {
                    const auto body_end = text.find("</translation>", tag_end);
                    if (body_end != std::string::npos && body_end <= message_end) {
                        message.translation = text.substr(tag_end + 1, body_end - (tag_end + 1));
                    }
                }
            }
        }
        messages.push_back(message);
        at = message_end;
    }
    return messages;
}

[[nodiscard]] bool marked(const Message& message, std::string_view type) {
    const std::string wanted = "type=\"" + std::string{type} + "\"";
    return message.attributes.find(wanted) != std::string::npos;
}

// An entry lupdate has just extracted, or one whose English changed under a
// translation: it holds the old text or nothing at all, and the rules below
// that read the translation itself leave it alone. The case that forbids it
// outright is the one kCatalogueRefilled turns on.
[[nodiscard]] bool awaiting_translation(const Message& message) {
    return marked(message, "unfinished");
}

[[nodiscard]] bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// The bodies of the <numerusform> elements of a plural message's translation,
// in order. A self-closed element is an empty form.
[[nodiscard]] std::vector<std::string> numerus_forms(const std::string& translation) {
    std::vector<std::string> forms;
    std::size_t at = 0;
    while (true) {
        const auto open = translation.find("<numerusform", at);
        if (open == std::string::npos) {
            break;
        }
        const auto tag_end = translation.find('>', open);
        if (tag_end == std::string::npos) {
            break;
        }
        if (translation[tag_end - 1] == '/') {
            forms.emplace_back();
            at = tag_end;
            continue;
        }
        const auto close = translation.find("</numerusform>", tag_end);
        if (close == std::string::npos) {
            break;
        }
        forms.push_back(translation.substr(tag_end + 1, close - (tag_end + 1)));
        at = close;
    }
    return forms;
}

// The texts a rule reads: one per plural form for a plural message, the
// translation itself for any other.
[[nodiscard]] std::vector<std::string> bodies(const Message& message) {
    if (message.numerus) {
        return numerus_forms(message.translation);
    }
    return {message.translation};
}

// %1 .. %9 and %n, in sorted order, so a translation that reorders them is
// accepted and one that drops or invents one is not. %L1 and %Ln (the
// locale-aware forms) count as %1 and %n.
[[nodiscard]] std::vector<std::string> placeholders(const std::string& text) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '%') {
            continue;
        }
        std::size_t at = i + 1;
        if (text[at] == 'L' && at + 1 < text.size()) {
            ++at;
        }
        const char c = text[at];
        if ((c >= '1' && c <= '9') || c == 'n') {
            found.push_back(std::string{'%', c});
            i = at;
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Whether `translation` carries the placeholders of `source`: all of them,
// and none that the source lacks.
[[nodiscard]] bool carries_placeholders(const std::string& source, const std::string& translation) {
    auto missing = placeholders(source);
    for (const auto& have : placeholders(translation)) {
        const auto it = std::find(missing.begin(), missing.end(), have);
        if (it == missing.end()) {
            return false;
        }
        missing.erase(it);
    }
    return missing.empty();
}

// An empty string when the translation is fine, otherwise what is wrong. A
// plural message holds the placeholders in every form; %n is the one that a
// form may spell out in words ("One file"), so it is asked of at least one.
[[nodiscard]] std::string placeholder_fault(const Message& message) {
    if (!message.numerus) {
        return carries_placeholders(message.source, message.translation)
                   ? std::string{}
                   : std::string{"placeholders differ from the source"};
    }
    const auto forms = numerus_forms(message.translation);
    if (forms.empty()) {
        return "a plural entry with no form";
    }
    const bool wants_count = message.source.find("%n") != std::string::npos ||
                             message.source.find("%Ln") != std::string::npos;
    bool counted = false;
    for (const auto& form : forms) {
        std::string source_without_count = message.source;
        for (const std::string_view count : {"%Ln", "%n"}) {
            for (auto at = source_without_count.find(count); at != std::string::npos;
                 at = source_without_count.find(count)) {
                source_without_count.erase(at, count.size());
            }
        }
        std::string form_without_count = form;
        for (const std::string_view count : {"%Ln", "%n"}) {
            for (auto at = form_without_count.find(count); at != std::string::npos;
                 at = form_without_count.find(count)) {
                form_without_count.erase(at, count.size());
                counted = true;
            }
        }
        if (!carries_placeholders(source_without_count, form_without_count)) {
            return "a plural form's placeholders differ from the source";
        }
    }
    if (wants_count && !counted) {
        return "no plural form shows the count (%n)";
    }
    return {};
}

[[nodiscard]] bool carries_term(const std::string& text, std::string_view term) {
    if (text.find(term) != std::string::npos) {
        return true;
    }
    std::string hyphenated{term};
    std::replace(hyphenated.begin(), hyphenated.end(), ' ', '-');
    return text.find(hyphenated) != std::string::npos;
}

// A short, quotable form of a source, for the message a failure prints.
// The cut walks back off a continuation byte first: every source and every
// translation in ar, he and yi is multi-byte UTF-8, and half a codepoint in
// the one line a reader has to read would print as a broken character.
[[nodiscard]] std::string quoted(const std::string& source) {
    constexpr std::size_t kCap = 72;
    if (source.size() <= kCap) {
        return source;
    }
    std::size_t cut = kCap;
    while (cut > 0 && (static_cast<unsigned char>(source[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return source.substr(0, cut) + "...";
}

// Every entry across the set's six files that carries the given type, named
// so a failure says which file and which string.
[[nodiscard]] std::vector<std::string> entries_marked(const CatalogueSet& set,
                                                      std::string_view type) {
    std::vector<std::string> found;
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(set, code))) {
            if (marked(message, type)) {
                found.push_back(std::string{code} + " / " + message.context + " / " +
                                quoted(message.source));
            }
        }
    }
    return found;
}

// The first few of them, for the line a failure prints; the rest are in the
// files, and a reader who needs all of them greps for the type.
[[nodiscard]] std::string listed(const std::vector<std::string>& entries) {
    constexpr std::size_t kShown = 5;
    std::string out;
    for (std::size_t i = 0; i < entries.size() && i < kShown; ++i) {
        out += (out.empty() ? "" : "; ") + entries[i];
    }
    if (entries.size() > kShown) {
        out += "; and " + std::to_string(entries.size() - kShown) + " more";
    }
    return out.empty() ? std::string{"none"} : out;
}

// The rules, once for each app. A case below names the app it covers, so a
// failure says whose catalogue it was and ctest can run one app's alone.

void check_no_unfinished(const CatalogueSet& set) {
    const auto unfinished = entries_marked(set, "unfinished");
    if (!unfinished.empty()) {
        WARN("unfinished entries: " << unfinished.size() << ", first: " << unfinished.front()
             << " - this case only fails once kCatalogueRefilled is turned on, which is the"
                " commit that lands the refilled catalogues");
    }
    INFO(listed(unfinished));
    // With the catalogues refilled an unfinished entry is a failure. Before
    // that it is the expected state of a string lupdate has just extracted,
    // and the count above is what the pass still owes a translator.
    CHECK((unfinished.empty() || !kCatalogueRefilled));
}

void check_no_dead(const CatalogueSet& set) {
    auto dead = entries_marked(set, "vanished");
    const auto obsolete = entries_marked(set, "obsolete");
    dead.insert(dead.end(), obsolete.begin(), obsolete.end());
    if (!dead.empty()) {
        WARN("dead entries: " << dead.size() << ", first: " << dead.front()
             << " - this case only fails once kCatalogueRefilled is turned on, which is the"
                " commit that lands the refilled catalogues");
    }
    INFO(listed(dead));
    // The same two states as above: a string this pass reworded leaves its
    // old entry behind until the regeneration drops it.
    CHECK((dead.empty() || !kCatalogueRefilled));
}

void check_non_empty(const CatalogueSet& set) {
    for (const auto code : kLanguages) {
        const auto messages = parse(read_catalogue(set, code));
        REQUIRE_FALSE(messages.empty());
        for (const Message& message : messages) {
            if (awaiting_translation(message) || blank(message.source)) {
                continue;
            }
            INFO(code << " / " << message.context << ": " << quoted(message.source));
            const auto texts = bodies(message);
            CHECK_FALSE(texts.empty());
            for (const auto& text : texts) {
                CHECK_FALSE(blank(text));
            }
        }
    }
}

void check_placeholders(const CatalogueSet& set) {
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(set, code))) {
            const auto texts = bodies(message);
            if (awaiting_translation(message) ||
                std::all_of(texts.begin(), texts.end(), blank)) {
                continue;
            }
            INFO(code << " / " << message.context << ": " << quoted(message.source));
            INFO("translation: " << quoted(message.translation));
            const auto fault = placeholder_fault(message);
            INFO(fault);
            CHECK(fault.empty());
        }
    }
}

void check_brand_terms(const CatalogueSet& set) {
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(set, code))) {
            if (awaiting_translation(message)) {
                continue;
            }
            const auto texts = bodies(message);
            if (std::all_of(texts.begin(), texts.end(), blank)) {
                continue;
            }
            for (const auto term : set.brand_terms) {
                if (!carries_term(message.source, term)) {
                    continue;
                }
                for (const auto& text : texts) {
                    INFO(code << " / " << message.context << " [" << term << "]: "
                              << quoted(message.source));
                    INFO("translation: " << quoted(text));
                    CHECK(carries_term(text, term));
                }
            }
        }
    }
}

void check_one_source_set(const CatalogueSet& set) {
    // Every language is generated from the same extraction, so a difference
    // here is a hand edit or a file that missed the last lupdate rather than
    // anything about the language itself.
    std::set<std::pair<std::string, std::string>> first;
    std::string_view first_code;
    for (const auto code : kLanguages) {
        std::set<std::pair<std::string, std::string>> here;
        for (const Message& message : parse(read_catalogue(set, code))) {
            here.emplace(message.context, message.source);
        }
        if (first.empty()) {
            first = here;
            first_code = code;
            REQUIRE_FALSE(first.empty());
            continue;
        }
        for (const auto& entry : first) {
            INFO(first_code << " has an entry " << code << " does not: " << entry.first << " / "
                            << quoted(entry.second));
            CHECK(here.contains(entry));
        }
        for (const auto& entry : here) {
            INFO(code << " has an entry " << first_code << " does not: " << entry.first << " / "
                      << quoted(entry.second));
            CHECK(first.contains(entry));
        }
    }
}

}  // namespace

TEST_CASE("crucible translation catalogues carry no unfinished entry",
          "[crucible][translations]") {
    check_no_unfinished(kCrucible);
}

// The GUI's seven catalogues, held to the two rules that matter most once a
// set is complete: nothing unfinished, nothing dead. They are read with the
// parser above and not through Qt, the same way and for the same reason.
//
// Seven and not six: apps/forge/gui/assets/translations carries an `xx` pseudo-locale
// beside the six languages, which exists to make an untranslated string
// obvious in a screenshot, so it is included here rather than excepted - a
// pseudo-locale with a hole in it is as wrong as any other.
//
// This lives beside Crucible's rather than in a workflow step, because a
// step gates one leg and reports "the diff is not empty", where a case runs
// everywhere and names the entry.
TEST_CASE("gui translation catalogues carry no unfinished or dead entry",
          "[gui][translations]") {
    static constexpr std::array<std::string_view, 7> kGuiLanguages{"ar", "de", "es", "fr",
                                                                   "he", "xx", "yi"};
    std::vector<std::string> offenders;
    for (const std::string_view code : kGuiLanguages) {
        const auto path =
            fs::path{ICLFORGE_GUI_TS_DIR} / ("forge_gui_" + std::string{code} + ".ts");
        std::ifstream in(path, std::ios::binary);
        INFO("catalogue " << path.string());
        REQUIRE(in.is_open());
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        // A file that parsed to nothing would pass every rule below it, so
        // the count is asserted before the contents are judged.
        const auto messages = parse(text);
        INFO("messages parsed from " << path.string() << ": " << messages.size());
        REQUIRE(messages.size() > 100);
        for (const Message& message : messages) {
            for (const std::string_view mark : {"unfinished", "vanished", "obsolete"}) {
                if (message.attributes.find(mark) != std::string::npos) {
                    offenders.push_back(std::string{code} + ": " + message.source);
                }
            }
        }
    }
    INFO(listed(offenders));
    CHECK(offenders.empty());
}

TEST_CASE("crucible translation catalogues carry no dead entry",
          "[crucible][translations]") {
    check_no_dead(kCrucible);
}

TEST_CASE("every finished crucible translation is non-empty", "[crucible][translations]") {
    check_non_empty(kCrucible);
}

TEST_CASE("placeholders survive translation in every crucible catalogue",
          "[crucible][translations]") {
    check_placeholders(kCrucible);
}

TEST_CASE("brand terms survive translation in every crucible catalogue",
          "[crucible][translations]") {
    check_brand_terms(kCrucible);
}

TEST_CASE("all six crucible catalogues share one source set", "[crucible][translations]") {
    check_one_source_set(kCrucible);
}

// Hearth's six, held to the same six rules as Crucible's. They were the only
// catalogues of the three that a case did not read: Hearth's window shipped
// the language machinery with every entry but the Karaoke page's still
// unfinished, and a test that asked for complete catalogues would have failed
// from its first commit. They were filled on 2026-10-13.
//
// The tag is `hearth`, the way the GUI's is `gui`, so the Linux pass that runs
// "[crucible],[seams]" leaves these to the legs that run all of ctest.

TEST_CASE("hearth translation catalogues carry no unfinished entry",
          "[hearth][translations]") {
    check_no_unfinished(kHearth);
}

TEST_CASE("hearth translation catalogues carry no dead entry", "[hearth][translations]") {
    check_no_dead(kHearth);
}

TEST_CASE("every finished hearth translation is non-empty", "[hearth][translations]") {
    check_non_empty(kHearth);
}

TEST_CASE("placeholders survive translation in every hearth catalogue",
          "[hearth][translations]") {
    check_placeholders(kHearth);
}

TEST_CASE("brand terms survive translation in every hearth catalogue",
          "[hearth][translations]") {
    check_brand_terms(kHearth);
}

TEST_CASE("all six hearth catalogues share one source set", "[hearth][translations]") {
    check_one_source_set(kHearth);
}

TEST_CASE("hearth catalogues carry the messages the window has", "[hearth][translations]") {
    // A file that parsed to nothing, or to a fragment, would pass every rule
    // above it. About 1100 messages today; the floor is below that so that a string
    // the window drops does not fail this, and far above anything a broken
    // read produces.
    for (const auto code : kLanguages) {
        const auto messages = parse(read_catalogue(kHearth, code));
        INFO("hearth_" << code << ".ts parsed to " << messages.size() << " messages");
        CHECK(messages.size() > 900);
    }
}

// English holds no catalogue of its own for ordinary strings, but a plural
// message needs one: Qt takes the form a count asks for from the catalogue, and
// the source holds only one of the two, so without an entry here "1 entries" is
// what the window says. hearth_en.ts is that catalogue (kept by
// PLURALS_TS_FILE in apps/hearth/ui/CMakeLists.txt, which runs lupdate and then
// lconvert -pluralonly over it): the plural messages of the window and nothing
// else, two forms each. It is English written by the people who wrote the
// source, so unlike the six it is not a machine draft.
TEST_CASE("hearth's English plural catalogue holds every plural message, finished",
          "[hearth][translations]") {
    using Pair = std::pair<std::string, std::string>;
    std::set<Pair> english;
    for (const Message& message : parse(read_catalogue(kHearth, "en"))) {
        INFO("en / " << message.context << ": " << quoted(message.source));
        // Only plural messages belong in the file.
        CHECK(message.numerus);
        CHECK_FALSE(marked(message, "unfinished"));
        CHECK_FALSE(marked(message, "vanished"));
        CHECK_FALSE(marked(message, "obsolete"));
        // English has two forms: one, and every other count including 0.
        const auto forms = numerus_forms(message.translation);
        CHECK(forms.size() == 2);
        CHECK(std::none_of(forms.begin(), forms.end(), blank));
        CHECK(placeholder_fault(message).empty());
        english.emplace(message.context, message.source);
    }
    // A file that parsed to nothing would hold every plural message of the
    // six by holding none of them.
    REQUIRE_FALSE(english.empty());

    for (const auto code : kLanguages) {
        std::set<Pair> plural;
        std::set<std::size_t> form_counts;
        for (const Message& message : parse(read_catalogue(kHearth, code))) {
            if (message.numerus) {
                plural.emplace(message.context, message.source);
                form_counts.insert(numerus_forms(message.translation).size());
            }
        }
        // The same plural messages in every file, so a `%n` message added to
        // the window is added to English's file as well.
        for (const auto& entry : plural) {
            INFO(code << " has a plural message the English file does not: " << entry.first
                      << " / " << quoted(entry.second));
            CHECK(english.contains(entry));
        }
        for (const auto& entry : english) {
            INFO("the English file has a plural message " << code << " does not: " << entry.first
                 << " / " << quoted(entry.second));
            CHECK(plural.contains(entry));
        }
        // lupdate writes the number of forms the language needs (Arabic six,
        // Hebrew four); one file holding two counts is a hand edit.
        INFO(code << " holds plural messages with " << form_counts.size() << " different form counts");
        CHECK(form_counts.size() <= 1);
    }
}

// The plural rules run on a made-up catalogue, so that each way a plural entry
// can be wrong is seen to be caught: the real catalogues hold none of them.
TEST_CASE("the plural rules read a catalogue correctly", "[hearth][translations]") {
    const std::string text = R"(<?xml version="1.0" encoding="utf-8"?>
<TS version="2.1" language="fr" sourcelanguage="en">
<context>
    <name>Demo</name>
    <message numerus="yes">
        <source>%n file(s) in %1</source>
        <translation>
            <numerusform>%n fichier dans %1</numerusform>
            <numerusform>%n fichiers dans %1</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1</source>
        <translation>
            <numerusform>Un fichier dans %1</numerusform>
            <numerusform>Des fichiers dans %1</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1</source>
        <translation>
            <numerusform>%n fichier dans %1</numerusform>
            <numerusform></numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1</source>
        <translation>
            <numerusform>%n fichier</numerusform>
            <numerusform>%n fichiers dans %1</numerusform>
        </translation>
    </message>
    <message>
        <source>%1 entries in %2</source>
        <translation>entrees : %1</translation>
    </message>
    <message>
        <source>%1 of %2</source>
        <translation>%L1 sur %L2</translation>
    </message>
</context>
</TS>
)";
    const auto messages = parse(text);
    REQUIRE(messages.size() == 6);

    // A plural entry with both forms and the count: fine.
    CHECK(messages[0].numerus);
    CHECK(placeholder_fault(messages[0]).empty());
    // No form shows the count: caught.
    CHECK_FALSE(placeholder_fault(messages[1]).empty());
    // An empty form: the non-empty rule sees it.
    const auto empty_forms = bodies(messages[2]);
    REQUIRE(empty_forms.size() == 2);
    CHECK(std::any_of(empty_forms.begin(), empty_forms.end(), blank));
    // A form that drops %1: caught.
    CHECK_FALSE(placeholder_fault(messages[3]).empty());
    // A plain message that leaves a placeholder out: caught, with no
    // exception for a message that builds its plural by a suffix.
    CHECK_FALSE(placeholder_fault(messages[4]).empty());
    // %L1 and %L2 are %1 and %2.
    CHECK(placeholder_fault(messages[5]).empty());
}
