// Carries FLAC in IAMF (AOM IAMF v2.0.0) with mux_coded() and reads it back with
// reconstruct_channels(): the two halves of working with a codec iclforge::containers::iamf does
// not link. The stereo programme is cut into 960 sample blocks, each packed as a FLAC frame with
// verbatim subframes (a valid, if unambitious, FLAC encoder); mux_coded() writes them as one Audio
// Substream with the STREAMINFO decoder_config 3.13.3 asks for; read_isobmff() reads the file back
// and the frames are unpacked again (the caller's decoder, here the inverse of the packer) and
// handed to reconstruct_channels(), which returns the channels with the trimming applied.
//
// The program writes no files; it checks that what comes back is what went in and exits non-zero
// when it is not. An Opus or AAC-LC programme goes the same way with the packets of libopus or an
// AAC encoder, and CodedCodec::kOpus or kAacLc.

#include <fmt/printf.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"

namespace {

namespace iamf = iclforge::containers::iamf;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlock = 960;
constexpr std::size_t kFrames = 10;
constexpr unsigned kRateCode48k = 0b1010;  // RFC 9639, 9.1.2
constexpr unsigned kDepthCode16 = 0b100;   // RFC 9639, 9.1.4

std::uint8_t crc8(const iamf::Bytes& data) {
    unsigned crc = 0;
    for (const std::byte b : data) {
        crc ^= std::to_integer<unsigned>(b);
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 0x80U) != 0 ? (crc << 1) ^ 0x07U : crc << 1) & 0xFFU;
        }
    }
    return static_cast<std::uint8_t>(crc);
}

std::uint16_t crc16(const iamf::Bytes& data) {
    unsigned crc = 0;
    for (const std::byte b : data) {
        crc ^= std::to_integer<unsigned>(b) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 0x8000U) != 0 ? (crc << 1) ^ 0x8005U : crc << 1) & 0xFFFFU;
        }
    }
    return static_cast<std::uint16_t>(crc);
}

// One FLAC frame of a stereo block: the frame header (fixed block size, independent channels), then
// a verbatim subframe per channel, then the CRC-16.
iamf::Bytes pack_flac_frame(std::size_t number,
                            const std::array<const std::int16_t*, 2>& channels) {
    iamf::Bytes frame;
    const auto put = [&frame](unsigned value) {
        frame.push_back(static_cast<std::byte>(value & 0xFFU));
    };
    put(0xFF);
    put(0xF8);                                  // sync code, fixed block size
    put((0b0111U << 4) | kRateCode48k);         // block size: 16 bit value follows, then the rate
    put((0b0001U << 4) | (kDepthCode16 << 1));  // two channels, independent; 16 bits
    put(static_cast<unsigned>(number));         // the frame number, UTF-8 coded (below 128)
    put((kBlock - 1) >> 8);
    put((kBlock - 1) & 0xFFU);
    put(crc8(frame));
    for (const std::int16_t* samples : channels) {
        put(0x02);  // subframe header: verbatim, no wasted bits
        for (std::uint32_t n = 0; n < kBlock; ++n) {
            put(static_cast<std::uint16_t>(samples[n]) >> 8);
            put(static_cast<std::uint16_t>(samples[n]));
        }
    }
    const std::uint16_t crc = crc16(frame);
    put(crc >> 8);
    put(crc);
    return frame;
}

// The inverse for frames pack_flac_frame() made: a real decoder (libFLAC, FFmpeg) goes here.
std::vector<std::vector<float>> unpack_flac_frame(const iamf::Bytes& frame) {
    constexpr std::size_t kHeader = 8;
    std::vector<std::vector<float>> channels(2);
    std::size_t at = kHeader;
    for (auto& channel : channels) {
        ++at;  // the subframe header
        for (std::uint32_t n = 0; n < kBlock; ++n, at += 2) {
            const auto raw =
                static_cast<std::uint16_t>((std::to_integer<unsigned>(frame[at]) << 8) |
                                           std::to_integer<unsigned>(frame[at + 1]));
            channel.push_back(static_cast<float>(static_cast<std::int16_t>(raw)) / 32768.0F);
        }
    }
    return channels;
}

}  // namespace

int main() {
    // A stereo programme: 330 Hz on the left, 440 Hz on the right, 16 bit.
    std::array<std::vector<std::int16_t>, 2> pcm;
    const std::array<double, 2> hertz{330.0, 440.0};
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t n = 0; n < kFrames * kBlock; ++n) {
            const double phase =
                2.0 * std::numbers::pi * hertz[c] * static_cast<double>(n) / kSampleRate;
            pcm[c].push_back(
                static_cast<std::int16_t>(std::lround(0.3 * 32767.0 * std::sin(phase))));
        }
    }

    iamf::CodedTrack track;
    track.codec = iamf::CodedCodec::kFlac;
    track.sample_rate = kSampleRate;
    track.samples_per_frame = kBlock;
    track.bit_depth = 16;
    track.loudspeaker_layout = 1;  // Stereo: one coupled Audio Substream

    std::vector<iamf::CodedFrame> frames(kFrames);
    for (std::size_t f = 0; f < kFrames; ++f) {
        frames[f].substreams.push_back(
            pack_flac_frame(f, {pcm[0].data() + f * kBlock, pcm[1].data() + f * kBlock}));
    }

    const auto file = iamf::mux_coded(track, frames);
    if (!file) {
        fmt::print("mux_coded failed: {}\n", std::string(iamf::describe(file.error())));
        return 1;
    }
    const auto read = iamf::read_isobmff(*file);
    if (!read) {
        fmt::print("read_isobmff failed: {}\n", std::string(iamf::describe(read.error())));
        return 1;
    }
    fmt::print("ISO-BMFF: {} bytes, {} IA Samples, codecs \"{}\"\n", file->size(),
               read->sequence.temporal_units.size(),
               iamf::codecs_string(read->sequence).value_or("?"));

    // The caller's decoder: every frame of every Audio Substream, planar, untrimmed.
    std::vector<iamf::SubstreamPcm> decoded(1);
    decoded[0].audio_substream_id = read->sequence.audio_elements[0].audio_substream_ids[0];
    decoded[0].channels.resize(2);
    for (const iamf::TemporalUnit& unit : read->sequence.temporal_units) {
        const auto channels = unpack_flac_frame(unit.audio_frames[0].data);
        for (std::size_t c = 0; c < 2; ++c) {
            decoded[0].channels[c].insert(decoded[0].channels[c].end(), channels[c].begin(),
                                          channels[c].end());
        }
    }
    const auto element = iamf::reconstruct_channels(read->sequence, 0, decoded);
    if (!element) {
        fmt::print("reconstruct_channels failed: {}\n",
                   std::string(iamf::describe(element.error())));
        return 1;
    }
    fmt::print("decoded {} channels of {} samples at {} Hz\n", element->channels.size(),
               element->channels[0].size(), element->sample_rate);

    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t n = 0; n < pcm[c].size(); ++n) {
            if (element->channels[c][n] != static_cast<float>(pcm[c][n]) / 32768.0F) {
                fmt::print("channel {} differs at sample {}\n", c, n);
                return 1;
            }
        }
    }
    fmt::print("every sample came back unchanged\n");
    return 0;
}
