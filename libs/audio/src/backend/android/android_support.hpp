#pragma once

#include <cstddef>
#include <cstdint>

#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/audio/passthrough.hpp"

// The Android backend's pure half, split out for the same reason
// platform/alsa/device_names.hpp is: everything here compiles and tests on a
// machine with no Android SDK/NDK at all, unlike the rest of this directory
// (passthrough.cpp, monitor.cpp), which needs <jni.h>/<aaudio/AAudio.h> and a
// real Android toolchain. Not part of the library's public interface -
// backend-internal, like alsa_support.hpp/device_names.hpp are for ALSA.

namespace iclforge::android_audio {

// Which IEC 61937 burst size a format uses - the longest, for AC-4, whose
// bursts follow its frame rate (audio::max_burst_bytes()). AudioTrack.write()
// needs the caller (submit()) to know this up front so it can validate the
// burst it was handed, and size the buffers it copies bursts into, before
// ever touching JNI.
[[nodiscard]] inline std::size_t burst_bytes_for(audio::BitstreamFormat format) {
    return audio::max_burst_bytes(format);
}

// Mirrors platform/alsa/device_names.hpp's carrier_rate exactly (same
// physical fact, not a coincidence of naming): a Dolby Digital Plus burst is
// four times the size of an AC-3 one (kEac3BurstBytes vs kBurstBytes) and
// covers the same span of time, so the link has to clock four times as fast
// to deliver it - Microsoft's "Representing Formats for IEC 61937
// Transmissions" states it as a requirement, and it is not an
// WASAPI-specific one. The Kotlin PassthroughBridge's AudioTrack is opened
// with AudioFormat.ENCODING_IEC61937 (pre-wrapped bursts straight through,
// not ENCODING_E_AC3's "raw elementary frames, Android wraps them" contract
// - see passthrough.cpp's header comment for why), which is exactly the
// same "the declared rate describes the wire, not the content" situation
// ALSA/WASAPI are already in - so PassthroughSink::start() passes THIS
// value to PassthroughBridge.open(), not the raw content sample_rate. AC-4's
// link runs at the content rate and its HBR4 link at 4x (audio::
// carrier_ratio()).
[[nodiscard]] constexpr std::uint32_t carrier_rate(audio::BitstreamFormat format,
                                                    std::uint32_t content_rate) {
    return audio::link_rate(format, content_rate);
}

// The eight-channel mask the Kotlin bridge opens a MAT track with is CHANNEL_OUT_7POINT1_SURROUND
// (0x18FC), and the bridge is told the CHANNEL COUNT, so that the native side names no Android
// constant it does not need: audio::carrier_channels() is the count. A format whose link is not
// two channels wide needs the bridge's link methods (isLinkSupported, openLink) rather than the
// two-channel ones (isDirectPlaybackSupported, open), which every bridge has.
[[nodiscard]] constexpr bool needs_link_bridge(audio::BitstreamFormat format) {
    return audio::is_mat(format);
}

// AudioTrack.ERROR_DEAD_OBJECT, which AudioTrack.write() returns once the
// track is no longer valid - its output closed under it when the HDMI sink
// went away, or the audio server restarted - and, in the SDK's words, "needs
// to be recreated". PassthroughBridge.submit() hands write()'s result back
// unchanged, so this is the answer that tells PassthroughSink::submit() the
// stream has ended rather than paused for breath. It is the only negative
// answer read that way: the others are counted as before, and submit() keeps
// its place in the burst for them.
inline constexpr int kAudioTrackErrorDeadObject = -6;

[[nodiscard]] constexpr bool track_is_dead(int written) {
    return written == kAudioTrackErrorDeadObject;
}

// The Kotlin side's PassthroughBridge.probeCapabilities(int) returns
// [ac3Supported, eac3Supported, pcmSupported] as 0/1 flags (see
// passthrough.cpp's header comment for the full contract) - this is the pure
// half of turning that into the RenderDeviceInfo enumerate_render_devices()
// hands back. Android has exactly one addressable output route (there is no
// WASAPI-style per-endpoint enumeration to walk - see
// enumerate_render_devices()'s own comment), so the id/name/is_default
// fields are fixed rather than read from anywhere. `ac4_supported` is the
// same question AC-3's is - an ENCODING_IEC61937 track at the content rate -
// which the route either takes or does not, whatever the bursts hold.
[[nodiscard]] inline audio::RenderDeviceInfo make_render_device_info(bool ac3_supported,
                                                                     bool eac3_supported,
                                                                     bool pcm_supported,
                                                                     bool ac4_supported = false,
                                                                     bool mat_supported = false) {
    audio::RenderDeviceInfo info;
    info.id = "default";
    info.name = "Android system audio output (HDMI-routed)";
    info.is_default = true;
    info.supports_ac3_passthrough = ac3_supported;
    info.supports_eac3_passthrough = eac3_supported;
    info.supports_ac4_passthrough = ac4_supported;
    // An ENCODING_IEC61937 track on the eight-channel mask at the MAT link rate, asked of the
    // route with the bridge's isLinkSupported(): audio::carrier_channels() and carrier_rate().
    info.supports_mat_passthrough = mat_supported;
    info.supports_exclusive_pcm = pcm_supported;
    return info;
}

}  // namespace iclforge::android_audio
