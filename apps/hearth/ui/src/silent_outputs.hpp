#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/audio/passthrough.hpp"
#include "iclforge/audio/speakers.hpp"
#include "silent_sink.hpp"
#include "test_outputs.hpp"

// The outputs of a run that plays to nothing (`hearth --sing`, main.cpp): one render endpoint,
// stereo, that is the default, and a sink that takes what is played and lets it go
// (silent_sink.hpp). Handed to HearthController::set_test_outputs() before the window starts it, so
// that the engine decides its output among this one endpoint, never reads the machine's own, and
// opens nothing: the same seam the Qt Quick suites give a fake room, used here for a capture that
// has to play an item for some seconds without sounding it.
//
// `name` is what the window shows as the output's. The machine's own would be one more thing of
// the machine in a picture, and is no more true of a run that does not use it.

namespace iclforge::hearth::ui {

[[nodiscard]] inline std::shared_ptr<TestOutputs> make_silent_outputs(const std::string& name) {
    audio::RenderDeviceInfo device;
    device.id = "silent";
    device.name = name;
    device.is_default = true;
    device.supports_exclusive_pcm = false;
    device.channels = 2;
    device.speakers = audio::default_speakers(2);
    device.sample_rates = {44100, 48000};

    auto outputs = std::make_shared<TestOutputs>();
    outputs->make_pcm = [name] { return make_silent_sink(name); };
    outputs->endpoints = [device](std::uint32_t /*sample_rate*/) {
        std::vector<EndpointReading> readings;
        readings.push_back(EndpointReading{.device = device});
        return readings;
    };
    outputs->enumerate = [device]() -> decltype(audio::enumerate_render_devices()) {
        return std::vector<audio::RenderDeviceInfo>{device};
    };
    return outputs;
}

}  // namespace iclforge::hearth::ui
