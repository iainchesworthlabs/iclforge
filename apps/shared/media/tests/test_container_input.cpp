#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/encoder/silent_frame.hpp"
#include "iclforge/ac3/io/dec3.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "container_input.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "multi_track_files.hpp"

namespace {

using Bytes = std::vector<std::byte>;

// A 48 kHz track as the MP4 reader reports one, with `edits`.
iclforge::containers::mp4::ReadTrack track_with(
    std::vector<iclforge::containers::mp4::EditListEntry> edits, std::uint32_t timescale = 48000,
    std::uint32_t movie_timescale = 48000) {
    iclforge::containers::mp4::ReadTrack track;
    track.sample_rate = 48000;
    track.timescale = timescale;
    track.movie_timescale = movie_timescale;
    track.edits = std::move(edits);
    return track;
}

}  // namespace

// container readers (mkv/mp4/ts)'s own regression (tools/ci/fuzz_encoder_space.py's
// REGRESSION_SEEDS, seed 3600083275727211684): AC-3 at 48 kbit/s and 48 kHz
// codes exactly 192-byte frames - one of MPEG-TS's own three packet grid
// strides - so a stream whose frames repeat a byte at that same offset every
// frame (a low-entropy signal encodes near-identically frame to frame) looks
// exactly like a packet grid to a check that only counts recurrence. This
// builds that collision directly rather than relying on a fuzz seed landing
// on it: a real, syntactically-valid AC-3 header (so sniff_container's own
// elementary-stream check has something genuine to find) padded with 0x47 at
// a fixed 192-byte stride for several "frames" in a row.
TEST_CASE("sniff_container does not mistake a repetitive elementary stream for MPEG-TS",
          "[containers][io2]") {
    // The smallest legal AC-3 frame at 48 kHz (Table 5.18's lowest rung,
    // 32 kbit/s) is 64 words = 128 bytes - iclforge::ac3::io::read_frame_header only
    // ever reads the syncinfo/bsi header (well under 128 bytes for a plain
    // 2/0 layout), so everything past it is free to overwrite.
    const auto frame = iclforge::ac3::build_silent_stereo_frame({.bitrate_kbps = 32});
    REQUIRE(frame.has_value());
    REQUIRE(iclforge::ac3::io::read_frame_header(*frame).has_value());

    constexpr std::size_t kStride = 192;    // one of the three grid strides
    constexpr int kRepeats = 8;             // past kTsSyncRuns's own 5
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    // The real header goes at the very start, untouched - what makes this a
    // genuine elementary stream and not just an arbitrary byte pattern. The
    // 0x47 recurrence starts one stride later, at i=1, so it never
    // overwrites the sync word this test depends on: the grid still needs
    // only 5 consecutive hits, and i=1..7 already gives it 7.
    std::ranges::copy(*frame, stream.begin());
    for (int i = 1; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(iclforge::apps::sniff_container(stream) == iclforge::apps::ContainerKind::kUnknown);
}

TEST_CASE("sniff_container still finds a real MPEG-TS packet grid", "[containers][io2]") {
    // The negative case beside the one above: a plain 0x47 recurrence with
    // no valid frame header anywhere in it (the read_frame_header check
    // fails immediately on all-zero bytes) is still read as a transport
    // stream - the fix narrows the false positive, it does not disable the
    // grid check.
    constexpr std::size_t kStride = 188;
    constexpr int kRepeats = 6;
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    for (int i = 0; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(iclforge::apps::sniff_container(stream) == iclforge::apps::ContainerKind::kMpegTs);
}

// The same false positive from a WAV: steady PCM repeats bytes at a fixed
// period (a 1 kHz sine at 48 kHz is 48 samples a cycle), and a float WAV
// `forge decode` wrote had five 0x47 bytes exactly 192 apart, so every
// command that sniffs its input refused it as an undemuxable transport
// stream. Its RIFF/WAVE magic - and RF64's and BW64's - settles it first.
TEST_CASE("sniff_container does not mistake a WAV with a 0x47 grid for MPEG-TS",
          "[containers][io2]") {
    for (const std::string_view id : {"RIFF", "RF64", "BW64"}) {
        CAPTURE(id);
        constexpr int kRepeats = 8;
        Bytes wav(44 + (204 * kRepeats), std::byte{0x00});
        const std::string_view header_ids[] = {id, "WAVE"};
        std::ranges::copy(std::as_bytes(std::span{header_ids[0]}), wav.begin());
        std::ranges::copy(std::as_bytes(std::span{header_ids[1]}), wav.begin() + 8);
        // Both grids at once, each starting inside its first stride as a real
        // capture's would, so only the magic can tell this apart.
        for (int i = 0; i < kRepeats; ++i) {
            wav[44 + (188 * static_cast<std::size_t>(i))] = std::byte{0x47};
            wav[52 + (192 * static_cast<std::size_t>(i))] = std::byte{0x47};
        }
        CHECK(iclforge::apps::sniff_container(wav) == iclforge::apps::ContainerKind::kUnknown);
    }
}

// A capture may start mid-packet, but then its first whole packet's sync
// byte is less than one stride in; a grid that only starts further in has
// something that is not transport stream before it, and is not one.
TEST_CASE("sniff_container wants the packet grid to start within its first stride",
          "[containers][io2]") {
    constexpr std::size_t kStride = 188;
    constexpr int kRepeats = 6;
    for (const std::size_t start : {std::size_t{100}, std::size_t{187}, std::size_t{188},
                                    std::size_t{1000}}) {
        CAPTURE(start);
        Bytes stream(start + (kStride * kRepeats), std::byte{0x00});
        for (int i = 0; i < kRepeats; ++i) {
            stream[start + (static_cast<std::size_t>(i) * kStride)] = std::byte{0x47};
        }
        CHECK(iclforge::apps::sniff_container(stream) ==
              (start < kStride ? iclforge::apps::ContainerKind::kMpegTs
                               : iclforge::apps::ContainerKind::kUnknown));
    }
}

TEST_CASE("elementary_stream_from_bytes leaves a bare elementary stream untouched",
          "[containers][io2]") {
    const auto frame = iclforge::ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());

    const auto result = iclforge::apps::elementary_stream_from_bytes(*frame);
    CHECK(result.error.empty());
    CHECK(result.bytes == *frame);
    CHECK(result.trim.start == 0);
    CHECK_FALSE(result.trim.length.has_value());
    CHECK(result.trim_note.empty());
    CHECK(result.container.kind == iclforge::apps::ContainerKind::kUnknown);
    CHECK(result.container.codec_id.empty());
    CHECK(result.container.samples == 0);
    CHECK_FALSE(result.container.codec_box.has_value());
}

TEST_CASE("elementary_stream_from_bytes reports what each container says about its track",
          "[containers][io2]") {
    using iclforge::apps::ContainerKind;
    const auto frame = iclforge::ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    Bytes stream;
    for (int i = 0; i < 4; ++i) {
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    const auto scanned = iclforge::ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    const std::span<const std::span<const std::byte>> units(scanned->access_units);

    SECTION("MP4, with its codec configuration box") {
        iclforge::containers::mp4::AudioTrack track;
        track.codec_id = std::string{iclforge::containers::mp4::kCodecAc3};
        track.sample_rate = 48000;
        track.channels = 2;
        track.codec_config = iclforge::ac3::io::build_codec_config_box(*scanned);
        const auto file = iclforge::containers::mp4::mux(track, units);
        REQUIRE(file.has_value());
        const auto result = iclforge::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMp4);
        CHECK(iclforge::apps::container_token(facts.kind) == "mp4");
        CHECK(facts.codec_id == "ac-3");
        CHECK(facts.track == 1);
        CHECK(facts.language == "und");
        CHECK(facts.samples == 4);
        CHECK(facts.sample_rate == 48000);
        CHECK(facts.channels == 2);
        CHECK(facts.timescale == 48000);
        CHECK(facts.edits == 0);
        REQUIRE(facts.codec_box.has_value());
        CHECK(facts.codec_box->type == "dac3");
        CHECK(facts.codec_box->fscod == 0);
        CHECK(facts.codec_box->bsid == 8);
        CHECK(facts.codec_box->acmod == 2);
        CHECK_FALSE(facts.codec_box->lfeon);
        CHECK(facts.codec_box->bit_rate_code == 10);  // Table 5.18: 192 kbit/s
        CHECK(facts.codec_box->independent_substreams == 0);
        CHECK(facts.codec_box->bytes > 0);
    }
    SECTION("Matroska") {
        iclforge::containers::matroska::AudioTrack track;
        track.codec_id = std::string{iclforge::containers::matroska::kCodecAc3};
        track.sample_rate = 48000;
        track.channels = 2;
        const auto file = iclforge::containers::matroska::mux(track, units);
        REQUIRE(file.has_value());
        const auto result = iclforge::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMatroska);
        CHECK(iclforge::apps::container_token(facts.kind) == "matroska");
        CHECK(facts.codec_id == "A_AC3");
        CHECK(facts.track == 1);
        CHECK(facts.samples == 4);
        CHECK(facts.channels == 2);
        CHECK_FALSE(facts.codec_box.has_value());
    }
    SECTION("MPEG-TS") {
        iclforge::containers::mpegts::AudioTrack track;
        track.codec = iclforge::containers::mpegts::AudioCodec::kAc3;
        iclforge::containers::mpegts::MuxOptions options;
        options.profile = iclforge::containers::mpegts::BroadcastProfile::kAtsc;
        options.program_number = 3;
        options.pmt_pid = 0x0200;
        options.audio_pid = 0x0210;
        const auto file = iclforge::containers::mpegts::mux(track, units, options);
        REQUIRE(file.has_value());
        const auto result = iclforge::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMpegTs);
        CHECK(iclforge::apps::container_token(facts.kind) == "mpegts");
        CHECK(facts.codec_id.empty());
        CHECK(facts.track == 0x0210);
        CHECK(facts.language.empty());
        CHECK(facts.samples > 0);
        CHECK(facts.program_number == 3);
        CHECK(facts.pmt_pid == 0x0200);
        CHECK(facts.stream_type == 0x81);
        CHECK(facts.signalling == "atsc_stream_type");
        CHECK(facts.packet_size == 188);
    }
    CHECK(iclforge::apps::container_token(ContainerKind::kUnknown).empty());
}

namespace {

// One EBML element with an eight-byte size, as much Matroska as the reader's Tracks and one
// cluster need. Built here, not with the project's writer, which has one audio track.
Bytes ebml_element(std::uint32_t id, const Bytes& body) {
    Bytes out;
    const int width = id <= 0xFF ? 1 : id <= 0xFFFF ? 2 : id <= 0xFFFFFF ? 3 : 4;
    for (int i = width - 1; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((id >> (8 * i)) & 0xFFU));
    }
    out.push_back(std::byte{0x01});
    for (int i = 6; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(body.size()) >> (8 * i)) & 0xFFU));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes ebml_uint(std::uint32_t id, std::uint64_t value) {
    return ebml_element(id, Bytes{static_cast<std::byte>(value)});
}

Bytes ebml_text(std::uint32_t id, std::string_view text) {
    Bytes body;
    for (const char c : text) {
        body.push_back(static_cast<std::byte>(c));
    }
    return ebml_element(id, body);
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// A track entry: its number, type (1 video, 2 audio, 17 subtitle), codec and language, and for
// audio the Audio master the reader needs.
Bytes ebml_track(std::uint64_t number, std::uint64_t type, std::string_view codec,
                 std::string_view language) {
    Bytes entry = concat({ebml_uint(0xD7, number), ebml_uint(0x83, type), ebml_text(0x86, codec),
                          ebml_text(0x22B59C, language)});
    if (type == 2) {
        // SamplingFrequency as a 32-bit float, 48000, and six channels.
        const Bytes rate{std::byte{0x47}, std::byte{0x3B}, std::byte{0x80}, std::byte{0x00}};
        const Bytes audio = concat({ebml_element(0xB5, rate), ebml_uint(0x9F, 6)});
        const Bytes master = ebml_element(0xE1, audio);
        entry.insert(entry.end(), master.begin(), master.end());
    }
    return ebml_element(0xAE, entry);
}

}  // namespace

TEST_CASE("elementary_stream_from_bytes lists the other tracks of a Matroska file",
          "[containers][io2]") {
    using iclforge::apps::ContainerKind;
    // A karaoke video's shape: a picture, the AC-3 audio, and a text subtitle track.
    const Bytes tracks = concat({ebml_track(1, 1, "V_MPEG4/ISO/AVC", "eng"),
                                 ebml_track(2, 2, "A_AC3", "eng"),
                                 ebml_track(3, 17, "S_TEXT/UTF8", "fra")});
    // One SimpleBlock for track 2: a track vint, a relative timestamp, flags, then its frame.
    const Bytes block = concat({Bytes{std::byte{0x82}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80}},
                                Bytes(16, std::byte{0x5A})});
    const Bytes cluster = concat({ebml_uint(0xE7, 0), ebml_element(0xA3, block)});
    const Bytes segment = concat({ebml_element(0x1654AE6B, tracks), ebml_element(0x1F43B675, cluster)});
    const Bytes file = concat({ebml_element(0x1A45DFA3, ebml_text(0x4282, "matroska")),
                               ebml_element(0x18538067, segment)});

    const auto result = iclforge::apps::elementary_stream_from_bytes(file);
    REQUIRE(result.error.empty());
    const auto& facts = result.container;
    CHECK(facts.kind == ContainerKind::kMatroska);
    CHECK(facts.codec_id == "A_AC3");
    CHECK(facts.track == 2);
    REQUIRE(facts.other_tracks.size() == 2);
    CHECK(facts.other_tracks[0].id == 1);
    CHECK(facts.other_tracks[0].kind == "video");
    CHECK(facts.other_tracks[0].codec_id == "V_MPEG4/ISO/AVC");
    CHECK(facts.other_tracks[0].language == "eng");
    CHECK(facts.other_tracks[1].id == 3);
    CHECK(facts.other_tracks[1].kind == "text");
    CHECK(facts.other_tracks[1].codec_id == "S_TEXT/UTF8");
    CHECK(facts.other_tracks[1].language == "fra");
}

// An MP4 edit list, read as the part of the stream a player should play.

TEST_CASE("trim_from_edit_list reads the edit list an audio encoder writes",
          "[containers][edit-list]") {
    using iclforge::apps::trim_from_edit_list;
    std::string note = "left over from before";

    SECTION("no edit list, or only empty edits: all of the track") {
        auto trim = trim_from_edit_list(track_with({}), note);
        CHECK(trim.start == 0);
        CHECK_FALSE(trim.length.has_value());
        CHECK(note.empty());

        trim = trim_from_edit_list(
            track_with({{.segment_duration = 480, .media_time = -1}}), note);
        CHECK(trim.start == 0);
        CHECK_FALSE(trim.length.has_value());
        CHECK(note.empty());
    }

    SECTION("one edit, counted in samples") {
        const auto trim = trim_from_edit_list(
            track_with({{.segment_duration = 44800, .media_time = 256}}), note);
        CHECK(trim.start == 256);
        REQUIRE(trim.length.has_value());
        CHECK(*trim.length == 44800);
        CHECK(note.empty());
    }

    SECTION("an empty edit ahead of it is a delay, not audio, and changes nothing") {
        const auto trim = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = -1},
                        {.segment_duration = 44800, .media_time = 1024}}),
            note);
        CHECK(trim.start == 1024);
        REQUIRE(trim.length.has_value());
        CHECK(*trim.length == 44800);
    }

    SECTION("a duration in a 1000 Hz movie timescale, to the nearest sample") {
        const auto at_48k = trim_from_edit_list(
            track_with({{.segment_duration = 1234, .media_time = 256}}, 48000, 1000), note);
        REQUIRE(at_48k.length.has_value());
        CHECK(*at_48k.length == 1234 * 48);

        auto track = track_with({{.segment_duration = 7, .media_time = 0}}, 44100, 1000);
        track.sample_rate = 44100;
        const auto rounded_up = trim_from_edit_list(track, note);  // 308.7 samples
        REQUIRE(rounded_up.length.has_value());
        CHECK(*rounded_up.length == 309);
        track.edits[0].segment_duration = 3;
        const auto rounded_down = trim_from_edit_list(track, note);  // 132.3 samples
        REQUIRE(rounded_down.length.has_value());
        CHECK(*rounded_down.length == 132);
    }

    SECTION("a media timescale other than the rate") {
        const auto doubled = trim_from_edit_list(
            track_with({{.segment_duration = 48000, .media_time = 512}}, 96000), note);
        CHECK(doubled.start == 256);
        const auto ninety = trim_from_edit_list(
            track_with({{.segment_duration = 48000, .media_time = 45000}}, 90000), note);
        CHECK(ninety.start == 24000);
    }

    SECTION("a zero duration, or no movie timescale to count one in, runs to the end") {
        const auto zero = trim_from_edit_list(
            track_with({{.segment_duration = 0, .media_time = 256}}), note);
        CHECK(zero.start == 256);
        CHECK_FALSE(zero.length.has_value());

        const auto no_movie = trim_from_edit_list(
            track_with({{.segment_duration = 44800, .media_time = 256}}, 48000, 0), note);
        CHECK(no_movie.start == 256);
        CHECK_FALSE(no_movie.length.has_value());
    }

    SECTION("any other shape is not applied, and says why") {
        const auto two = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = 0},
                        {.segment_duration = 1000, .media_time = 5000}}),
            note);
        CHECK(two.start == 0);
        CHECK_FALSE(two.length.has_value());
        CHECK(note.find("2 edits with audio") != std::string::npos);

        const auto fast = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = 0, .media_rate = 0x00020000}}),
            note);
        CHECK(fast.start == 0);
        CHECK(note.find("another speed") != std::string::npos);

        auto no_timescale = track_with({{.segment_duration = 1000, .media_time = 256}}, 0);
        const auto unread = trim_from_edit_list(no_timescale, note);
        CHECK(unread.start == 0);
        CHECK(note.find("no timescale") != std::string::npos);
    }

    SECTION("a value no real file holds saturates rather than wrapping") {
        const auto huge = trim_from_edit_list(
            track_with({{.segment_duration = 1,
                         .media_time = std::numeric_limits<std::int64_t>::max()}},
                       1),
            note);
        CHECK(huge.start == std::numeric_limits<std::uint64_t>::max());
    }
}

TEST_CASE("elementary_stream_from_bytes reports an MP4's edit list as a trim",
          "[containers][edit-list]") {
    const auto frame = iclforge::ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    Bytes stream;
    for (int i = 0; i < 3; ++i) {
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    const auto scanned = iclforge::ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_units.size() == 3);

    iclforge::containers::mp4::AudioTrack track;
    track.codec_id = std::string{iclforge::containers::mp4::kCodecAc3};
    track.sample_rate = 48000;
    track.channels = 2;
    track.codec_config = iclforge::ac3::io::build_codec_config_box(*scanned);
    const std::span<const std::span<const std::byte>> units(scanned->access_units);

    iclforge::containers::mp4::MuxOptions edited;
    edited.edit = iclforge::containers::mp4::MuxOptions::Edit{.start_samples = 256,
                                                              .duration_samples = (3 * 1536) - 356};
    const auto with_edit = iclforge::containers::mp4::mux(track, units, edited);
    REQUIRE(with_edit.has_value());
    const auto trimmed = iclforge::apps::elementary_stream_from_bytes(*with_edit);
    CHECK(trimmed.error.empty());
    CHECK(trimmed.bytes == stream);
    CHECK(trimmed.trim.start == 256);
    REQUIRE(trimmed.trim.length.has_value());
    CHECK(*trimmed.trim.length == (3 * 1536) - 356);
    CHECK(trimmed.trim_note.empty());

    const auto without_edit = iclforge::containers::mp4::mux(track, units);
    REQUIRE(without_edit.has_value());
    const auto whole = iclforge::apps::elementary_stream_from_bytes(*without_edit);
    CHECK(whole.bytes == stream);
    CHECK(whole.trim.start == 0);
    CHECK_FALSE(whole.trim.length.has_value());
}

TEST_CASE("elementary_stream_from_bytes names a protected MP4 track and reads nothing of it",
          "[containers][io2][protected]") {
    // The three protected files the MP4 fuzz harness seeds with (tools/generators/
    // gen_cenc_reader_fixtures.py). The first has a clear sample entry beside the protected one,
    // which used to be read as clear, ciphertext and all.
    const std::string kid = "a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90";
    const auto read = [](const std::string& name) {
        std::ifstream in{std::string{ICLFORGE_MP4_SEED_DIR} + "/" + name, std::ios::binary};
        REQUIRE(in.good());
        const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                    std::istreambuf_iterator<char>{}};
        Bytes bytes(raw.size());
        std::ranges::transform(raw, bytes.begin(),
                               [](char c) { return static_cast<std::byte>(c); });
        return bytes;
    };

    const auto mixed =
        iclforge::apps::elementary_stream_from_bytes(read("cenc-clear-entry-then-enca.mp4"));
    CHECK(mixed.bytes.empty());
    CHECK(mixed.error ==
          "MP4 file this build cannot demux: track 1 mixes a clear sample entry with a protected "
          "one, entry 2 (cenc, original format ec-3, default KID " +
              kid + "), and is refused whole; decryption is not supported");

    const auto plain = iclforge::apps::elementary_stream_from_bytes(read("cenc-enca-only.mp4"));
    CHECK(plain.bytes.empty());
    CHECK(plain.error ==
          "MP4 file this build cannot demux: track 1 is protected (cenc, original format ec-3, "
          "default KID " +
              kid + "); decryption is not supported");

    const auto cbcs = iclforge::apps::elementary_stream_from_bytes(read("cbcs-enca-only.mp4"));
    CHECK(cbcs.bytes.empty());
    CHECK(cbcs.error.find("track 1 is protected (cbcs, original format ec-3") != std::string::npos);
}

// Which track a file with several is read as. A Blu-ray remux has TrueHD first and AC-3 or E-AC-3
// after it; the readers took the first track of a codec they read, and once they read TrueHD that
// was the TrueHD one for every caller, including those that cannot play it. The tracks here are
// real streams: the access units of a TrueHD file FFmpeg made and of a 5.1 E-AC-3 file.
namespace {

namespace mt = iclforge::test::multi_track;

Bytes read_stream(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    Bytes bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

Bytes joined(std::span<const std::span<const std::byte>> units) {
    Bytes out;
    for (const auto& unit : units) {
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

// The two streams, kept for the length of the test: the tracks hold views into them.
struct TwoStreams {
    Bytes truehd_bytes =
        read_stream(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/truehd_m51_s24.thd");
    Bytes eac3_bytes = read_stream(std::string{ICLFORGE_GOLDEN_AUDIO_DIR} +
                                   "/reference_51_eac3_448k_cplbndstrce0.ec3");
    mt::Track truehd{};
    mt::Track eac3{};
    Bytes truehd_stream{};
    Bytes eac3_stream{};

    TwoStreams() {
        truehd.codec = mt::Codec::kTruehd;
        truehd.units = mt::split_truehd(truehd_bytes);
        eac3.codec = mt::Codec::kEac3;
        const auto scanned = iclforge::ac3::io::scan(eac3_bytes);
        REQUIRE(scanned.has_value());
        eac3.units = scanned->access_units;
        REQUIRE(truehd.units.size() > 3);
        REQUIRE(eac3.units.size() > 3);
        truehd_stream = joined(truehd.units);
        eac3_stream = joined(eac3.units);
    }
    TwoStreams(const TwoStreams&) = delete;
    TwoStreams& operator=(const TwoStreams&) = delete;
};

using Build = Bytes (*)(std::span<const mt::Track>);

struct ContainerCase {
    const char* name;
    Build build;
    iclforge::apps::ContainerKind kind;
    // What the container calls the E-AC-3 track and the TrueHD one.
    const char* eac3_id;
    const char* truehd_id;
};

constexpr ContainerCase kContainers[] = {
    {"Matroska", &mt::matroska_file, iclforge::apps::ContainerKind::kMatroska, "A_EAC3",
     "A_TRUEHD"},
    {"MP4", &mt::mp4_file, iclforge::apps::ContainerKind::kMp4, "ec-3", "mlpa"},
    // MPEG-TS names a stream by its stream_type, not a string.
    {"MPEG-TS", &mt::mpegts_file, iclforge::apps::ContainerKind::kMpegTs, "", ""},
};

}  // namespace

TEST_CASE("elementary_stream_from_bytes takes the first track whose codec the caller can use",
          "[containers][io2][tracks]") {
    using iclforge::apps::CodecSet;
    using iclforge::apps::TrackCodec;
    const TwoStreams streams;

    for (const ContainerCase& container : kContainers) {
        SECTION(container.name) {
            const std::array<mt::Track, 2> truehd_first{streams.truehd, streams.eac3};
            const std::array<mt::Track, 2> eac3_first{streams.eac3, streams.truehd};
            const Bytes with_truehd_first = container.build(truehd_first);
            const Bytes with_eac3_first = container.build(eac3_first);
            REQUIRE(iclforge::apps::sniff_container(with_truehd_first) == container.kind);

            // A caller that does not say gets what every caller got before TrueHD was read: the
            // E-AC-3 track, which here is the second.
            const auto by_default = iclforge::apps::elementary_stream_from_bytes(with_truehd_first);
            CHECK(by_default.error.empty());
            CHECK_FALSE(by_default.declined_codec.has_value());
            CHECK(by_default.bytes == streams.eac3_stream);
            CHECK(by_default.container.kind == container.kind);
            if (container.kind == iclforge::apps::ContainerKind::kMpegTs) {
                CHECK(by_default.container.stream_type == 0x87);
                CHECK(by_default.container.track == 0x101);
            } else {
                CHECK(by_default.container.codec_id == container.eac3_id);
                CHECK(by_default.container.track == 2);
                // The track passed over is listed beside it.
                REQUIRE(by_default.container.other_tracks.size() == 1);
                CHECK(by_default.container.other_tracks[0].id == 1);
                CHECK(by_default.container.other_tracks[0].codec_id == container.truehd_id);
            }

            // A caller that reads TrueHD, as forge does, gets the first track of the file.
            const auto reads_truehd = iclforge::apps::elementary_stream_from_bytes(
                with_truehd_first, CodecSet{.truehd = true});
            CHECK(reads_truehd.error.empty());
            CHECK(reads_truehd.bytes == streams.truehd_stream);
            if (container.kind == iclforge::apps::ContainerKind::kMpegTs) {
                CHECK(reads_truehd.container.stream_type == 0x83);
            } else {
                CHECK(reads_truehd.container.codec_id == container.truehd_id);
            }

            // First in the file, not first in the codec list: E-AC-3 ahead of TrueHD is taken
            // whichever of the two the caller allows.
            const auto either = iclforge::apps::elementary_stream_from_bytes(
                with_eac3_first, CodecSet{.truehd = true});
            CHECK(either.bytes == streams.eac3_stream);

            // Leaving E-AC-3 out as well leaves the TrueHD track the only choice.
            const auto only_truehd = iclforge::apps::elementary_stream_from_bytes(
                with_truehd_first, CodecSet{.eac3 = false, .truehd = true});
            CHECK(only_truehd.bytes == streams.truehd_stream);
        }
    }
}

TEST_CASE("elementary_stream_from_bytes says when a file's audio is in no codec the caller can use",
          "[containers][io2][tracks]") {
    using iclforge::apps::CodecSet;
    using iclforge::apps::TrackCodec;
    const TwoStreams streams;

    for (const ContainerCase& container : kContainers) {
        SECTION(container.name) {
            const std::array<mt::Track, 1> truehd_only{streams.truehd};
            const Bytes file = container.build(truehd_only);

            const auto declined = iclforge::apps::elementary_stream_from_bytes(file);
            CHECK(declined.bytes.empty());
            REQUIRE(declined.declined_codec.has_value());
            CHECK(*declined.declined_codec == TrackCodec::kTruehd);
            CHECK(declined.error.find("whose audio is TrueHD, which this program does not read") !=
                  std::string::npos);
            // The track the reader would have taken is described, for a caller that shows it.
            CHECK(declined.container.kind == container.kind);
            if (container.kind == iclforge::apps::ContainerKind::kMpegTs) {
                CHECK(declined.container.stream_type == 0x83);
            } else {
                CHECK(declined.container.codec_id == container.truehd_id);
            }

            // The same file for a caller that reads TrueHD.
            const auto read = iclforge::apps::elementary_stream_from_bytes(
                file, CodecSet{.truehd = true});
            CHECK(read.error.empty());
            CHECK_FALSE(read.declined_codec.has_value());
            CHECK(read.bytes == streams.truehd_stream);

            // TrueHD is not special: a codec the set leaves out is reported the same way, and one
            // it allows is read.
            const std::array<mt::Track, 1> eac3_only{streams.eac3};
            const auto fine =
                iclforge::apps::elementary_stream_from_bytes(container.build(eac3_only));
            CHECK(fine.error.empty());
            CHECK(fine.bytes == streams.eac3_stream);
            const auto no_eac3 = iclforge::apps::elementary_stream_from_bytes(
                container.build(eac3_only), CodecSet{.eac3 = false});
            REQUIRE(no_eac3.declined_codec.has_value());
            CHECK(*no_eac3.declined_codec == TrackCodec::kEac3);
        }
    }
}

TEST_CASE("looks_like_truehd tells a bare TrueHD or MLP stream from the other streams",
          "[containers][io2][tracks]") {
    const TwoStreams streams;
    CHECK(iclforge::apps::looks_like_truehd(streams.truehd_bytes));
    CHECK(iclforge::apps::looks_like_truehd(
        read_stream(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/mlp_st48_s24.mlp")));
    CHECK_FALSE(iclforge::apps::looks_like_truehd(streams.eac3_bytes));
    CHECK_FALSE(iclforge::apps::looks_like_truehd(std::span<const std::byte>{}));
    CHECK_FALSE(iclforge::apps::looks_like_truehd(
        std::span<const std::byte>{streams.truehd_bytes}.first(7)));
    CHECK(iclforge::apps::codec_name(iclforge::apps::TrackCodec::kTruehd) == "TrueHD");
    CHECK(iclforge::apps::codec_name(iclforge::apps::TrackCodec::kEac3) == "E-AC-3");
}
