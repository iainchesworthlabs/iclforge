// The preference registry (iclforge/preferences.hpp) against every board this product knows
// (iclforge/board_variants.hpp): each entry is well formed, each says why it is greyed where it is, a default is
// never a choice the board cannot make, and a setting that is refused is refused before the board is asked.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <set>
#include <string>

#include "fake_prefs_backend.hpp"
#include "iclforge/board_variants.hpp"
#include "iclforge/preferences.hpp"

namespace prefs = iclforge::prefs;
namespace nv = iclforge::nowview;
using iclforge::BoardCaps;
using prefs::testing::FakeBackend;

namespace {

bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

const prefs::ChoiceState* choice_of(const prefs::Entry& entry, std::string_view id) {
    for (const prefs::ChoiceState& choice : entry.choices) {
        if (choice.id == id) {
            return &choice;
        }
    }
    return nullptr;
}

// A bracket and quote check, enough to catch a stray comma or an unbalanced brace in what GET /preferences writes.
bool json_is_balanced(const std::string& text) {
    std::string stack;
    bool in_string = false;
    char previous = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
            previous = c;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            stack += c;
        } else if (c == '}' || c == ']') {
            if (previous == ',' || stack.empty() || (c == '}') != (stack.back() == '{')) {
                return false;
            }
            stack.pop_back();
        }
        if (c != ' ' && c != '\n') {
            previous = c;
        }
    }
    return stack.empty() && !in_string;
}

}  // namespace

TEST_CASE("every entry is well formed", "[preferences]") {
    std::set<std::string> ids;
    for (const prefs::Pref& pref : prefs::registry()) {
        INFO(pref.id);
        const std::string id = pref.id;
        REQUIRE_FALSE(id.empty());
        REQUIRE(ids.insert(id).second);
        for (const char c : id) {
            REQUIRE(((c >= 'a' && c <= 'z') || c == '-' || (c >= '0' && c <= '9')));
        }
        REQUIRE(std::string(pref.label).size() > 2);
        REQUIRE(std::string(pref.label).size() <= 32);  // a row's label has a column to fit in
        REQUIRE((std::string(pref.route).empty() || std::string(pref.route).front() == '/'));
        REQUIRE(pref.get != nullptr);
        REQUIRE(pref.set != nullptr);
        REQUIRE(prefs::find(pref.id) == &pref);

        if (pref.kind == prefs::Kind::kChoice || pref.kind == prefs::Kind::kStepper ||
            pref.kind == prefs::Kind::kToggle) {
            REQUIRE(pref.choices.size() >= 2);
            std::set<std::string> choice_ids;
            bool default_is_a_choice = false;
            for (const prefs::ChoiceDef& choice : pref.choices) {
                REQUIRE(choice_ids.insert(choice.id).second);
                REQUIRE(std::string(choice.label).size() >= 2);
                // A button of a choice sheet is a fingertip wide and holds the label on one line.
                REQUIRE(std::string(choice.label).size() <= 12);
                default_is_a_choice |= std::string(choice.id) == pref.default_word;
            }
            REQUIRE(default_is_a_choice);
        }
        if (pref.kind == prefs::Kind::kToggle) {
            REQUIRE(pref.choices.size() == 2);
            REQUIRE(std::string(pref.choices[0].id) == "on");
            REQUIRE(std::string(pref.choices[1].id) == "off");
        }
        if (pref.kind == prefs::Kind::kStepper) {
            REQUIRE(pref.choices.size() >= 3);
            // Stops step in order: numbers up, and a word (never) only at the end.
            long last = -1;
            for (const prefs::ChoiceDef& choice : pref.choices) {
                const long rank = prefs::detail::stop_rank(choice.id);
                REQUIRE(rank > last);
                last = rank;
            }
        }
    }
    REQUIRE(prefs::find("no-such-preference") == nullptr);
}

TEST_CASE("the groups have their words, in order", "[preferences]") {
    REQUIRE(std::string(prefs::group_name(prefs::Group::kDisplay)) == "Display");
    REQUIRE(std::string(prefs::group_name(prefs::Group::kSound)) == "Sound");
    REQUIRE(std::string(prefs::group_name(prefs::Group::kNetwork)) == "Network");
    REQUIRE(std::string(prefs::group_name(prefs::Group::kTime)) == "Time");
    REQUIRE(std::string(prefs::group_name(prefs::Group::kUpdates)) == "Updates");
    REQUIRE(std::string(prefs::group_name(prefs::Group::kSystem)) == "System");
    REQUIRE(prefs::kGroups.size() == 6);
    REQUIRE(std::string(prefs::kind_name(prefs::Kind::kStepper)) == "stepper");
    REQUIRE(std::string(prefs::apply_name(prefs::Apply::kNextPlay)) == "next-play");
}

TEST_CASE("the defaults are the Kconfig defaults the settings start from", "[preferences]") {
    REQUIRE(std::string(prefs::find("now-view")->default_word) == nv::view_name(nv::kDefaultView));
    REQUIRE(std::string(prefs::find("dim-after")->default_word) == nv::dim_after_text(nv::kDefaultDimMinutes));
    REQUIRE(std::string(prefs::find("speakers-style")->default_word) == nv::speakers_style_name(nv::kDefaultSpeakersStyle));
}

TEST_CASE("the speakers style: glow or pulse, on a board with a screen, set by word and followed by the route",
          "[preferences]") {
    const prefs::Pref& style = *prefs::find("speakers-style");
    REQUIRE(style.group == prefs::Group::kDisplay);
    REQUIRE(std::string(style.route) == "/speakers-style");
    REQUIRE(style.choices.size() == 2);
    // The choices are the words the route takes, in the order the screen shows them.
    REQUIRE(std::string(style.choices[0].id) == nv::speakers_style_name(nv::SpeakersStyle::kGlow));
    REQUIRE(std::string(style.choices[1].id) == nv::speakers_style_name(nv::SpeakersStyle::kPulse));
    for (const auto& variant : iclforge::known_variants()) {
        INFO(variant.name);
        FakeBackend backend(variant.caps);
        const prefs::Entry entry = prefs::resolve(style, backend, variant.caps);
        // A screen is what it needs and nothing else (no touch, no clock, no objects).
        REQUIRE(entry.availability.available == variant.caps.has_screen());
        if (entry.availability.available) {
            REQUIRE(entry.value == "glow");
            REQUIRE(prefs::set_value(backend, style, "pulse").ok);
            REQUIRE(backend.style_ == nv::SpeakersStyle::kPulse);
            REQUIRE(prefs::resolve(style, backend, variant.caps).value == "pulse");
            REQUIRE_FALSE(prefs::set_value(backend, style, "Pulse").ok);
            REQUIRE_FALSE(prefs::set_value(backend, style, "speakers").ok);
            backend.refuse_next = true;
            REQUIRE_FALSE(prefs::set_value(backend, style, "glow").ok);
            REQUIRE(backend.style_ == nv::SpeakersStyle::kPulse);
        }
    }
}

TEST_CASE("the speakers style is inert where the screen draws no speakers", "[preferences]") {
    const prefs::Pref& style = *prefs::find("speakers-style");
    FakeBackend backend(iclforge::known_variants().front().caps);
    for (const nv::View view : {nv::View::kSpeakers, nv::View::kTitle, nv::View::kDark}) {
        backend.view_ = view;
        REQUIRE(style.inert(backend).empty());
    }
    for (const nv::View view : {nv::View::kClock, nv::View::kOff}) {
        backend.view_ = view;
        const std::string why = style.inert(backend);
        REQUIRE_FALSE(why.empty());
        REQUIRE(why.back() == '.');
    }
}

TEST_CASE("each entry against each known board: a reason wherever it is greyed, a default wherever it is on",
          "[preferences]") {
    for (const auto& variant : iclforge::known_variants()) {
        for (const prefs::Pref& pref : prefs::registry()) {
            INFO(variant.name << " / " << pref.id);
            FakeBackend backend(variant.caps);
            const prefs::Entry entry = prefs::resolve(pref, backend, variant.caps);

            if (!entry.availability.available) {
                REQUIRE_FALSE(entry.availability.reason.empty());
                // The reason is a sentence about the board.
                REQUIRE(entry.availability.reason.back() == '.');
                REQUIRE(std::isupper(static_cast<unsigned char>(entry.availability.reason.front())) != 0);
            }
            bool any_choice_available = false;
            for (const prefs::ChoiceState& choice : entry.choices) {
                INFO(choice.id);
                if (!choice.availability.available) {
                    REQUIRE_FALSE(choice.availability.reason.empty());
                    REQUIRE(choice.availability.reason.back() == '.');
                }
                // An unavailable entry greys every one of its choices.
                if (!entry.availability.available) {
                    REQUIRE_FALSE(choice.availability.available);
                    REQUIRE(choice.availability.reason == entry.availability.reason);
                }
                any_choice_available |= choice.availability.available;
            }
            if (entry.availability.available) {
                // Something to choose, and the board's starting point is one the board can make.
                REQUIRE(any_choice_available);
                const prefs::ChoiceState* const start = choice_of(entry, pref.default_word);
                REQUIRE(start != nullptr);
                REQUIRE(start->availability.available);
                // The fake starts at the defaults, so the value read is the default.
                REQUIRE(entry.value == pref.default_word);
                REQUIRE_FALSE(entry.value_label.empty());
            }
        }
    }
}

TEST_CASE("a board with no screen has no Display preference it can use, and says so", "[preferences]") {
    for (const char* name : {"s3-n16r8", "c6-16mb", "p4-rev1"}) {
        for (const auto& variant : iclforge::known_variants()) {
            if (std::string(variant.name) != name) {
                continue;
            }
            INFO(name);
            FakeBackend backend(variant.caps);
            for (const prefs::Entry& entry : prefs::resolve(backend)) {
                if (entry.pref->group == prefs::Group::kDisplay) {
                    REQUIRE_FALSE(entry.availability.available);
                    REQUIRE(contains(entry.availability.reason, "no screen"));
                }
            }
        }
    }
}

TEST_CASE("the P4X kit can make every choice it has", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const std::vector<prefs::Entry> entries = prefs::resolve(backend);
    REQUIRE(entries.size() == prefs::registry().size());
    for (const prefs::Entry& entry : entries) {
        INFO(entry.pref->id);
        REQUIRE(entry.availability.available);
        for (const prefs::ChoiceState& choice : entry.choices) {
            REQUIRE(choice.availability.available);
        }
    }
}

TEST_CASE("a screen with no touch cannot offer the dark view or a dim time, and the other views stay", "[preferences]") {
    FakeBackend backend(iclforge::variants::c3_spi_320x240_no_touch());
    const std::vector<prefs::Entry> entries = prefs::resolve(backend);
    const prefs::Entry& view = entries[0];
    REQUIRE(std::string(view.pref->id) == "now-view");
    REQUIRE(view.availability.available);
    REQUIRE(choice_of(view, "speakers")->availability.available);
    REQUIRE(choice_of(view, "title")->availability.available);
    REQUIRE(choice_of(view, "clock")->availability.available);
    REQUIRE(choice_of(view, "off")->availability.available);
    const prefs::ChoiceState* const dark = choice_of(view, "dark");
    REQUIRE_FALSE(dark->availability.available);
    REQUIRE(contains(dark->availability.reason, "no touch"));

    const prefs::Entry& dim = entries[1];
    REQUIRE(std::string(dim.pref->id) == "dim-after");
    REQUIRE_FALSE(dim.availability.available);
    REQUIRE(contains(dim.availability.reason, "no touch"));
}

TEST_CASE("a board that keeps no time greys the clock view and keeps the rest", "[preferences]") {
    BoardCaps caps = iclforge::variants::p4_rev3_frame_buffer();
    caps.wall_clock = false;
    FakeBackend backend(caps);
    const prefs::Entry view = prefs::resolve(*prefs::find("now-view"), backend, caps);
    REQUIRE(view.availability.available);
    const prefs::ChoiceState* const clock = choice_of(view, "clock");
    REQUIRE_FALSE(clock->availability.available);
    REQUIRE(contains(clock->availability.reason, "keeps no time"));
    REQUIRE(choice_of(view, "speakers")->availability.available);
}

TEST_CASE("a value a PUT stored that is not a stop is read as it is, with its unit", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    backend.dim_minutes_ = 7;
    const prefs::Entry dim = prefs::resolve(*prefs::find("dim-after"), backend, backend.caps());
    REQUIRE(dim.value == "7");
    REQUIRE(dim.value_label == "7 min");
    backend.dim_minutes_ = nv::kDimNever;
    REQUIRE(prefs::resolve(*prefs::find("dim-after"), backend, backend.caps()).value_label == "Never");
    backend.dim_minutes_ = 60;
    REQUIRE(prefs::resolve(*prefs::find("dim-after"), backend, backend.caps()).value_label == "1 hour");
}

TEST_CASE("the dim time is inert while the dark view is chosen, and says why", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Entry lit = prefs::resolve(*prefs::find("dim-after"), backend, backend.caps());
    REQUIRE(lit.inert.empty());
    backend.view_ = nv::View::kDark;
    const prefs::Entry dark = prefs::resolve(*prefs::find("dim-after"), backend, backend.caps());
    REQUIRE(contains(dark.inert, "Dark view"));
    REQUIRE(contains(dark.inert, "30 seconds"));
    // Still available: the hardware can do it, another setting just makes it moot.
    REQUIRE(dark.availability.available);
}

TEST_CASE("a stepper steps along its stops and stops at the ends", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& dim = *prefs::find("dim-after");
    const auto step_from = [&](int minutes, int direction) {
        backend.dim_minutes_ = minutes;
        return prefs::step_target(prefs::resolve(dim, backend, backend.caps()), direction);
    };
    REQUIRE(*step_from(5, +1) == "15");
    REQUIRE(*step_from(5, -1) == "1");
    REQUIRE_FALSE(step_from(1, -1).has_value());
    REQUIRE(*step_from(240, +1) == "never");
    REQUIRE_FALSE(step_from(nv::kDimNever, +1).has_value());
    REQUIRE(*step_from(nv::kDimNever, -1) == "240");
    // A number between two stops goes to the next stop in the direction asked.
    REQUIRE(*step_from(7, +1) == "15");
    REQUIRE(*step_from(7, -1) == "5");
    REQUIRE(*step_from(300 / 2, +1) == "240");
    // No direction, no step.
    REQUIRE_FALSE(step_from(5, 0).has_value());
}

TEST_CASE("a stepper skips the stops its board cannot take", "[preferences]") {
    prefs::Entry entry;
    entry.value = "5";
    for (const char* id : {"1", "5", "15", "30"}) {
        entry.choices.push_back({id, id, prefs::Availability::yes()});
    }
    entry.choices[2].availability = prefs::Availability::no("Not here.");
    REQUIRE(*prefs::step_target(entry, +1) == "30");
    entry.choices[3].availability = prefs::Availability::no("Not here either.");
    REQUIRE_FALSE(prefs::step_target(entry, +1).has_value());
    REQUIRE(*prefs::step_target(entry, -1) == "1");
}

TEST_CASE("setting a choice goes to the board and reads back", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    for (const auto* id : {"clock", "dark", "title", "off", "speakers"}) {
        const prefs::SetResult result = prefs::set_value(backend, *prefs::find("now-view"), id);
        INFO(id);
        REQUIRE(result.ok);
        REQUIRE(result.message.empty());
        REQUIRE(prefs::resolve(*prefs::find("now-view"), backend, backend.caps()).value == id);
    }
    REQUIRE(prefs::set_value(backend, *prefs::find("dim-after"), "never").ok);
    REQUIRE(backend.dim_minutes_ == nv::kDimNever);
    REQUIRE(prefs::set_value(backend, *prefs::find("dim-after"), "30").ok);
    REQUIRE(backend.dim_minutes_ == 30);
}

TEST_CASE("a choice the board cannot make is refused with its reason, and the board is never asked", "[preferences]") {
    FakeBackend backend(iclforge::variants::c3_spi_320x240_no_touch());
    const prefs::SetResult dark = prefs::set_value(backend, *prefs::find("now-view"), "dark");
    REQUIRE_FALSE(dark.ok);
    REQUIRE(contains(dark.message, "no touch"));
    REQUIRE(backend.sets == 0);
    REQUIRE(backend.view_ == nv::kDefaultView);

    const prefs::SetResult dim = prefs::set_value(backend, *prefs::find("dim-after"), "30");
    REQUIRE_FALSE(dim.ok);
    REQUIRE(backend.sets == 0);

    FakeBackend none(iclforge::variants::s3_n16r8());
    const prefs::SetResult screenless = prefs::set_value(none, *prefs::find("now-view"), "speakers");
    REQUIRE_FALSE(screenless.ok);
    REQUIRE(contains(screenless.message, "no screen"));
    REQUIRE(none.sets == 0);
}

TEST_CASE("a word that is not a choice is refused, and a number between a stepper's stops is taken", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    REQUIRE_FALSE(prefs::set_value(backend, *prefs::find("now-view"), "sideways").ok);
    REQUIRE_FALSE(prefs::set_value(backend, *prefs::find("dim-after"), "soon").ok);
    REQUIRE(backend.sets == 0);
    REQUIRE(prefs::set_value(backend, *prefs::find("dim-after"), "7").ok);
    REQUIRE(backend.dim_minutes_ == 7);
    // A number the board's own parser refuses (240 is the most) is the board's refusal.
    const prefs::SetResult too_long = prefs::set_value(backend, *prefs::find("dim-after"), "999");
    REQUIRE_FALSE(too_long.ok);
    REQUIRE(backend.dim_minutes_ == 7);
}

TEST_CASE("a board that cannot store a value says so and keeps the old one", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    backend.refuse_next = true;
    const prefs::SetResult result = prefs::set_value(backend, *prefs::find("now-view"), "clock");
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.message.empty());
    REQUIRE(backend.view_ == nv::kDefaultView);
}

TEST_CASE("the listing carries id, group, label, kind, value, availability and reason", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "\"profile\":\"p4-rev3\""));
    REQUIRE(contains(json, "\"display\":\"dsi\""));
    REQUIRE(contains(json, "\"id\":\"now-view\",\"group\":\"Display\",\"label\":\"Screen without lyrics\",\"kind\":\"choice\""));
    REQUIRE(contains(json, "\"apply\":\"live\",\"route\":\"/now-playing\",\"value\":\"speakers\",\"available\":true"));
    REQUIRE(contains(json, "\"id\":\"dim-after\""));
    REQUIRE(contains(json, "\"kind\":\"stepper\""));
    REQUIRE(contains(json, "\"value\":\"5\""));
    REQUIRE(contains(json, "{\"id\":\"never\",\"label\":\"Never\",\"available\":true,\"reason\":\"\"}"));
    REQUIRE(json.back() == '\n');
}

TEST_CASE("the listing of a board with no screen greys the Display entries and gives the reason", "[preferences]") {
    FakeBackend backend(iclforge::variants::c6_16mb());
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "\"display\":\"none\""));
    REQUIRE(contains(json, "\"available\":false,\"reason\":\"This board has no screen.\""));
    REQUIRE_FALSE(contains(json, "\"available\":true,\"reason\":\"\"}],"));
}

TEST_CASE("the listing says why a choice is greyed and says nothing of a reason it does not have", "[preferences]") {
    FakeBackend backend(iclforge::variants::c3_spi_320x240_no_touch());
    backend.view_ = nv::View::kDark;  // set directly, as a stored setting from another build would be
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "{\"id\":\"dark\",\"label\":\"Dark\",\"available\":false,\"reason\":\"A touch lights the dark view, and this screen has no touch.\"}"));
    REQUIRE(contains(json, "\"note\":\"Not used by the Dark view"));
}

TEST_CASE("the listing escapes what it quotes", "[preferences]") {
    std::string out;
    prefs::detail::json_text(out, "a \"b\" \\ c\n\t");
    REQUIRE(out == "\"a \\\"b\\\" \\\\ c\\n\\t\"");
    out.clear();
    prefs::detail::json_text(out, std::string("x\x01y"));
    REQUIRE(out == "\"x y\"");
}
