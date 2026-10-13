#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio_devices.hpp"
#include "default_device.hpp"
#include "foreground.hpp"
#include "session_monitor.hpp"
#include "virtual_device.hpp"

// A machine that is a list, for `crucible --shot ... --app Name` (main.cpp): the window and the
// engine run as they do on a computer, over applications, a sound output and a silent device that
// are only what this says. A picture of the window taken over it reads no setting of a person's,
// opens no sound device, and shows no application that happens to be running, so it comes out
// the same on any machine and is heard on none.
//
// The same stand-ins the Qt Quick tests script their machines with (apps/crucible/engine/tests,
// fake_devices.hpp and fake_services.hpp), which the engine's own cases use too: the controller
// takes them through set_test_services(), so nothing in the window knows the difference.
//
// Qt-free, so that plain Catch2 cases hold it (test_scripted_machine.cpp): what an `--app` option
// means, and what machine the applications are on.

namespace iclforge::crucible::ui {

struct ScriptedApp {
    // The name the room lists it by ("Hearth", "Microsoft Edge"), which `--place` and `--lyrics`
    // find it by too, without regard to case.
    std::string name;
    // Playing: it has a session with sound in it, so there is something to tap.
    bool sounding = true;
    // It has an audio session at all. False is an application with a window and nothing to tap,
    // which the room lists greyed.
    bool session = true;
    // It owns a window. False is a background process, which the room leaves out unless it is
    // asked to show them.
    bool window = true;
};

// `Name`, or `Name,idle` (a session with no sound in it), `Name,silent` (no session at all) and
// `Name,background` (no window), in any order and any number. None for an empty name or a word
// that is not one of those.
[[nodiscard]] std::optional<ScriptedApp> parse_scripted_app(std::string_view spec);

// The applications as the session monitor lists them: ids from 1000 in the order given, the
// name for the room's display name, no executable (so the room draws a monogram and not a
// machine's own icon), a window unless `window` is false.
[[nodiscard]] std::vector<AppSession> scripted_sessions(const std::vector<ScriptedApp>& apps);

// The session monitor of a scripted machine: it lists what it was given, and says `rule` when it
// is asked what the list means. A picture of the room shows that sentence under the list, so it
// is the platform's own (what `platform_session_monitor()->listing_rule()` says), not a
// stand-in's.
class ScriptedSessions final : public SessionMonitor {
public:
    ScriptedSessions(std::vector<AppSession> apps, std::string rule)
        : apps_(std::move(apps)), rule_(std::move(rule)) {}

    [[nodiscard]] std::string listing_rule() const override { return rule_; }
    [[nodiscard]] std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep = {}) override;

private:
    std::vector<AppSession> apps_;
    std::string rule_;
};

// The five seams of the machine, to hand to CrucibleController::set_test_services().
struct ScriptedMachine {
    std::shared_ptr<SessionMonitor> sessions;
    std::shared_ptr<AudioDevices> devices;
    std::shared_ptr<Foreground> foreground;
    std::shared_ptr<DefaultDevice> default_device;
    std::shared_ptr<VirtualDevice> virtual_device;
};

// The machine the documentation's pictures are of: one pair of stereo speakers, which is the
// default and has a spatial sound format switched on, and no silent device and no driver to
// install one with, which is what a copy of Crucible that was downloaded finds on Windows.
[[nodiscard]] ScriptedMachine make_scripted_machine(const std::vector<ScriptedApp>& apps,
                                                    std::string listing_rule);

}  // namespace iclforge::crucible::ui
