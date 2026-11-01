#pragma once

#include <cstdint>

#include <string_view>

#include "iclforge/audio/passthrough.hpp"

// What has been run, per platform audio API, of sending TrueHD to a local output as MAT
// (planning/truehd-parity-hearth-player.md, H6; libs/containers/ERRATA.md, question 7).
//
// The output decision reads this and says so in its reason: Hearth does not claim what it has not
// tried (the document's definition of first class, point 7). A platform is NOT RUN until a run
// passes: a receiver that names the stream TrueHD, played from that platform by someone with the
// receiver on the desk. What the code does is a different thing from what has been seen to work,
// and this file is where the two are kept apart. Run on 2026-11-01, from a Raspberry Pi 4B into
// an Onkyo TX-RZ740 (Direct mode, status by eISCP, and for the aplay run the LCD): ALSA and
// PipeWire, each through this library's own PassthroughSink.
//
// To mark a platform as run: the owner (or a session working with the owner's receiver) plays a
// MAT stream through it to a receiver that shows TrueHD, the result goes into docs/platforms and
// planning/truehd-parity-hearth-player.md with the date, the receiver and the stream, and the
// row below changes in the same commit as the page that records the run. A test pins the table, so
// that a platform cannot become "run" without someone reading this.

namespace iclforge::hearth {

enum class MatRun : std::uint8_t {
    // The code exists (or does not) and no run on a receiver has shown it to work.
    kNotRun,
    // A run passed.
    kRun,
};

struct MatPlatformStatus {
    audio::PassthroughBackend backend = audio::PassthroughBackend::kNone;
    MatRun run = MatRun::kNotRun;
    // What is true of the platform's MAT output instead of a run: what it opens and what was
    // checked, in a clause a person can read after "TrueHD passthrough through <backend> has not
    // been run against a receiver:".
    std::string_view basis{};
};

[[nodiscard]] constexpr MatPlatformStatus mat_status(audio::PassthroughBackend backend) {
    using audio::PassthroughBackend;
    switch (backend) {
        case PassthroughBackend::kAlsa:
            return {backend, MatRun::kRun,
                    "an Onkyo TX-RZ740 reported TrueHD (and its display showed DTHD) for this "
                    "library's sink and for aplay on a Raspberry Pi 4B on 2026-11-01, at 48 kHz "
                    "and 96 kHz content"};
        case PassthroughBackend::kPipeWire:
            return {backend, MatRun::kRun,
                    "an Onkyo TX-RZ740 reported TrueHD for this library's sink on a Raspberry "
                    "Pi 4B on 2026-11-01, at 48 kHz content"};
        case PassthroughBackend::kWasapi:
            return {backend, MatRun::kNotRun,
                    "it offers the Dolby MLP subtype on 8 channels at 192 kHz (176.4 kHz) in "
                    "exclusive mode, and has never been opened on a device"};
        case PassthroughBackend::kCoreAudio:
            return {backend, MatRun::kNotRun,
                    "no Core Audio format is known for it, so the backend refuses it"};
        case PassthroughBackend::kAndroid:
            return {backend, MatRun::kNotRun,
                    "it needs an 8-channel IEC 61937 track from the app's bridge, and has not "
                    "been tried on a Shield"};
        case PassthroughBackend::kNone:
            break;
    }
    return {backend, MatRun::kNotRun, "this build has no passthrough backend"};
}

}  // namespace iclforge::hearth
