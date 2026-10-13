#include "scripted_machine.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "fake_devices.hpp"
#include "fake_services.hpp"

namespace iclforge::crucible::ui {

namespace {

constexpr AppId kFirstAppId = 1000;

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool same_word(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// What Windows' own sound settings call the output of a desktop with Realtek audio, and what a
// person with Windows Sonic switched on for it sees: the room's "Headphones" mode runs the
// objects through the platform's spatial renderer, at its limit of fifteen.
[[nodiscard]] DeviceFacts speakers() {
    DeviceFacts facts;
    facts.id = "scripted-speakers";
    facts.name = "Speakers (Realtek(R) Audio)";
    facts.is_default = true;
    facts.shared_channels = 2;
    facts.spatial = true;
    facts.spatial_max_objects = 15;
    return facts;
}

}  // namespace

std::optional<ScriptedApp> parse_scripted_app(std::string_view spec) {
    ScriptedApp app;
    bool first = true;
    while (true) {
        const auto comma = spec.find(',');
        const std::string_view word = trimmed(spec.substr(0, comma));
        if (first) {
            if (word.empty()) {
                return std::nullopt;
            }
            app.name = std::string(word);
            first = false;
        } else if (same_word(word, "idle")) {
            app.sounding = false;
        } else if (same_word(word, "silent")) {
            app.sounding = false;
            app.session = false;
        } else if (same_word(word, "background")) {
            app.window = false;
        } else {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            return app;
        }
        spec.remove_prefix(comma + 1);
    }
}

std::vector<AppSession> scripted_sessions(const std::vector<ScriptedApp>& apps) {
    std::vector<AppSession> listed;
    listed.reserve(apps.size());
    AppId next = kFirstAppId;
    for (const ScriptedApp& app : apps) {
        AppSession session;
        session.app = next++;
        // The image name is lower case with no extension, as the platforms give it; the
        // description is what the room shows.
        session.name = app.name;
        std::transform(session.name.begin(), session.name.end(), session.name.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        session.description = app.name;
        session.endpoint_name = speakers().name;
        session.active = app.session && app.sounding;
        session.has_session = app.session;
        session.has_window = app.window;
        session.session_pids.push_back(session.app);
        listed.push_back(std::move(session));
    }
    return listed;
}

std::vector<AppSession> ScriptedSessions::refresh(const std::vector<std::uint32_t>& /*keep*/) {
    // Everything is listed already, so there is nothing a placement has to keep.
    return apps_;
}

ScriptedMachine make_scripted_machine(const std::vector<ScriptedApp>& apps, std::string listing_rule) {
    ScriptedMachine machine;

    machine.sessions = std::make_shared<ScriptedSessions>(scripted_sessions(apps), std::move(listing_rule));

    const DeviceFacts output = speakers();
    auto devices = std::make_shared<testing::FakeDevices>();
    devices->devices = {output};
    machine.devices = std::move(devices);

    machine.foreground = std::make_shared<testing::FakeForeground>();

    // The sound settings agree with what the engine can see.
    auto default_device = std::make_shared<testing::FakeDefaultDevice>();
    default_device->set_endpoints({{.id = output.id, .name = output.name, .is_default = true}});
    machine.default_device = std::move(default_device);

    // The state the Windows implementation gives a copy that was downloaded: no device, and
    // the package that would make one is not in the folder (engine/src/platform/windows/
    // driver_tools.cpp). Its own words, so the window says what it says there.
    auto silent = std::make_shared<testing::FakeVirtualDevice>();
    silent->set_device_name("Crucible Silent Output");
    silent->set_state({.needed = true,
                       .present = false,
                       .in_use = false,
                       .can_install = false,
                       .blocker = "no built driver package to install",
                       .detail = {"no built package in the driver folder"}});
    machine.virtual_device = std::move(silent);
    return machine;
}

}  // namespace iclforge::crucible::ui
