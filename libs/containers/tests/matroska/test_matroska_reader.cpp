#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/matroska/reader.hpp"

// The reader's tests come in two halves, for two different questions.
//
// Round-trip (against iclforge::containers::matroska::mux/Writer) answers "does the reader
// understand what this project writes". That is the cheap half, and on its
// own it proves very little: a reader and a writer that share a
// misunderstanding round-trip perfectly.
//
// So the rest build Matroska by hand, byte by byte, out of shapes the
// writer never emits - all three lacing forms, BlockGroup-wrapped Blocks,
// several tracks, unknown-size clusters, and a pile of malformed layouts.
// Those are what a file from a disc rip or another muxer actually looks
// like, and they are the half that can fail.

namespace {

using Bytes = std::vector<std::byte>;

// --- a hand EBML writer, independent of libs/containers/src/matroska -------------------
// Deliberately NOT matroska.cpp's put_* helpers: a test that builds its
// input with the same code the reader was written against tests the pair
// against itself. These are transcribed from the EBML element layout.

void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_id(Bytes& out, std::uint32_t id) {
    const int width = id <= 0xFF ? 1 : id <= 0xFFFF ? 2 : id <= 0xFF'FFFF ? 3 : 4;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(out, static_cast<std::uint8_t>(id >> (8 * i)));
    }
}

// A size vint at an explicit width, so a test can pin the narrow forms the
// writer never chooses.
void put_vint_width(Bytes& out, std::uint64_t value, int width) {
    const auto marker = static_cast<std::uint64_t>(1) << (7 * width);
    const std::uint64_t encoded = value | marker;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(out, static_cast<std::uint8_t>(encoded >> (8 * i)));
    }
}

void put_vint(Bytes& out, std::uint64_t value) {
    int width = 1;
    while (width < 8 && value >= (1ULL << (7 * width)) - 1) {
        ++width;
    }
    put_vint_width(out, value, width);
}

void put_bytes(Bytes& out, std::span<const std::byte> in) {
    out.insert(out.end(), in.begin(), in.end());
}

void put_element(Bytes& out, std::uint32_t id, std::span<const std::byte> body) {
    put_id(out, id);
    put_vint(out, body.size());
    put_bytes(out, body);
}

void put_uint_element(Bytes& out, std::uint32_t id, std::uint64_t value) {
    int width = 1;
    while (width < 8 && (value >> (8 * width)) != 0) {
        ++width;
    }
    Bytes body;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(value >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_f64_element(Bytes& out, std::uint32_t id, double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    Bytes body;
    for (int i = 7; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(bits >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_f32_element(Bytes& out, std::uint32_t id, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    Bytes body;
    for (int i = 3; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(bits >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_string_element(Bytes& out, std::uint32_t id, std::string_view value) {
    Bytes body;
    for (const char c : value) {
        put_u8(body, static_cast<std::uint8_t>(c));
    }
    put_element(out, id, body);
}

constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;
constexpr std::uint32_t kDocType = 0x4282;
constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kTrackNumber = 0xD7;
constexpr std::uint32_t kTrackType = 0x83;
constexpr std::uint32_t kCodecId = 0x86;
constexpr std::uint32_t kLanguage = 0x22B59C;
constexpr std::uint32_t kAudio = 0xE1;
constexpr std::uint32_t kSamplingFrequency = 0xB5;
constexpr std::uint32_t kChannels = 0x9F;
constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kClusterTimestamp = 0xE7;
constexpr std::uint32_t kSimpleBlock = 0xA3;
constexpr std::uint32_t kBlockGroup = 0xA0;
constexpr std::uint32_t kBlock = 0xA1;
constexpr std::uint32_t kCues = 0x1C53BB6B;
constexpr std::uint32_t kAttachments = 0x1941A469;

Bytes ebml_header(std::string_view doc_type = "matroska") {
    Bytes body;
    put_string_element(body, kDocType, doc_type);
    Bytes out;
    put_element(out, kEbmlHeader, body);
    return out;
}

Bytes frame_of(std::size_t size, std::uint8_t fill);

struct TrackSpec {
    std::uint64_t number = 1;
    std::uint64_t type = 2;  // audio
    std::string codec_id{"A_EAC3"};
    double sample_rate = 48000.0;
    std::uint64_t channels = 6;
    std::string language{"eng"};
    bool float32_rate = false;
    // A ContentEncodings element: with a ContentEncryption (AES, a key ID) when `encrypted`, with
    // a ContentCompression (header stripping) alone when `compressed`.
    bool encrypted = false;
    bool compressed = false;
};

Bytes track_entry(const TrackSpec& spec) {
    Bytes audio;
    if (spec.float32_rate) {
        put_f32_element(audio, kSamplingFrequency, static_cast<float>(spec.sample_rate));
    } else {
        put_f64_element(audio, kSamplingFrequency, spec.sample_rate);
    }
    put_uint_element(audio, kChannels, spec.channels);

    Bytes entry;
    put_uint_element(entry, kTrackNumber, spec.number);
    put_uint_element(entry, kTrackType, spec.type);
    put_string_element(entry, kCodecId, spec.codec_id);
    put_string_element(entry, kLanguage, spec.language);
    put_element(entry, kAudio, audio);
    if (spec.encrypted || spec.compressed) {
        // ContentEncodings > ContentEncoding > ContentEncodingOrder, Scope, Type and then the
        // ContentEncryption (ContentEncAlgo 5 = AES, ContentEncKeyID) or ContentCompression
        // (ContentCompAlgo 3 = header stripping). Ids as the Matroska specification has them.
        Bytes encoding;
        put_uint_element(encoding, 0x5031, 0);
        put_uint_element(encoding, 0x5032, 1);
        if (spec.encrypted) {
            put_uint_element(encoding, 0x5033, 1);
            Bytes encryption;
            put_uint_element(encryption, 0x47E1, 5);
            put_element(encryption, 0x47E2, frame_of(16, 0x6B));
            put_element(encoding, 0x5035, encryption);
        } else {
            put_uint_element(encoding, 0x5033, 0);
            Bytes compression;
            put_uint_element(compression, 0x4254, 3);
            put_element(encoding, 0x5034, compression);
        }
        Bytes encodings;
        put_element(encodings, 0x6240, encoding);
        put_element(entry, 0x6D80, encodings);
    }

    Bytes out;
    put_element(out, kTrackEntry, entry);
    return out;
}

// A SimpleBlock/Block payload: track vint, int16 relative timestamp, flags,
// then whatever the caller laced together.
Bytes block_payload(std::uint64_t track, std::int16_t relative, std::uint8_t flags,
                    std::span<const std::byte> laced) {
    Bytes out;
    put_vint(out, track);
    put_u8(out, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) >> 8));
    put_u8(out, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) & 0xFF));
    put_u8(out, flags);
    put_bytes(out, laced);
    return out;
}

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

std::vector<std::span<const std::byte>> views_of(const std::vector<Bytes>& frames) {
    return {frames.begin(), frames.end()};
}

// Collects a demux/Reader result into owned bytes so a comparison survives
// the buffer going away.
std::vector<Bytes> owned(std::span<const std::span<const std::byte>> frames) {
    std::vector<Bytes> out;
    out.reserve(frames.size());
    for (const auto& f : frames) {
        out.emplace_back(f.begin(), f.end());
    }
    return out;
}

// Drives iclforge::containers::matroska::Reader over `file` in fixed-size chunks, returning the
// frames it produced. Chunk sizes that do not divide any element cleanly are
// the point: an element must survive being split across pushes.
std::vector<Bytes> read_in_chunks(std::span<const std::byte> file, std::size_t chunk,
                                  const iclforge::containers::matroska::ReadOptions& options = {}) {
    iclforge::containers::matroska::Reader reader{options};
    std::vector<Bytes> got;
    const auto sink = [&got](std::span<const std::byte> frame) {
        got.emplace_back(frame.begin(), frame.end());
    };
    for (std::size_t offset = 0; offset < file.size(); offset += chunk) {
        const auto take = std::min(chunk, file.size() - offset);
        REQUIRE(reader.push(file.subspan(offset, take), sink).has_value());
    }
    REQUIRE(reader.finish().has_value());
    return got;
}

}  // namespace

TEST_CASE("Matroska round-trips mux()'s frames back byte-for-byte", "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(512, 0x22),
                                    frame_of(1024, 0x33), frame_of(64, 0x44)};
    const iclforge::containers::matroska::AudioTrack track{
        .codec_id = std::string{iclforge::containers::matroska::kCodecEac3},
        .sample_rate = 48000,
        .channels = 6,
        .samples_per_frame = 1536,
        .language = "eng"};
    const auto file = iclforge::containers::matroska::mux(track, views_of(frames));
    REQUIRE(file.has_value());

    const auto out = iclforge::containers::matroska::demux(*file);
    REQUIRE(out.has_value());
    CHECK(out->track.codec_id == "A_EAC3");
    CHECK(out->track.sample_rate == 48000);
    CHECK(out->track.channels == 6);
    CHECK(out->track.language == "eng");
    CHECK(out->track.track_number == 1);
    CHECK(owned(out->frames) == frames);
}

TEST_CASE("Matroska reads back a Writer's unknown-size Segment", "[matroska][reader][writer]") {
    // The streaming writer leaves Segment open-ended (EBML's reserved
    // all-ones size) and omits Duration - the one shape mux() never
    // produces, and the one a live recording always has.
    const std::vector<Bytes> frames{frame_of(300, 0xA0), frame_of(300, 0xA1),
                                    frame_of(300, 0xA2), frame_of(300, 0xA3),
                                    frame_of(300, 0xA4)};
    auto writer = iclforge::containers::matroska::Writer::create(
        iclforge::containers::matroska::AudioTrack{
            .codec_id = std::string{iclforge::containers::matroska::kCodecAc3},
            .sample_rate = 44100,
            .channels = 2,
            .samples_per_frame = 1536,
            .language = "und"},
        iclforge::containers::matroska::MuxOptions{.cluster_ms = 100, .writing_app = "iclforge"});
    REQUIRE(writer.has_value());

    Bytes file = writer->header();
    for (const auto& frame : frames) {
        auto closed = writer->push(frame);
        REQUIRE(closed.has_value());
        file.insert(file.end(), closed->begin(), closed->end());
    }
    const auto tail = writer->finalize();
    file.insert(file.end(), tail.begin(), tail.end());

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(out->track.codec_id == "A_AC3");
    CHECK(out->track.sample_rate == 44100);
    CHECK(out->track.channels == 2);
    CHECK(owned(out->frames) == frames);
}

TEST_CASE("Matroska Reader over arbitrary chunk boundaries matches demux()",
          "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(3, 0x22), frame_of(1500, 0x33),
                                    frame_of(64, 0x44), frame_of(900, 0x55)};
    const auto file = iclforge::containers::matroska::mux(
        iclforge::containers::matroska::AudioTrack{
            .codec_id = std::string{iclforge::containers::matroska::kCodecEac3},
            .sample_rate = 48000,
            .channels = 6,
            .samples_per_frame = 1536,
            .language = "und"},
        views_of(frames),
        iclforge::containers::matroska::MuxOptions{.cluster_ms = 50, .writing_app = "iclforge"});
    REQUIRE(file.has_value());

    // 1 byte at a time splits every id, every size vint and every frame;
    // the primes split them in different places again.
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                    std::size_t{64}, std::size_t{997}, file->size()}) {
        INFO("chunk size " << chunk);
        CHECK(read_in_chunks(*file, chunk) == frames);
    }
}

TEST_CASE("Matroska Reader reports the track and frame count it read", "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(100, 1), frame_of(100, 2), frame_of(100, 3)};
    const auto file = iclforge::containers::matroska::mux(
        iclforge::containers::matroska::AudioTrack{
            .codec_id = std::string{iclforge::containers::matroska::kCodecEac3},
            .sample_rate = 48000,
            .channels = 2,
            .samples_per_frame = 1536,
            .language = "und"},
        views_of(frames));
    REQUIRE(file.has_value());

    iclforge::containers::matroska::Reader reader{};
    CHECK_FALSE(reader.track_found());
    const auto sink = [](std::span<const std::byte>) {};
    REQUIRE(reader.push(*file, sink).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.track_found());
    CHECK(reader.track().codec_id == "A_EAC3");
    CHECK(reader.frames_read() == frames.size());
}

TEST_CASE("Matroska reads every lacing form", "[matroska][reader][lacing]") {
    // iclforge::containers::matroska::mux never laces. Every one of these blocks is hand-built,
    // because a file from another muxer is where lacing actually comes from.
    const Bytes a = frame_of(5, 0xA1);
    const Bytes b = frame_of(9, 0xB2);
    const Bytes c = frame_of(4, 0xC3);

    Bytes clusters;

    SECTION("Xiph lacing") {
        Bytes laced;
        put_u8(laced, 2);  // frame count - 1
        put_u8(laced, 5);  // size of a
        put_u8(laced, 9);  // size of b; c takes the remainder
        put_bytes(laced, a);
        put_bytes(laced, b);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        put_element(clusters, kCluster, cluster);
    }

    SECTION("Xiph lacing with a size past 255") {
        const Bytes big = frame_of(300, 0xD4);
        Bytes laced;
        put_u8(laced, 1);    // two frames
        put_u8(laced, 255);  // 300 = 255 + 45, the 0xFF-run encoding
        put_u8(laced, 45);
        put_bytes(laced, big);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        put_element(clusters, kCluster, cluster);

        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_bytes(segment, clusters);
        put_element(file, kSegment, segment);

        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(owned(out->frames) == std::vector<Bytes>{big, c});
        return;
    }

    SECTION("EBML lacing") {
        Bytes laced;
        put_u8(laced, 2);   // three frames
        put_vint(laced, 5); // size of a
        // Signed delta to b's size: 9 - 5 = +4, biased by 2^6 - 1 at width 1.
        put_vint_width(laced, static_cast<std::uint64_t>(4 + 63), 1);
        put_bytes(laced, a);
        put_bytes(laced, b);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x06, laced));
        put_element(clusters, kCluster, cluster);
    }

    SECTION("fixed-size lacing") {
        Bytes laced;
        put_u8(laced, 2);  // three frames, all 5 bytes
        put_bytes(laced, a);
        put_bytes(laced, frame_of(5, 0xB2));
        put_bytes(laced, frame_of(5, 0xC3));

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x04, laced));
        put_element(clusters, kCluster, cluster);

        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_bytes(segment, clusters);
        put_element(file, kSegment, segment);

        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(owned(out->frames) ==
                std::vector<Bytes>{a, frame_of(5, 0xB2), frame_of(5, 0xC3)});
        return;
    }

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_bytes(segment, clusters);
    put_element(file, kSegment, segment);

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(owned(out->frames) == std::vector<Bytes>{a, b, c});
}

TEST_CASE("Matroska reads a BlockGroup-wrapped Block", "[matroska][reader]") {
    // The writer only emits SimpleBlock; a muxer that wants to carry a
    // BlockDuration has to use the BlockGroup form instead, and plenty do.
    const Bytes frame = frame_of(256, 0x5A);

    Bytes group;
    put_element(group, kBlock, block_payload(1, 0, 0x00, frame));
    put_uint_element(group, 0x9B, 32);  // BlockDuration, an element to skip past

    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kBlockGroup, group);

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    REQUIRE(out->frames.size() == 1);
    CHECK(owned(out->frames)[0] == frame);
}

TEST_CASE("Matroska track selection picks the audio AC-3 track", "[matroska][reader]") {
    // Track 1 is video, track 2 is a subtitle codec on an audio track type,
    // track 3 is the E-AC-3 one. Only the last is auto-selectable, and its
    // blocks are the only ones that come out.
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{.number = 1, .type = 1, .codec_id = "V_MPEG4/ISO/AVC"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 2, .type = 2, .codec_id = "A_AAC"}));
    put_bytes(tracks,
              track_entry(TrackSpec{.number = 3, .type = 2, .codec_id = "A_EAC3", .channels = 8}));

    const Bytes wanted = frame_of(200, 0x33);
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(50, 0x11)));
    put_element(cluster, kSimpleBlock, block_payload(2, 0, 0x80, frame_of(50, 0x22)));
    put_element(cluster, kSimpleBlock, block_payload(3, 0, 0x80, wanted));

    Bytes file = ebml_header();
    Bytes segment;
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    SECTION("auto-selection takes the E-AC-3 track and nothing else") {
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 3);
        CHECK(out->track.channels == 8);
        REQUIRE(out->frames.size() == 1);
        CHECK(owned(out->frames)[0] == wanted);
    }

    SECTION("an explicit track number overrides the codec filter") {
        const auto out = iclforge::containers::matroska::demux(
            file, iclforge::containers::matroska::ReadOptions{.track_number = 2});
        REQUIRE(out.has_value());
        CHECK(out->track.codec_id == "A_AAC");
        REQUIRE(out->frames.size() == 1);
        CHECK(owned(out->frames)[0] == frame_of(50, 0x22));
    }

    SECTION("a track number nothing matches is kNoAudioTrack") {
        const auto out = iclforge::containers::matroska::demux(
            file, iclforge::containers::matroska::ReadOptions{.track_number = 99});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("Matroska lists the tracks it does not read, by type, codec and language",
          "[matroska][reader][skipped]") {
    // What a karaoke video rip holds beside its Dolby audio: the picture, a second audio track, a
    // text subtitle track, a bitmap subtitle track and a track of a type this reader has no
    // name for.
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{.number = 1, .type = 1, .codec_id = "V_MPEG4/ISO/AVC"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 2, .type = 2, .codec_id = "A_AAC"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 3, .type = 2, .codec_id = "A_EAC3"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 4, .type = 17, .codec_id = "S_TEXT/UTF8",
                                            .language = "fre"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 5, .type = 17, .codec_id = "S_VOBSUB",
                                            .language = "jpn"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 6, .type = 33, .codec_id = "D_WEBVTT/METADATA"}));

    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(3, 0, 0x80, frame_of(40, 0x33)));
    Bytes file = ebml_header();
    Bytes segment;
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    using iclforge::containers::matroska::TrackKind;

    SECTION("the selected track is read and is not listed; every other track is, in file order") {
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 3);
        REQUIRE(out->skipped.size() == 5);
        const auto& skipped = out->skipped;
        CHECK(skipped[0].track_number == 1);
        CHECK(skipped[0].kind == TrackKind::kVideo);
        CHECK(skipped[0].codec_id == "V_MPEG4/ISO/AVC");
        CHECK(skipped[1].track_number == 2);
        CHECK(skipped[1].kind == TrackKind::kAudio);
        CHECK(skipped[1].codec_id == "A_AAC");
        CHECK(skipped[2].track_number == 4);
        CHECK(skipped[2].kind == TrackKind::kText);
        CHECK(skipped[2].codec_id == "S_TEXT/UTF8");
        CHECK(skipped[2].language == "fre");
        CHECK(skipped[3].track_number == 5);
        CHECK(skipped[3].kind == TrackKind::kText);
        CHECK(skipped[3].codec_id == "S_VOBSUB");
        CHECK(skipped[4].track_number == 6);
        CHECK(skipped[4].kind == TrackKind::kOther);
    }

    SECTION("an explicit track number is read and not listed, and the one auto-selection would take is") {
        const auto out = iclforge::containers::matroska::demux(
            file, iclforge::containers::matroska::ReadOptions{.track_number = 2});
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 2);
        REQUIRE(out->skipped.size() == 5);
        bool has_two = false;
        bool has_three = false;
        for (const auto& track : out->skipped) {
            has_two = has_two || track.track_number == 2;
            has_three = has_three || track.track_number == 3;
        }
        CHECK_FALSE(has_two);
        CHECK(has_three);
    }

    SECTION("the streaming Reader reads the same file without an error") {
        const auto frames = read_in_chunks(file, 7);
        CHECK(frames.size() == 1);
    }

    CHECK(iclforge::containers::matroska::describe(TrackKind::kText) == "text");
}

TEST_CASE("Matroska lists no more than 64 skipped tracks, however many Tracks declares",
          "[matroska][reader][skipped]") {
    Bytes tracks;
    for (std::uint64_t number = 1; number <= 70; ++number) {
        put_bytes(tracks, track_entry(TrackSpec{.number = number, .type = 1, .codec_id = "V_VP9"}));
    }
    put_bytes(tracks, track_entry(TrackSpec{.number = 71, .type = 2, .codec_id = "A_AC3"}));
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(71, 0, 0x80, frame_of(40, 0x33)));
    Bytes file = ebml_header();
    Bytes segment;
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(out->track.track_number == 71);
    REQUIRE(out->skipped.size() == 64);
    CHECK(out->skipped.front().track_number == 1);
    CHECK(out->skipped.back().track_number == 64);
}

TEST_CASE("Matroska reads a 32-bit SamplingFrequency and a defaulted track", "[matroska][reader]") {
    SECTION("a float32 sample rate") {
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{.sample_rate = 32000.0, .float32_rate = true}));
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(8, 0x01)));
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);

        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.sample_rate == 32000);
    }

    SECTION("an Audio element omitted entirely keeps Matroska's own defaults") {
        Bytes entry;
        put_uint_element(entry, kTrackNumber, 1);
        put_uint_element(entry, kTrackType, 2);
        put_string_element(entry, kCodecId, "A_AC3");
        Bytes tracks;
        put_element(tracks, kTrackEntry, entry);

        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(8, 0x01)));
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);

        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.sample_rate == 8000);
        CHECK(out->track.channels == 1);
        CHECK(out->track.language == "und");
    }
}

TEST_CASE("Matroska skips elements it has no use for", "[matroska][reader]") {
    // A Cues index and an Attachment sit between the tracks and the audio in
    // any real file. Neither is ever buffered - the reader counts past them.
    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_element(segment, kAttachments, frame_of(4096, 0xEE));
    put_element(segment, kCues, frame_of(2048, 0xCC));

    const Bytes frame = frame_of(128, 0x77);
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame));
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    REQUIRE(out->frames.size() == 1);
    CHECK(owned(out->frames)[0] == frame);

    // And the same across chunk boundaries, where a skip spans pushes.
    CHECK(read_in_chunks(file, 100) == std::vector<Bytes>{frame});
}

TEST_CASE("Matroska reads an unknown-size Cluster ended by its sibling", "[matroska][reader]") {
    // A live muxer may leave the Cluster open too, not just the Segment.
    // Nothing declares where it ends; the next Segment-level id does.
    const Bytes first = frame_of(64, 0x01);
    const Bytes second = frame_of(64, 0x02);

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);

    put_id(segment, kCluster);
    put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);  // unknown size
    put_uint_element(segment, kClusterTimestamp, 0);
    put_element(segment, kSimpleBlock, block_payload(1, 0, 0x80, first));

    put_id(segment, kCluster);
    put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);
    put_uint_element(segment, kClusterTimestamp, 1000);
    put_element(segment, kSimpleBlock, block_payload(1, 0, 0x80, second));

    put_element(file, kSegment, segment);

    const auto out = iclforge::containers::matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(owned(out->frames) == std::vector<Bytes>{first, second});
    CHECK(read_in_chunks(file, 5) == std::vector<Bytes>{first, second});
}

TEST_CASE("Matroska truncated mid-cluster keeps the frames before the cut", "[matroska][reader]") {
    // How a live recording actually ends: the process stopped. Everything
    // written before the cut is real audio and is returned; only a cut
    // before the track was ever described has nothing to give back.
    const std::vector<Bytes> frames{frame_of(400, 0x11), frame_of(400, 0x22),
                                    frame_of(400, 0x33)};
    const auto complete = iclforge::containers::matroska::mux(
        iclforge::containers::matroska::AudioTrack{
            .codec_id = std::string{iclforge::containers::matroska::kCodecEac3},
            .sample_rate = 48000,
            .channels = 2,
            .samples_per_frame = 1536,
            .language = "und"},
        views_of(frames), iclforge::containers::matroska::MuxOptions{.cluster_ms = 30});
    REQUIRE(complete.has_value());

    SECTION("cut after some frames") {
        const Bytes cut{complete->begin(), complete->end() - 300};
        const auto out = iclforge::containers::matroska::demux(cut);
        REQUIRE(out.has_value());
        CHECK(out->frames.size() < frames.size());
        for (std::size_t i = 0; i < out->frames.size(); ++i) {
            CHECK(owned(out->frames)[i] == frames[i]);
        }
    }

    SECTION("cut before the track is described") {
        const Bytes cut{complete->begin(), complete->begin() + 30};
        const auto out = iclforge::containers::matroska::demux(cut);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kTruncated);
    }
}

TEST_CASE("Matroska refuses what is not Matroska", "[matroska][reader]") {
    SECTION("empty input") {
        const auto out = iclforge::containers::matroska::demux({});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kNotMatroska);
    }

    SECTION("an MP4 file's ftyp") {
        const Bytes ftyp{std::byte{0}, std::byte{0},   std::byte{0},   std::byte{0x18},
                         std::byte{'f'}, std::byte{'t'}, std::byte{'y'}, std::byte{'p'}};
        const auto out = iclforge::containers::matroska::demux(ftyp);
        REQUIRE_FALSE(out.has_value());
        // A leading 0x00 has no EBML id marker at all.
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("a valid element that is not the EBML header") {
        Bytes file;
        put_element(file, kSegment, {});
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kNotMatroska);
    }

    SECTION("an EBML header but no tracks") {
        const auto file = ebml_header();
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kTruncated);
    }

    SECTION("tracks that hold nothing selectable") {
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{.type = 1, .codec_id = "V_VP9"}));
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        put_element(file, kSegment, segment);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("Matroska rejects malformed and hostile layouts", "[matroska][reader]") {
    const auto with_cluster = [](const Bytes& cluster_body) {
        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_element(segment, kCluster, cluster_body);
        put_element(file, kSegment, segment);
        return file;
    };

    SECTION("a Xiph lace whose declared sizes overrun the block") {
        Bytes laced;
        put_u8(laced, 1);    // two frames
        put_u8(laced, 200);  // but only 10 bytes follow
        put_bytes(laced, frame_of(10, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        const auto out = iclforge::containers::matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("a fixed-size lace that does not divide evenly") {
        Bytes laced;
        put_u8(laced, 2);  // three frames out of 10 bytes
        put_bytes(laced, frame_of(10, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x04, laced));
        const auto out = iclforge::containers::matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("an EBML lace whose delta drives a size negative") {
        Bytes laced;
        put_u8(laced, 2);    // three frames
        put_vint(laced, 4);  // first is 4 bytes
        put_vint_width(laced, 0, 1);  // delta of -63, taking the next size below zero
        put_bytes(laced, frame_of(20, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x06, laced));
        const auto out = iclforge::containers::matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("a block too short to hold its own header") {
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, frame_of(2, 0x81));
        const auto out = iclforge::containers::matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("a wanted leaf claiming more bytes than the limit allows") {
        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        // A SimpleBlock header declaring 4 GiB, with nothing behind it.
        put_id(cluster, kSimpleBlock);
        put_vint(cluster, 4ULL << 30);
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kLimitExceeded);
    }

    SECTION("nesting past max_depth") {
        Bytes file = ebml_header();
        // BlockGroup is a descended master, so a stack of them is the
        // cheapest way for an input to ask for unbounded depth.
        Bytes inner;
        for (int i = 0; i < 40; ++i) {
            Bytes wrapped;
            put_element(wrapped, kBlockGroup, inner);
            inner = wrapped;
        }
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_element(segment, kCluster, inner);
        put_element(file, kSegment, segment);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kLimitExceeded);
    }

    SECTION("an unknown size on an element that may not have one") {
        Bytes file = ebml_header();
        Bytes segment;
        put_id(segment, kTracks);
        put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);
        put_element(file, kSegment, segment);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }

    SECTION("a Channels value no layout could mean") {
        Bytes audio;
        put_uint_element(audio, kChannels, 100000);
        Bytes entry;
        put_uint_element(entry, kTrackNumber, 1);
        put_uint_element(entry, kTrackType, 2);
        put_string_element(entry, kCodecId, "A_EAC3");
        put_element(entry, kAudio, audio);
        Bytes tracks;
        put_element(tracks, kTrackEntry, entry);
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        put_element(file, kSegment, segment);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kMalformed);
    }
}

TEST_CASE("Matroska describe() names every demux error", "[matroska][reader]") {
    for (const auto error : {iclforge::containers::matroska::DemuxError::kNotMatroska,
                             iclforge::containers::matroska::DemuxError::kTruncated,
                             iclforge::containers::matroska::DemuxError::kMalformed,
                             iclforge::containers::matroska::DemuxError::kNoAudioTrack,
                             iclforge::containers::matroska::DemuxError::kLimitExceeded,
                             iclforge::containers::matroska::DemuxError::kEncryptedTrack}) {
        CHECK_FALSE(iclforge::containers::matroska::describe(error).empty());
        CHECK(iclforge::containers::matroska::describe(error) != "unknown error");
    }
}

namespace {

// A file of `tracks`, one cluster holding one block per entry of `blocks` (track, frame).
Bytes file_of(const std::vector<TrackSpec>& tracks,
              const std::vector<std::pair<std::uint64_t, Bytes>>& blocks) {
    Bytes track_entries;
    for (const auto& track : tracks) {
        put_bytes(track_entries, track_entry(track));
    }
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    for (const auto& [number, frame] : blocks) {
        put_element(cluster, kSimpleBlock, block_payload(number, 0, 0x80, frame));
    }
    Bytes segment;
    put_element(segment, kTracks, track_entries);
    put_element(segment, kCluster, cluster);
    Bytes file = ebml_header();
    put_element(file, kSegment, segment);
    return file;
}

}  // namespace

TEST_CASE("Matroska refuses an audio track that declares ContentEncryption",
          "[matroska][reader][encrypted]") {
    using iclforge::containers::matroska::DemuxError;
    using iclforge::containers::matroska::ReadOptions;
    // Nothing else in the entry marks its frames as ciphertext, so a reader that ignored
    // ContentEncodings handed them back as audio with exit code 0 (audit M11, G4).
    const std::vector<std::pair<std::uint64_t, Bytes>> blocks{{1, frame_of(64, 0xEE)},
                                                              {1, frame_of(64, 0xEF)}};

    SECTION("the only audio track is encrypted") {
        const auto file = file_of({TrackSpec{.encrypted = true}}, blocks);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == DemuxError::kEncryptedTrack);

        // The streaming Reader reaches the same verdict, and delivers no frame on the way.
        iclforge::containers::matroska::Reader reader{};
        std::size_t delivered = 0;
        const auto sink = [&delivered](std::span<const std::byte>) { ++delivered; };
        for (std::size_t offset = 0; offset < file.size(); offset += 5) {
            const auto take = std::min<std::size_t>(5, file.size() - offset);
            REQUIRE(reader.push(std::span<const std::byte>{file}.subspan(offset, take), sink)
                        .has_value());
        }
        const auto done = reader.finish();
        REQUIRE_FALSE(done.has_value());
        CHECK(done.error() == DemuxError::kEncryptedTrack);
        CHECK(delivered == 0);
        CHECK_FALSE(reader.track_found());
    }

    SECTION("naming the encrypted track is a refusal, naming an absent one is not") {
        const auto file = file_of({TrackSpec{.encrypted = true}}, blocks);
        const auto named =
            iclforge::containers::matroska::demux(file, ReadOptions{.track_number = 1});
        REQUIRE_FALSE(named.has_value());
        CHECK(named.error() == DemuxError::kEncryptedTrack);
        const auto absent =
            iclforge::containers::matroska::demux(file, ReadOptions{.track_number = 7});
        REQUIRE_FALSE(absent.has_value());
        CHECK(absent.error() == DemuxError::kNoAudioTrack);
    }

    SECTION("a clear audio track beside it is read, and the encrypted one is listed") {
        const auto file =
            file_of({TrackSpec{.number = 1, .encrypted = true},
                     TrackSpec{.number = 2, .codec_id = "A_AC3", .channels = 2}},
                    {{1, frame_of(64, 0xEE)}, {2, frame_of(32, 0x21)}, {2, frame_of(32, 0x22)}});
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 2);
        CHECK(owned(out->frames) == std::vector<Bytes>{frame_of(32, 0x21), frame_of(32, 0x22)});
        REQUIRE(out->skipped.size() == 1);
        CHECK(out->skipped[0].track_number == 1);
        // The encrypted track is still one the caller can ask for by number, and is refused.
        const auto named =
            iclforge::containers::matroska::demux(file, ReadOptions{.track_number = 1});
        REQUIRE_FALSE(named.has_value());
        CHECK(named.error() == DemuxError::kEncryptedTrack);
    }

    SECTION("an encrypted track of another codec is not this reader's to name") {
        const auto file = file_of({TrackSpec{.codec_id = "A_AAC", .encrypted = true}}, blocks);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == DemuxError::kNoAudioTrack);
    }

    SECTION("a ContentEncodings with no ContentEncryption is not taken for encryption") {
        // Compression (here header stripping) is another matter, which the reader does not apply;
        // this shows only that the refusal keys on ContentEncryption.
        const auto file = file_of({TrackSpec{.compressed = true}}, blocks);
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->frames.size() == 2);
    }
}

// A Blu-ray remux has TrueHD first and AC-3 or E-AC-3 after it. Auto-selection takes the first
// track of a codec it reads; AutoSelect lets a caller that cannot use one of them have the next.
TEST_CASE("Matroska auto-selection passes over a codec that ReadOptions::auto_select leaves out",
          "[matroska][reader][truehd]") {
    using iclforge::containers::matroska::AutoSelect;
    using iclforge::containers::matroska::ReadOptions;
    const Bytes truehd_frame = frame_of(80, 0x31);
    const Bytes eac3_frame = frame_of(120, 0x32);
    const auto file = file_of({TrackSpec{.number = 1, .codec_id = "A_TRUEHD", .channels = 8},
                               TrackSpec{.number = 2, .codec_id = "A_EAC3", .channels = 6}},
                              {{1, truehd_frame}, {2, eac3_frame}, {1, truehd_frame}});

    SECTION("by default the first track is taken, TrueHD, as it was") {
        const auto out = iclforge::containers::matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 1);
        CHECK(out->track.codec_id == "A_TRUEHD");
        CHECK(out->frames.size() == 2);
    }

    SECTION("without TrueHD the E-AC-3 track after it is taken, and the other is listed") {
        const auto out = iclforge::containers::matroska::demux(
            file, ReadOptions{.auto_select = AutoSelect{.truehd = false}});
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 2);
        CHECK(out->track.codec_id == "A_EAC3");
        CHECK(owned(out->frames) == std::vector<Bytes>{eac3_frame});
        REQUIRE(out->skipped.size() == 1);
        CHECK(out->skipped[0].track_number == 1);
        CHECK(out->skipped[0].kind == iclforge::containers::matroska::TrackKind::kAudio);
        CHECK(out->skipped[0].codec_id == "A_TRUEHD");

        // The streaming reader makes the same choice.
        CHECK(read_in_chunks(file, 7, ReadOptions{.auto_select = AutoSelect{.truehd = false}}) ==
              std::vector<Bytes>{eac3_frame});
    }

    SECTION("without E-AC-3 the first track stays the one taken") {
        const auto out = iclforge::containers::matroska::demux(
            file, ReadOptions{.auto_select = AutoSelect{.eac3 = false}});
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 1);
    }

    SECTION("with nothing left to take there is no audio track") {
        const auto out = iclforge::containers::matroska::demux(
            file, ReadOptions{.auto_select = AutoSelect{.eac3 = false, .truehd = false}});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == iclforge::containers::matroska::DemuxError::kNoAudioTrack);
    }

    SECTION("naming a track number is not narrowed by it") {
        const auto out = iclforge::containers::matroska::demux(
            file, ReadOptions{.track_number = 1, .auto_select = AutoSelect{.truehd = false}});
        REQUIRE(out.has_value());
        CHECK(out->track.codec_id == "A_TRUEHD");
    }
}
