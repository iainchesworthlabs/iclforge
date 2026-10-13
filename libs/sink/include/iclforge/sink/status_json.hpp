#pragma once

#include <string>

#include "iclforge/sink/sink.hpp"

// The sink's status as one line of JSON for a layer that has no C++ types (the Android app's JNI,
// over NativeSink.statusJson()): who is connected and what each is doing, how its stream leaves
// the sink (the role's `output` state, D6) and what the output found in it (the role's `decoder`
// state, for the screen's format chip), the code a server asked to pair by, and how many servers
// are paired. Strings are escaped to the letter of RFC 8259 and left in UTF-8.

namespace iclforge::sink {

[[nodiscard]] std::string status_json(const Sink::Status& status);

}  // namespace iclforge::sink
