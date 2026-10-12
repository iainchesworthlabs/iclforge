#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The app's one sink (planning/hearth-android.md, phase 1b): iclforge::sink::Sink with outputs that
// count what they are given and drop it. The pass-through output replaces them in phase 1c; until
// then a stream a server sends is accepted, paired, timed and counted, and nobody hears it.
//
// One per process, started and stopped by the service. Nothing here is Android's: the JNI layer
// (jni_entry.cpp) is the only file that names a JNIEnv, so this is what a host test of the app's
// native core would link.

namespace iclforge::hearth_android {

struct SinkTotals {
    std::uint32_t connections = 0;
    std::uint32_t streams = 0;
    std::uint64_t chunks = 0;
    std::uint64_t frames = 0;
    std::uint32_t burst_streams = 0;
    std::uint64_t bursts = 0;
    std::uint64_t burst_frames = 0;
};

class NativeSink {
   public:
    NativeSink();
    ~NativeSink();
    NativeSink(const NativeSink&) = delete;
    NativeSink& operator=(const NativeSink&) = delete;
    NativeSink(NativeSink&&) = delete;
    NativeSink& operator=(NativeSink&&) = delete;

    // Starts the sink, listening on `port` (0 for any), its identity and pairing records in
    // `state_directory`. Empty on success, else what went wrong. Stops one that is running first.
    [[nodiscard]] std::string start(const std::string& state_directory, const std::string& name,
                                    std::uint16_t port);
    void stop();

    [[nodiscard]] bool running() const;
    // Another source holds the device (audio focus was taken), or has given it back: servers are
    // told, and what is playing waits.
    void set_external_source(bool external);
    // Whether a stream is open on the device, which is when the app holds audio focus.
    [[nodiscard]] bool streaming() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string client_id() const;
    [[nodiscard]] SinkTotals totals() const;
    // The lines the sink has logged since the last call, oldest first.
    [[nodiscard]] std::vector<std::string> drain_log();

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::hearth_android
