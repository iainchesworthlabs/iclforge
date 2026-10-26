// The preference registry (iclforge/preferences.hpp) against every board this product knows
// (iclforge/board_variants.hpp): each entry is well formed, each says why it is greyed where it is, a default is
// never a choice the board cannot make, and a setting that is refused is refused before the board is asked.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

#include "fake_prefs_backend.hpp"
#include "iclforge/board_variants.hpp"
#include "iclforge/preferences.hpp"

namespace prefs = iclforge::prefs;
namespace nv = iclforge::nowview;
namespace wc = iclforge::wallclock;
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
        // Every entry says whether Reset preferences touches it, and only a value with a default can be reset.
        REQUIRE(pref.on_reset != prefs::OnReset::kUnset);
        if (pref.on_reset == prefs::OnReset::kReset) {
            REQUIRE(pref.default_word[0] != '\0');
            // A choice, a stepper, a toggle, or a text whose default is a word its setter takes (the night's times).
            REQUIRE((pref.kind == prefs::Kind::kChoice || pref.kind == prefs::Kind::kStepper ||
                     pref.kind == prefs::Kind::kToggle || pref.kind == prefs::Kind::kText));
        }

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
        if (pref.kind == prefs::Kind::kSheet) {
            // A sheet's value is its summary, not one of a few words: no choices to draw.
            REQUIRE(pref.choices.empty());
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
    REQUIRE(std::string(prefs::find("brightness")->default_word) == std::to_string(nv::kDefaultBrightness));
    REQUIRE(std::string(prefs::find("clock-hours")->default_word) == "24");
    REQUIRE(std::string(prefs::find("clock-seconds")->default_word) == "off");
    const nv::NightSchedule night;
    REQUIRE(std::string(prefs::find("night-schedule")->default_word) == nv::on_off_text(night.enabled));
    REQUIRE(std::string(prefs::find("night-times")->default_word) ==
            nv::time_of_day_text(night.from_min) + "-" + nv::time_of_day_text(night.to_min));
    REQUIRE(std::string(prefs::find("night-idle")->default_word) == nv::on_off_text(night.idle));
    REQUIRE(std::string(prefs::find("time-zone")->default_word) == "none");
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

TEST_CASE("Reset preferences puts the speakers style back to glow, on a board with a screen only", "[preferences]") {
    REQUIRE(prefs::find("speakers-style")->on_reset == prefs::OnReset::kReset);
    for (const auto& variant : iclforge::known_variants()) {
        INFO(variant.name);
        FakeBackend backend(variant.caps);
        backend.style_ = nv::SpeakersStyle::kPulse;
        const prefs::ResetReport report = prefs::reset_preferences(backend);
        const bool listed = std::find(report.reset.begin(), report.reset.end(), "speakers-style") != report.reset.end();
        REQUIRE(listed == variant.caps.has_screen());
        REQUIRE(backend.style_ == (variant.caps.has_screen() ? nv::SpeakersStyle::kGlow : nv::SpeakersStyle::kPulse));
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
                // Something to choose, and the board's starting point is one the board can make. An entry that is not a
                // choice among words (a name, an action, a list, About, a place or a pair of times chosen in an editor)
                // has no choices to check.
                if (!pref.choices.empty()) {
                    REQUIRE(any_choice_available);
                    const prefs::ChoiceState* const start = choice_of(entry, pref.default_word);
                    REQUIRE(start != nullptr);
                    REQUIRE(start->availability.available);
                }
                // The fake starts at the defaults, so a value that has a default reads as it.
                if (pref.default_word[0] != '\0') {
                    REQUIRE(entry.value == pref.default_word);
                    REQUIRE_FALSE(entry.value_label.empty());
                }
                // A sheet says what is set.
                if (pref.kind == prefs::Kind::kSheet) {
                    REQUIRE_FALSE(entry.value.empty());
                }
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

TEST_CASE("the Meters choice needs a screen of 240 by 240 and the Sendspin player, and says why it is greyed", "[preferences]") {
    for (const auto& variant : iclforge::known_variants()) {
        INFO(variant.name);
        FakeBackend backend(variant.caps);
        const prefs::Entry view = prefs::resolve(*prefs::find("now-view"), backend, variant.caps);
        const prefs::ChoiceState* const meters = choice_of(view, "meters");
        REQUIRE(meters != nullptr);
        const bool big_enough = variant.caps.display.width >= 240 && variant.caps.display.height >= 240;
        if (!variant.caps.has_screen()) {
            // No screen: the whole entry is greyed, and the choice with it.
            REQUIRE_FALSE(meters->availability.available);
            continue;
        }
        REQUIRE(meters->availability.available == (big_enough && variant.caps.sendspin));
        if (!big_enough) {
            REQUIRE(contains(meters->availability.reason, "240 by 240"));
        } else if (!variant.caps.sendspin) {
            REQUIRE(contains(meters->availability.reason, "Sendspin"));
        }
        // The rest of the views are as they were.
        REQUIRE(choice_of(view, "speakers")->availability.available);
    }
    // The kit shows it, and the speakers style has nothing to do while it does.
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    REQUIRE(prefs::set_value(kit, *prefs::find("now-view"), "meters").ok);
    REQUIRE(kit.view_ == nv::View::kMeters);
    const std::string why = prefs::find("speakers-style")->inert(kit);
    REQUIRE(contains(why, "Meters"));
    REQUIRE(why.back() == '.');
}

TEST_CASE("setting a choice goes to the board and reads back", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    for (const auto* id : {"clock", "dark", "title", "off", "speakers", "meters"}) {
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

TEST_CASE("brightness is a stepper of ten stops that steps and refuses what is not a brightness", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& brightness = *prefs::find("brightness");
    REQUIRE(brightness.kind == prefs::Kind::kStepper);
    REQUIRE(brightness.choices.size() == 10);
    const auto entry = [&] { return prefs::resolve(brightness, backend, backend.caps()); };
    REQUIRE(entry().value == "80");
    REQUIRE(entry().value_label == "80%");
    REQUIRE(*prefs::step_target(entry(), +1) == "90");
    REQUIRE(*prefs::step_target(entry(), -1) == "70");
    backend.brightness_ = 100;
    REQUIRE_FALSE(prefs::step_target(entry(), +1).has_value());
    backend.brightness_ = 10;
    REQUIRE_FALSE(prefs::step_target(entry(), -1).has_value());
    // A number a route stored between the stops is read as it is and stepped to the next stop either way.
    backend.brightness_ = 55;
    REQUIRE(entry().value_label == "55 %");
    REQUIRE(*prefs::step_target(entry(), +1) == "60");
    REQUIRE(*prefs::step_target(entry(), -1) == "50");
    // The stops are exactly the ones the stepper in now_view.hpp walks.
    for (int percent = nv::kBrightnessMin; percent < nv::kBrightnessMax; percent += nv::kBrightnessStep) {
        backend.brightness_ = percent;
        REQUIRE(*prefs::step_target(entry(), +1) == std::to_string(nv::brightness_stepped(percent, +1)));
    }
    REQUIRE(prefs::set_value(backend, brightness, "40").ok);
    REQUIRE(backend.brightness_ == 40);
    // Any whole number in range is taken, and one that is not a brightness is refused before it is stored.
    REQUIRE(prefs::set_value(backend, brightness, "55").ok);
    REQUIRE(backend.brightness_ == 55);
    REQUIRE_FALSE(prefs::set_value(backend, brightness, "5").ok);
    REQUIRE_FALSE(prefs::set_value(backend, brightness, "101").ok);
    REQUIRE_FALSE(prefs::set_value(backend, brightness, "bright").ok);
    REQUIRE(backend.brightness_ == 55);
}

TEST_CASE("brightness is greyed where the backlight is not the board's to set, with the reason", "[preferences]") {
    for (const char* name : {"p4-rev3-frame-buffer", "c3-spi-320x240-no-touch", "c6-16mb"}) {
        for (const auto& variant : iclforge::known_variants()) {
            if (std::string(variant.name) != name) {
                continue;
            }
            INFO(name);
            FakeBackend backend(variant.caps);
            const prefs::Entry entry = prefs::resolve(*prefs::find("brightness"), backend, backend.caps());
            REQUIRE_FALSE(entry.availability.available);
            REQUIRE((contains(entry.availability.reason, "backlight") || contains(entry.availability.reason, "no screen")));
            const prefs::SetResult result = prefs::set_value(backend, *prefs::find("brightness"), "50");
            REQUIRE_FALSE(result.ok);
            REQUIRE(backend.sets == 0);
        }
    }
}

TEST_CASE("the clock's hours and seconds are two entries of one route and keep each other", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& hours = *prefs::find("clock-hours");
    const prefs::Pref& seconds = *prefs::find("clock-seconds");
    REQUIRE(std::string(hours.route) == std::string(seconds.route));
    REQUIRE(prefs::set_value(backend, hours, "12").ok);
    REQUIRE(backend.clock_.twelve_hour);
    REQUIRE_FALSE(backend.clock_.seconds);
    REQUIRE(prefs::set_value(backend, seconds, "on").ok);
    REQUIRE(backend.clock_.twelve_hour);
    REQUIRE(backend.clock_.seconds);
    REQUIRE(prefs::set_value(backend, hours, "24").ok);
    REQUIRE_FALSE(backend.clock_.twelve_hour);
    REQUIRE(backend.clock_.seconds);
    REQUIRE(prefs::resolve(hours, backend, backend.caps()).value == "24");
    REQUIRE(prefs::resolve(seconds, backend, backend.caps()).value == "on");
    REQUIRE_FALSE(prefs::set_value(backend, hours, "13").ok);
    REQUIRE_FALSE(prefs::set_value(backend, seconds, "yes").ok);
    // The route's own words say the same thing the two entries do.
    REQUIRE(wc::clock_format_text(backend.clock_) == "24 seconds");
}

TEST_CASE("the clock's format is greyed on a board that keeps no time, and on one with no screen", "[preferences]") {
    BoardCaps caps = iclforge::variants::p4_rev3_kit();
    caps.wall_clock = false;
    FakeBackend no_time(caps);
    for (const char* id : {"clock-hours", "clock-seconds"}) {
        const prefs::Entry entry = prefs::resolve(*prefs::find(id), no_time, caps);
        INFO(id);
        REQUIRE_FALSE(entry.availability.available);
        REQUIRE(contains(entry.availability.reason, "keeps no time"));
    }
    FakeBackend no_screen(iclforge::variants::c6_16mb());
    REQUIRE(contains(prefs::resolve(*prefs::find("clock-hours"), no_screen, no_screen.caps()).availability.reason, "no screen"));
}

TEST_CASE("the night schedule is a toggle and a pair of times, and setting one keeps the other", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& toggle = *prefs::find("night-schedule");
    const prefs::Pref& times = *prefs::find("night-times");
    REQUIRE(toggle.kind == prefs::Kind::kToggle);
    REQUIRE(times.kind == prefs::Kind::kText);
    REQUIRE(times.editor == prefs::Editor::kTimes);
    REQUIRE(prefs::resolve(toggle, backend, backend.caps()).value == "off");
    REQUIRE(prefs::resolve(times, backend, backend.caps()).value == "22:00-07:00");
    REQUIRE(prefs::resolve(times, backend, backend.caps()).value_label == "22:00 to 07:00");
    // The times are set while the schedule is off, and it stays off.
    REQUIRE(prefs::set_value(backend, times, "23:30-06:15").ok);
    REQUIRE(backend.night_.from_min == 23 * 60 + 30);
    REQUIRE(backend.night_.to_min == 6 * 60 + 15);
    REQUIRE_FALSE(backend.night_.enabled);
    REQUIRE(prefs::set_value(backend, toggle, "on").ok);
    REQUIRE(backend.night_.enabled);
    REQUIRE(backend.night_.from_min == 23 * 60 + 30);
    // ... and set again while it is on, it stays on.
    REQUIRE(prefs::set_value(backend, times, "21:00-05:00").ok);
    REQUIRE(backend.night_.enabled);
    REQUIRE(prefs::resolve(times, backend, backend.caps()).value == "21:00-05:00");
    // What is not two times of a day is refused, and nothing changes. The idle screen's setting is not here.
    const nv::NightSchedule before = backend.night_;
    for (const char* bad : {"25:00-07:00", "22:00", "22:00-07:00 off", "later", ""}) {
        INFO(bad);
        REQUIRE_FALSE(prefs::set_value(backend, times, bad).ok);
    }
    REQUIRE(backend.night_ == before);
    REQUIRE_FALSE(prefs::set_value(backend, toggle, "maybe").ok);
    REQUIRE_FALSE(backend.night_.idle);
}

TEST_CASE("the night schedule needs a screen, a touch to light it and a clock to tell the time by", "[preferences]") {
    for (const char* id : {"night-schedule", "night-times", "night-idle"}) {
        INFO(id);
        FakeBackend no_screen(iclforge::variants::c6_16mb());
        REQUIRE(contains(prefs::resolve(*prefs::find(id), no_screen, no_screen.caps()).availability.reason, "no screen"));

        FakeBackend no_touch(iclforge::variants::c3_spi_320x240_no_touch());
        REQUIRE(contains(prefs::resolve(*prefs::find(id), no_touch, no_touch.caps()).availability.reason, "no touch"));

        BoardCaps caps = iclforge::variants::p4_rev3_kit();
        caps.wall_clock = false;
        FakeBackend no_time(caps);
        REQUIRE(contains(prefs::resolve(*prefs::find(id), no_time, caps).availability.reason, "keeps no time"));
        REQUIRE_FALSE(prefs::set_value(no_time, *prefs::find(id), "on").ok);
        REQUIRE(no_time.sets == 0);
    }
}

TEST_CASE("the idle screen's night setting is its own, and is moot while the schedule is off", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& idle = *prefs::find("night-idle");
    REQUIRE(idle.kind == prefs::Kind::kToggle);
    REQUIRE(std::string(idle.route) == "/night-idle");
    REQUIRE(prefs::resolve(idle, backend, backend.caps()).value == "off");
    REQUIRE(contains(prefs::resolve(idle, backend, backend.caps()).inert, "night schedule is on"));
    REQUIRE(prefs::set_value(backend, *prefs::find("night-times"), "23:00-06:00").ok);
    REQUIRE(prefs::set_value(backend, *prefs::find("night-schedule"), "on").ok);
    REQUIRE(prefs::resolve(idle, backend, backend.caps()).inert.empty());
    // Turning it on changes nothing else of the schedule, and turning the schedule off keeps it.
    REQUIRE(prefs::set_value(backend, idle, "on").ok);
    REQUIRE(backend.night_.idle);
    REQUIRE(backend.night_.enabled);
    REQUIRE(backend.night_.from_min == 23 * 60);
    REQUIRE(backend.night_.to_min == 6 * 60);
    REQUIRE(prefs::set_value(backend, *prefs::find("night-schedule"), "off").ok);
    REQUIRE(backend.night_.idle);
    REQUIRE(prefs::resolve(idle, backend, backend.caps()).value == "on");
    REQUIRE_FALSE(prefs::set_value(backend, idle, "maybe").ok);
    REQUIRE(backend.night_.idle);
}

TEST_CASE("the night schedule is moot where the screen is dark already or says nothing, and says why", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    for (const char* id : {"night-schedule", "night-times"}) {
        INFO(id);
        const prefs::Pref& pref = *prefs::find(id);
        REQUIRE(prefs::resolve(pref, backend, backend.caps()).inert.empty());
        backend.view_ = nv::View::kDark;
        REQUIRE(contains(prefs::resolve(pref, backend, backend.caps()).inert, "dark already"));
        backend.view_ = nv::View::kOff;
        REQUIRE(contains(prefs::resolve(pref, backend, backend.caps()).inert, "Off"));
        for (const nv::View view : {nv::View::kSpeakers, nv::View::kTitle, nv::View::kClock}) {
            backend.view_ = view;
            REQUIRE(prefs::resolve(pref, backend, backend.caps()).inert.empty());
        }
        // Moot, not impossible: still available.
        backend.view_ = nv::View::kDark;
        REQUIRE(prefs::resolve(pref, backend, backend.caps()).availability.available);
        backend.view_ = nv::kDefaultView;
    }
}

TEST_CASE("the time zone is chosen by name or by string, and read back as a person says it", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& zone = *prefs::find("time-zone");
    REQUIRE(zone.kind == prefs::Kind::kText);
    REQUIRE(zone.editor == prefs::Editor::kZone);
    REQUIRE(zone.group == prefs::Group::kTime);
    prefs::Entry entry = prefs::resolve(zone, backend, backend.caps());
    REQUIRE(entry.value == "none");
    REQUIRE(entry.value_label == "UTC");
    // By name: the string is stored with it, and the label has the region after the city.
    REQUIRE(prefs::set_value(backend, zone, "Australia/Melbourne").ok);
    REQUIRE(backend.zone_.name == "Australia/Melbourne");
    REQUIRE(backend.zone_.posix == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    entry = prefs::resolve(zone, backend, backend.caps());
    REQUIRE(entry.value == "Australia/Melbourne");
    REQUIRE(entry.value_label == "Melbourne (Australia)");
    // A city with a place between its region and itself says that place and not the region, and UTC is itself.
    REQUIRE(prefs::detail::time_zone_label("America/Argentina/Buenos_Aires") == "Buenos Aires (Argentina)");
    REQUIRE(prefs::detail::time_zone_label("UTC") == "UTC");
    // By string: no name stored, the string is the word the route reads back, and a person reads the place the table
    // holds for it (Tokyo's string), never the string.
    REQUIRE(prefs::set_value(backend, zone, "JST-9").ok);
    REQUIRE(backend.zone_.name.empty());
    entry = prefs::resolve(zone, backend, backend.caps());
    REQUIRE(entry.value == "JST-9");
    REQUIRE(entry.value_label == "Tokyo (Asia)");
    // A zone set through a page that kept only the string (Melbourne's rule): the place, as if it had been picked.
    REQUIRE(prefs::set_value(backend, zone, "AEST-10AEDT,M10.1.0,M4.1.0/3").ok);
    REQUIRE(backend.zone_.name.empty());
    entry = prefs::resolve(zone, backend, backend.caps());
    REQUIRE(entry.value == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(entry.value_label == "Melbourne (Australia)");
    // A string in no table: its offset, and that it is the person's own. Never the string.
    REQUIRE(prefs::set_value(backend, zone, "ZZZ-9:45").ok);
    entry = prefs::resolve(zone, backend, backend.caps());
    REQUIRE(entry.value == "ZZZ-9:45");
    REQUIRE(entry.value_label == "UTC+09:45 (custom)");
    REQUIRE(entry.value_label.find("ZZZ") == std::string::npos);
    // None takes the zone away.
    REQUIRE(prefs::set_value(backend, zone, "none").ok);
    REQUIRE(backend.zone_.posix.empty());
    REQUIRE(prefs::resolve(zone, backend, backend.caps()).value_label == "UTC");
    // What is neither is refused, and nothing is stored.
    const int sets = backend.sets;
    for (const char* bad : {"Melbourne", "australia/melbourne", "AEST", ""}) {
        INFO(bad);
        REQUIRE_FALSE(prefs::set_value(backend, zone, bad).ok);
    }
    REQUIRE(backend.sets == sets);
    // Every name in the table is a word the entry takes.
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        REQUIRE(prefs::set_value(backend, zone, wc::time_zone_at(i).name).ok);
        REQUIRE(prefs::resolve(zone, backend, backend.caps()).value == wc::time_zone_at(i).name);
    }
}

TEST_CASE("the time zone is greyed on a board that keeps no time", "[preferences]") {
    BoardCaps caps = iclforge::variants::p4_rev3_kit();
    caps.wall_clock = false;
    FakeBackend backend(caps);
    const prefs::Entry entry = prefs::resolve(*prefs::find("time-zone"), backend, caps);
    REQUIRE_FALSE(entry.availability.available);
    REQUIRE(contains(entry.availability.reason, "keeps no time"));
    REQUIRE_FALSE(prefs::set_value(backend, *prefs::find("time-zone"), "Australia/Melbourne").ok);
    REQUIRE(backend.sets == 0);
    // A board with no screen keeps its zone for its page and its status: the Time group does not need one.
    FakeBackend no_screen(iclforge::variants::c6_16mb());
    REQUIRE(prefs::resolve(*prefs::find("time-zone"), no_screen, no_screen.caps()).availability.available);
}

TEST_CASE("the listing carries the new entries with their routes, kinds and values", "[preferences]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    REQUIRE(prefs::set_value(backend, *prefs::find("time-zone"), "Australia/Melbourne").ok);
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "\"id\":\"brightness\",\"group\":\"Display\",\"label\":\"Brightness\",\"kind\":\"stepper\""));
    REQUIRE(contains(json, "\"route\":\"/brightness\",\"value\":\"80\",\"available\":true"));
    REQUIRE(contains(json, "\"id\":\"clock-hours\""));
    REQUIRE(contains(json, "\"route\":\"/clock-format\",\"value\":\"24\""));
    REQUIRE(contains(json, "\"id\":\"night-times\",\"group\":\"Display\",\"label\":\"Night from and to\",\"kind\":\"text\""));
    REQUIRE(contains(json, "\"value\":\"22:00-07:00\""));
    REQUIRE(contains(json, "\"id\":\"time-zone\",\"group\":\"Time\",\"label\":\"Time zone\",\"kind\":\"text\""));
    REQUIRE(contains(json, "\"route\":\"/timezone\",\"value\":\"Australia/Melbourne\",\"available\":true"));
    // A text has no choices to list.
    const std::size_t at = json.find("\"id\":\"time-zone\"");
    REQUIRE(json.find("\"choices\"", at) == std::string::npos);
}

// --- Sound: the identify tone and the per-speaker trim ---------------------------------------------------------------

TEST_CASE("Identify speakers is a button that starts the tone through every speaker, and the listing says when it plays",
          "[preferences][sound]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& identify = *prefs::find("identify-speakers");
    REQUIRE(identify.kind == prefs::Kind::kAction);
    REQUIRE(identify.group == prefs::Group::kSound);
    REQUIRE(std::string(identify.route) == "/identify");
    // The sheet it opens has Stop; the button on the list is the one that says what it does.
    REQUIRE(std::string(identify.button) == "Identify");

    const prefs::Entry rest = prefs::resolve(identify, backend, backend.caps());
    REQUIRE(rest.availability.available);
    REQUIRE(rest.value == "run");
    REQUIRE(rest.inert.empty());

    // A press starts the tone through the board.
    const prefs::SetResult started = prefs::set_value(backend, identify, "run");
    REQUIRE(started.ok);
    REQUIRE(backend.identifying_);
    REQUIRE(backend.tones_started == 1);
    const prefs::Entry playing = prefs::resolve(identify, backend, backend.caps());
    REQUIRE(playing.value == "running");
    REQUIRE(playing.value_label == "Stop");

    // Pressed again while it plays, it starts over (the list's button never stops anything; the sheet's Stop does).
    REQUIRE(prefs::set_value(backend, identify, "run").ok);
    REQUIRE(backend.identifying_);
    REQUIRE(backend.tones_started == 2);
}

TEST_CASE("Identify speakers says why it cannot play now, and the board's refusal is reported", "[preferences][sound]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& identify = *prefs::find("identify-speakers");
    backend.blocked_ = "A song is playing: the test tone waits until it stops.";
    const prefs::Entry blocked = prefs::resolve(identify, backend, backend.caps());
    REQUIRE(blocked.availability.available);  // the board can; it is busy
    REQUIRE(blocked.inert == backend.blocked_);
    const prefs::SetResult refused = prefs::set_value(backend, identify, "run");
    REQUIRE_FALSE(refused.ok);
    REQUIRE_FALSE(backend.identifying_);

    // A tone that is on is not blocked by what came after it (the board ended it): the row says nothing of it.
    backend.blocked_.clear();
    REQUIRE(prefs::set_value(backend, identify, "run").ok);
    backend.blocked_ = "A song is playing: the test tone waits until it stops.";
    REQUIRE(prefs::resolve(identify, backend, backend.caps()).inert.empty());
}

TEST_CASE("the speaker trim is a sheet whose row says what is set, and its text goes to the board whole",
          "[preferences][sound]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& trim = *prefs::find("speaker-trim");
    REQUIRE(trim.kind == prefs::Kind::kSheet);
    REQUIRE(std::string(prefs::kind_name(trim.kind)) == "sheet");
    REQUIRE(std::string(trim.route) == "/trim");
    REQUIRE(prefs::resolve(trim, backend, backend.caps()).value == "none set");

    REQUIRE(prefs::set_value(backend, trim, "L level=-1.5 distance=2.4").ok);
    REQUIRE(backend.trim_text_ == "L level=-1.5 distance=2.4");
    const prefs::Entry set = prefs::resolve(trim, backend, backend.caps());
    REQUIRE(set.value == "1 speaker set");
    REQUIRE(set.value_label == "1 speaker set");

    backend.refuse_next = true;
    const prefs::SetResult refused = prefs::set_value(backend, trim, "reset");
    REQUIRE_FALSE(refused.ok);
    REQUIRE(backend.trim_summary_ == "1 speaker set");
}

TEST_CASE("a build with no player has no tone and no trim, says why, and the board is never asked",
          "[preferences][sound]") {
    BoardCaps caps = iclforge::variants::p4_rev3_kit();
    caps.sendspin = false;
    FakeBackend backend(caps);
    for (const char* id : {"identify-speakers", "speaker-trim"}) {
        INFO(id);
        const prefs::Entry entry = prefs::resolve(*prefs::find(id), backend, backend.caps());
        REQUIRE_FALSE(entry.availability.available);
        REQUIRE(contains(entry.availability.reason, "no player"));
        const prefs::SetResult result = prefs::set_value(backend, *prefs::find(id), "run");
        REQUIRE_FALSE(result.ok);
        REQUIRE(contains(result.message, "no player"));
    }
    REQUIRE(backend.sets == 0);
}

TEST_CASE("the listing carries the Sound group, after Display, with the kinds action and sheet", "[preferences][sound]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    backend.trim_summary_ = "3 speakers set";
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "\"id\":\"identify-speakers\",\"group\":\"Sound\",\"label\":\"Identify speakers\",\"kind\":\"action\""));
    REQUIRE(contains(json, "\"route\":\"/identify\",\"value\":\"run\",\"available\":true"));
    REQUIRE(contains(json, "\"id\":\"speaker-trim\",\"group\":\"Sound\",\"label\":\"Speaker levels and distances\",\"kind\":\"sheet\""));
    REQUIRE(contains(json, "\"route\":\"/trim\",\"value\":\"3 speakers set\",\"available\":true"));
    REQUIRE(json.find("\"id\":\"dim-after\"") < json.find("\"id\":\"identify-speakers\""));
    // A sheet has no choices to list (the entries after it may).
    const std::size_t at = json.find("\"id\":\"speaker-trim\"");
    const std::size_t next = json.find("\"id\":", at + 1);
    REQUIRE(json.substr(at, next - at).find("\"choices\"") == std::string::npos);
}

TEST_CASE("late night is a choice of standard and late night, in the Sound group, applied from the next stream",
          "[preferences][sound][late-night]") {
    const prefs::Pref* const pref = prefs::find("late-night");
    REQUIRE(pref != nullptr);
    CHECK(pref->group == prefs::Group::kSound);
    CHECK(pref->kind == prefs::Kind::kChoice);
    CHECK(pref->apply == prefs::Apply::kNextPlay);
    CHECK(std::string_view(pref->route) == "/dynamic-range");
    CHECK(std::string_view(pref->default_word) == "standard");
    // Chosen once and left: Reset preferences puts it back.
    CHECK(pref->on_reset == prefs::OnReset::kReset);
    REQUIRE(pref->choices.size() == 2);
    CHECK(std::string_view(pref->choices[0].id) == "standard");
    CHECK(std::string_view(pref->choices[1].id) == "late-night");
    // The note says what it does not do: PCM carries nothing to compress.
    CHECK(contains(pref->note, "PCM"));
}

TEST_CASE("late night starts on standard, goes to the board and reads back", "[preferences][sound][late-night]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("late-night");
    prefs::Entry entry = prefs::resolve(pref, backend, backend.caps());
    REQUIRE(entry.availability.available);
    CHECK(entry.value == "standard");
    CHECK(entry.value_label == "Standard");

    REQUIRE(prefs::set_value(backend, pref, "late-night").ok);
    CHECK(backend.late_night_);
    entry = prefs::resolve(pref, backend, backend.caps());
    CHECK(entry.value == "late-night");
    CHECK(entry.value_label == "Late night");

    REQUIRE(prefs::set_value(backend, pref, "standard").ok);
    CHECK_FALSE(backend.late_night_);
    CHECK(backend.sets == 2);
}

TEST_CASE("late night refuses a word that is not one of its two, and a board that cannot store it says so",
          "[preferences][sound][late-night]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("late-night");
    for (const char* word : {"Late-night", "late night", "wide", "on", ""}) {
        INFO(word);
        CHECK_FALSE(prefs::set_value(backend, pref, word).ok);
    }
    CHECK(backend.sets == 0);
    backend.refuse_next = true;
    CHECK_FALSE(prefs::set_value(backend, pref, "late-night").ok);
    CHECK_FALSE(backend.late_night_);
}

TEST_CASE("late night is greyed on a build with no Sendspin player, says why, and the board is never asked",
          "[preferences][sound][late-night]") {
    BoardCaps caps = iclforge::variants::p4_rev3_kit();
    caps.sendspin = false;
    FakeBackend backend(caps);
    const prefs::Pref& pref = *prefs::find("late-night");
    const prefs::Entry entry = prefs::resolve(pref, backend, backend.caps());
    REQUIRE_FALSE(entry.availability.available);
    CHECK(contains(entry.availability.reason, "decodes no Dolby stream"));
    for (const prefs::ChoiceState& choice : entry.choices) {
        CHECK_FALSE(choice.availability.available);
        CHECK(choice.availability.reason == entry.availability.reason);
    }
    const prefs::SetResult result = prefs::set_value(backend, pref, "late-night");
    CHECK_FALSE(result.ok);
    CHECK(contains(result.message, "decodes no Dolby stream"));
    CHECK(backend.sets == 0);
}

TEST_CASE("Reset preferences puts late night back on standard through its own setter",
          "[preferences][sound][late-night]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    REQUIRE(prefs::set_value(backend, *prefs::find("late-night"), "late-night").ok);
    const int before = backend.sets;
    const prefs::ResetReport report = prefs::reset_preferences(backend);
    CHECK(report.failed.empty());
    CHECK_FALSE(backend.late_night_);
    CHECK(std::count(report.reset.begin(), report.reset.end(), "late-night") == 1);
    CHECK(backend.sets > before);
    // Already on standard, it is left alone: the board is not asked again.
    const int after = backend.sets;
    const prefs::ResetReport again = prefs::reset_preferences(backend);
    CHECK(std::count(again.reset.begin(), again.reset.end(), "late-night") == 0);
    CHECK(backend.sets == after);
}

TEST_CASE("the listing carries late night in the Sound group with its route and value", "[preferences][sound][late-night]") {
    FakeBackend backend(iclforge::variants::p4_rev3_kit());
    backend.late_night_ = true;
    const std::string json = prefs::to_json(backend);
    REQUIRE(json_is_balanced(json));
    REQUIRE(contains(json, "\"id\":\"late-night\",\"group\":\"Sound\",\"label\":\"Late night\",\"kind\":\"choice\""));
    REQUIRE(contains(json, "\"apply\":\"next-play\",\"route\":\"/dynamic-range\",\"value\":\"late-night\",\"available\":true"));
}
