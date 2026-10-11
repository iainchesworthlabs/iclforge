#pragma once

#include <cstddef>

// How a caller lets the decoder spread a frame's independent work over more than one thread.
//
// A frame's per-channel stages (the inverse transform and QMF analysis of each channel, the QMF
// synthesis and sample rate converter of each output) do not read each other's results, and a
// part with two cores and a single-precision FPU (the ESP32-S3, the ESP32-P4) has a core that the
// decoder, which is one thread, leaves to the network. A decoder given an Executor
// (DecoderConfig::executor) hands such a stage to it as `count` tasks; without one it runs them
// itself, in order, on the calling thread, as it always did. Each task does what the loop's
// iteration did, to state that no other task touches, so the decoded PCM is the same bit for bit
// whichever way they ran.
//
// The decoder also gives it one background task a frame, the next frame's syntax, which it reads
// while this frame's reconstruction runs (Decoder::decode()'s `next`): run_async() starts it and
// wait_async() joins it before decode() returns. An executor with no second lane runs it in
// run_async(), in place, which is the same reading in the same order.
//
// An executor is the caller's: it must outlive the decoder, and only one decoder (one thread)
// may call run(), run_async() and wait_async() on it at a time.

namespace iclforge::ac4 {

class Executor {
   public:
    // A task of a run: `context` is run()'s, `index` is in [0, count) and `lane` in
    // [0, lanes()). Tasks that run at once have different lanes, which is how a task finds the
    // scratch that is its own.
    using Task = void (*)(void* context, std::size_t index, std::size_t lane);

    Executor() = default;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    virtual ~Executor() = default;

    // How many tasks may be running at once, the calling thread's included: at least 1.
    [[nodiscard]] virtual std::size_t lanes() const noexcept = 0;

    // Calls task(context, index, lane) once for each index in [0, count), in any order and on
    // any lane, and returns when every call has returned. Lane 0 is the calling thread.
    virtual void run(std::size_t count, Task task, void* context) = 0;

    // Starts task(context, 0, 1) on the other lane and returns without waiting for it, or where
    // lanes() is 1 runs it now, on the calling thread as lane 0. The task must be over when
    // wait_async() returns, and the caller may call run() (whose tasks the other lane then takes
    // up when it is free) and nothing else of the executor's before that. At most one task is
    // outstanding.
    virtual void run_async(Task task, void* context) { task(context, 0, 0); }

    // Returns when the task run_async() started is over; at once where there is none.
    virtual void wait_async() {}
};

}  // namespace iclforge::ac4
