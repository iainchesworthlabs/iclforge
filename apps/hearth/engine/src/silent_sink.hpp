#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "pcm_sink.hpp"

// An output that plays to nothing (planning/karaoke-microphone.md, MI2; `hearth --sing`).
//
// It is a PcmSink with a device's behaviour and no device: it takes the rendered blocks, lets
// them "play" at the rate of the steady clock, and drops them, and it reports a position as a
// device does. So an item runs in real time, the engine paces itself against it and the heard
// position moves as it would, and nothing reaches a speaker, no endpoint is opened, and a machine
// with no sound output plays as well as any other.
//
// What it is for is a run nobody listens to that still needs an item to play for some seconds: a
// capture of a window part way through a sung performance, which on a real output would play the
// song at whoever runs it. The name is what the window shows as the output's.
//
// The device's clock begins with the first block submitted after an open or a flush and runs on
// from there, through whatever the engine does not supply in time, as a device's does.
//
// `clock` is the steady clock's nanoseconds, and is the only thing a test replaces.

namespace iclforge::hearth {

[[nodiscard]] std::unique_ptr<PcmSink> make_silent_sink(
    std::string device_name, std::function<std::int64_t()> clock = {});

}  // namespace iclforge::hearth
