#pragma once

#include <windows.h>
// windows.h must precede the audio headers.
#include <mmreg.h>

#include <cstdint>

#include "iclforge/audio/passthrough.hpp"

// The WAVEFORMATEXTENSIBLE_IEC61937 formats the Windows passthrough backend opens an exclusive
// stream with, split out of passthrough.cpp so that what each one says can be tested without a
// device (libs/audio/tests/backend/windows/test_windows_formats.cpp): a format is a handful of
// fields, and a field wrong by one is a device that refuses it or a receiver that hears noise.
//
// Three families, one per KSDATAFORMAT_SUBTYPE_IEC61937_* the backend opens: Dolby Digital (AC-3),
// Dolby Digital Plus (E-AC-3) and Dolby MLP, which is what TrueHD's MAT frame is called here.
// AC-4 has no subtype in the SDK (passthrough.hpp), so it has no format.

namespace iclforge::windows_audio {

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL from ksmedia.h:
// {00000092-0000-0010-8000-00aa00389b71}. The low 16 bits of Data1 are the
// wFormatTag (0x0092 = WAVE_FORMAT_DOLBY_AC3_SPDIF).
inline constexpr GUID kSubtypeIec61937DolbyDigital = {
    0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS from ksmedia.h:
// {0000000a-0cea-0010-8000-00aa00389b71}. A different GUID family from AC-3's
// (Data2 0x0cea rather than 0x0000) - confirmed against a Windows SDK
// ksmedia.h mirror and against Microsoft's own "Representing Formats for IEC
// 61937 Transmissions" documentation, which gives this exact value in a
// worked Dolby Digital Plus example.
inline constexpr GUID kSubtypeIec61937DolbyDigitalPlus = {
    0x0000000a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP from ksmedia.h (Windows SDK 10.0.26100):
// {0000000c-0cea-0010-8000-00aa00389b71}, the next in DD+'s family (0x0a Dolby Digital Plus, 0x0b
// DTS-HD, 0x0c Dolby MLP). The SDK names it MLP and no other TrueHD subtype, and MAT is what a
// TrueHD stream travels as; that an exclusive open of it takes the fields make_mat_format() gives
// is not something this backend has seen, and nothing has been played through it to a receiver.
inline constexpr GUID kSubtypeIec61937DolbyMlp = {
    0x0000000c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The IEC 61937 carrier is a 16-bit stream, 2 channels wide for AC-3 and E-AC-3 (one AC-3 frame
// occupies one 6144-byte burst = 1536 stereo frames, matching the AC-3 frame duration) and 8 for
// MAT (one 61,440-byte burst = 3,840 frames).
inline constexpr WORD kCarrierChannels = 2;
inline constexpr WORD kMatCarrierChannels = 8;
inline constexpr WORD kCarrierBits = 16;
inline constexpr DWORD kSpeakerStereo = 0x3;  // KSAUDIO_SPEAKER_STEREO
// KSAUDIO_SPEAKER_7POINT1_SURROUND (ksmedia.h): front left and right, centre, LFE, back left and
// right, side left and right: the eight positions of a 7.1 programme, which is what the eight
// carrier channels are asked to be called.
inline constexpr DWORD kSpeaker7Point1Surround = 0x63F;

// WAVEFORMATEXTENSIBLE_IEC61937 (ksmedia.h): a WAVEFORMATEXTENSIBLE followed
// by three fields describing the *encoded* stream inside the carrier.
struct WaveFormatIec61937 {
    WAVEFORMATEXTENSIBLE FormatExt;
    DWORD dwEncodedSamplesPerSec;
    DWORD dwEncodedChannelCount;
    DWORD dwAverageBytesPerSec;
};

[[nodiscard]] inline WaveFormatIec61937 make_ac3_format(std::uint32_t sample_rate,
                                                        DWORD encoded_channels) {
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
[[nodiscard]] inline WaveFormatIec61937 make_eac3_format(std::uint32_t sample_rate,
                                                         DWORD encoded_channels) {
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

// TrueHD in its MAT frame: eight channels of 16 bits on a link of audio::link_rate() (192 kHz,
// or 176.4 kHz for the 44.1 kHz family, at any content rate of its family), the Dolby MLP subtype,
// and the encoded fields as the other two families have them: the content rate and the channels
// the receiver reproduces. A content rate MAT has no link for gives a format with no rate (0), so
// that nothing can open it by accident; the caller checks audio::link_rate() first.
//
// PROVISIONAL, like the carrier itself (libs/containers/ERRATA.md, question 7): the link and the
// eight channels are the owner's decision of 2026-10-29, the subtype is the SDK's, and which
// encoded rate Windows expects in dwEncodedSamplesPerSec for MLP (the content rate here, as
// for the other two) and which mask it expects on the eight channels are not something this
// repository has seen a driver accept or refuse. WASAPI exclusive mode has not been run with it.
[[nodiscard]] inline WaveFormatIec61937 make_mat_format(std::uint32_t content_rate,
                                                        DWORD encoded_channels = 8) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kMatCarrierChannels;
    wf.nSamplesPerSec = audio::link_rate(audio::BitstreamFormat::kMat, content_rate);
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kMatCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeaker7Point1Surround;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyMlp;

    format.dwEncodedSamplesPerSec = content_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;
    return format;
}

// The format a bitstream is opened with. AC-4 has none (start() refuses it first).
[[nodiscard]] inline WaveFormatIec61937 make_format(audio::BitstreamFormat format,
                                                    std::uint32_t sample_rate,
                                                    DWORD encoded_channels) {
    switch (format) {
        case audio::BitstreamFormat::kEac3:
            return make_eac3_format(sample_rate, encoded_channels);
        case audio::BitstreamFormat::kMat:
            return make_mat_format(sample_rate, encoded_channels);
        case audio::BitstreamFormat::kAc3:
        case audio::BitstreamFormat::kAc4:
        case audio::BitstreamFormat::kAc4Hbr4:
        case audio::BitstreamFormat::kAc4Hbr16:
            break;
    }
    return make_ac3_format(sample_rate, encoded_channels);
}

}  // namespace iclforge::windows_audio
