#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "iclforge/sink/sink.hpp"

// The pass-through path end to end, in one process (planning/hearth-android.md, phase 1d, AR1): a
// Sendspin server session packs an AC-3 or E-AC-3 elementary stream into IEC 61937 bursts the way
// Hearth's server does, sends each as a burst chunk over a loopback WebSocket to an
// iclforge::sink::Sink whose extension-role output is the PassthroughOutput, whose link is a model
// of a device that writes down every burst it is handed. What the device was handed is then compared,
// byte for byte and in order, with the bursts the packer wrote: the rebuild from (Pc, Pd, payload)
// is held against the packers' own output through the transport, the sink, the output and the link,
// and on whatever machine this is run on.
//
// It runs in real time (a stream's own length and about a second and a half of lead), so it is a
// few seconds a fixture. The Android app's instrumented test runs it through JNI on the device
// (apps/hearth/android), and the host tests over every fixture (libs/sink/tests).

namespace iclforge::sink::harness {

struct Options {
    // How far ahead of its time each burst is sent, as a server does. The sink asks for 0.5 s by
    // default, and a link that has to open and prime needs more than that for its first burst.
    std::int64_t lead_us = 1'500'000;
    // How many times through the stream. Each pass is a burst stream's worth of the file; a stream
    // is sent once and its bursts repeated, as hearth-testserver plays a file over and over.
    int passes = 1;
    // The most bursts sent, from the start of the stream; 0 for all of them. A long clip is a long
    // run in real time, and the first hundred bursts (three seconds) carry everything the path does.
    std::size_t max_bursts = 0;
    // Where the sink keeps its identity and pairing records: a directory this may create.
    std::filesystem::path state_directory;
    // The most it waits for the stream to have been played out.
    std::int64_t timeout_us = 60'000'000;
};

struct Result {
    // Empty when the run completed; otherwise why it did not.
    std::string error;
    // The bursts the server sent, the real bursts the model device was handed, and the null bursts
    // it was handed between them.
    std::size_t sent = 0;
    std::size_t written = 0;
    std::size_t nulls = 0;
    // Whether what was written is what was sent, every burst, byte for byte and in the order sent.
    bool equal = false;
    // The index of the first burst that differs, or of the first missing one; sent if none.
    std::size_t first_difference = 0;
    // FNV-1a over the bytes of the bursts sent and of the ones written, in order, for a log.
    std::uint64_t sent_hash = 0;
    std::uint64_t written_hash = 0;
    // What the output says it did: late and dropped bursts and underruns are zero in a run that
    // had no trouble.
    OutputCounts counts;
    // Where the time went, in microseconds: the sink up and the stream started, the bursts sent and
    // played out, and the connection and the sink closed.
    std::int64_t setup_us = 0;
    std::int64_t play_us = 0;
    std::int64_t teardown_us = 0;
};

// Plays `stream`, an AC-3 or E-AC-3 elementary stream, through the path. `error` says what stopped
// the run, if anything; `equal` is the answer.
[[nodiscard]] Result play_through(std::span<const std::byte> stream, const Options& options);

}  // namespace iclforge::sink::harness
