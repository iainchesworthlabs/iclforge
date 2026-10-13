#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "scripted_machine.hpp"

// apps/crucible/ui/src/scripted_machine.cpp: the machine `crucible --shot ... --app Name` puts the
// window over, so that a picture of it is the same on any computer (docs/crucible/room.md's
// pictures, tools/pictures/pictures.json). The option's words, the applications as a session
// monitor lists them, and the engine running over the machine and finding them where the window
// would. What the window then draws is a capture's own, and is looked at.

namespace {

using iclforge::crucible::ui::make_scripted_machine;
using iclforge::crucible::ui::parse_scripted_app;
using iclforge::crucible::ui::ScriptedApp;
using iclforge::crucible::ui::scripted_sessions;

const char* const kRule = "Every running application with a window is listed.";

}  // namespace

TEST_CASE("[scripted-machine] a name alone is an application with a window, playing", "[crucible][capture]") {
    const auto app = parse_scripted_app("Microsoft Edge");
    REQUIRE(app.has_value());
    CHECK(app->name == "Microsoft Edge");
    CHECK(app->sounding);
    CHECK(app->session);
    CHECK(app->window);
}

TEST_CASE("[scripted-machine] its words say what is not so of it, in any order and case", "[crucible][capture]") {
    const auto idle = parse_scripted_app("Hearth,idle");
    REQUIRE(idle.has_value());
    CHECK_FALSE(idle->sounding);
    CHECK(idle->session);

    // A silent one has no session at all, so it cannot be sounding either.
    const auto silent = parse_scripted_app("Hearth, SILENT");
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->sounding);
    CHECK_FALSE(silent->session);
    CHECK(silent->window);

    const auto both = parse_scripted_app("Steam,background,idle");
    REQUIRE(both.has_value());
    CHECK_FALSE(both->window);
    CHECK_FALSE(both->sounding);
    CHECK(both->session);
}

TEST_CASE("[scripted-machine] a spec with no name, or a word it does not know, is refused", "[crucible][capture]") {
    CHECK_FALSE(parse_scripted_app("").has_value());
    CHECK_FALSE(parse_scripted_app("   ").has_value());
    CHECK_FALSE(parse_scripted_app(",idle").has_value());
    CHECK_FALSE(parse_scripted_app("Hearth,loud").has_value());
    CHECK_FALSE(parse_scripted_app("Hearth,idle,").has_value());
}

TEST_CASE("[scripted-machine] the applications are listed in order, with ids from 1000", "[crucible][capture]") {
    const std::vector<ScriptedApp> apps{
        {.name = "Hearth"},
        {.name = "Microsoft Edge", .sounding = false},
        {.name = "Steam", .sounding = false, .session = false, .window = false},
    };
    const auto listed = scripted_sessions(apps);
    REQUIRE(listed.size() == 3);

    CHECK(listed[0].app == 1000);
    CHECK(listed[1].app == 1001);
    CHECK(listed[2].app == 1002);

    // The room shows the description; the image name is what the platforms give, in lower case.
    CHECK(listed[1].description == "Microsoft Edge");
    CHECK(listed[1].name == "microsoft edge");
    // No executable, so no icon of a machine's own is drawn.
    CHECK(listed[0].image_path.empty());

    CHECK(listed[0].active);
    CHECK(listed[0].has_session);
    CHECK(listed[0].has_window);

    CHECK_FALSE(listed[1].active);
    CHECK(listed[1].has_session);

    CHECK_FALSE(listed[2].active);
    CHECK_FALSE(listed[2].has_session);
    CHECK_FALSE(listed[2].has_window);
}

TEST_CASE("[scripted-machine] the machine is one pair of speakers and no silent device", "[crucible][capture]") {
    const auto machine = make_scripted_machine({{.name = "Hearth"}}, kRule);

    REQUIRE(machine.sessions != nullptr);
    CHECK(machine.sessions->listing_rule() == kRule);
    const auto listed = machine.sessions->refresh();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].description == "Hearth");

    REQUIRE(machine.devices != nullptr);
    const auto endpoints = machine.devices->render_devices(0);
    REQUIRE(endpoints.size() == 1);
    CHECK(endpoints[0].is_default);
    CHECK(endpoints[0].spatial);

    // The sound settings and the engine's enumeration say the same about the default.
    REQUIRE(machine.default_device != nullptr);
    CHECK(machine.default_device->default_id() == endpoints[0].id);
    REQUIRE(machine.default_device->endpoints().size() == 1);

    // Nothing to send applications to, and nothing to make one with.
    REQUIRE(machine.virtual_device != nullptr);
    const auto state = machine.virtual_device->state({});
    CHECK(state.needed);
    CHECK_FALSE(state.present);
    CHECK_FALSE(state.can_install);
    CHECK_FALSE(state.blocker.empty());
}

TEST_CASE("[scripted-machine] a tap is produced at the rate it would be played, and not all at once",
          "[crucible][capture]") {
    const auto machine = make_scripted_machine({{.name = "Hearth"}}, kRule);
    auto tap = machine.devices->tap();
    REQUIRE(tap != nullptr);
    REQUIRE(tap->start(1000, 48000, 2).has_value());

    // Straight after it began there is next to nothing, and what a reader asks for is not
    // handed over in one go: an engine that is given everything it asks for never waits.
    std::vector<float> asked(48000 * 2);
    CHECK(tap->available() < asked.size() / 2);
    CHECK(tap->read(asked) < asked.size() / 2);

    // After a little while there is some, in whole frames of both channels, and it is a tone.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::size_t waiting = tap->available();
    CHECK(waiting > 0);
    const std::size_t got = tap->read(asked);
    CHECK(got > 0);
    CHECK(got % 2 == 0);
    float loudest = 0.0F;
    for (std::size_t i = 0; i < got; ++i) {
        loudest = std::max(loudest, std::abs(asked[i]));
    }
    CHECK(loudest > 0.0F);
    CHECK(loudest < 0.5F);
    tap->stop();
}

TEST_CASE("[scripted-machine] the engine over it lists the applications and plays them", "[crucible][capture]") {
    const auto machine =
        make_scripted_machine({{.name = "Hearth"}, {.name = "Microsoft Edge", .sounding = false}}, kRule);

    iclforge::crucible::EngineConfig config;
    config.devices = machine.devices;
    config.sessions = machine.sessions;
    config.foreground = machine.foreground;
    iclforge::crucible::Engine engine(config);
    REQUIRE(engine.start().has_value());

    // Thirty seconds, as the engine's own cases wait: a poll that returns the moment it holds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto listed = [&engine] { return engine.status().apps.size() == 2; };
    while (!listed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(listed());

    auto apps = engine.status().apps;
    std::ranges::sort(apps, {}, &iclforge::crucible::AppStatus::app);
    CHECK(apps[0].description == "Hearth");
    CHECK(apps[0].active);
    CHECK(apps[1].description == "Microsoft Edge");
    CHECK_FALSE(apps[1].active);

    engine.stop();
}
