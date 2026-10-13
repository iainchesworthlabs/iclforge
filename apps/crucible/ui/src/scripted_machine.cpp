#include "scripted_machine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

// An application's tap on a machine that is a list: a quiet steady tone, produced at the rate it
// would be played, which is what a real tap is ("silence-filled at wall-clock rate by the
// producer", audio_devices.hpp). The test suites' own tap hands over whatever is asked of it at
// once, which is right for a case that runs frames on demand and wrong for a window left up: the
// engine's frame loop would never wait, spin a core, and report a cadence of two milliseconds.
class PacedTap final : public TapSource {
public:
    std::expected<void, std::string> start(std::uint32_t process_id, std::uint32_t sample_rate,
                                           std::uint16_t channels) override {
        const std::lock_guard lock(mutex_);
        rate_ = sample_rate;
        channels_ = channels;
        // The same pitch for the same application as the test suites' tap gives it, so one is
        // told from another by ear if anyone listens to a scripted machine's taps.
        step_ = 2.0 * std::numbers::pi * 100.0 * static_cast<double>(1 + process_id % 100) /
                static_cast<double>(sample_rate);
        began_ = std::chrono::steady_clock::now();
        produced_ = 0;
        return {};
    }

    std::size_t read(std::span<float> out) override {
        const std::lock_guard lock(mutex_);
        if (channels_ == 0) {
            return 0;
        }
        // What was not taken within the backlog is gone, as in a ring that is not read.
        produced_ = std::max(produced_, oldest_kept_locked());
        const std::size_t n = std::min(out.size(), due_locked()) / channels_ * channels_;
        for (std::size_t i = 0; i < n; i += channels_) {
            const auto v = static_cast<float>(kLevel * std::sin(phase_));
            for (std::size_t c = 0; c < channels_; ++c) {
                out[i + c] = v;
            }
            phase_ += step_;
        }
        produced_ += n;
        return n;
    }

    [[nodiscard]] std::size_t available() const override {
        const std::lock_guard lock(mutex_);
        return due_locked();
    }

    void stop() override {}

private:
    // A tone 20 dB down, so that a meter has somewhere to go.
    static constexpr double kLevel = 0.1;
    // What the producer would have let go of by now and the reader has not taken. A reader that
    // stays away for a second is given the second, and not the whole time it was away.
    static constexpr double kBacklogSeconds = 1.0;

    // Samples produced since start(), in whole frames of all channels.
    [[nodiscard]] std::size_t produced_by_now_locked() const {
        if (rate_ == 0 || channels_ == 0) {
            return 0;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began_).count();
        return static_cast<std::size_t>(seconds * rate_) * channels_;
    }

    // The oldest sample still kept: a second of them, at most.
    [[nodiscard]] std::size_t oldest_kept_locked() const {
        const std::size_t total = produced_by_now_locked();
        const std::size_t cap = static_cast<std::size_t>(kBacklogSeconds * rate_) * channels_;
        return total > cap ? total - cap : 0;
    }

    [[nodiscard]] std::size_t due_locked() const {
        const std::size_t total = produced_by_now_locked();
        const std::size_t from = std::max(produced_, oldest_kept_locked());
        return total > from ? total - from : 0;
    }

    mutable std::mutex mutex_;
    std::uint32_t rate_ = 0;
    std::uint16_t channels_ = 0;
    double step_ = 0.0;
    double phase_ = 0.0;
    std::chrono::steady_clock::time_point began_;
    std::size_t produced_ = 0;
};

// The test suites' endpoint list and sinks, with the paced tap above in place of theirs.
class ScriptedDevices final : public AudioDevices {
public:
    explicit ScriptedDevices(std::vector<DeviceFacts> devices) { inner_.devices = std::move(devices); }

    std::vector<DeviceFacts> render_devices(std::uint32_t sample_rate) override {
        return inner_.render_devices(sample_rate);
    }
    std::unique_ptr<BurstSink> burst_sink() override { return inner_.burst_sink(); }
    std::unique_ptr<PcmSink> pcm_sink() override { return inner_.pcm_sink(); }
    std::unique_ptr<ObjectSink> object_sink() override { return inner_.object_sink(); }
    std::unique_ptr<TapSource> tap() override { return std::make_unique<PacedTap>(); }

private:
    testing::FakeDevices inner_;
};

// The silent device of a machine that has none and no driver to make one with: the state and the
// words the Windows implementation gives a copy that was downloaded (engine/src/platform/windows/
// driver_tools.cpp), which is the machine the documentation's pictures are of.
class ScriptedSilentDevice final : public VirtualDevice {
public:
    [[nodiscard]] std::string device_name() const override { return "Crucible Silent Output"; }
    [[nodiscard]] std::string how_to_get_one() const override { return "install the driver (Settings)"; }
    // A package in a folder, as on Windows: the page shows the driver's folder and its state.
    [[nodiscard]] bool from_package() const override { return true; }

    SilentDeviceState state(const SilentDeviceQuery& /*query*/) override {
        return {.needed = true,
                .present = false,
                .in_use = false,
                .can_install = false,
                .blocker = "no built driver package to install",
                .detail = {"no built package in the driver folder"}};
    }

    std::expected<void, std::string> install() override { return std::unexpected("this machine is scripted"); }
    std::expected<void, std::string> remove() override { return std::unexpected("this machine is scripted"); }
    DeviceActionStatus action_status() override { return {}; }
};

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
    machine.devices = std::make_shared<ScriptedDevices>(std::vector<DeviceFacts>{output});

    machine.foreground = std::make_shared<testing::FakeForeground>();

    // The sound settings agree with what the engine can see.
    auto default_device = std::make_shared<testing::FakeDefaultDevice>();
    default_device->set_endpoints({{.id = output.id, .name = output.name, .is_default = true}});
    machine.default_device = std::move(default_device);

    machine.virtual_device = std::make_shared<ScriptedSilentDevice>();
    return machine;
}

}  // namespace iclforge::crucible::ui
