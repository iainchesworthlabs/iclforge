#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/iamf/iamf.hpp"
#include "iclforge/iamf/model.hpp"
#include "iclforge/iamf/sequence.hpp"

// OBU level tests for iclforge::iamf: the bytes of individual OBUs against hand-assembled
// expectations from the IAMF v2.0.0 syntax, a Sequence that uses every structure round-tripped
// through write_sequence() and read_sequence(), and the reader's handling of what a parser is told
// to ignore or cannot trust.

namespace iamf = iclforge::iamf;

namespace {

using iamf::Bytes;

[[nodiscard]] Bytes bytes_of(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

[[nodiscard]] std::vector<unsigned> to_ints(const Bytes& bytes) {
    std::vector<unsigned> out;
    for (const std::byte b : bytes) {
        out.push_back(std::to_integer<unsigned>(b));
    }
    return out;
}

[[nodiscard]] iamf::ParamDefinition frame_definition(std::uint32_t id, std::uint32_t frame = 960) {
    iamf::ParamDefinition d;
    d.parameter_id = id;
    d.parameter_rate = 48000;
    d.param_definition_mode = 0;
    d.duration = frame;
    d.constant_subblock_duration = frame;
    return d;
}

[[nodiscard]] iamf::MixGainParamDefinition mix_gain(std::uint32_t id, std::int16_t default_gain = 0) {
    iamf::MixGainParamDefinition d;
    d.definition = frame_definition(id);
    d.default_mix_gain = default_gain;
    return d;
}

[[nodiscard]] iamf::ElementGainOffset gain_offset(std::uint8_t type, std::int16_t offset, std::int16_t min = 0,
                                                  std::int16_t max = 0) {
    iamf::ElementGainOffset g;
    g.type = type;
    g.offset = offset;
    g.min_offset = min;
    g.max_offset = max;
    return g;
}

// Two codecs (LPCM and Opus), channel-based scalable stereo + 5.1, mono-mode scene-based
// Ambisonics, a single and a dual object, and two Mix Presentations, with every parameter type.
[[nodiscard]] iamf::Sequence make_full_sequence() {
    iamf::Sequence s;
    s.header = {static_cast<std::uint8_t>(iamf::Profile::kAdvanced1), static_cast<std::uint8_t>(iamf::Profile::kAdvanced1)};

    iamf::CodecConfig pcm;
    pcm.codec_config_id = 0;
    pcm.codec_id = "ipcm";
    pcm.num_samples_per_frame = 960;
    pcm.lpcm = iamf::LpcmConfig{.sample_format_flags = 0x01, .sample_size = 24, .sample_rate = 48000};
    s.codec_configs.push_back(pcm);
    iamf::CodecConfig opus;
    opus.codec_config_id = 1;
    opus.codec_id = "Opus";
    opus.num_samples_per_frame = 960;
    opus.audio_roll_distance = -4;
    opus.decoder_config = bytes_of({0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    s.codec_configs.push_back(opus);

    iamf::Metadata tags;
    tags.metadata_type = 2;
    tags.tags = {{"iamf_encoder", "iclforge test"}};
    s.descriptor_metadata.push_back(tags);
    iamf::Metadata t35;
    t35.metadata_type = 1;
    t35.itu_t_t35_country_code = 0xFF;
    t35.itu_t_t35_country_code_extension = 0x12;
    t35.itu_t_t35_payload = bytes_of({0xAA, 0xBB});
    s.descriptor_metadata.push_back(t35);

    // e0: scalable channel audio, stereo then 5.1 (Ls/Rs, C, LFE added), Opus coded.
    iamf::AudioElement e0;
    e0.audio_element_id = 0;
    e0.type = iamf::ElementType::kChannelBased;
    e0.codec_config_id = 1;
    e0.audio_substream_ids = {0, 1, 2, 3};
    iamf::DemixingParamDefinition demixing;
    demixing.definition = frame_definition(10);
    demixing.default_dmixp_mode = 2;
    demixing.default_w = 5;
    e0.demixing = demixing;
    e0.recon_gain = iamf::ReconGainParamDefinition{frame_definition(11)};
    iamf::ChannelLayer stereo;
    stereo.loudspeaker_layout = 1;
    stereo.output_gain_is_present = true;
    stereo.output_gain_flags = 0x30;
    stereo.output_gain = -256;
    stereo.substream_count = 1;
    stereo.coupled_substream_count = 1;
    iamf::ChannelLayer surround;
    surround.loudspeaker_layout = 2;
    surround.recon_gain_is_present = true;
    surround.substream_count = 3;
    surround.coupled_substream_count = 1;
    e0.layers = {stereo, surround};
    s.audio_elements.push_back(e0);

    // e1: scene-based, mono mode, first-order Ambisonics.
    iamf::AudioElement e1;
    e1.audio_element_id = 1;
    e1.type = iamf::ElementType::kSceneBased;
    e1.codec_config_id = 0;
    e1.audio_substream_ids = {4, 5, 6, 7};
    e1.ambisonics.ambisonics_mode = 0;
    e1.ambisonics.output_channel_count = 4;
    e1.ambisonics.substream_count = 4;
    e1.ambisonics.channel_mapping = {0, 1, 2, 3};
    s.audio_elements.push_back(e1);

    // e2 and e3: a single object and a dual object, in the first mix presentation's own parameter ids.
    iamf::AudioElement e2;
    e2.audio_element_id = 2;
    e2.type = iamf::ElementType::kObjectBased;
    e2.codec_config_id = 0;
    e2.audio_substream_ids = {8};
    e2.objects.num_objects = 1;
    e2.objects.extension_bytes = bytes_of({0xEE});
    s.audio_elements.push_back(e2);
    iamf::AudioElement e3 = e2;
    e3.audio_element_id = 3;
    e3.audio_substream_ids = {20};  // beyond OBU_IA_Audio_Frame_ID17: needs an explicit id
    e3.objects.num_objects = 2;
    e3.objects.extension_bytes.clear();
    s.audio_elements.push_back(e3);

    // Mix presentation 0: e0 and e1 mixed, in two languages, with tags and optional fields.
    iamf::MixPresentation mix0;
    mix0.mix_presentation_id = 100;
    mix0.annotations_language = {"en-us", "fr-fr"};
    mix0.localized_presentation_annotations = {"Main mix", "Mixage principal"};
    iamf::SubMix sub0;
    iamf::SubMixElement m0;
    m0.audio_element_id = 0;
    m0.localized_element_annotations = {"Bed", "Lit"};
    m0.rendering.headphones_rendering_mode = 1;
    m0.rendering.binaural_filter_profile = 2;
    m0.rendering.element_gain_offset = gain_offset(1, 256, -512, 128);
    m0.element_mix_gain = mix_gain(20, -128);
    // Timing carried by the Parameter Blocks (param_definition_mode 1) for this one.
    m0.element_mix_gain.definition.param_definition_mode = 1;
    m0.element_mix_gain.definition.duration = 0;
    m0.element_mix_gain.definition.constant_subblock_duration = 0;
    sub0.elements.push_back(m0);
    iamf::SubMixElement m1;
    m1.audio_element_id = 1;
    m1.localized_element_annotations = {"Ambience", "Ambiance"};
    m1.element_mix_gain = mix_gain(21);
    m1.rendering.element_gain_offset = gain_offset(0, -256);
    sub0.elements.push_back(m1);
    sub0.output_mix_gain = mix_gain(22);
    iamf::SubMixLayout stereo_loudness;
    stereo_loudness.layout = {2, 0};
    stereo_loudness.loudness.integrated_loudness = -5888;
    stereo_loudness.loudness.digital_peak = -384;
    stereo_loudness.loudness.true_peak = -300;
    stereo_loudness.loudness.anchored = {{1, -5700}, {2, -5600}};
    iamf::SubMixLayout binaural_loudness;
    binaural_loudness.layout = {3, 0};
    binaural_loudness.loudness.info_type = 0x04;  // live
    iamf::MomentaryLoudnessInfo momentary;
    momentary.definition = frame_definition(23);
    momentary.num_bin_pairs_minus_one = 1;
    momentary.bin_width_minus_one = 2;
    momentary.first_bin_center = 10;
    momentary.counts = {1, 2, 3, 300};
    stereo_loudness.loudness.momentary = momentary;
    stereo_loudness.loudness.loudness_range = 17;
    stereo_loudness.loudness.info_type_bytes = bytes_of({0x99});
    sub0.layouts = {stereo_loudness, binaural_loudness};
    mix0.sub_mixes.push_back(sub0);
    mix0.tags = std::vector<iamf::MixTag>{{"content_language", "eng"}, {"content_type", "main"}};
    mix0.preferred_renderers = std::pair<std::uint8_t, std::uint8_t>{0, 0};
    mix0.optional_fields_remaining_bytes = bytes_of({0x01});
    s.mix_presentations.push_back(mix0);

    // Mix presentation 1: the two objects, with polar and 16-bit dual Cartesian positions.
    iamf::MixPresentation mix1;
    mix1.mix_presentation_id = 101;
    iamf::SubMix sub1;
    iamf::SubMixElement o2;
    o2.audio_element_id = 2;
    o2.element_mix_gain = mix_gain(24);
    iamf::PositionParamDefinition polar;
    polar.type = iamf::ParamType::kPolar;
    polar.definition = frame_definition(30);
    polar.defaults = {-45, 30, 100};
    o2.rendering.position = polar;
    o2.rendering.extension_bytes = bytes_of({0x55});
    sub1.elements.push_back(o2);
    iamf::SubMixElement o3;
    o3.audio_element_id = 3;
    o3.element_mix_gain = mix_gain(25);
    iamf::PositionParamDefinition dual;
    dual.type = iamf::ParamType::kDualCart16;
    dual.definition = frame_definition(31);
    dual.defaults = {-32767, 0, 32767, 100, -100, 5};
    o3.rendering.position = dual;
    sub1.elements.push_back(o3);
    sub1.output_mix_gain = mix_gain(26);
    sub1.layouts.push_back({{2, 0}, {}});
    mix1.sub_mixes.push_back(sub1);
    s.mix_presentations.push_back(mix1);

    // IA Data: two Temporal Units, the second not a key frame, carrying every kind of block.
    iamf::TemporalUnit u0;
    u0.has_temporal_delimiter = true;
    iamf::ParameterBlock demix;
    demix.parameter_id = 10;
    iamf::ParameterSubblock demix_sub;
    demix_sub.dmixp_mode = 5;
    demix.subblocks.push_back(demix_sub);
    u0.parameter_blocks.push_back(demix);

    iamf::ParameterBlock recon;
    recon.parameter_id = 11;
    iamf::ParameterSubblock recon_sub;
    recon_sub.recon_layers.resize(2);
    recon_sub.recon_layers[1].recon_gain_flags = 0b0001011;
    recon_sub.recon_layers[1].recon_gains = {200, 100, 50};
    recon.subblocks.push_back(recon_sub);
    u0.parameter_blocks.push_back(recon);

    iamf::ParameterBlock gain;  // mode 1: this block carries its own timing, three sub blocks
    gain.parameter_id = 20;
    gain.duration = 960;
    gain.constant_subblock_duration = 0;
    for (const auto& [duration, animation] : {std::pair<unsigned, unsigned>{300, 0}, {400, 1}, {260, 2}}) {
        iamf::ParameterSubblock sub;
        sub.subblock_duration = duration;
        sub.animation_type = animation;
        iamf::AnimatedValue v;
        v.start = -256;
        v.end = 128;
        v.control = 64;
        v.control_relative_time = 128;
        sub.components.push_back(v);
        gain.subblocks.push_back(sub);
    }
    u0.parameter_blocks.push_back(gain);

    iamf::ParameterBlock loudness;
    loudness.parameter_id = 23;
    iamf::ParameterSubblock loudness_sub;
    loudness_sub.momentary_loudness = 23;
    loudness.subblocks.push_back(loudness_sub);
    u0.parameter_blocks.push_back(loudness);

    iamf::ParameterBlock polar_block;
    polar_block.parameter_id = 30;
    iamf::ParameterSubblock polar_sub;
    polar_sub.animation_type = 3;  // INTER_LINEAR
    polar_sub.components = {{.end = -90}, {.end = 45}, {.end = 127}};
    polar_block.subblocks.push_back(polar_sub);
    u0.parameter_blocks.push_back(polar_block);

    iamf::ParameterBlock dual_block;
    dual_block.parameter_id = 31;
    iamf::ParameterSubblock dual_sub;
    dual_sub.animation_type = 4;  // INTER_BEZIER
    for (int i = 0; i < 6; ++i) {
        iamf::AnimatedValue v;
        v.end = 1000 * (i + 1) * (i % 2 == 0 ? 1 : -1);
        v.control = i * 7;
        v.control_relative_time = 200;
        dual_sub.components.push_back(v);
    }
    dual_block.subblocks.push_back(dual_sub);
    u0.parameter_blocks.push_back(dual_block);

    iamf::Metadata in_unit;
    in_unit.metadata_type = 1;
    in_unit.itu_t_t35_country_code = 0xB5;
    in_unit.itu_t_t35_payload = bytes_of({1, 2, 3});
    u0.metadata.push_back(in_unit);

    for (const std::uint32_t id : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 20U}) {
        iamf::AudioFrame frame;
        frame.audio_substream_id = id;
        frame.data = bytes_of({id, id + 1, id + 2});
        u0.audio_frames.push_back(frame);
    }
    iamf::AudioFrame trimmed;
    trimmed.audio_substream_id = 5;
    trimmed.has_trimming = true;
    trimmed.num_samples_to_trim_at_start = 0;
    trimmed.num_samples_to_trim_at_end = 700;
    trimmed.data = bytes_of({9});
    u0.audio_frames.push_back(trimmed);
    s.temporal_units.push_back(u0);

    iamf::TemporalUnit u1;
    u1.has_temporal_delimiter = true;
    u1.is_not_key_frame = true;
    iamf::AudioFrame f;
    f.audio_substream_id = 1;
    f.data = bytes_of({0xFE});
    u1.audio_frames.push_back(f);
    s.temporal_units.push_back(u1);
    return s;
}

}  // namespace

TEST_CASE("IAMF OBU headers match the syntax", "[iamf][obu]") {
    iamf::Sequence s = make_full_sequence();
    const auto descriptors = iamf::write_descriptors(s);
    REQUIRE(descriptors.has_value());
    // IA Sequence Header: type 31 -> 0xF8, size 6, "iamf", primary and additional profile 4.
    CHECK(to_ints(*descriptors).front() == 0xF8);
    CHECK(to_ints(*descriptors)[1] == 6);
    CHECK(to_ints(*descriptors)[2] == 'i');
    CHECK(to_ints(*descriptors)[6] == 4);
    CHECK(to_ints(*descriptors)[7] == 4);

    const auto redundant = iamf::write_descriptors(s, true);
    REQUIRE(redundant.has_value());
    CHECK(to_ints(*redundant).front() == 0xFC);  // obu_redundant_copy set

    SECTION("a Temporal Delimiter, key frame or not") {
        iamf::Sequence minimal = s;
        minimal.temporal_units.clear();
        iamf::TemporalUnit unit;
        unit.has_temporal_delimiter = true;
        auto key = iamf::write_temporal_unit(minimal, unit);
        REQUIRE(key.has_value());
        CHECK(to_ints(*key) == std::vector<unsigned>{0x20, 0x00});
        unit.is_not_key_frame = true;
        auto non_key = iamf::write_temporal_unit(minimal, unit);
        REQUIRE(non_key.has_value());
        CHECK(to_ints(*non_key) == std::vector<unsigned>{0x22, 0x00});
    }
    SECTION("an Audio Frame with trimming and an implicit substream id") {
        iamf::TemporalUnit unit;
        iamf::AudioFrame frame;
        frame.audio_substream_id = 0;
        frame.has_trimming = true;
        frame.num_samples_to_trim_at_end = 5;
        frame.num_samples_to_trim_at_start = 300;
        frame.data = bytes_of({0xD0, 0xD1, 0xD2});
        unit.audio_frames.push_back(frame);
        auto out = iamf::write_temporal_unit(s, unit);
        REQUIRE(out.has_value());
        // OBU_IA_Audio_Frame_ID0 = 6 -> 0x30 | trimming flag 0x02; obu_size counts the trim fields
        // (1 + 2 bytes) and the 3 byte payload; trim at end, then trim at start (300 = 0xAC 0x02).
        CHECK(to_ints(*out) == std::vector<unsigned>{0x32, 0x06, 0x05, 0xAC, 0x02, 0xD0, 0xD1, 0xD2});
    }
    SECTION("an Audio Frame beyond substream 17 carries its id explicitly") {
        iamf::TemporalUnit unit;
        iamf::AudioFrame frame;
        frame.audio_substream_id = 20;
        frame.data = bytes_of({0x77});
        unit.audio_frames.push_back(frame);
        auto out = iamf::write_temporal_unit(s, unit);
        REQUIRE(out.has_value());
        CHECK(to_ints(*out) == std::vector<unsigned>{0x28, 0x02, 0x14, 0x77});  // type 5, id 20, payload
    }
    SECTION("a Mix Presentation with optional fields sets its header flag") {
        // The first Mix Presentation has optional fields; the second does not.
        const auto& d = *descriptors;
        bool saw_flagged = false;
        bool saw_plain = false;
        std::size_t pos = 0;
        while (pos < d.size()) {
            const unsigned header = std::to_integer<unsigned>(d[pos]);
            std::size_t size = 0;
            std::size_t shift = 0;
            std::size_t at = pos + 1;
            while (true) {
                const unsigned byte = std::to_integer<unsigned>(d[at++]);
                size |= static_cast<std::size_t>(byte & 0x7F) << shift;
                shift += 7;
                if ((byte & 0x80) == 0) {
                    break;
                }
            }
            if ((header >> 3) == 2) {
                (header & 0x02) != 0 ? saw_flagged = true : saw_plain = true;
            }
            pos = at + size;
        }
        CHECK(saw_flagged);
        CHECK(saw_plain);
    }
}

TEST_CASE("IAMF position parameter data is bit packed across its fields", "[iamf][obu]") {
    iamf::Sequence s = make_full_sequence();
    iamf::TemporalUnit unit;
    iamf::ParameterBlock block;
    block.parameter_id = 30;
    iamf::ParameterSubblock sub;
    sub.animation_type = 3;  // INTER_LINEAR
    sub.components = {{.end = -90}, {.end = 45}, {.end = 127}};
    block.subblocks.push_back(sub);
    unit.parameter_blocks.push_back(block);
    auto out = iamf::write_temporal_unit(s, unit);
    REQUIRE(out.has_value());
    // Parameter Block OBU (type 3 -> 0x18): parameter_id 30, animation_type 3, then
    // azimuth -90 as 9 bits (110100110), elevation 45 as 8 bits (00101101) and distance 127 as 7
    // bits (1111111): 11010011 00010110 11111111.
    CHECK(to_ints(*out) == std::vector<unsigned>{0x18, 0x05, 0x1E, 0x03, 0xD3, 0x16, 0xFF});
}

TEST_CASE("IAMF Sequence with every structure survives write and read", "[iamf][obu]") {
    const iamf::Sequence original = make_full_sequence();
    auto bytes = iamf::write_sequence(original);
    REQUIRE(bytes.has_value());
    auto parsed = iamf::read_sequence(*bytes);
    REQUIRE(parsed.has_value());

    // Writing what was read gives the same bytes: nothing was lost or reinterpreted.
    auto again = iamf::write_sequence(*parsed);
    REQUIRE(again.has_value());
    CHECK(*again == *bytes);

    CHECK(parsed->header.primary_profile == 4);
    REQUIRE(parsed->codec_configs.size() == 2);
    CHECK(parsed->codec_configs[0].lpcm->sample_size == 24);
    CHECK(parsed->codec_configs[1].codec_id == "Opus");
    CHECK(parsed->codec_configs[1].audio_roll_distance == -4);
    REQUIRE(parsed->descriptor_metadata.size() == 2);
    CHECK(parsed->descriptor_metadata[0].tags[0].value == "iclforge test");
    CHECK(parsed->descriptor_metadata[1].itu_t_t35_country_code_extension == 0x12);

    REQUIRE(parsed->audio_elements.size() == 4);
    const auto& e0 = parsed->audio_elements[0];
    REQUIRE(e0.demixing.has_value());
    CHECK(e0.demixing->default_dmixp_mode == 2);
    CHECK(e0.demixing->default_w == 5);
    REQUIRE(e0.layers.size() == 2);
    CHECK(e0.layers[0].output_gain == -256);
    CHECK(e0.layers[0].output_gain_flags == 0x30);
    CHECK(e0.layers[1].recon_gain_is_present);
    const auto& e1 = parsed->audio_elements[1];
    CHECK(e1.type == iamf::ElementType::kSceneBased);
    CHECK(e1.ambisonics.channel_mapping == std::vector<std::uint8_t>{0, 1, 2, 3});
    const auto& e2 = parsed->audio_elements[2];
    CHECK(e2.type == iamf::ElementType::kObjectBased);
    CHECK(e2.objects.num_objects == 1);
    CHECK(e2.objects.extension_bytes == bytes_of({0xEE}));
    CHECK(parsed->audio_elements[3].objects.num_objects == 2);

    REQUIRE(parsed->mix_presentations.size() == 2);
    const auto& mix0 = parsed->mix_presentations[0];
    CHECK(mix0.annotations_language == std::vector<std::string>{"en-us", "fr-fr"});
    REQUIRE(mix0.sub_mixes.size() == 1);
    const auto& element0 = mix0.sub_mixes[0].elements[0];
    CHECK(element0.rendering.headphones_rendering_mode == 1);
    CHECK(element0.rendering.binaural_filter_profile == 2);
    REQUIRE(element0.rendering.element_gain_offset.has_value());
    CHECK(element0.rendering.element_gain_offset->min_offset == -512);
    CHECK(element0.element_mix_gain.definition.param_definition_mode == 1);
    CHECK(element0.element_mix_gain.default_mix_gain == -128);
    const auto& layouts = mix0.sub_mixes[0].layouts;
    REQUIRE(layouts.size() == 2);
    CHECK(layouts[0].loudness.true_peak == -300);
    REQUIRE(layouts[0].loudness.anchored.size() == 2);
    REQUIRE(layouts[0].loudness.momentary.has_value());
    CHECK(layouts[0].loudness.momentary->counts == std::vector<std::uint32_t>{1, 2, 3, 300});
    CHECK(layouts[0].loudness.loudness_range == 17);
    CHECK(layouts[0].loudness.info_type_bytes == bytes_of({0x99}));
    CHECK(layouts[1].layout.layout_type == 3);
    REQUIRE(mix0.tags.has_value());
    CHECK((*mix0.tags)[1].value == "main");
    REQUIRE(mix0.preferred_renderers.has_value());
    CHECK(mix0.optional_fields_remaining_bytes == bytes_of({0x01}));

    const auto& mix1 = parsed->mix_presentations[1];
    REQUIRE(mix1.sub_mixes[0].elements[0].rendering.position.has_value());
    CHECK(mix1.sub_mixes[0].elements[0].rendering.position->defaults == std::vector<std::int32_t>{-45, 30, 100});
    CHECK(mix1.sub_mixes[0].elements[0].rendering.extension_bytes == bytes_of({0x55}));
    CHECK(mix1.sub_mixes[0].elements[1].rendering.position->type == iamf::ParamType::kDualCart16);
    CHECK(mix1.sub_mixes[0].elements[1].rendering.position->defaults ==
          std::vector<std::int32_t>{-32767, 0, 32767, 100, -100, 5});
    CHECK_FALSE(mix1.tags.has_value());

    REQUIRE(parsed->temporal_units.size() == 2);
    const auto& u0 = parsed->temporal_units[0];
    CHECK(u0.has_temporal_delimiter);
    CHECK_FALSE(u0.is_not_key_frame);
    REQUIRE(u0.parameter_blocks.size() == 6);
    CHECK(u0.parameter_blocks[0].subblocks[0].dmixp_mode == 5);
    CHECK(u0.parameter_blocks[1].subblocks[0].recon_layers[1].recon_gains == std::vector<std::uint8_t>{200, 100, 50});
    const auto& gain_block = u0.parameter_blocks[2];
    CHECK(gain_block.duration == 960);
    REQUIRE(gain_block.subblocks.size() == 3);
    CHECK(gain_block.subblocks[1].subblock_duration == 400);
    CHECK(gain_block.subblocks[2].animation_type == 2);
    CHECK(gain_block.subblocks[2].components[0].control == 64);
    CHECK(gain_block.subblocks[2].components[0].control_relative_time == 128);
    CHECK(gain_block.subblocks[0].components[0].start == -256);
    CHECK(u0.parameter_blocks[3].subblocks[0].momentary_loudness == 23);
    const auto& polar = u0.parameter_blocks[4].subblocks[0];
    CHECK(polar.components[0].end == -90);
    CHECK(polar.components[1].end == 45);
    CHECK(polar.components[2].end == 127);
    const auto& dual = u0.parameter_blocks[5].subblocks[0];
    REQUIRE(dual.components.size() == 6);
    CHECK(dual.components[1].end == -2000);
    CHECK(dual.components[5].control == 35);
    REQUIRE(u0.metadata.size() == 1);
    CHECK(u0.metadata[0].itu_t_t35_payload == bytes_of({1, 2, 3}));
    REQUIRE(u0.audio_frames.size() == 11);
    CHECK(u0.audio_frames[9].audio_substream_id == 20);
    CHECK(u0.audio_frames[10].has_trimming);
    CHECK(u0.audio_frames[10].num_samples_to_trim_at_end == 700);
    CHECK(parsed->temporal_units[1].is_not_key_frame);
}

TEST_CASE("IAMF writer refuses what the syntax cannot carry", "[iamf][obu]") {
    iamf::Sequence good = make_full_sequence();
    REQUIRE(iamf::write_descriptors(good).has_value());

    SECTION("a codec_id that is not four characters") {
        iamf::Sequence s = good;
        s.codec_configs[0].codec_id = "pcm";
        auto out = iamf::write_descriptors(s);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iamf::Error::kInvalidArgument);
    }
    SECTION("an LPCM config outside the allowed sample sizes and rates") {
        iamf::Sequence s = good;
        s.codec_configs[0].lpcm->sample_size = 20;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
        s = good;
        s.codec_configs[0].lpcm->sample_rate = 44000;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("substream counts that disagree with the substream list") {
        iamf::Sequence s = good;
        s.audio_elements[0].layers[1].substream_count = 2;
        auto out = iamf::write_descriptors(s);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iamf::Error::kBadDescriptor);
    }
    SECTION("an element that names a codec config that does not exist") {
        iamf::Sequence s = good;
        s.audio_elements[1].codec_config_id = 9;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("a duplicate parameter_id") {
        iamf::Sequence s = good;
        s.mix_presentations[1].sub_mixes[0].output_mix_gain.definition.parameter_id = 24;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("a position definition on a channel-based element") {
        iamf::Sequence s = good;
        s.mix_presentations[0].sub_mixes[0].elements[0].rendering.position =
            s.mix_presentations[1].sub_mixes[0].elements[0].rendering.position;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("a single-object position type on a dual-object element") {
        iamf::Sequence s = good;
        s.mix_presentations[1].sub_mixes[0].elements[1].rendering.position->type = iamf::ParamType::kCart16;
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("an annotation count that does not match the labels") {
        iamf::Sequence s = good;
        s.mix_presentations[0].sub_mixes[0].elements[0].localized_element_annotations.pop_back();
        CHECK_FALSE(iamf::write_descriptors(s).has_value());
    }
    SECTION("a Parameter Block for an undefined parameter") {
        iamf::TemporalUnit unit;
        iamf::ParameterBlock block;
        block.parameter_id = 999;
        unit.parameter_blocks.push_back(block);
        auto out = iamf::write_temporal_unit(good, unit);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iamf::Error::kUnknownParameter);
    }
    SECTION("a Parameter Block with the wrong number of sub blocks") {
        iamf::TemporalUnit unit;
        iamf::ParameterBlock block;
        block.parameter_id = 10;  // demixing: one sub block per frame
        auto out = iamf::write_temporal_unit(good, unit);
        REQUIRE(out.has_value());
        unit.parameter_blocks.push_back(block);
        auto bad = iamf::write_temporal_unit(good, unit);
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error() == iamf::Error::kBadParameterBlock);
    }
}

TEST_CASE("IAMF reader ignores what the specification says to ignore", "[iamf][obu]") {
    const iamf::Sequence original = make_full_sequence();
    auto descriptors = iamf::write_descriptors(original);
    auto redundant = iamf::write_descriptors(original, true);
    auto unit = iamf::write_temporal_unit(original, original.temporal_units[1]);
    REQUIRE(descriptors.has_value());
    REQUIRE(redundant.has_value());
    REQUIRE(unit.has_value());

    SECTION("a Reserved OBU between descriptors and in IA Data") {
        Bytes stream = *descriptors;
        const Bytes reserved = bytes_of({26 << 3, 0x03, 1, 2, 3});
        // After the sequence header (8 bytes) and again after the descriptors.
        stream.insert(stream.begin() + 8, reserved.begin(), reserved.end());
        stream.insert(stream.end(), reserved.begin(), reserved.end());
        stream.insert(stream.end(), unit->begin(), unit->end());
        auto parsed = iamf::read_sequence(stream);
        REQUIRE(parsed.has_value());
        CHECK(parsed->audio_elements.size() == 4);
        CHECK(parsed->temporal_units.size() == 1);
    }
    SECTION("descriptors repeated as redundant copies mid-sequence") {
        Bytes stream = *descriptors;
        stream.insert(stream.end(), unit->begin(), unit->end());
        stream.insert(stream.end(), redundant->begin(), redundant->end());
        stream.insert(stream.end(), unit->begin(), unit->end());
        auto parsed = iamf::read_sequence(stream);
        REQUIRE(parsed.has_value());
        CHECK(parsed->codec_configs.size() == 2);
        CHECK(parsed->mix_presentations.size() == 2);
        CHECK(parsed->temporal_units.size() == 2);
    }
    SECTION("a second IA Sequence ends the first") {
        Bytes stream = *descriptors;
        stream.insert(stream.end(), unit->begin(), unit->end());
        stream.insert(stream.end(), descriptors->begin(), descriptors->end());
        stream.insert(stream.end(), unit->begin(), unit->end());
        auto parsed = iamf::read_sequence(stream);
        REQUIRE(parsed.has_value());
        CHECK(parsed->temporal_units.size() == 1);
    }
    SECTION("bytes past the syntax an OBU defines") {
        // Grow the Temporal Delimiter-less unit's first OBU: an Audio Frame is opaque anyway, so
        // instead grow the IA Sequence Header, whose syntax ends after additional_profile.
        Bytes stream = *descriptors;
        REQUIRE(to_ints(stream)[1] == 6);
        stream[1] = std::byte{8};
        stream.insert(stream.begin() + 8, {std::byte{0xCC}, std::byte{0xDD}});
        auto parsed = iamf::read_descriptors(stream);
        REQUIRE(parsed.has_value());
        CHECK(parsed->audio_elements.size() == 4);
    }
    SECTION("a Parameter Block of an unknown parameter_id is dropped") {
        Bytes stream = *descriptors;
        // Parameter Block OBU: parameter_id 77, which no definition declares.
        const Bytes block = bytes_of({3 << 3, 0x02, 77, 0});
        stream.insert(stream.end(), block.begin(), block.end());
        stream.insert(stream.end(), unit->begin(), unit->end());
        auto parsed = iamf::read_sequence(stream);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->temporal_units.size() == 1);
        CHECK(parsed->temporal_units[0].parameter_blocks.empty());
    }
}

TEST_CASE("IAMF read_sequence splits Temporal Units without delimiters", "[iamf][obu]") {
    iamf::Sequence s = make_full_sequence();
    for (auto& unit : s.temporal_units) {
        unit.has_temporal_delimiter = false;
    }
    // Make the two units look like a real stream: the same substreams in each.
    s.temporal_units[1].audio_frames.clear();
    for (const std::uint32_t id : {0U, 1U, 2U}) {
        iamf::AudioFrame frame;
        frame.audio_substream_id = id;
        frame.data = bytes_of({id});
        s.temporal_units[1].audio_frames.push_back(frame);
    }
    s.temporal_units[0].audio_frames.pop_back();  // drop the repeated substream 5
    auto bytes = iamf::write_sequence(s);
    REQUIRE(bytes.has_value());
    auto parsed = iamf::read_sequence(*bytes);
    REQUIRE(parsed.has_value());
    // The second unit starts where substream 0 repeats.
    REQUIRE(parsed->temporal_units.size() == 2);
    CHECK(parsed->temporal_units[0].audio_frames.size() == 10);
    CHECK(parsed->temporal_units[1].audio_frames.size() == 3);
}

TEST_CASE("IAMF reader reports a stream that is not an IA Sequence", "[iamf][obu]") {
    SECTION("empty input") {
        auto parsed = iamf::read_sequence({});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kNoSequenceHeader);
    }
    SECTION("an OBU that is not a sequence header first") {
        const Bytes bytes = bytes_of({0x20, 0x00});
        auto parsed = iamf::read_sequence(bytes);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kNoSequenceHeader);
    }
    SECTION("an ia_code that is not iamf") {
        const Bytes bytes = bytes_of({0xF8, 0x06, 'a', 'b', 'c', 'd', 0, 0});
        auto parsed = iamf::read_sequence(bytes);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kBadSequenceHeader);
    }
    SECTION("a leb128 value over 32 bits") {
        const Bytes bytes = bytes_of({0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F});
        auto parsed = iamf::read_sequence(bytes);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kBadLeb128);
    }
    SECTION("a leb128 value of nine bytes") {
        const Bytes bytes = bytes_of({0xF8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01});
        auto parsed = iamf::read_sequence(bytes);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kBadLeb128);
    }
}

TEST_CASE("IAMF reader never reads past its input", "[iamf][obu]") {
    const iamf::Sequence original = make_full_sequence();
    auto bytes = iamf::write_sequence(original);
    REQUIRE(bytes.has_value());

    // Every truncation is either an error or a shorter valid sequence, never a crash.
    for (std::size_t length = 0; length < bytes->size(); ++length) {
        const auto parsed = iamf::read_sequence(std::span<const std::byte>(bytes->data(), length));
        (void)parsed;
    }

    // And so is every single-byte corruption.
    std::uint32_t state = 7;
    for (int round = 0; round < 3000; ++round) {
        Bytes copy = *bytes;
        state = state * 1664525U + 1013904223U;
        copy[state % copy.size()] = static_cast<std::byte>(state >> 24);
        const auto parsed = iamf::read_sequence(copy);
        (void)parsed;
    }
    SUCCEED();
}

TEST_CASE("IAMF layout table matches the specification's substream orders", "[iamf][obu]") {
    struct Row {
        std::uint8_t layout;
        std::size_t substreams;
        std::uint8_t coupled;
        std::uint8_t channels;
    };
    // Mono, Stereo, 5.1, 5.1.2, 5.1.4, 7.1, 7.1.2, 7.1.4, 3.1.2, Binaural.
    const Row rows[] = {{0, 1, 0, 1}, {1, 1, 1, 2},  {2, 4, 2, 6},  {3, 5, 3, 8}, {4, 6, 4, 10},
                        {5, 5, 3, 8}, {6, 6, 4, 10}, {7, 7, 5, 12}, {8, 4, 2, 6}, {9, 1, 1, 2}};
    for (const Row& row : rows) {
        const auto info = iamf::layout_info(row.layout);
        REQUIRE(info.has_value());
        CHECK(info->substreams.size() == row.substreams);
        CHECK(info->coupled_substream_count == row.coupled);
        CHECK(info->channel_count == row.channels);
        // Coupled substreams come first.
        for (std::size_t i = 0; i < info->substreams.size(); ++i) {
            CHECK(info->substreams[i].second.empty() == (i >= row.coupled));
        }
    }
    CHECK_FALSE(iamf::layout_info(10).has_value());
    CHECK_FALSE(iamf::layout_info(15).has_value());
    CHECK(iamf::layout_info(7)->substreams[0].first == "L");
    CHECK(iamf::layout_info(7)->substreams[5].first == "C");
    CHECK(iamf::layout_info(7)->substreams[6].first == "LFE");
}

TEST_CASE("iclforge::iamf::parameter_type finds a definition's type by id", "[iamf][obu]") {
    const iamf::Sequence s = make_full_sequence();
    CHECK(iamf::parameter_type(s, 10) == iamf::ParamType::kDemixing);
    CHECK(iamf::parameter_type(s, 11) == iamf::ParamType::kReconGain);
    CHECK(iamf::parameter_type(s, 20) == iamf::ParamType::kMixGain);
    CHECK(iamf::parameter_type(s, 23) == iamf::ParamType::kMomentaryLoudness);
    CHECK(iamf::parameter_type(s, 30) == iamf::ParamType::kPolar);
    CHECK(iamf::parameter_type(s, 31) == iamf::ParamType::kDualCart16);
    CHECK_FALSE(iamf::parameter_type(s, 12345).has_value());
}
