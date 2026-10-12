#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "iclforge/audio/passthrough.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/passthrough.hpp"

// The link of the pass-through output on Android (planning/hearth-android.md, phase 1c): the process's
// one AudioTrack, which iclforge::audio's passthrough backend owns through the app's registered
// PassthroughBridge. There is one device, and each connection's output has a link to it of its
// own that is good while that output is the one that opened it: a server that has been displaced
// is still holding an output, and must not write null bursts into the stream that displaced it.

namespace iclforge::hearth_android {

class SharedAudio {
   public:
    SharedAudio() = default;
    SharedAudio(const SharedAudio&) = delete;
    SharedAudio& operator=(const SharedAudio&) = delete;
    SharedAudio(SharedAudio&&) = delete;
    SharedAudio& operator=(SharedAudio&&) = delete;

    // A link for one output. Several may exist; the one that opened the track last owns it.
    [[nodiscard]] std::unique_ptr<sink::BurstLink> make_link();

    // The coded formats the route takes now, as the bridge says: asked each time, because a TV box
    // loses its HDMI route when the display sleeps and gets it back when it wakes.
    [[nodiscard]] std::vector<sendspin::player::DataType> routes() const;

   private:
    class Link;

    mutable std::mutex mutex_;
    audio::PassthroughSink track_;
    std::uint64_t owner_ = 0;
    std::uint64_t next_id_ = 1;
};

}  // namespace iclforge::hearth_android
