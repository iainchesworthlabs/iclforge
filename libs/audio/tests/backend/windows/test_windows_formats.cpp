#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include <windows.h>
// windows.h must precede the audio headers.
#include <ks.h>
// ks.h must precede ksmedia.h.
#include <ksmedia.h>
#include <mmreg.h>

#include "iec61937_format.hpp"

// The formats the Windows passthrough backend opens an exclusive stream with, as fields: no device
// is needed to say what a format asks for, and each field below is one a driver can refuse. Added
// to the suite with the other Windows-only file (libs/audio/tests/CMakeLists.txt).

using iclforge::audio::BitstreamFormat;
using namespace iclforge::windows_audio;

namespace {

bool same_guid(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// The SDK's STATIC_KSDATAFORMAT_SUBTYPE_* macros expand to the eleven values of a GUID, flat. As the
// arguments of a function they build one without the braces around the last eight bytes that
// an aggregate initialiser would need (clang-cl's -Wmissing-braces).
GUID guid_of(unsigned long data1, unsigned short data2, unsigned short data3, unsigned char b0,
             unsigned char b1, unsigned char b2, unsigned char b3, unsigned char b4,
             unsigned char b5, unsigned char b6, unsigned char b7) {
    return GUID{data1, data2, data3, {b0, b1, b2, b3, b4, b5, b6, b7}};
}

}  // namespace

TEST_CASE("the Dolby MLP subtype is the SDK's, and the other two are unchanged") {
    // Compared with the SDK's own initialisers, so a mistyped digit cannot agree with itself.
    const GUID sdk_mlp = guid_of(STATIC_KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP);
    const GUID sdk_dd = guid_of(STATIC_KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL);
    const GUID sdk_ddp = guid_of(STATIC_KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS);
    CHECK(same_guid(kSubtypeIec61937DolbyMlp, sdk_mlp));
    CHECK(same_guid(kSubtypeIec61937DolbyDigital, sdk_dd));
    CHECK(same_guid(kSubtypeIec61937DolbyDigitalPlus, sdk_ddp));
    CHECK_FALSE(same_guid(kSubtypeIec61937DolbyMlp, kSubtypeIec61937DolbyDigitalPlus));
}

TEST_CASE("MAT is eight channels of 16 bits on the 192 kHz link for the 48 kHz family") {
    for (const std::uint32_t content : {48000U, 96000U, 192000U}) {
        const WaveFormatIec61937 format = make_mat_format(content);
        const WAVEFORMATEX& wf = format.FormatExt.Format;
        CHECK(wf.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
        CHECK(wf.nChannels == 8);
        CHECK(wf.nSamplesPerSec == 192000U);
        CHECK(wf.wBitsPerSample == 16);
        CHECK(wf.nBlockAlign == 16);
        CHECK(wf.nAvgBytesPerSec == 192000U * 16U);
        CHECK(wf.cbSize == sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX));
        CHECK(format.FormatExt.Samples.wValidBitsPerSample == 16);
        CHECK(format.FormatExt.dwChannelMask == KSAUDIO_SPEAKER_7POINT1_SURROUND);
        CHECK(same_guid(format.FormatExt.SubFormat, kSubtypeIec61937DolbyMlp));
        // The encoded fields describe the stream inside, as the other two families' do.
        CHECK(format.dwEncodedSamplesPerSec == content);
        CHECK(format.dwEncodedChannelCount == 8);
    }
}

TEST_CASE("MAT for the 44.1 kHz family runs the link at 176.4 kHz") {
    for (const std::uint32_t content : {44100U, 88200U, 176400U}) {
        const WaveFormatIec61937 format = make_mat_format(content);
        CHECK(format.FormatExt.Format.nSamplesPerSec == 176400U);
        CHECK(format.FormatExt.Format.nAvgBytesPerSec == 176400U * 16U);
        CHECK(format.dwEncodedSamplesPerSec == content);
    }
}

TEST_CASE("MAT has no format for a rate it has no link at, so nothing can open one by accident") {
    for (const std::uint32_t content : {0U, 32000U, 22050U, 384000U}) {
        CHECK(make_mat_format(content).FormatExt.Format.nSamplesPerSec == 0U);
    }
}

TEST_CASE("make_format picks the family of the bitstream, and AC-3 and E-AC-3 are as they were") {
    const WaveFormatIec61937 ac3 = make_format(BitstreamFormat::kAc3, 48000, 6);
    CHECK(ac3.FormatExt.Format.nChannels == 2);
    CHECK(ac3.FormatExt.Format.nSamplesPerSec == 48000U);
    CHECK(ac3.FormatExt.Format.nBlockAlign == 4);
    CHECK(ac3.dwEncodedChannelCount == 6);
    CHECK(same_guid(ac3.FormatExt.SubFormat, kSubtypeIec61937DolbyDigital));

    const WaveFormatIec61937 eac3 = make_format(BitstreamFormat::kEac3, 48000, 6);
    CHECK(eac3.FormatExt.Format.nChannels == 2);
    CHECK(eac3.FormatExt.Format.nSamplesPerSec == 192000U);
    CHECK(eac3.dwEncodedSamplesPerSec == 48000U);
    CHECK(same_guid(eac3.FormatExt.SubFormat, kSubtypeIec61937DolbyDigitalPlus));

    const WaveFormatIec61937 mat = make_format(BitstreamFormat::kMat, 48000, 8);
    CHECK(mat.FormatExt.Format.nChannels == 8);
    CHECK(mat.FormatExt.Format.nBlockAlign == 16);
    CHECK(same_guid(mat.FormatExt.SubFormat, kSubtypeIec61937DolbyMlp));
}

TEST_CASE("a MAT burst is exactly the 3,840 frames of the 8 channel link") {
    const WaveFormatIec61937 format = make_mat_format(48000);
    CHECK(iclforge::audio::max_burst_bytes(BitstreamFormat::kMat) %
              format.FormatExt.Format.nBlockAlign ==
          0U);
    CHECK(iclforge::audio::max_burst_bytes(BitstreamFormat::kMat) /
              format.FormatExt.Format.nBlockAlign ==
          3840U);
}
