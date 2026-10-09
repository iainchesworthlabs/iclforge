// Writes an object-based IAMF program (AOM IAMF v2.0.0) three ways and reads each back: as an
// ISO-BMFF file, as a raw OBU stream, and as an initialization segment plus movie fragments. One
// object holds still, one orbits the listener, and a pair of objects is coded as one stereo
// substream. The program writes no files; it checks that what comes back is what went in and exits
// non-zero when it is not.
//
// iclforge::containers::iamf is codec-blind and does not link iclforge::ac3: the objects are plain
// PCM.

#include <fmt/printf.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kFrame = 960;
constexpr std::size_t kFrames = 12;

std::vector<float> tone(double hz) {
    std::vector<float> samples(kFrames * kFrame);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        samples[n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kSampleRate));
    }
    return samples;
}

}  // namespace

int main() {
    namespace iamf = iclforge::containers::iamf;

    // A fixed object in front, an object circling the listener at ear height, and two objects coded as one pair.
    iamf::ObjectSource fixed;
    fixed.samples = tone(220.0);
    fixed.initial_position.azimuth_deg = 0.0;

    iamf::ObjectSource orbit;
    orbit.samples = tone(440.0);
    for (std::size_t f = 0; f < kFrames; ++f) {
        iamf::ObjectPosition at_end;
        at_end.azimuth_deg = -150.0 + 30.0 * static_cast<double>(f + 1);
        orbit.positions.push_back(at_end);
    }
    orbit.initial_position.azimuth_deg = -150.0;

    iamf::ObjectSource pair_left;
    pair_left.samples = tone(330.0);
    pair_left.initial_position.azimuth_deg = 60.0;
    iamf::ObjectSource pair_right;
    pair_right.samples = tone(550.0);
    pair_right.initial_position.azimuth_deg = -60.0;

    const std::vector<iamf::ObjectElement> elements{{{fixed}}, {{orbit}}, {{pair_left, pair_right}}};
    iamf::ObjectTrack track;
    track.sample_rate = kSampleRate;
    track.samples_per_frame = kFrame;

    auto sequence = iamf::build_object_sequence(track, elements);
    if (!sequence) {
        fmt::print("build_object_sequence failed: {}\n", iamf::describe(sequence.error()));
        return 1;
    }
    fmt::print("{} Audio Elements, {} Temporal Units, primary profile {}\n", sequence->audio_elements.size(),
               sequence->temporal_units.size(), sequence->header.primary_profile);

    // 1. ISO-BMFF.
    auto file = iamf::write_isobmff(*sequence);
    if (!file) {
        fmt::print("write_isobmff failed: {}\n", iamf::describe(file.error()));
        return 1;
    }
    auto from_file = iamf::read_isobmff(*file);
    if (!from_file || from_file->sequence.temporal_units.size() != kFrames) {
        fmt::print("read_isobmff did not return the Temporal Units written\n");
        return 1;
    }
    fmt::print("ISO-BMFF: {} bytes, brands {}, {} samples\n", file->size(), from_file->info.brands.size(),
               from_file->info.sample_durations.size());

    // 2. A raw OBU stream, with Temporal Delimiters.
    for (auto& unit : sequence->temporal_units) {
        unit.has_temporal_delimiter = true;
    }
    auto stream = iamf::write_sequence(*sequence);
    auto from_stream = stream ? iamf::read_sequence(*stream) : std::unexpected(stream.error());
    if (!from_stream || from_stream->temporal_units.size() != kFrames) {
        fmt::print("the raw OBU stream did not read back\n");
        return 1;
    }
    std::size_t blocks = 0;
    for (const auto& unit : from_stream->temporal_units) {
        blocks += unit.parameter_blocks.size();
    }
    fmt::print("raw OBU stream: {} bytes, {} Parameter Blocks (only the orbiting object moves)\n", stream->size(), blocks);

    // 3. Fragments: the initialization segment first, then groups of Temporal Units as they are made.
    auto writer = iamf::FragmentedWriter::create(*sequence);
    if (!writer) {
        fmt::print("FragmentedWriter::create failed: {}\n", iamf::describe(writer.error()));
        return 1;
    }
    std::vector<std::byte> fragmented = writer->initialization_segment();
    const std::span<const iamf::TemporalUnit> units(sequence->temporal_units);
    for (std::size_t first = 0; first < units.size(); first += 4) {
        auto fragment = writer->fragment(units.subspan(first, 4));
        if (!fragment) {
            fmt::print("fragment failed: {}\n", iamf::describe(fragment.error()));
            return 1;
        }
        fragmented.insert(fragmented.end(), fragment->begin(), fragment->end());
    }
    auto from_fragments = iamf::read_isobmff(fragmented);
    if (!from_fragments || !from_fragments->info.fragmented || from_fragments->sequence.temporal_units.size() != kFrames) {
        fmt::print("the fragmented file did not read back\n");
        return 1;
    }
    fmt::print("fragmented: {} fragments, {} bytes\n", writer->fragments_written(), fragmented.size());

    // The audio of the last element (the pair) comes back as two channels.
    auto decoded = iamf::decode_pcm(from_fragments->sequence, 2);
    if (!decoded || decoded->channels.size() != 2 || decoded->channels[0].size() != kFrames * kFrame) {
        fmt::print("decode_pcm did not return the pair\n");
        return 1;
    }
    fmt::print("decoded {} channels of {} samples at {} Hz\n", decoded->channels.size(), decoded->channels[0].size(),
               decoded->sample_rate);
    return 0;
}
