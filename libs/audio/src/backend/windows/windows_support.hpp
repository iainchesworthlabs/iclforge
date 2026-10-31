#pragma once

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <expected>

// The handful of things capture.cpp, device_watcher.cpp, monitor.cpp and
// passthrough.cpp all need from WASAPI/COM, kept in one place so the four
// cannot drift - the same role alsa_support.hpp plays for the ALSA backend
// (see its own header comment), coreaudio_support.hpp for CoreAudio, and
// pipewire_support.hpp for PipeWire.
//
// Before this existed, ComScope and the CLSID_MMDeviceEnumerator/IID byte
// arrays below were hand-copied into each Windows backend file that needed
// them, capture.cpp's own copies being the ones the others' comments pointed
// back to ("same as capture.cpp's") rather than actually sharing. That the
// copies can drift is not hypothetical: monitor.cpp and passthrough.cpp each
// grew their own "the render device went away under me" detection
// independently, on their own schedules, and disagreed on which HRESULT
// codes end the stream - see stream_gone()'s own comment below for what
// that disagreement is and why part of it is intentional rather than a bug.
//
// spatial.cpp is not wired to this header yet. It duplicates ComScope and
// the two enumerator constants the same way the other four once did, and its
// render loop does not check for device loss at all today - see
// stream_gone()'s own comment for the shape that would need.

namespace iclforge::windows_audio {

// COM lifetime for one thread. WASAPI is apartment-sensitive, so every thread
// that touches an interface initialises and uninitialises its own.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// The class and interface identifiers spelled out rather than taken from
// __uuidof, for the reason every file that used to define these locally
// already noted: the SDK declares these but ships no import library that
// defines them as linkable symbols, and __uuidof is an MSVC extension clang
// rejects under -Wpedantic. Values are the DECLSPEC_UUID / MIDL_INTERFACE
// strings in mmdeviceapi.h and audioclient.h.
//
// Only the ones genuinely identical across two or more of this directory's
// files live here. kIidAudioCaptureClient, kIidActivateCompletionHandler and
// kIidAgileObject (capture.cpp's process-loopback activation),
// kIidMmNotificationClient (device_watcher.cpp's listener registration) and
// spatial.cpp's ISpatialAudioClient/ISpatialAudioObjectRenderStream ids each
// have exactly one caller and stay there.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidAudioClient = {  // {1cb9ad4c-dbfa-4c32-b178-c2f568a703b2}
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr IID kIidAudioRenderClient = {  // {f294acfc-3146-4483-a7bf-addca7c260e2}
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
constexpr IID kIidUnknown = {  // {00000000-0000-0000-c000-000000000046}
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// CoCreateInstance for the one COM class every backend file starts from.
// Generic over the caller's own error enum so a call site stays a plain
// substitution - make_enumerator(XError::kComFailure) - rather than
// translating a new shared error type back into its own; every caller's
// error enum already has the kComFailure member this asks for.
template <typename Error>
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<IMMDeviceEnumerator>, Error> make_enumerator(
    Error on_failure) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return std::unexpected(on_failure);
    }
    return enumerator;
}

// The answers that mean a render stream has gone for good: its endpoint
// unplugged, disabled or reconfigured under it, or the audio service stopped
// (Microsoft's "Recovering from an Invalid-Device Error"). Distinct from a
// call a driver merely declines to answer, which is what the callers below
// use this narrow allowlist to tell apart from an actual loss - anything
// else FAILED() reports is treated as the driver being uncooperative rather
// than gone.
//
// passthrough.cpp (exclusive mode) is this predicate's original and current
// caller, around GetCurrentPadding: real hardware has been seen to decline
// that call without the device having gone - see docs/platforms/windows.md -
// so only these two codes end the stream there.
//
// monitor.cpp's shared-mode GetCurrentPadding checks deliberately do NOT
// call this, and that is not an oversight this header should paper over: a
// shared-mode stream always answers for its padding (monitor.cpp's own
// comment on the point), so there a refusal for ANY reason already means the
// stream is over, and narrowing that check to this allowlist would silently
// stop noticing a real loss that happens to fail a different way. The two
// backends are answering different questions that happen to look like the
// same call.
//
// spatial.cpp does not check for device loss at all yet: its render loop
// has no equivalent of monitor.cpp's/passthrough.cpp's `lost` flag today.
// Its ISpatialAudioClient is activated off the same IMMDevice/audio engine
// as the other two, and Microsoft's own reference pages for
// BeginUpdatingAudioObjects/GetAvailableDynamicObjectCount list the same
// AUDCLNT_E_DEVICE_INVALIDATED endpoint-removal code this predicate already
// matches, so it is the plausible starting point once that loop gains one -
// not necessarily the final answer, the same way monitor.cpp's shared-mode
// loop needed a different one from passthrough.cpp's exclusive-mode loop.
[[nodiscard]] inline bool stream_gone(HRESULT result) {
    return result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL from ksmedia.h:
// {00000092-0000-0010-8000-00aa00389b71}. The low 16 bits of Data1 are the
// wFormatTag (0x0092 = WAVE_FORMAT_DOLBY_AC3_SPDIF).
constexpr GUID kSubtypeIec61937DolbyDigital = {
    0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS from ksmedia.h:
// {0000000a-0cea-0010-8000-00aa00389b71}. A different GUID family from AC-3's
// (Data2 0x0cea rather than 0x0000) - confirmed against a Windows SDK
// ksmedia.h mirror and against Microsoft's own "Representing Formats for IEC
// 61937 Transmissions" documentation, which gives this exact value in a
// worked Dolby Digital Plus example.
constexpr GUID kSubtypeIec61937DolbyDigitalPlus = {
    0x0000000a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_PCM: {00000001-0000-0010-8000-00aa00389b71}, the
// WAVE_FORMAT_PCM tag in the same GUID family. Needed for the rate probes,
// which ask about ordinary PCM rather than a bitstream.
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The IEC 61937 carrier is a 2-channel 16-bit stream; one AC-3 frame occupies
// one 6144-byte burst = 1536 stereo frames, matching the AC-3 frame duration.
constexpr WORD kCarrierChannels = 2;
constexpr WORD kCarrierBits = 16;
constexpr DWORD kSpeakerStereo = 0x3;  // KSAUDIO_SPEAKER_STEREO

// WAVEFORMATEXTENSIBLE_IEC61937 (ksmedia.h): a WAVEFORMATEXTENSIBLE followed
// by three fields describing the *encoded* stream inside the carrier.
struct WaveFormatIec61937 {
    WAVEFORMATEXTENSIBLE FormatExt;
    DWORD dwEncodedSamplesPerSec;
    DWORD dwEncodedChannelCount;
    DWORD dwAverageBytesPerSec;
};

inline WaveFormatIec61937 make_ac3_format(std::uint32_t sample_rate, DWORD encoded_channels) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kCarrierChannels;
    wf.nSamplesPerSec = sample_rate;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = sample_rate * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeakerStereo;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyDigital;

    // Describes the AC-3 payload, not the carrier: the decoded rate and how
    // many channels the receiver will reproduce.
    format.dwEncodedSamplesPerSec = sample_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;  // 0 is permitted and means "unspecified"
    return format;
}

// Dolby Digital Plus over IEC 60958/61937: per Microsoft's "Representing
// Formats for IEC 61937 Transmissions", "the link-sampling rate must be four
// times the sampling rate of the content" - so unlike AC-3, the carrier
// itself (nSamplesPerSec/nAvgBytesPerSec) runs at 4x `sample_rate`, which
// stays the CONTENT rate throughout. Field values otherwise mirror
// Microsoft's own worked 48 kHz DD+ example verbatim.
inline WaveFormatIec61937 make_eac3_format(std::uint32_t sample_rate, DWORD encoded_channels) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kCarrierChannels;
    wf.nSamplesPerSec = sample_rate * 4;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeakerStereo;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyDigitalPlus;

    format.dwEncodedSamplesPerSec = sample_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;  // ignored for this format (MS docs)
    return format;
}

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP from ksmedia.h (Windows SDK 10.0.26100):
// {0000000c-0cea-0010-8000-00aa00389b71}, Dolby MAT 1.0, which Microsoft's "Representing
// Formats for IEC 61937 Transmissions" describes as TrueHD at 24 bits, 192 kHz, up to 18 Mbps,
// 8 channels. MAT 2.0 and 2.1 have GUIDs of their own (0000010c and 0000030c); a stream of this
// library's is MAT 1.0 (no metadata beyond the coded bitstream), so this is the one it asks for.
constexpr GUID kSubtypeIec61937DolbyMlp = {
    0x0000000c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr WORD kTruehdCarrierChannels = 8;
constexpr DWORD kSpeaker71 = 0xFF;  // KSAUDIO_SPEAKER_7POINT1

// TrueHD in MAT: unlike every format above the carrier is eight channels wide (four IEC 60958
// lines), 16 bytes to a link frame, at `link_rate` (192000 or 176400, audio::link_rate()) whatever
// the content rate. The values are Microsoft's own worked example for MAT 1.0 verbatim, which is
// written for a 96 kHz stream: nChannels 8, a 7.1 mask, 16 valid bits, dwEncodedChannelCount 8.
// Whether a 5.1 stream wants dwEncodedChannelCount 6 instead is not something the page says, and
// no driver has been asked.
inline WaveFormatIec61937 make_truehd_format(std::uint32_t link_rate, std::uint32_t content_rate) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kTruehdCarrierChannels;
    wf.nSamplesPerSec = link_rate;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kTruehdCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = link_rate * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeaker71;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyMlp;

    format.dwEncodedSamplesPerSec = content_rate;
    format.dwEncodedChannelCount = kTruehdCarrierChannels;
    format.dwAverageBytesPerSec = 0;  // ignored for this format (MS docs)
    return format;
}

}  // namespace iclforge::windows_audio
