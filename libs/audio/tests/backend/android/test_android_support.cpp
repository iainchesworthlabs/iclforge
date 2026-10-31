#include <catch2/catch_test_macros.hpp>

#include "android_support.hpp"

// The Android backend's pure half, tested on a machine with no Android
// SDK/NDK at all - see android_support.hpp's header comment. Matches
// libs/audio/tests/backend/alsa/test_alsa_device_names.cpp's role for that backend:
// CMake adds this file to the suite only when it selected the android/
// platform directory, and puts that directory on the include path.

using iclforge::android_audio::burst_bytes_for;
using iclforge::android_audio::carrier_rate;
using iclforge::android_audio::make_render_device_info;
using iclforge::android_audio::track_is_dead;
using iclforge::audio::BitstreamFormat;

TEST_CASE("burst size follows the format, not a fixed guess") {
    // Getting this wrong means submit() validates bursts against the wrong
    // length, silently rejecting every real AtmosEncoder/Eac3BurstPacker
    // frame the app hands it.
    CHECK(burst_bytes_for(BitstreamFormat::kAc3) == iclforge::containers::iec61937::kBurstBytes);
    CHECK(burst_bytes_for(BitstreamFormat::kEac3) ==
          iclforge::containers::iec61937::kEac3BurstBytes);
    CHECK(burst_bytes_for(BitstreamFormat::kEac3) > burst_bytes_for(BitstreamFormat::kAc3));
}

TEST_CASE("E-AC-3 declares its carrier at four times the content rate") {
    // Getting this wrong means the AudioFormat/AudioTrack Kotlin opens
    // describes a link running at the wrong speed for what the burst bytes
    // actually are - the same physical fact ALSA's and WASAPI's backends
    // already encode (see platform/alsa/device_names.hpp's carrier_rate),
    // not an Android-specific rule.
    CHECK(carrier_rate(BitstreamFormat::kAc3, 48000) == 48000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 48000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 44100) == 176400);
}

TEST_CASE("AC-4 declares its carrier at the content rate and HBR4 at four times it") {
    // IEC 61937-14 5.3.1 and 5.3.3; the same arithmetic as ALSA's.
    CHECK(carrier_rate(BitstreamFormat::kAc4, 48000) == 48000);
    CHECK(carrier_rate(BitstreamFormat::kAc4, 44100) == 44100);
    CHECK(carrier_rate(BitstreamFormat::kAc4Hbr4, 48000) == 192000);
    // An AC-4 burst is as long as its period; the buffers are sized to the
    // longest.
    CHECK(burst_bytes_for(BitstreamFormat::kAc4) == 2048 * 4);
    CHECK(burst_bytes_for(BitstreamFormat::kAc4Hbr4) == 8192 * 4);
}

TEST_CASE("TrueHD declares an eight-channel carrier at the MAT link rate") {
    // The same arithmetic as every backend's: 192 kHz for the 48 kHz family at any of its rates,
    // 176.4 kHz for the 44.1 kHz one, and none for a rate MAT has no link at.
    CHECK(carrier_rate(BitstreamFormat::kMat, 48000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kMat, 96000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kMat, 44100) == 176400);
    CHECK(carrier_rate(BitstreamFormat::kMat, 32000) == 0);
    // A MAT burst is the 61,440 bytes the buffers are sized to, and the bridge is told eight
    // channels: the count the Kotlin side turns into CHANNEL_OUT_7POINT1_SURROUND.
    CHECK(burst_bytes_for(BitstreamFormat::kMat) == 61440);
    CHECK(iclforge::audio::carrier_channels(BitstreamFormat::kMat) == 8);
    CHECK(iclforge::audio::carrier_frame_bytes(BitstreamFormat::kMat) == 16);
    // Only MAT needs the bridge's link methods; AC-3, E-AC-3 and AC-4 open as they always have.
    CHECK(iclforge::android_audio::needs_link_bridge(BitstreamFormat::kMat));
    CHECK_FALSE(iclforge::android_audio::needs_link_bridge(BitstreamFormat::kAc3));
    CHECK_FALSE(iclforge::android_audio::needs_link_bridge(BitstreamFormat::kEac3));
    CHECK_FALSE(iclforge::android_audio::needs_link_bridge(BitstreamFormat::kAc4));
    CHECK_FALSE(iclforge::android_audio::needs_link_bridge(BitstreamFormat::kAc4Hbr4));
}

TEST_CASE("the synthetic render device reports the MAT link only when it was probed") {
    const auto without = make_render_device_info(true, true, true);
    CHECK_FALSE(without.supports_mat_passthrough);
    const auto with = make_render_device_info(true, true, true, true, true);
    CHECK(with.supports_mat_passthrough);
    // The answer is the eight-channel probe's alone: AC-3 and E-AC-3 saying yes do not imply it.
    const auto only_mat = make_render_device_info(false, false, false, false, true);
    CHECK(only_mat.supports_mat_passthrough);
    CHECK_FALSE(only_mat.supports_ac3_passthrough);
}

TEST_CASE("the synthetic render device reports exactly what was probed") {
    const auto none = make_render_device_info(false, false, false);
    CHECK_FALSE(none.supports_ac3_passthrough);
    CHECK_FALSE(none.supports_eac3_passthrough);
    CHECK_FALSE(none.supports_ac4_passthrough);
    CHECK_FALSE(none.supports_exclusive_pcm);
    CHECK(none.is_default);
    CHECK(none.id == "default");

    const auto eac3_only = make_render_device_info(false, true, true);
    CHECK_FALSE(eac3_only.supports_ac3_passthrough);
    CHECK(eac3_only.supports_eac3_passthrough);
    CHECK(eac3_only.supports_exclusive_pcm);

    const auto with_ac4 = make_render_device_info(true, false, true, true);
    CHECK(with_ac4.supports_ac4_passthrough);
    CHECK_FALSE(with_ac4.supports_eac3_passthrough);
}

TEST_CASE("the synthetic device is always marked default") {
    // Android has exactly one addressable output route - there is nothing
    // for is_default to be false against, unlike WASAPI's real device list.
    CHECK(make_render_device_info(true, true, true).is_default);
    CHECK(make_render_device_info(false, false, false).is_default);
}

TEST_CASE("only a dead track ends the passthrough stream") {
    // AudioTrack.ERROR_DEAD_OBJECT is -6 in the SDK. A write that took some
    // of a burst, or none of it because the track was full, or that failed
    // some other way, leaves the stream running.
    CHECK(track_is_dead(-6));
    CHECK_FALSE(track_is_dead(6144));
    CHECK_FALSE(track_is_dead(0));
    CHECK_FALSE(track_is_dead(-1));  // ERROR
    CHECK_FALSE(track_is_dead(-2));  // ERROR_BAD_VALUE
    CHECK_FALSE(track_is_dead(-3));  // ERROR_INVALID_OPERATION
}
