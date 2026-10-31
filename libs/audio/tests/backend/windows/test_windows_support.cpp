#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>

#include "windows_support.hpp"

// The Windows backend's pure half, tested with no live audio device: a plain
// Windows CI runner has WASAPI/COM available (they are core OS components,
// not drivers), so the only thing this file cannot exercise is what
// ComScope/make_enumerator actually touch - COM itself, and whatever
// endpoints happen to be attached. What is left, and worth testing on its
// own, is stream_gone(): a pure HRESULT classifier, the same shape
// libs/audio/tests/backend/android/test_android_support.cpp's track_is_dead() is for
// AudioTrack.write()'s return value.
//
// CMake adds this file to the suite only when it selected the windows/
// platform directory, and puts that directory on the include path - the same
// selection libs/audio/tests/backend/alsa, libs/audio/tests/backend/android and
// libs/audio/tests/backend/macos already use for their own backend's internal header.

using iclforge::windows_audio::stream_gone;

TEST_CASE("an invalidated or stopped device ends the stream") {
    // The two answers Microsoft's own "Recovering from an Invalid-Device
    // Error" names: the endpoint went away, or the audio service that ran it
    // did. Getting either wrong means a render thread spins forever against
    // a device that is never coming back.
    CHECK(stream_gone(AUDCLNT_E_DEVICE_INVALIDATED));
    CHECK(stream_gone(AUDCLNT_E_SERVICE_NOT_RUNNING));
}

TEST_CASE("success is never mistaken for the stream having gone") {
    CHECK_FALSE(stream_gone(S_OK));
}

TEST_CASE("a driver merely declining the call is not the stream ending") {
    // The distinction this predicate exists to draw: passthrough.cpp calls it
    // around GetCurrentPadding specifically because real exclusive-mode
    // hardware has been seen to refuse that call for reasons that are not
    // "the device is gone" - see windows_support.hpp's own comment. Any other
    // FAILED() HRESULT has to read as "answer withheld", not "stream over",
    // or a driver hiccup would end a stream that is still there.
    CHECK_FALSE(stream_gone(E_FAIL));
    CHECK_FALSE(stream_gone(AUDCLNT_E_BUFFER_TOO_LARGE));
    CHECK_FALSE(stream_gone(AUDCLNT_E_BUFFER_ERROR));
}

TEST_CASE("the performance counter and the steady clock share an epoch") {
    // Capture stamps a packet with the performance counter's reading (GetBuffer's QPC position, in
    // 100 ns) and says the time is on std::chrono::steady_clock's clock (capture.hpp,
    // CaptureStamp): a consumer compares it with a playback position taken from that clock. The
    // claim is that the two count from the same zero, which this holds the standard library to.
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    REQUIRE(QueryPerformanceFrequency(&frequency));
    const auto before = std::chrono::steady_clock::now().time_since_epoch();
    REQUIRE(QueryPerformanceCounter(&counter));
    const auto after = std::chrono::steady_clock::now().time_since_epoch();

    const std::int64_t whole_seconds = counter.QuadPart / frequency.QuadPart;
    const std::int64_t remainder = counter.QuadPart % frequency.QuadPart;
    const std::int64_t counter_ns = whole_seconds * 1'000'000'000LL + remainder * 1'000'000'000LL / frequency.QuadPart;

    const auto before_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(before).count();
    const auto after_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after).count();
    // The counter was read between the two clock reads, to within a millisecond of slack.
    CHECK(counter_ns >= before_ns - 1'000'000);
    CHECK(counter_ns <= after_ns + 1'000'000);
}
