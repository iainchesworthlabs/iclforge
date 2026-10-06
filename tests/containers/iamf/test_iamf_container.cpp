#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"

// ISO-BMFF tests for iclforge::iamf: a file written by write_isobmff() read back by read_isobmff(),
// trimming and the edit list, non-sync samples, and the fragmented writer, whose output is also
// walked box by box with a reader written here independently of src/iamf.

namespace iamf = iclforge::iamf;

namespace {

using iamf::Bytes;

[[nodiscard]] std::uint32_t u32_at(const Bytes& data, std::size_t pos) {
    return (std::to_integer<std::uint32_t>(data[pos]) << 24) | (std::to_integer<std::uint32_t>(data[pos + 1]) << 16) |
           (std::to_integer<std::uint32_t>(data[pos + 2]) << 8) | std::to_integer<std::uint32_t>(data[pos + 3]);
}

[[nodiscard]] std::string type_at(const Bytes& data, std::size_t pos) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(std::to_integer<char>(data[pos + i]));
    }
    return s;
}

struct Box {
    std::string type;
    std::size_t start = 0;
    std::size_t body = 0;
    std::size_t end = 0;
};

[[nodiscard]] std::vector<Box> boxes_in(const Bytes& data, std::size_t begin, std::size_t end) {
    std::vector<Box> out;
    std::size_t pos = begin;
    while (pos < end) {
        REQUIRE(end - pos >= 8);
        const std::size_t size = u32_at(data, pos);
        REQUIRE(size >= 8);
        REQUIRE(pos + size <= end);
        out.push_back({type_at(data, pos + 4), pos, pos + 8, pos + size});
        pos += size;
    }
    return out;
}

[[nodiscard]] const Box* find(const std::vector<Box>& boxes, const std::string& type) {
    for (const auto& b : boxes) {
        if (b.type == type) {
            return &b;
        }
    }
    return nullptr;
}

iamf::Frame make_frame(std::uint32_t samples, float base) {
    iamf::Frame frame;
    for (std::size_t c = 0; c < frame.channels.size(); ++c) {
        frame.channels[c].resize(samples);
        for (std::uint32_t n = 0; n < samples; ++n) {
            // A slow ramp that differs per channel, so a channel mix-up shows.
            frame.channels[c][n] = base + 0.01F * static_cast<float>(c) + 0.0001F * static_cast<float>(n % 100);
        }
    }
    return frame;
}

[[nodiscard]] std::vector<iamf::Frame> make_frames(std::size_t count, std::uint32_t samples) {
    std::vector<iamf::Frame> frames;
    for (std::size_t i = 0; i < count; ++i) {
        frames.push_back(make_frame(samples, -0.3F + 0.1F * static_cast<float>(i)));
    }
    return frames;
}

// 24-bit quantization the way mux() applies it.
[[nodiscard]] float quantized(float value) {
    const double full_scale = 8388607.0;
    const double scaled = std::round(static_cast<double>(value) * full_scale);
    return static_cast<float>(scaled / 8388608.0);
}

}  // namespace

TEST_CASE("IAMF file round trips through write_isobmff and read_isobmff", "[iamf][container]") {
    iamf::AudioTrack track;
    track.samples_per_frame = 480;
    track.stereo_loudness = {.integrated_loudness_lkfs = -23.0F, .digital_peak_dbfs = -1.0F};
    const auto frames = make_frames(5, 480);
    auto sequence = iamf::build_sequence(track, frames);
    REQUIRE(sequence.has_value());
    auto file = iamf::write_isobmff(*sequence);
    REQUIRE(file.has_value());

    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    CHECK(parsed->info.brands.front() == "iamf");
    CHECK(parsed->info.timescale == 48000);
    CHECK(parsed->info.duration == 5U * 480U);
    CHECK_FALSE(parsed->info.edit.has_value());
    CHECK_FALSE(parsed->info.fragmented);
    REQUIRE(parsed->sequence.temporal_units.size() == 5);

    // The Descriptors and the IA Data are those written.
    auto original_descriptors = iamf::write_descriptors(*sequence);
    auto read_descriptors = iamf::write_descriptors(parsed->sequence);
    REQUIRE(original_descriptors.has_value());
    REQUIRE(read_descriptors.has_value());
    CHECK(*original_descriptors == *read_descriptors);
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& want = sequence->temporal_units[i];
        const auto& got = parsed->sequence.temporal_units[i];
        CHECK_FALSE(got.has_temporal_delimiter);  // an IA Sample never holds one
        REQUIRE(got.audio_frames.size() == want.audio_frames.size());
        for (std::size_t f = 0; f < want.audio_frames.size(); ++f) {
            CHECK(got.audio_frames[f].audio_substream_id == want.audio_frames[f].audio_substream_id);
            CHECK(got.audio_frames[f].data == want.audio_frames[f].data);
        }
    }

    // The decoded audio is the input, to 24 bits.
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    CHECK(decoded->sample_rate == 48000);
    REQUIRE(decoded->channel_names.size() == 12);
    CHECK(decoded->channel_names[0] == "L");
    CHECK(decoded->channel_names[1] == "R");
    CHECK(decoded->channel_names[10] == "C");
    CHECK(decoded->channel_names[11] == "LFE");
    REQUIRE(decoded->channels[0].size() == 5U * 480U);
    // Substream order is L,R,Lss,Rss,... so channel 2 is Lss, Frame channel 3.
    CHECK(decoded->channels[0][0] == quantized(frames[0].channels[0][0]));
    CHECK(decoded->channels[1][7] == quantized(frames[0].channels[2][7]));
    CHECK(decoded->channels[2][0] == quantized(frames[0].channels[3][0]));
    CHECK(decoded->channels[10][480] == quantized(frames[1].channels[1][0]));  // C is Frame channel 1
    CHECK(decoded->channels[11][2399] == quantized(frames[4].channels[11][479]));
}

TEST_CASE("IAMF file layout: brands, sample entry, stts, stsz and chunk offsets", "[iamf][container]") {
    const iamf::AudioTrack track{.samples_per_frame = 256};
    const auto frames = make_frames(3, 256);
    auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());

    const auto top = boxes_in(*file, 0, file->size());
    REQUIRE(top.size() == 3);
    CHECK(top[0].type == "ftyp");
    CHECK(top[1].type == "moov");
    CHECK(top[2].type == "mdat");
    CHECK(type_at(*file, top[0].body) == "iamf");  // major brand
    const auto moov = boxes_in(*file, top[1].body, top[1].end);
    const Box* trak = find(moov, "trak");
    REQUIRE(trak != nullptr);
    const auto track_boxes = boxes_in(*file, trak->body, trak->end);
    CHECK(find(track_boxes, "edts") == nullptr);  // nothing trimmed
    const Box* mdia = find(track_boxes, "mdia");
    REQUIRE(mdia != nullptr);
    const auto mdia_boxes = boxes_in(*file, mdia->body, mdia->end);
    const Box* minf = find(mdia_boxes, "minf");
    REQUIRE(minf != nullptr);
    const auto minf_boxes = boxes_in(*file, minf->body, minf->end);
    const Box* stbl = find(minf_boxes, "stbl");
    REQUIRE(stbl != nullptr);
    const auto stbl_boxes = boxes_in(*file, stbl->body, stbl->end);

    // stts: one run, 3 samples of 256.
    const Box* stts = find(stbl_boxes, "stts");
    REQUIRE(stts != nullptr);
    CHECK(u32_at(*file, stts->body + 4) == 1);
    CHECK(u32_at(*file, stts->body + 8) == 3);
    CHECK(u32_at(*file, stts->body + 12) == 256);
    CHECK(find(stbl_boxes, "stss") == nullptr);  // every sample is a sync sample

    // stco/stsz index mdat exactly.
    const Box* stsz = find(stbl_boxes, "stsz");
    const Box* stco = find(stbl_boxes, "stco");
    REQUIRE(stsz != nullptr);
    REQUIRE(stco != nullptr);
    std::size_t cursor = top[2].body;
    for (std::uint32_t i = 0; i < 3; ++i) {
        CHECK(u32_at(*file, stco->body + 8 + i * 4) == cursor);
        cursor += u32_at(*file, stsz->body + 12 + i * 4);
    }
    CHECK(cursor == top[2].end);
}

TEST_CASE("IAMF trimming becomes Audio Frame fields, shorter samples and an edit list", "[iamf][container]") {
    iamf::AudioTrack track;
    track.samples_per_frame = 1000;
    track.trim_start_samples = 1500;  // the first frame entirely, half of the second
    track.trim_end_samples = 100;
    const auto frames = make_frames(4, 1000);
    auto sequence = iamf::build_sequence(track, frames);
    REQUIRE(sequence.has_value());

    // Frames trimmed fully, partially, not at all, and at the end.
    const auto& units = sequence->temporal_units;
    CHECK(units[0].audio_frames[0].num_samples_to_trim_at_start == 1000);
    CHECK(units[1].audio_frames[0].num_samples_to_trim_at_start == 500);
    CHECK_FALSE(units[2].audio_frames[0].has_trimming);
    CHECK(units[3].audio_frames[0].num_samples_to_trim_at_end == 100);

    auto file = iamf::write_isobmff(*sequence);
    REQUIRE(file.has_value());
    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    // The last sample's duration leaves out the samples trimmed from its end.
    CHECK(parsed->info.sample_durations == std::vector<std::uint32_t>{1000, 1000, 1000, 900});
    REQUIRE(parsed->info.edit.has_value());
    CHECK(parsed->info.edit->media_time == 1500);
    CHECK(parsed->info.edit->segment_duration == 3900 - 1500);

    // The Audio Frame OBUs kept their trimming.
    CHECK(parsed->sequence.temporal_units[1].audio_frames[3].num_samples_to_trim_at_start == 500);
    CHECK(parsed->sequence.temporal_units[3].audio_frames[0].has_trimming);

    // And the decoded audio is what remains: 4000 - 1500 - 100 samples.
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels[0].size() == 2400);
    CHECK(decoded->channels[0][0] == quantized(frames[1].channels[0][500]));
    CHECK(decoded->channels[0][2399] == quantized(frames[3].channels[0][899]));
}

TEST_CASE("IAMF trimming that the Audio Frame rules forbid is refused", "[iamf][container]") {
    const auto frames = make_frames(2, 1000);
    iamf::AudioTrack track;
    track.samples_per_frame = 1000;

    track.trim_end_samples = 1001;  // more than a frame at the end
    auto too_much = iamf::build_sequence(track, frames);
    REQUIRE_FALSE(too_much.has_value());
    CHECK(too_much.error() == iamf::MuxError::kInvalidTrim);

    track.trim_end_samples = 0;
    track.trim_start_samples = 2001;  // more than the audio
    CHECK_FALSE(iamf::build_sequence(track, frames).has_value());

    // Within the last frame the two trims must not overlap.
    track.trim_start_samples = 1600;
    track.trim_end_samples = 500;
    auto overlapping = iamf::build_sequence(track, frames);
    REQUIRE_FALSE(overlapping.has_value());
    CHECK(overlapping.error() == iamf::MuxError::kInvalidTrim);

    track.trim_end_samples = 400;  // 600 + 400 fills the last frame exactly
    CHECK(iamf::build_sequence(track, frames).has_value());
}

TEST_CASE("IAMF Temporal Units that are not key frames become non-sync samples", "[iamf][container]") {
    const iamf::AudioTrack track{.samples_per_frame = 256};
    const auto frames = make_frames(4, 256);
    auto sequence = iamf::build_sequence(track, frames);
    REQUIRE(sequence.has_value());
    sequence->temporal_units[1].is_not_key_frame = true;
    sequence->temporal_units[3].is_not_key_frame = true;

    auto file = iamf::write_isobmff(*sequence);
    REQUIRE(file.has_value());
    auto parsed = iamf::read_isobmff(*file);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->sequence.temporal_units[0].is_not_key_frame);
    CHECK(parsed->sequence.temporal_units[1].is_not_key_frame);
    CHECK_FALSE(parsed->sequence.temporal_units[2].is_not_key_frame);
    CHECK(parsed->sequence.temporal_units[3].is_not_key_frame);
}

TEST_CASE("IAMF raw OBU stream carries Temporal Delimiters and reads back", "[iamf][container]") {
    iamf::AudioTrack track;
    track.samples_per_frame = 256;
    track.temporal_delimiters = true;
    const auto frames = make_frames(3, 256);
    auto sequence = iamf::build_sequence(track, frames);
    REQUIRE(sequence.has_value());
    auto stream = iamf::write_sequence(*sequence);
    REQUIRE(stream.has_value());
    auto parsed = iamf::read_sequence(*stream);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->temporal_units.size() == 3);
    for (const auto& unit : parsed->temporal_units) {
        CHECK(unit.has_temporal_delimiter);
        CHECK(unit.audio_frames.size() == 7);
    }
    auto decoded = iamf::decode_pcm(*parsed, 0);
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels[0].size() == 768);
}

TEST_CASE("IAMF FragmentedWriter writes an initialization segment and movie fragments", "[iamf][container]") {
    iamf::AudioTrack track;
    track.samples_per_frame = 480;
    const auto frames = make_frames(6, 480);
    auto sequence = iamf::build_sequence(track, frames);
    REQUIRE(sequence.has_value());
    sequence->temporal_units[4].is_not_key_frame = true;

    auto writer = iamf::FragmentedWriter::create(*sequence);
    REQUIRE(writer.has_value());
    const Bytes& init = writer->initialization_segment();

    // ftyp, then a moov with an empty sample table and an mvex.
    const auto init_top = boxes_in(init, 0, init.size());
    REQUIRE(init_top.size() == 2);
    CHECK(init_top[0].type == "ftyp");
    CHECK(init_top[1].type == "moov");
    const auto moov = boxes_in(init, init_top[1].body, init_top[1].end);
    CHECK(find(moov, "mvex") != nullptr);

    const std::span<const iamf::TemporalUnit> all(sequence->temporal_units);
    auto first = writer->fragment(all.subspan(0, 2));
    auto second = writer->fragment(all.subspan(2, 1));
    auto third = writer->fragment(all.subspan(3, 3));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    CHECK(writer->fragments_written() == 3);
    CHECK(writer->next_decode_time() == 6U * 480U);

    // Walk the first fragment's boxes: moof (mfhd, traf (tfhd, tfdt, trun)) then mdat.
    const auto top = boxes_in(*first, 0, first->size());
    REQUIRE(top.size() == 2);
    CHECK(top[0].type == "moof");
    CHECK(top[1].type == "mdat");
    const auto moof = boxes_in(*first, top[0].body, top[0].end);
    const Box* mfhd = find(moof, "mfhd");
    const Box* traf = find(moof, "traf");
    REQUIRE(mfhd != nullptr);
    REQUIRE(traf != nullptr);
    CHECK(u32_at(*first, mfhd->body + 4) == 1);  // sequence_number
    const auto traf_boxes = boxes_in(*first, traf->body, traf->end);
    const Box* tfdt = find(traf_boxes, "tfdt");
    const Box* trun = find(traf_boxes, "trun");
    REQUIRE(tfdt != nullptr);
    REQUIRE(trun != nullptr);
    CHECK(u32_at(*first, tfdt->body + 4) == 0);  // baseMediaDecodeTime (64 bits), high word
    CHECK(u32_at(*first, tfdt->body + 8) == 0);
    CHECK(u32_at(*first, trun->body + 4) == 2);  // sample_count
    // data_offset counts from the first byte of moof (default-base-is-moof) to the sample data.
    CHECK(u32_at(*first, trun->body + 8) == top[1].body);
    CHECK(u32_at(*first, trun->body + 12) == 480);  // first sample's duration
    const std::uint32_t first_size = u32_at(*first, trun->body + 16);
    CHECK(top[1].end - top[1].body > first_size);

    // The second fragment continues the numbering and the clock.
    const auto second_top = boxes_in(*second, 0, second->size());
    const auto second_moof = boxes_in(*second, second_top[0].body, second_top[0].end);
    const auto* second_mfhd = find(second_moof, "mfhd");
    const auto* second_traf_box = find(second_moof, "traf");
    REQUIRE(second_mfhd != nullptr);
    REQUIRE(second_traf_box != nullptr);
    CHECK(u32_at(*second, second_mfhd->body + 4) == 2);
    const auto second_traf = boxes_in(*second, second_traf_box->body, second_traf_box->end);
    const auto* second_tfdt = find(second_traf, "tfdt");
    REQUIRE(second_tfdt != nullptr);
    CHECK(u32_at(*second, second_tfdt->body + 8) == 960);

    // Concatenated, the pieces are one file that reads like the batch file.
    Bytes fragmented = init;
    fragmented.insert(fragmented.end(), first->begin(), first->end());
    fragmented.insert(fragmented.end(), second->begin(), second->end());
    fragmented.insert(fragmented.end(), third->begin(), third->end());
    auto parsed = iamf::read_isobmff(fragmented);
    REQUIRE(parsed.has_value());
    CHECK(parsed->info.fragmented);
    REQUIRE(parsed->sequence.temporal_units.size() == 6);
    CHECK(parsed->info.duration == 6U * 480U);
    CHECK(parsed->sequence.temporal_units[4].is_not_key_frame);
    CHECK_FALSE(parsed->sequence.temporal_units[3].is_not_key_frame);
    for (std::size_t i = 0; i < 6; ++i) {
        for (std::size_t f = 0; f < 7; ++f) {
            CHECK(parsed->sequence.temporal_units[i].audio_frames[f].data ==
                  sequence->temporal_units[i].audio_frames[f].data);
        }
    }
    auto decoded = iamf::decode_pcm(parsed->sequence, 0);
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels[0].size() == 6U * 480U);
    CHECK(decoded->channels[0][5 * 480 + 3] == quantized(frames[5].channels[0][3]));

    auto empty = writer->fragment({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == iamf::Error::kInvalidArgument);
}

TEST_CASE("IAMF reader rejects what is not an IAMF file", "[iamf][container]") {
    const iamf::AudioTrack track{.samples_per_frame = 128};
    const auto frames = make_frames(2, 128);
    auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());

    SECTION("empty and non-ISO-BMFF input") {
        CHECK_FALSE(iamf::read_isobmff({}).has_value());
        const Bytes junk(64, std::byte{0x41});
        CHECK_FALSE(iamf::read_isobmff(junk).has_value());
    }
    SECTION("a file without the iamf brand") {
        Bytes copy = *file;
        // ftyp: size, 'ftyp', major 'iamf', minor, then compatible brands 'iamf' 'iso6'.
        REQUIRE(type_at(copy, 8) == "iamf");
        copy[8] = std::byte{'m'};
        copy[9] = std::byte{'p'};
        copy[10] = std::byte{'4'};
        copy[11] = std::byte{'2'};
        copy[16] = std::byte{'m'};
        copy[17] = std::byte{'p'};
        copy[18] = std::byte{'4'};
        copy[19] = std::byte{'1'};
        auto parsed = iamf::read_isobmff(copy);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == iamf::Error::kNotIamf);
    }
    SECTION("every truncation, and a thousand corruptions, are handled") {
        for (std::size_t length = 0; length < file->size(); length += 7) {
            (void)iamf::read_isobmff(std::span<const std::byte>(file->data(), length));
        }
        std::uint32_t state = 99;
        for (int round = 0; round < 1500; ++round) {
            Bytes copy = *file;
            state = state * 1664525U + 1013904223U;
            copy[state % copy.size()] = static_cast<std::byte>(state >> 24);
            (void)iamf::read_isobmff(copy);
        }
        SUCCEED();
    }
}
