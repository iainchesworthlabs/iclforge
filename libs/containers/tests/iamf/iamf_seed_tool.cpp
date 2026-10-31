// iclforge-iamf-seed-tool OUT_DIR: writes fuzz_iamf_parse's seeds (tools/fuzz/generate-seeds.sh runs it).
//
// The harness reads one input as a raw OBU stream and as an ISO-BMFF file, so a seed of either
// framing reaches the other's reader only as a rejection; the tool writes each programme both ways.
// Every file is made by the module's own writers from a constant: a 7.1.4 LPCM programme with
// trimming and Temporal Delimiters, object-based programmes in the three position codings with a
// moving object and a coupled pair, an object programme as an initialization segment and two movie
// fragments, and Opus, AAC-LC and FLAC programmes carrying packets of the shape 3.13 allows (the
// codecs' payloads are filler: the module carries them undecoded). The programmes are a few frames
// of a few dozen samples, so that a seed is a few kilobytes and the engine mutates it fast.
//
// Exits 1 with the writer's own description of any programme it could not write.

#include <fmt/core.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <numbers>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

namespace {

namespace iamf = iclforge::containers::iamf;
namespace fs = std::filesystem;

using iamf::Bytes;

constexpr std::uint32_t kFrameSamples = 64;
constexpr std::size_t kFrames = 4;

int failures = 0;

void write_file(const fs::path& dir, const std::string& name, const Bytes& data) {
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
        fmt::print(stderr, "cannot write {}\n", (dir / name).string());
        ++failures;
    }
}

template <typename T, typename E>
bool keep(const std::string& what, std::expected<T, E>& result, T& into) {
    if (!result) {
        fmt::print(stderr, "{}: {}\n", what, iamf::describe(result.error()));
        ++failures;
        return false;
    }
    into = std::move(*result);
    return true;
}

// A programme written as both framings.
void write_both(const fs::path& dir, const std::string& stem, const iamf::Sequence& sequence,
                const iamf::IsobmffOptions& options = {}) {
    auto raw = iamf::write_sequence(sequence);
    Bytes raw_bytes;
    if (keep(stem + " (OBU stream)", raw, raw_bytes)) {
        write_file(dir, stem + ".obu", raw_bytes);
    }
    auto file = iamf::write_isobmff(sequence, options);
    Bytes file_bytes;
    if (keep(stem + " (ISO-BMFF)", file, file_bytes)) {
        write_file(dir, stem + ".mp4", file_bytes);
    }
}

std::vector<float> tone(double hz, std::size_t frames) {
    std::vector<float> samples(frames * kFrameSamples);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        samples[n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return samples;
}

void pcm_programmes(const fs::path& dir) {
    std::vector<iamf::Frame> frames(kFrames);
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (std::size_t c = 0; c < frames[f].channels.size(); ++c) {
            frames[f].channels[c] = tone(220.0 + 55.0 * static_cast<double>(c), 1);
        }
    }
    iamf::AudioTrack plain;
    plain.bit_depth = 16;
    plain.samples_per_frame = kFrameSamples;
    auto sequence = iamf::build_sequence(plain, frames);
    iamf::Sequence built;
    if (keep("714 pcm", sequence, built)) {
        write_both(dir, "pcm-714-16bit", built);
    }

    // Trimming at both ends, 24-bit samples and Temporal Delimiters: the paths that read the trim
    // fields, the edit list and a delimiter's key-frame flag.
    iamf::AudioTrack trimmed;
    trimmed.bit_depth = 24;
    trimmed.samples_per_frame = kFrameSamples;
    trimmed.trim_start_samples = 10;
    trimmed.trim_end_samples = 20;
    trimmed.temporal_delimiters = true;
    trimmed.stereo_loudness = {-23.0F, -1.5F};
    auto trimmed_sequence = iamf::build_sequence(trimmed, frames);
    iamf::Sequence trimmed_built;
    if (keep("714 pcm trimmed", trimmed_sequence, trimmed_built)) {
        write_both(dir, "pcm-714-24bit-trimmed-delimiters", trimmed_built);
    }
}

iamf::ObjectSource moving(double hz, double start_azimuth, double step) {
    iamf::ObjectSource source;
    source.samples = tone(hz, kFrames);
    source.initial_position.azimuth_deg = start_azimuth;
    source.initial_position.x = start_azimuth / 180.0;
    for (std::size_t f = 0; f < kFrames; ++f) {
        iamf::ObjectPosition at_end;
        at_end.azimuth_deg = start_azimuth + step * static_cast<double>(f + 1);
        at_end.x = at_end.azimuth_deg / 180.0;
        at_end.y = 0.5;
        at_end.z = 0.25;
        source.positions.push_back(at_end);
    }
    return source;
}

void object_programmes(const fs::path& dir) {
    iamf::ObjectSource still;
    still.samples = tone(330.0, kFrames);
    const std::vector<iamf::ObjectElement> elements{
        {{still}}, {{moving(440.0, -90.0, 45.0)}}, {{moving(550.0, 30.0, 10.0), moving(660.0, -30.0, -10.0)}}};

    const std::pair<const char*, iamf::PositionCoding> codings[] = {
        {"objects-polar", iamf::PositionCoding::kPolar},
        {"objects-cartesian8", iamf::PositionCoding::kCartesian8},
        {"objects-cartesian16", iamf::PositionCoding::kCartesian16},
    };
    for (const auto& [name, coding] : codings) {
        iamf::ObjectTrack track;
        track.bit_depth = 16;
        track.samples_per_frame = kFrameSamples;
        track.position_coding = coding;
        track.temporal_delimiters = coding == iamf::PositionCoding::kPolar;
        auto sequence = iamf::build_object_sequence(track, elements);
        iamf::Sequence built;
        if (keep(name, sequence, built)) {
            write_both(dir, name, built);
        }
    }

    // The same programme as an initialization segment and two movie fragments, the form a stream
    // arrives in: the reader's fragment walk (moof, traf, trun) and its decode-time bookkeeping.
    iamf::ObjectTrack track;
    track.bit_depth = 16;
    track.samples_per_frame = kFrameSamples;
    auto sequence = iamf::build_object_sequence(track, elements);
    iamf::Sequence built;
    if (!keep("objects (fragmented)", sequence, built) || built.temporal_units.size() < 2) {
        return;
    }
    auto writer = iamf::FragmentedWriter::create(built, {});
    if (!writer) {
        fmt::print(stderr, "objects (fragmented): {}\n", iamf::describe(writer.error()));
        ++failures;
        return;
    }
    Bytes file = writer->initialization_segment();
    const std::size_t half = built.temporal_units.size() / 2;
    const std::span<const iamf::TemporalUnit> units(built.temporal_units);
    for (const auto part : {units.first(half), units.subspan(half)}) {
        auto fragment = writer->fragment(part);
        if (!fragment) {
            fmt::print(stderr, "objects (fragmented): {}\n", iamf::describe(fragment.error()));
            ++failures;
            return;
        }
        file.insert(file.end(), fragment->begin(), fragment->end());
    }
    write_file(dir, "objects-fragmented.mp4", file);
}

Bytes filler(std::initializer_list<unsigned> head, std::size_t total) {
    Bytes out;
    for (const unsigned v : head) {
        out.push_back(static_cast<std::byte>(v));
    }
    out.resize(total, std::byte{0x5A});
    return out;
}

void coded_programmes(const fs::path& dir) {
    // Stereo (loudspeaker_layout 1): one coupled substream, so one packet a Temporal Unit.
    constexpr std::uint8_t kStereo = 1;

    iamf::CodedTrack opus;
    opus.codec = iamf::CodedCodec::kOpus;
    opus.samples_per_frame = 960;
    opus.loudspeaker_layout = kStereo;
    opus.trim_start_samples = 312;
    // The TOC byte: CELT full band, 20 ms, stereo, one frame (frame count code 0).
    const iamf::CodedFrame opus_frame{{filler({(31U << 3) | 4U}, 60)}};
    const std::vector<iamf::CodedFrame> opus_frames(kFrames, opus_frame);

    iamf::CodedTrack aac;
    aac.codec = iamf::CodedCodec::kAacLc;
    aac.samples_per_frame = 1024;
    aac.loudspeaker_layout = kStereo;
    // A raw_data_block() that opens with a channel_pair_element (id_syn_ele 1) and ends with the
    // end element (7): the shape, not a decodable frame.
    const iamf::CodedFrame aac_frame{{filler({0x20}, 80)}};
    const std::vector<iamf::CodedFrame> aac_frames(kFrames, aac_frame);

    iamf::CodedTrack flac;
    flac.codec = iamf::CodedCodec::kFlac;
    flac.samples_per_frame = 960;
    flac.bit_depth = 16;
    flac.loudspeaker_layout = kStereo;
    // A FLAC frame header: sync, block size coded in 16 bits, 48 kHz, stereo, 16-bit samples.
    const iamf::CodedFrame flac_frame{{filler({0xFF, 0xF8, 0x7A, 0x18, 0x00, 0x03, 0xBF, 0x00}, 68)}};
    const std::vector<iamf::CodedFrame> flac_frames(kFrames, flac_frame);

    struct Programme {
        const char* name;
        const iamf::CodedTrack* track;
        const std::vector<iamf::CodedFrame>* frames;
    };
    for (const Programme& programme : {Programme{"coded-opus-stereo", &opus, &opus_frames},
                                       Programme{"coded-aac-lc-stereo", &aac, &aac_frames},
                                       Programme{"coded-flac-stereo", &flac, &flac_frames}}) {
        auto sequence = iamf::build_coded_sequence(*programme.track, *programme.frames);
        iamf::Sequence built;
        if (keep(programme.name, sequence, built)) {
            write_both(dir, programme.name, built);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fmt::print(stderr, "usage: iclforge-iamf-seed-tool OUT_DIR\n");
        return 2;
    }
    const fs::path dir(argv[1]);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        fmt::print(stderr, "cannot create {}: {}\n", dir.string(), ec.message());
        return 1;
    }
    pcm_programmes(dir);
    object_programmes(dir);
    coded_programmes(dir);
    return failures == 0 ? 0 : 1;
}
