#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/dlc.hpp"
#include "iclforge/iab/writer.hpp"

// The IAB writer (SMPTE ST 2098-2:2022 §7, §9, §10) and the AudioDataDLC encoder (Annex B). The
// writer is checked three ways: its output for a small frame against bytes laid out by hand from
// the syntax tables, a full frame round-tripped through iclforge::iab's own reader, and the
// byte stream written a second time from what the reader returned. The encoder is checked by
// decoding what it wrote and comparing the integers.

using Catch::Approx;

namespace iab = iclforge::iab;

namespace {

constexpr std::uint8_t kFrameRate48Fps = 0x3;  // 4 pan sub blocks, 1000 samples per frame at 48 kHz

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(const Bytes& bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes.at(index));
}

// A gain that §5.5's formula gives for the 10-bit code `code`.
[[nodiscard]] double gain_for_code(unsigned code) { return std::pow(2.0, -static_cast<double>(code) / 64.0); }

iab::ObjectPanSubBlock make_pan_block(double x, double y, double z) {
    iab::ObjectPanSubBlock block;
    block.has_pan_info = true;
    block.gain = gain_for_code(40);
    block.position = {.x = x, .y = y, .z = z};
    block.decorrelation = 0.0;
    return block;
}

iab::IaFrame make_frame() {
    iab::IaFrame frame;
    frame.version = 1;
    frame.sample_rate = 48000;
    frame.bit_depth = 24;
    frame.frame_rate_code = kFrameRate48Fps;
    frame.max_rendered = 300;  // exercises the Plex(8) escape

    // A bed: unity channel, attenuated channel with decorrelation, mute channel, a conditional
    // activation, a description with text, a child bed and a remap.
    iab::BedDefinition bed;
    bed.meta_id = 7;
    bed.activation = {.conditional = true, .use_case = 0x30};
    bed.channels.push_back({.channel_id = 0x2, .audio_data_id = 1, .gain = 1.0, .decorrelation = std::nullopt});
    bed.channels.push_back({.channel_id = 0x3, .audio_data_id = 2, .gain = gain_for_code(96), .decorrelation = 0.5});
    bed.channels.push_back({.channel_id = 0x1, .audio_data_id = 0, .gain = 0.0, .decorrelation = 1.0});
    bed.description.dialog = true;
    bed.description.effects = true;
    bed.description.text = "front bed";

    iab::BedDefinition child;
    child.meta_id = 8;
    child.channels.push_back({.channel_id = 0x2, .audio_data_id = 3, .gain = 1.0, .decorrelation = std::nullopt});
    bed.beds.push_back(child);

    iab::BedRemap remap;
    remap.meta_id = 9;
    remap.use_case = 0x01;
    remap.source_channels = 2;
    remap.destination_channels = 1;
    iab::BedRemapSubBlock first;
    first.has_remap_info = true;
    first.destination_channel_ids = {0x4};
    first.gains = {{1.0, gain_for_code(10)}};
    iab::BedRemapSubBlock hold;
    hold.has_remap_info = false;
    remap.sub_blocks = {first, hold, first, hold};
    bed.remaps.push_back(remap);
    frame.beds.push_back(bed);

    // An object: sub block 0 with snap, tolerance, zone gains and 3-D spread; sub block 1 holds;
    // sub block 2 uses the one-dimensional spread; sub block 3 the low-resolution one.
    iab::ObjectDefinition object;
    object.meta_id = 20;
    object.audio_data_id = 4;
    object.activation = {.conditional = false, .use_case = 0};
    object.sub_blocks.resize(4);
    object.sub_blocks[0] = make_pan_block(0.25, 0.75, 0.5);
    object.sub_blocks[0].snap = true;
    object.sub_blocks[0].snap_tolerance = 819.0 / 4095.0;
    object.sub_blocks[0].zone_gains = std::array<double, iab::kZoneCount>{0.0, 1.0, 512.0 / 1023.0, 1.0, 0.0,
                                                                          1.0, 100.0 / 1023.0, 0.0, 1.0};
    object.sub_blocks[0].spread = {.mode = iab::ObjectSpreadMode::kThreeD,
                                   .x = 100.0 / 4095.0,
                                   .y = 2000.0 / 4095.0,
                                   .z = 4095.0 / 4095.0};
    object.sub_blocks[0].decorrelation = 128.0 / 255.0;
    object.sub_blocks[1].has_pan_info = false;
    object.sub_blocks[2] = make_pan_block(1.0, 0.0, 1.0);
    object.sub_blocks[2].spread = {.mode = iab::ObjectSpreadMode::kOneD,
                                   .x = 300.0 / 4095.0,
                                   .y = 300.0 / 4095.0,
                                   .z = 300.0 / 4095.0};
    object.sub_blocks[2].decorrelation = 1.0;
    object.sub_blocks[3] = make_pan_block(0.5, 0.5, 0.0);
    object.sub_blocks[3].snap = true;  // snap without a tolerance
    object.sub_blocks[3].spread = {.mode = iab::ObjectSpreadMode::kLowRez,
                                   .x = 50.0 / 255.0,
                                   .y = 50.0 / 255.0,
                                   .z = 50.0 / 255.0};
    object.description.music = true;

    iab::ObjectZoneDefinition19 zone19;
    zone19.sub_blocks.resize(4);
    zone19.sub_blocks[0].zone_gains.fill(1.0);
    zone19.sub_blocks[0].zone_gains[3] = 0.0;
    zone19.sub_blocks[0].zone_gains[5] = 700.0 / 1023.0;
    zone19.sub_blocks[1].has_zone_info = false;
    zone19.sub_blocks[2].zone_gains.fill(0.0);
    zone19.sub_blocks[3].has_zone_info = false;
    object.zone19 = zone19;

    iab::ObjectDefinition child_object;
    child_object.meta_id = 21;
    child_object.audio_data_id = 0;
    child_object.sub_blocks.resize(4);
    child_object.sub_blocks[0] = make_pan_block(0.5, 0.5, 0.5);
    for (std::size_t sb = 1; sb < 4; ++sb) {
        child_object.sub_blocks[sb].has_pan_info = false;
    }
    object.objects.push_back(child_object);
    frame.objects.push_back(object);

    frame.audio_pcm.push_back({.audio_data_id = 1, .samples = std::vector<float>(1000, 0.5F)});
    frame.audio_pcm.push_back({.audio_data_id = 2, .samples = std::vector<float>(1000, -0.25F)});

    std::vector<float> tone(1000);
    for (std::size_t n = 0; n < tone.size(); ++n) {
        tone[n] = 0.5F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(n) / 48000.0F);
    }
    auto dlc = iab::encode_dlc(3, tone, kFrameRate48Fps, {.sample_rate = 48000, .bit_depth = 24});
    REQUIRE(dlc.has_value());
    frame.audio_dlc.push_back(*dlc);

    frame.authoring_tool = iab::AuthoringToolInfo{.uri = "urn:example:tool/1.0"};
    iab::UserData user;
    user.user_id.fill(std::byte{0xAB});
    user.data = {std::byte{1}, std::byte{2}, std::byte{3}};
    frame.user_data.push_back(user);
    return frame;
}

void check_pan_block(const iab::ObjectPanSubBlock& got, const iab::ObjectPanSubBlock& want) {
    CHECK(got.has_pan_info == want.has_pan_info);
    if (!want.has_pan_info) {
        return;
    }
    CHECK(got.gain == Approx(want.gain).epsilon(1e-12));
    CHECK(got.position.x == Approx(want.position.x).margin(1e-4));
    CHECK(got.position.y == Approx(want.position.y).margin(1e-4));
    CHECK(got.position.z == Approx(want.position.z).margin(1e-4));
    CHECK(got.snap == want.snap);
    CHECK(got.snap_tolerance.has_value() == want.snap_tolerance.has_value());
    if (want.snap_tolerance.has_value() && got.snap_tolerance.has_value()) {
        CHECK(*got.snap_tolerance == Approx(*want.snap_tolerance).margin(1e-9));
    }
    REQUIRE(got.zone_gains.has_value() == want.zone_gains.has_value());
    if (want.zone_gains.has_value()) {
        for (std::size_t z = 0; z < iab::kZoneCount; ++z) {
            CHECK((*got.zone_gains)[z] == Approx((*want.zone_gains)[z]).margin(1e-9));
        }
    }
    CHECK(got.spread.mode == want.spread.mode);
    CHECK(got.spread.x == Approx(want.spread.x).margin(1e-9));
    CHECK(got.spread.y == Approx(want.spread.y).margin(1e-9));
    CHECK(got.spread.z == Approx(want.spread.z).margin(1e-9));
    CHECK(got.decorrelation == Approx(want.decorrelation).margin(1e-9));
}

[[nodiscard]] std::vector<float> pcm_from_integers(const std::vector<std::int32_t>& values, unsigned bit_depth) {
    std::vector<float> out;
    for (const auto v : values) {
        out.push_back(static_cast<float>(v) / static_cast<float>(1U << (bit_depth - 1)));
    }
    return out;
}

}  // namespace

TEST_CASE("IAB writer: a small frame matches the bytes the syntax tables give", "[iab][iab-writer]") {
    iab::IaFrame frame;
    frame.sample_rate = 48000;
    frame.bit_depth = 16;
    frame.frame_rate_code = kFrameRate48Fps;
    frame.max_rendered = 2;
    frame.audio_pcm.push_back({.audio_data_id = 5, .samples = std::vector<float>(1000, 0.0F)});
    frame.audio_pcm[0].samples[0] = 0.5F;      // 0x4000
    frame.audio_pcm[0].samples[1] = -0.5F;     // 0xC000
    frame.audio_pcm[0].samples[2] = -1.0F;     // 0x8000

    auto payload = iab::write_iaframe(frame);
    REQUIRE(payload.has_value());

    // Table 5: Version 0x01; SampleRate 0b00, BitDepth 0b00, FrameRate 0b0011 -> 0x03;
    // MaxRendered Plex(8) = 2 -> 0x02, already aligned; SubElementCount Plex(8) = 1 -> 0x01.
    // Then the AudioDataPCM element: ElementID 0x400 as Plex(8) = 0xFF 0x0400, ElementSize
    // 1 + 2000 = 2001 = 0xFF 0x07D1, AudioDataID 5, and little-endian samples.
    const Bytes& b = *payload;
    REQUIRE(b.size() == 4 + 3 + 3 + 1 + 2000);
    CHECK(byte_at(b, 0) == 0x01);
    CHECK(byte_at(b, 1) == 0x03);
    CHECK(byte_at(b, 2) == 0x02);
    CHECK(byte_at(b, 3) == 0x01);
    CHECK(byte_at(b, 4) == 0xFF);
    CHECK(byte_at(b, 5) == 0x04);
    CHECK(byte_at(b, 6) == 0x00);
    CHECK(byte_at(b, 7) == 0xFF);
    CHECK(byte_at(b, 8) == 0x07);
    CHECK(byte_at(b, 9) == 0xD1);
    CHECK(byte_at(b, 10) == 0x05);
    CHECK(byte_at(b, 11) == 0x00);
    CHECK(byte_at(b, 12) == 0x40);
    CHECK(byte_at(b, 13) == 0x00);
    CHECK(byte_at(b, 14) == 0xC0);
    CHECK(byte_at(b, 15) == 0x00);
    CHECK(byte_at(b, 16) == 0x80);
}

TEST_CASE("IAB writer: a full frame survives write and parse", "[iab][iab-writer]") {
    const iab::IaFrame frame = make_frame();
    auto payload = iab::write_iaframe(frame);
    REQUIRE(payload.has_value());

    auto parsed = iab::parse_iaframe(*payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->sample_rate == 48000);
    CHECK(parsed->bit_depth == 24);
    CHECK(parsed->frame_rate_code == kFrameRate48Fps);
    CHECK(parsed->max_rendered == 300);

    REQUIRE(parsed->beds.size() == 1);
    const auto& bed = parsed->beds[0];
    CHECK(bed.meta_id == 7);
    CHECK(bed.activation.conditional);
    CHECK(bed.activation.use_case == 0x30);
    REQUIRE(bed.channels.size() == 3);
    CHECK(bed.channels[0].channel_id == 0x2);
    CHECK(bed.channels[0].gain == 1.0);
    CHECK_FALSE(bed.channels[0].decorrelation.has_value());
    CHECK(bed.channels[1].audio_data_id == 2);
    CHECK(bed.channels[1].gain == Approx(gain_for_code(96)).epsilon(1e-12));
    REQUIRE(bed.channels[1].decorrelation.has_value());
    CHECK(*bed.channels[1].decorrelation == Approx(0.5).margin(1.0 / 255.0));
    CHECK(bed.channels[2].gain == 0.0);
    REQUIRE(bed.channels[2].decorrelation.has_value());
    CHECK(*bed.channels[2].decorrelation == 1.0);
    CHECK(bed.description.dialog);
    CHECK(bed.description.effects);
    CHECK_FALSE(bed.description.music);
    REQUIRE(bed.description.text.has_value());
    CHECK(*bed.description.text == "front bed");
    REQUIRE(bed.beds.size() == 1);
    CHECK(bed.beds[0].meta_id == 8);
    REQUIRE(bed.remaps.size() == 1);
    const auto& remap = bed.remaps[0];
    CHECK(remap.meta_id == 9);
    CHECK(remap.source_channels == 2);
    REQUIRE(remap.sub_blocks.size() == 4);
    CHECK(remap.sub_blocks[0].has_remap_info);
    CHECK_FALSE(remap.sub_blocks[1].has_remap_info);
    REQUIRE(remap.sub_blocks[2].gains.size() == 1);
    CHECK(remap.sub_blocks[2].gains[0][0] == 1.0);
    CHECK(remap.sub_blocks[2].gains[0][1] == Approx(gain_for_code(10)).epsilon(1e-12));

    REQUIRE(parsed->objects.size() == 1);
    const auto& object = parsed->objects[0];
    CHECK(object.meta_id == 20);
    CHECK(object.audio_data_id == 4);
    REQUIRE(object.sub_blocks.size() == 4);
    for (std::size_t sb = 0; sb < 4; ++sb) {
        check_pan_block(object.sub_blocks[sb], frame.objects[0].sub_blocks[sb]);
    }
    REQUIRE(object.zone19.has_value());
    REQUIRE(object.zone19->sub_blocks.size() == 4);
    CHECK(object.zone19->sub_blocks[0].zone_gains[3] == 0.0);
    CHECK(object.zone19->sub_blocks[0].zone_gains[5] == Approx(700.0 / 1023.0).margin(1e-9));
    CHECK_FALSE(object.zone19->sub_blocks[1].has_zone_info);
    CHECK(object.zone19->sub_blocks[2].zone_gains[0] == 0.0);
    REQUIRE(object.objects.size() == 1);
    CHECK(object.objects[0].meta_id == 21);

    REQUIRE(parsed->audio_pcm.size() == 2);
    CHECK(parsed->audio_pcm[0].samples.size() == 1000);
    CHECK(parsed->audio_pcm[0].samples[0] == 0.5F);
    CHECK(parsed->audio_pcm[1].samples[999] == -0.25F);
    REQUIRE(parsed->audio_dlc.size() == 1);
    CHECK(parsed->audio_dlc[0].coded == frame.audio_dlc[0].coded);
    REQUIRE(parsed->authoring_tool.has_value());
    CHECK(parsed->authoring_tool->uri == "urn:example:tool/1.0");
    REQUIRE(parsed->user_data.size() == 1);
    CHECK(parsed->user_data[0].data.size() == 3);

    // Written again from what the reader returned, the bytes do not change.
    auto again = iab::write_iaframe(*parsed);
    REQUIRE(again.has_value());
    CHECK(*again == *payload);
}

TEST_CASE("IAB writer: an IABitstream round trips through parse_iabitstream", "[iab][iab-writer]") {
    iab::IABitstreamFrame first;
    first.preamble = {std::byte{0xDE}, std::byte{0xAD}};
    first.frame = make_frame();
    iab::IABitstreamFrame second;
    second.frame = make_frame();
    second.frame.max_rendered = 4;
    const std::vector<iab::IABitstreamFrame> frames{first, second};

    auto bytes = iab::write_iabitstream(frames);
    REQUIRE(bytes.has_value());
    CHECK(byte_at(*bytes, 0) == 0x01);

    const auto path = std::filesystem::temp_directory_path() / "iclforge_iab_writer_test.iab";
    REQUIRE(iab::write_iabitstream(path.string(), frames).has_value());
    auto from_file = iab::parse_iabitstream(path.string());
    std::filesystem::remove(path);
    REQUIRE(from_file.has_value());
    REQUIRE(from_file->size() == 2);
    CHECK((*from_file)[0].preamble == first.preamble);
    CHECK((*from_file)[1].preamble.empty());
    CHECK((*from_file)[0].frame.max_rendered == 300);
    CHECK((*from_file)[1].frame.max_rendered == 4);
}

TEST_CASE("IAB writer: 96 kHz, 16-bit frames", "[iab][iab-writer]") {
    iab::IaFrame frame;
    frame.sample_rate = 96000;
    frame.bit_depth = 16;
    frame.frame_rate_code = kFrameRate48Fps;
    frame.audio_pcm.push_back({.audio_data_id = 1, .samples = std::vector<float>(2000, 0.125F)});
    auto payload = iab::write_iaframe(frame);
    REQUIRE(payload.has_value());
    auto parsed = iab::parse_iaframe(*payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sample_rate == 96000);
    REQUIRE(parsed->audio_pcm.size() == 1);
    CHECK(parsed->audio_pcm[0].samples.size() == 2000);
    CHECK(parsed->audio_pcm[0].samples[1999] == 0.125F);
}

TEST_CASE("IAB writer: gains and codes at their boundaries", "[iab][iab-writer]") {
    iab::IaFrame frame;
    frame.frame_rate_code = kFrameRate48Fps;
    iab::BedDefinition bed;
    bed.meta_id = 1;
    for (const double gain : {1.0, 1.5, 0.0, -1.0, 1.0e-9, gain_for_code(0), gain_for_code(1022)}) {
        bed.channels.push_back({.channel_id = 0x2, .audio_data_id = 0, .gain = gain, .decorrelation = std::nullopt});
    }
    frame.beds.push_back(bed);
    auto payload = iab::write_iaframe(frame);
    REQUIRE(payload.has_value());
    auto parsed = iab::parse_iaframe(*payload);
    REQUIRE(parsed.has_value());
    const auto& channels = parsed->beds[0].channels;
    REQUIRE(channels.size() == 7);
    CHECK(channels[0].gain == 1.0);
    CHECK(channels[1].gain == 1.0);  // above unity is clamped
    CHECK(channels[2].gain == 0.0);
    CHECK(channels[3].gain == 0.0);
    CHECK(channels[4].gain == 0.0);  // rounds to the 0x3FF code, which means zero
    CHECK(channels[5].gain == 1.0);
    CHECK(channels[6].gain == Approx(gain_for_code(1022)).epsilon(1e-12));
}

TEST_CASE("IAB writer: refuses what the syntax cannot carry", "[iab][iab-writer]") {
    iab::IaFrame frame;
    frame.frame_rate_code = kFrameRate48Fps;

    SECTION("a sample rate other than 48000 or 96000") {
        frame.sample_rate = 44100;
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kBadSampleRate);
    }
    SECTION("a bit depth other than 16 or 24") {
        frame.bit_depth = 32;
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kBadBitDepth);
    }
    SECTION("a reserved frame rate") {
        frame.frame_rate_code = 0xB;
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kReservedFrameRate);
    }
    SECTION("a PCM element of the wrong length") {
        frame.audio_pcm.push_back({.audio_data_id = 1, .samples = std::vector<float>(999, 0.0F)});
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kSampleCount);
    }
    SECTION("an object with the wrong number of sub blocks") {
        iab::ObjectDefinition object;
        object.sub_blocks.resize(3);
        frame.objects.push_back(object);
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kSubBlockCount);
    }
    SECTION("a remap row that is not SourceChannels wide") {
        iab::BedDefinition bed;
        iab::BedRemap remap;
        remap.source_channels = 2;
        remap.destination_channels = 1;
        iab::BedRemapSubBlock block;
        block.destination_channel_ids = {1};
        block.gains = {{1.0}};
        remap.sub_blocks.assign(4, block);
        bed.remaps.push_back(remap);
        frame.beds.push_back(bed);
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kBadElement);
    }
    SECTION("a description text holding a NUL") {
        iab::BedDefinition bed;
        bed.description.text = std::string("a\0b", 3);
        frame.beds.push_back(bed);
        auto out = iab::write_iaframe(frame);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kBadElement);
    }
    SECTION("an output path that cannot be opened") {
        auto out = iab::write_iabitstream("/nonexistent-directory/out.iab", {});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kCannotOpen);
    }
}

namespace {

[[nodiscard]] std::vector<float> make_tone(std::size_t count, double hz, double rate, double amplitude) {
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        out[n] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
    }
    return out;
}

// Quantizes the way encode_dlc() and AudioDataPCM do.
[[nodiscard]] std::vector<std::int32_t> quantize(const std::vector<float>& samples, unsigned bit_depth) {
    const auto full_scale = static_cast<double>(1U << (bit_depth - 1));
    std::vector<std::int32_t> out;
    for (const float s : samples) {
        out.push_back(static_cast<std::int32_t>(std::clamp(std::round(static_cast<double>(s) * full_scale),
                                                           -full_scale, full_scale - 1.0)));
    }
    return out;
}

void check_dlc_round_trip(const std::vector<float>& input, std::uint8_t frame_rate_code,
                          const iab::DlcEncodeOptions& options, std::size_t* coded_size = nullptr) {
    auto element = iab::encode_dlc(1, input, frame_rate_code, options);
    REQUIRE(element.has_value());
    auto decoded = iab::decode_dlc(*element, frame_rate_code);
    REQUIRE(decoded.has_value());
    CHECK(decoded->sample_rate == options.sample_rate);
    REQUIRE(decoded->samples.size() == input.size());
    const auto expected = quantize(input, options.bit_depth);
    const unsigned shift = 32 - options.bit_depth;
    for (std::size_t n = 0; n < expected.size(); ++n) {
        REQUIRE(decoded->samples[n] ==
                static_cast<std::int32_t>(static_cast<std::uint32_t>(expected[n]) << shift));
    }
    if (coded_size != nullptr) {
        *coded_size = element->coded.size();
    }
}

}  // namespace

TEST_CASE("DLC encoder: 48 kHz round trips exactly and a tone compresses", "[iab][dlc-encoder]") {
    const auto tone = make_tone(1000, 440.0, 48000.0, 0.5);

    std::size_t predicted = 0;
    check_dlc_round_trip(tone, kFrameRate48Fps, {.sample_rate = 48000, .bit_depth = 24, .max_prediction_order = 8},
                         &predicted);
    std::size_t minimal = 0;
    check_dlc_round_trip(tone, kFrameRate48Fps, {.sample_rate = 48000, .bit_depth = 24, .max_prediction_order = 0},
                         &minimal);
    CHECK(predicted < minimal);
    CHECK(minimal < 3000);  // 24-bit PCM would be 3000 bytes
}

TEST_CASE("DLC encoder: silence, full scale and noise", "[iab][dlc-encoder]") {
    check_dlc_round_trip(std::vector<float>(1000, 0.0F), kFrameRate48Fps, {});

    std::vector<float> full_scale(1000);
    for (std::size_t n = 0; n < full_scale.size(); ++n) {
        full_scale[n] = (n % 2 == 0) ? 1.0F : -1.0F;  // +1.0 clips to the largest code
    }
    check_dlc_round_trip(full_scale, kFrameRate48Fps, {});

    std::vector<float> noise(1000);
    std::uint32_t state = 12345;
    for (auto& s : noise) {
        state = state * 1664525U + 1013904223U;
        s = static_cast<float>(static_cast<std::int32_t>(state >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    }
    check_dlc_round_trip(noise, kFrameRate48Fps, {.bit_depth = 24});
    check_dlc_round_trip(noise, kFrameRate48Fps, {.bit_depth = 16});
}

TEST_CASE("DLC encoder: 96 kHz round trips exactly through the base and extension layers", "[iab][dlc-encoder]") {
    auto tone = make_tone(2000, 5000.0, 96000.0, 0.4);
    const auto high = make_tone(2000, 30000.0, 96000.0, 0.2);
    for (std::size_t n = 0; n < tone.size(); ++n) {
        tone[n] += high[n];
    }
    check_dlc_round_trip(tone, kFrameRate48Fps, {.sample_rate = 96000, .bit_depth = 24});
    check_dlc_round_trip(tone, kFrameRate48Fps, {.sample_rate = 96000, .bit_depth = 16, .max_prediction_order = 0});

    // The base layer decodes on its own, near the input at the 48 kHz rate: 5 kHz sits well
    // inside it.
    auto element = iab::encode_dlc(1, tone, kFrameRate48Fps, {.sample_rate = 96000, .bit_depth = 24});
    REQUIRE(element.has_value());
    auto base = iab::decode_dlc(*element, kFrameRate48Fps, {.base_layer_only = true});
    REQUIRE(base.has_value());
    CHECK(base->sample_rate == 48000);
    CHECK(base->samples.size() == 1000);
}

TEST_CASE("DLC encoder: every integer frame rate", "[iab][dlc-encoder]") {
    for (std::uint8_t code = 0; code <= 8; ++code) {
        const auto count = static_cast<std::size_t>(*iab::sample_count(code, false));
        check_dlc_round_trip(make_tone(count, 1000.0, 48000.0, 0.3), code, {});
    }
}

TEST_CASE("DLC encoder: refuses what Annex B cannot carry", "[iab][dlc-encoder]") {
    const std::vector<float> samples(1000, 0.0F);

    SECTION("the non-integer frame rate") {
        auto out = iab::encode_dlc(1, std::vector<float>(2002, 0.0F), 0x9);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kDlcFrameRate);
    }
    SECTION("a reserved frame rate") {
        auto out = iab::encode_dlc(1, samples, 0xA);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kReservedFrameRate);
    }
    SECTION("the wrong number of samples") {
        auto out = iab::encode_dlc(1, std::vector<float>(999, 0.0F), kFrameRate48Fps);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iab::WriteError::kSampleCount);
    }
    SECTION("a bad sample rate or bit depth") {
        auto rate = iab::encode_dlc(1, samples, kFrameRate48Fps, {.sample_rate = 44100});
        REQUIRE_FALSE(rate.has_value());
        CHECK(rate.error() == iab::WriteError::kBadSampleRate);
        auto depth = iab::encode_dlc(1, samples, kFrameRate48Fps, {.bit_depth = 20});
        REQUIRE_FALSE(depth.has_value());
        CHECK(depth.error() == iab::WriteError::kBadBitDepth);
    }
}

TEST_CASE("IAB writer: a DLC-coded frame decodes to the PCM it was written from", "[iab][iab-writer][dlc-encoder]") {
    const auto tone = make_tone(1000, 1000.0, 48000.0, 0.25);

    iab::IaFrame frame;
    frame.sample_rate = 48000;
    frame.bit_depth = 24;
    frame.frame_rate_code = kFrameRate48Fps;
    auto dlc = iab::encode_dlc(2, tone, kFrameRate48Fps, {.sample_rate = 48000, .bit_depth = 24});
    REQUIRE(dlc.has_value());
    frame.audio_dlc.push_back(*dlc);
    frame.audio_pcm.push_back({.audio_data_id = 3, .samples = tone});

    auto payload = iab::write_iaframe(frame);
    REQUIRE(payload.has_value());
    auto parsed = iab::parse_iaframe(*payload);
    REQUIRE(parsed.has_value());
    auto audio = iab::decode_audio(*parsed);
    REQUIRE(audio.has_value());
    REQUIRE(audio->size() == 2);
    // The PCM element holds the 24-bit integers; the DLC one decodes to the same integers.
    const auto expected = pcm_from_integers(quantize(tone, 24), 24);
    for (std::size_t n = 0; n < expected.size(); ++n) {
        REQUIRE((*audio)[0].samples[n] == expected[n]);
        REQUIRE((*audio)[1].samples[n] == expected[n]);
    }
}
