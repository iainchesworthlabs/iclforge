#pragma once

#include "iclforge/base/executor.hpp"

// How a caller lets the decoder spread a frame's independent work over more than one thread.
// The interface is iclforge::base::Executor (libs/base/include/iclforge/base/executor.hpp), which
// the AC-3 and E-AC-3 decoders take as well; this is the name the AC-4 decoder, its tests and the
// ESP32 player's lane worker have always used for it.
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
// wait_async() joins it before decode() returns.

namespace iclforge::ac4 {

using Executor = iclforge::base::Executor;

}  // namespace iclforge::ac4
