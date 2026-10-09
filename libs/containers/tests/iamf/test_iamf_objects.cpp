#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"

// Object-based audio elements (IAMF v2.0.0): the Audio Element and Mix Presentation a program of
// objects produces, the Parameter Blocks that animate positions, and the audio read back.

namespace iamf = iclforge::containers::iamf;

namespace {

using iamf::Bytes;

[[nodiscard]] std::vector<float> tone(std::size_t count, double hz, double rate, double amplitude) {
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        out[n] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
    }
    return out;
}

[[nodiscard]] float quantized(float value) {
    const double full_scale = 8388607.0;
    return static_cast<float>(std::round(static_cast<double>(value) * full_scale) / 8388608.0);
}

[[nodiscard]] iamf::ObjectSource make_object(std::size_t samples, double hz) {
    iamf::ObjectSource object;
    object.samples = tone(samples, hz, 48000.0, 0.5);
    return object;
}

[[nodiscard]] iamf::ObjectPosition polar(double azimuth, double elevation, double distance) {
    iamf::ObjectPosition p;
    p.azimuth_deg = azimuth;
    p.elevation_deg = elevation;
    p.distance = distance;
    return p;
}

}  // namespace

TEST_CASE("IAMF a static object writes an Audio Element and no Parameter Blocks", "[iamf][objects]") {
    iamf::ObjectSource object = make_object(2048, 440.0);
    object.initial_position = polar(30.0, 10.0, 0.5);
    const iamf::ObjectElement element{{object}};
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;

    auto sequence = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(sequence.has_value());
    // Base-Advanced profile: the object-based profile whose first Mix Presentation holds only objects.
    CHECK(sequence->header.primary_profile == 3);
    CHECK(sequence->header.additional_profile == 3);
    REQUIRE(sequence->audio_elements.size() == 1);
    CHECK(sequence->audio_elements[0].type == iamf::ElementType::kObjectBased);
    CHECK(sequence->audio_elements[0].objects.num_objects == 1);
    REQUIRE(sequence->mix_presentations.size() == 1);
    const auto& rendering = sequence->mix_presentations[0].sub_mixes[0].elements[0].rendering;
    REQUIRE(rendering.position.has_value());
    CHECK(rendering.position->type == iamf::ParamType::kPolar);
    // Defaults: azimuth 30, elevation 10, distance 0.5 -> 64 of 127.
    CHECK(rendering.position->defaults == std::vector<std::int32_t>{30, 10, 64});
    for (const auto& unit : sequence->temporal_units) {
        CHECK(unit.parameter_blocks.empty());
    }

    // The Audio Element OBU's bytes: id 0, audio_element_type 2 in the top three bits, codec
    // config 0, one substream (id 0), no parameters, objects_config_size 1, num_objects 1.
    auto descriptors = iamf::write_descriptors(*sequence);
    REQUIRE(descriptors.has_value());
    const std::vector<unsigned char> expected{0x08, 0x08, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01};
    bool found = false;
    for (std::size_t i = 0; i + expected.size() <= descriptors->size() && !found; ++i) {
        found = true;
        for (std::size_t j = 0; j < expected.size(); ++j) {
            found = found && std::to_integer<unsigned char>((*descriptors)[i + j]) == expected[j];
        }
    }
    CHECK(found);

    auto file = iamf::mux_objects(track, std::span(&element, 1));
    REQUIRE(file.has_value());
    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channel_names == std::vector<std::string>{"Object 1"});
    REQUIRE(decoded->channels[0].size() == 2048);
    for (std::size_t n = 0; n < 2048; n += 97) {
        CHECK(decoded->channels[0][n] == quantized(object.samples[n]));
    }
}

TEST_CASE("IAMF a moving object writes one Parameter Block per frame", "[iamf][objects]") {
    iamf::ObjectSource object = make_object(3 * 512, 330.0);
    object.initial_position = polar(0.0, 0.0, 1.0);
    // Moves left, then up, then to the opposite direction (azimuth -90, elevation -45), a
    // 180 degree turn that linear interpolation of the direction cannot describe.
    object.positions = {polar(90.0, 0.0, 1.0), polar(90.0, 45.0, 1.0), polar(-90.0, -45.0, 0.5)};
    const iamf::ObjectElement element{{object}};
    iamf::ObjectTrack track;
    track.samples_per_frame = 512;

    auto sequence = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(sequence.has_value());
    REQUIRE(sequence->temporal_units.size() == 3);
    for (const auto& unit : sequence->temporal_units) {
        REQUIRE(unit.parameter_blocks.size() == 1);
        CHECK(unit.parameter_blocks[0].parameter_id == 1);
        REQUIRE(unit.parameter_blocks[0].subblocks.size() == 1);
    }
    // INTER_LINEAR carries only the end point: azimuth, elevation, distance of the frame's end.
    const auto& first = sequence->temporal_units[0].parameter_blocks[0].subblocks[0];
    CHECK(first.animation_type == 3);
    CHECK(first.components[0].end == 90);
    CHECK(first.components[1].end == 0);
    CHECK(first.components[2].end == 127);
    const auto& second = sequence->temporal_units[1].parameter_blocks[0].subblocks[0];
    CHECK(second.animation_type == 3);
    CHECK(second.components[1].end == 45);
    // 90/45 to -90/-45 is 180 degrees: written as a jump, which holds its start point.
    const auto& third = sequence->temporal_units[2].parameter_blocks[0].subblocks[0];
    CHECK(third.animation_type == 0);
    CHECK(third.components[0].start == -90);
    CHECK(third.components[1].start == -45);
    CHECK(third.components[2].start == 64);

    // The blocks survive the raw OBU stream, which needs the definitions to read them.
    auto stream = iamf::write_sequence(*sequence);
    REQUIRE(stream.has_value());
    auto parsed = iamf::read_sequence(*stream);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->temporal_units.size() == 3);
    CHECK(parsed->temporal_units[0].parameter_blocks[0].subblocks[0].components[0].end == 90);
    CHECK(parsed->temporal_units[2].parameter_blocks[0].subblocks[0].components[0].start == -90);
}

TEST_CASE("IAMF Cartesian positions use 8 and 16 bit coordinates", "[iamf][objects]") {
    iamf::ObjectSource object = make_object(1024, 220.0);
    object.initial_position.x = -1.0;
    object.initial_position.y = 0.5;
    object.initial_position.z = 0.0;
    object.positions = {iamf::ObjectPosition{.x = 1.0, .y = -0.5, .z = 0.25}};
    const iamf::ObjectElement element{{object}};

    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;

    track.position_coding = iamf::PositionCoding::kCartesian8;
    auto narrow = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(narrow.has_value());
    const auto& narrow_position = *narrow->mix_presentations[0].sub_mixes[0].elements[0].rendering.position;
    CHECK(narrow_position.type == iamf::ParamType::kCart8);
    CHECK(narrow_position.defaults == std::vector<std::int32_t>{-127, 64, 0});
    const auto& narrow_block = narrow->temporal_units[0].parameter_blocks[0].subblocks[0];
    CHECK(narrow_block.components[0].end == 127);
    CHECK(narrow_block.components[1].end == -64);
    CHECK(narrow_block.components[2].end == 32);

    track.position_coding = iamf::PositionCoding::kCartesian16;
    auto wide = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(wide.has_value());
    const auto& wide_position = *wide->mix_presentations[0].sub_mixes[0].elements[0].rendering.position;
    CHECK(wide_position.type == iamf::ParamType::kCart16);
    CHECK(wide_position.defaults == std::vector<std::int32_t>{-32767, 16384, 0});
    CHECK(wide->temporal_units[0].parameter_blocks[0].subblocks[0].components[0].end == 32767);

    // 16-bit coordinates are two bytes each in the Parameter Block: 4 + 6 bytes after the OBU header.
    auto stream = iamf::write_sequence(*wide);
    REQUIRE(stream.has_value());
    auto parsed = iamf::read_sequence(*stream);
    REQUIRE(parsed.has_value());
    CHECK(parsed->temporal_units[0].parameter_blocks[0].subblocks[0].components[1].end == -16384);
}

TEST_CASE("IAMF a dual object element codes two objects as one stereo substream", "[iamf][objects]") {
    iamf::ObjectSource left = make_object(2048, 300.0);
    iamf::ObjectSource right = make_object(2048, 700.0);
    left.initial_position = polar(45.0, 0.0, 1.0);
    right.initial_position = polar(-45.0, 0.0, 1.0);
    const iamf::ObjectElement element{{left, right}};
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;
    track.bit_depth = 16;

    auto sequence = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(sequence.has_value());
    CHECK(sequence->audio_elements[0].objects.num_objects == 2);
    CHECK(sequence->audio_elements[0].audio_substream_ids.size() == 1);
    const auto& position = *sequence->mix_presentations[0].sub_mixes[0].elements[0].rendering.position;
    CHECK(position.type == iamf::ParamType::kDualPolar);
    CHECK(position.defaults == std::vector<std::int32_t>{45, 0, 127, -45, 0, 127});

    // One substream's frame holds both objects' samples interleaved, 16 bits each.
    CHECK(sequence->temporal_units[0].audio_frames.size() == 1);
    CHECK(sequence->temporal_units[0].audio_frames[0].data.size() == 1024U * 2U * 2U);

    auto file = iamf::mux_objects(track, std::span(&element, 1));
    REQUIRE(file.has_value());
    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channel_names == std::vector<std::string>{"Object 1", "Object 2"});
    const auto quantize16 = [](float v) { return static_cast<float>(std::round(static_cast<double>(v) * 32767.0) / 32768.0); };
    CHECK(decoded->channels[0][5] == quantize16(left.samples[5]));
    CHECK(decoded->channels[1][5] == quantize16(right.samples[5]));
    CHECK(decoded->channels[1][2047] == quantize16(right.samples[2047]));
}

TEST_CASE("IAMF a program whose length is not a whole number of frames is padded and trimmed", "[iamf][objects]") {
    const iamf::ObjectElement element{{make_object(2500, 500.0)}};
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;
    auto sequence = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(sequence.has_value());
    REQUIRE(sequence->temporal_units.size() == 3);
    const auto& last = sequence->temporal_units[2].audio_frames[0];
    CHECK(last.has_trimming);
    CHECK(last.num_samples_to_trim_at_end == 3U * 1024U - 2500U);

    auto file = iamf::write_isobmff(*sequence);
    REQUIRE(file.has_value());
    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->info.sample_durations.size() == 3);
    CHECK(parsed->info.sample_durations[2] == 2500U - 2048U);
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels[0].size() == 2500);
}

TEST_CASE("IAMF several object elements share one Mix Presentation", "[iamf][objects]") {
    std::vector<iamf::ObjectElement> elements;
    for (int i = 0; i < 4; ++i) {
        iamf::ObjectSource object = make_object(1024, 200.0 + 100.0 * i);
        object.initial_position = polar(-90.0 + 60.0 * i, 0.0, 1.0);
        if (i % 2 == 1) {
            object.positions = {polar(45.0 * i, 10.0, 1.0)};
        }
        elements.push_back(iamf::ObjectElement{{object}});
    }
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;
    auto sequence = iamf::build_object_sequence(track, elements);
    REQUIRE(sequence.has_value());
    CHECK(sequence->audio_elements.size() == 4);
    CHECK(sequence->mix_presentations.size() == 1);
    CHECK(sequence->mix_presentations[0].sub_mixes[0].elements.size() == 4);
    // Only the two moving objects have Parameter Blocks, with their own parameter ids.
    REQUIRE(sequence->temporal_units[0].parameter_blocks.size() == 2);
    CHECK(sequence->temporal_units[0].parameter_blocks[0].parameter_id == 3);
    CHECK(sequence->temporal_units[0].parameter_blocks[1].parameter_id == 7);
    CHECK(sequence->temporal_units[0].audio_frames.size() == 4);

    // Everything the Sequence says can be written and read back.
    auto stream = iamf::write_sequence(*sequence);
    REQUIRE(stream.has_value());
    auto parsed = iamf::read_sequence(*stream);
    REQUIRE(parsed.has_value());
    auto again = iamf::write_sequence(*parsed);
    REQUIRE(again.has_value());
    CHECK(*again == *stream);
    for (std::uint32_t id = 0; id < 4; ++id) {
        auto decoded = iamf::decode_pcm(*parsed, id);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels.size() == 1);
    }
}

TEST_CASE("IAMF object programs are refused when they break the profile or the input rules", "[iamf][objects]") {
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;
    const auto one = [](std::size_t samples) { return iamf::ObjectElement{{make_object(samples, 440.0)}}; };

    SECTION("nothing to write") {
        auto none = iamf::build_object_sequence(track, {});
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error() == iamf::MuxError::kNoObjects);
        const iamf::ObjectElement empty;
        auto no_objects = iamf::build_object_sequence(track, std::span(&empty, 1));
        REQUIRE_FALSE(no_objects.has_value());
        CHECK(no_objects.error() == iamf::MuxError::kNoObjects);
    }
    SECTION("three objects in one element") {
        const iamf::ObjectElement three{{make_object(1024, 1.0), make_object(1024, 2.0), make_object(1024, 3.0)}};
        auto result = iamf::build_object_sequence(track, std::span(&three, 1));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kBadObjectElement);
    }
    SECTION("objects of different lengths") {
        const std::vector<iamf::ObjectElement> elements{one(1024), one(2048)};
        auto result = iamf::build_object_sequence(track, elements);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kObjectLengthMismatch);
    }
    SECTION("more than 18 channels") {
        std::vector<iamf::ObjectElement> elements;
        for (int i = 0; i < 19; ++i) {
            elements.push_back(one(1024));
        }
        auto result = iamf::build_object_sequence(track, elements);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kTooManyChannels);
        elements.pop_back();
        CHECK(iamf::build_object_sequence(track, elements).has_value());
    }
    SECTION("a position list that is not one entry per frame") {
        iamf::ObjectElement element = one(2048);
        element.objects[0].positions = {polar(0, 0, 1)};  // 2 frames, 1 position
        auto result = iamf::build_object_sequence(track, std::span(&element, 1));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kBadPositions);
    }
    SECTION("an invalid track") {
        const auto element = one(1024);
        track.sample_rate = 12345;
        auto result = iamf::build_object_sequence(track, std::span(&element, 1));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrack);
    }
}

TEST_CASE("IAMF decode_pcm handles the element types it supports and refuses the rest", "[iamf][objects]") {
    const iamf::ObjectElement element{{make_object(1024, 440.0)}};
    iamf::ObjectTrack track;
    track.samples_per_frame = 1024;
    auto sequence = iamf::build_object_sequence(track, std::span(&element, 1));
    REQUIRE(sequence.has_value());

    SECTION("an element id that does not exist") {
        auto decoded = iamf::decode_pcm(*sequence, 9);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kBadDescriptor);
    }
    SECTION("a codec other than ipcm") {
        sequence->codec_configs[0].codec_id = "Opus";
        sequence->codec_configs[0].lpcm.reset();
        auto decoded = iamf::decode_pcm(*sequence, 0);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kUnsupported);
    }
    SECTION("a scene-based element in mono mode reads its channels in ACN order") {
        iamf::AudioElement ambisonics;
        ambisonics.audio_element_id = 5;
        ambisonics.type = iamf::ElementType::kSceneBased;
        ambisonics.audio_substream_ids = {0};
        ambisonics.ambisonics.ambisonics_mode = 0;
        ambisonics.ambisonics.output_channel_count = 2;
        ambisonics.ambisonics.substream_count = 1;
        ambisonics.ambisonics.channel_mapping = {0, 0};  // both output channels read substream 0
        sequence->audio_elements.push_back(ambisonics);
        auto decoded = iamf::decode_pcm(*sequence, 5);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channel_names == std::vector<std::string>{"ACN 0", "ACN 1"});
        CHECK(decoded->channels[0] == decoded->channels[1]);
    }
}
