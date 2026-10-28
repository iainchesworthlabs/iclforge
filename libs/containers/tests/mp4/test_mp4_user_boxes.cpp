#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"

// AudioTrack::user_boxes and ReadTrack::user_boxes: ISO/IEC 14496-12 §4.2's `uuid` box as a
// direct child of the `trak`, written by mux(), fragment() and FragmentWriter and read back by
// demux(), the incremental Reader and demux_seekable(). The module is codec-blind, so the payloads
// here are arbitrary bytes. The cases that the writers cannot produce (a ninth box, a payload past
// the bound, a box too short for a usertype, a `uuid` box somewhere other than the `trak`) are
// made by growing the boxes of a file the writer did produce, which is possible without touching a
// sample table when the file is an initialization segment (its sample table is empty, so no chunk
// offset moves).

namespace {

namespace mp4 = iclforge::containers::mp4;
using Bytes = std::vector<std::byte>;

Bytes bytes_of(std::size_t count, std::uint8_t fill) {
    return Bytes(count, static_cast<std::byte>(fill));
}

mp4::AudioTrack ac4_track() {
    mp4::AudioTrack track;
    track.codec_id = std::string{mp4::kCodecAc4};
    track.sample_rate = 48000;
    track.channels = 2;
    track.samples_per_frame = 2048;
    track.codec_config = {std::byte{0x2A}, std::byte{0x04}, std::byte{0x10}, std::byte{0x00}};
    return track;
}

mp4::UserBox user_box(std::uint8_t usertype_fill, Bytes payload) {
    mp4::UserBox box;
    box.usertype.fill(static_cast<std::byte>(usertype_fill));
    box.usertype[0] = std::byte{0xB7};  // so no two kinds of box start alike by accident
    box.payload = std::move(payload);
    return box;
}

const std::vector<Bytes>& frames() {
    static const std::vector<Bytes> f{bytes_of(700, 0x6A), bytes_of(512, 0x6B),
                                      bytes_of(280, 0x6C)};
    return f;
}

void require_same_boxes(const std::vector<mp4::UserBox>& got,
                        const std::vector<mp4::UserBox>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        INFO("user box " << i);
        REQUIRE(got[i].usertype == want[i].usertype);
        REQUIRE(got[i].payload == want[i].payload);
    }
}

std::uint32_t get_u32(const Bytes& b, std::size_t at) {
    return (std::to_integer<std::uint32_t>(b[at]) << 24) |
           (std::to_integer<std::uint32_t>(b[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(b[at + 2]) << 8) |
           std::to_integer<std::uint32_t>(b[at + 3]);
}

void put_u32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::byte>(v >> 24);
    b[at + 1] = static_cast<std::byte>((v >> 16) & 0xFF);
    b[at + 2] = static_cast<std::byte>((v >> 8) & 0xFF);
    b[at + 3] = static_cast<std::byte>(v & 0xFF);
}

std::size_t find_text(const Bytes& haystack, std::string_view text) {
    const auto it = std::search(haystack.begin(), haystack.end(), text.begin(), text.end(),
                                [](std::byte a, char b) { return a == static_cast<std::byte>(b); });
    return it == haystack.end() ? std::string::npos
                                : static_cast<std::size_t>(it - haystack.begin());
}

// A whole `uuid` box: size, 'uuid', the usertype and the payload.
Bytes uuid_box(const mp4::UserBox& box) {
    Bytes out(4, std::byte{0});
    put_u32(out, 0, static_cast<std::uint32_t>(8 + 16 + box.payload.size()));
    for (const char c : std::string_view{"uuid"}) {
        out.push_back(static_cast<std::byte>(c));
    }
    out.insert(out.end(), box.usertype.begin(), box.usertype.end());
    out.insert(out.end(), box.payload.begin(), box.payload.end());
    return out;
}

// A `uuid` box whose body is `body` exactly, with no usertype added.
Bytes uuid_box_of_body(const Bytes& body) {
    Bytes out(4, std::byte{0});
    put_u32(out, 0, static_cast<std::uint32_t>(8 + body.size()));
    for (const char c : std::string_view{"uuid"}) {
        out.push_back(static_cast<std::byte>(c));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Appends `extra` at the end of the innermost of the nested boxes `path` names (outermost first),
// and grows each box on the path by its length. Finds each box by the first occurrence of its type
// after the previous one, which is unambiguous in a file this module wrote.
Bytes grow(Bytes file, std::initializer_list<std::string_view> path, const Bytes& extra) {
    std::vector<std::size_t> starts;
    std::size_t from = 0;
    for (const std::string_view type : path) {
        const Bytes tail(file.begin() + static_cast<std::ptrdiff_t>(from), file.end());
        const std::size_t at = find_text(tail, type);
        REQUIRE(at != std::string::npos);
        starts.push_back(from + at - 4);  // the box begins at its size field
        from = from + at + 4;
    }
    const std::size_t innermost_end = starts.back() + get_u32(file, starts.back());
    for (const std::size_t start : starts) {
        put_u32(file, start, get_u32(file, start) + static_cast<std::uint32_t>(extra.size()));
    }
    file.insert(file.begin() + static_cast<std::ptrdiff_t>(innermost_end), extra.begin(),
                extra.end());
    return file;
}

Bytes init_segment_with(const std::vector<mp4::UserBox>& boxes) {
    mp4::AudioTrack track = ac4_track();
    track.user_boxes = boxes;
    const auto out = mp4::fragment(track, frames());
    REQUIRE(out.has_value());
    return out->init_segment;
}

std::vector<mp4::UserBox> two_boxes() {
    return {user_box(0x11, bytes_of(300, 0xA1)), user_box(0x22, Bytes{})};
}

// Reads the way the incremental Reader does, a chunk at a time.
mp4::ReadTrack read_in_chunks(const Bytes& file, std::size_t chunk) {
    mp4::Reader reader;
    std::size_t samples = 0;
    for (std::size_t at = 0; at < file.size(); at += chunk) {
        const auto n = std::min(chunk, file.size() - at);
        const auto pushed = reader.push(std::span<const std::byte>{file}.subspan(at, n),
                                        [&samples](std::span<const std::byte>) { ++samples; });
        REQUIRE(pushed.has_value());
    }
    REQUIRE(reader.finish().has_value());
    REQUIRE(samples == frames().size());
    return reader.track();
}

mp4::ReadTrack read_seekable(const Bytes& file) {
    mp4::ReadTrack track;
    const auto read_at = [&file](std::uint64_t offset, std::span<std::byte> out) -> std::size_t {
        if (offset >= file.size()) {
            return 0;
        }
        const auto n =
            std::min<std::size_t>(out.size(), file.size() - static_cast<std::size_t>(offset));
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
        return n;
    };
    const auto result = mp4::demux_seekable(
        read_at, file.size(), [&track](const mp4::ReadTrack& t) { track = t; },
        [](std::span<const std::byte>) {});
    REQUIRE(result.has_value());
    return track;
}

}  // namespace

TEST_CASE("MP4 user boxes round-trip through mux() and demux()", "[mp4][user_box]") {
    mp4::AudioTrack track = ac4_track();
    track.user_boxes = two_boxes();
    const auto file = mp4::mux(track, frames());
    REQUIRE(file.has_value());

    const auto out = mp4::demux(*file);
    REQUIRE(out.has_value());
    require_same_boxes(out->track.user_boxes, track.user_boxes);

    // The samples are the ones that went in.
    REQUIRE(out->samples.size() == frames().size());
    for (std::size_t i = 0; i < frames().size(); ++i) {
        REQUIRE(std::equal(frames()[i].begin(), frames()[i].end(), out->samples[i].begin(),
                           out->samples[i].end()));
    }
    // The rest of what the track declares is as it is without them.
    REQUIRE(out->track.codec_id == mp4::kCodecAc4);
    REQUIRE(out->track.codec_config.payload == track.codec_config);
}

TEST_CASE("MP4 user boxes change the size of moov and nothing else", "[mp4][user_box]") {
    const mp4::AudioTrack plain = ac4_track();
    mp4::AudioTrack with_boxes = plain;
    with_boxes.user_boxes = two_boxes();

    const auto without = mp4::mux(plain, frames());
    const auto with = mp4::mux(with_boxes, frames());
    REQUIRE(without.has_value());
    REQUIRE(with.has_value());

    // A box is a size, the type, the 16-byte usertype and the payload.
    const std::size_t added = (8 + 16 + 300) + (8 + 16 + 0);
    REQUIRE(with->size() == without->size() + added);
    // A track without them has no 'uuid' box at all.
    REQUIRE(find_text(*without, "uuid") == std::string::npos);
    REQUIRE(find_text(*with, "uuid") != std::string::npos);

    // The media data, which follows moov, is the same bytes, and the chunk offsets moved with it.
    const auto a = mp4::demux(*without);
    const auto b = mp4::demux(*with);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->samples.size() == b->samples.size());
    for (std::size_t i = 0; i < a->samples.size(); ++i) {
        REQUIRE(std::equal(a->samples[i].begin(), a->samples[i].end(), b->samples[i].begin(),
                           b->samples[i].end()));
    }
    REQUIRE(a->track.user_boxes.empty());
}

TEST_CASE("MP4 user boxes sit in the trak, after the mdia, in the order given", "[mp4][user_box]") {
    mp4::AudioTrack track = ac4_track();
    track.user_boxes = two_boxes();
    const auto file = mp4::mux(track, frames());
    REQUIRE(file.has_value());

    const std::size_t trak = find_text(*file, "trak") - 4;
    const std::size_t trak_end = trak + get_u32(*file, trak);
    const std::size_t mdia = find_text(*file, "mdia") - 4;
    const std::size_t mdia_end = mdia + get_u32(*file, mdia);
    REQUIRE(mdia > trak);
    // The first user box starts where mdia ends, and the second where the first ends; the last
    // ends where the trak does.
    const std::size_t first_size = 8 + 16 + 300;
    REQUIRE(find_text(*file, "uuid") == mdia_end + 4);
    REQUIRE(get_u32(*file, mdia_end) == first_size);
    REQUIRE(get_u32(*file, mdia_end + first_size) == 8 + 16);
    REQUIRE(mdia_end + first_size + 8 + 16 == trak_end);
    // The first box's usertype follows its type.
    REQUIRE(std::equal(track.user_boxes[0].usertype.begin(), track.user_boxes[0].usertype.end(),
                       file->begin() + static_cast<std::ptrdiff_t>(mdia_end + 8)));
}

TEST_CASE("MP4 user boxes round-trip through fragment() and FragmentWriter", "[mp4][user_box]") {
    mp4::AudioTrack track = ac4_track();
    track.user_boxes = two_boxes();

    const auto batch = mp4::fragment(track, frames());
    REQUIRE(batch.has_value());
    // The init segment carries the boxes; the media segments do not.
    const auto init = mp4::demux(batch->init_segment);
    REQUIRE(init.has_value());
    require_same_boxes(init->track.user_boxes, track.user_boxes);
    for (const mp4::MediaSegment& segment : batch->media_segments) {
        REQUIRE(find_text(segment.bytes, "uuid") == std::string::npos);
    }

    // The whole fragmented file reads the same.
    Bytes whole = batch->init_segment;
    for (const mp4::MediaSegment& segment : batch->media_segments) {
        whole.insert(whole.end(), segment.bytes.begin(), segment.bytes.end());
    }
    const auto all = mp4::demux(whole);
    REQUIRE(all.has_value());
    require_same_boxes(all->track.user_boxes, track.user_boxes);
    REQUIRE(all->samples.size() == frames().size());

    // The live writer's init segment has them too.
    const auto writer = mp4::FragmentWriter::create(track);
    REQUIRE(writer.has_value());
    const auto live = mp4::demux(writer->init_segment());
    REQUIRE(live.has_value());
    require_same_boxes(live->track.user_boxes, track.user_boxes);
}

TEST_CASE("MP4 user boxes read back through the incremental Reader and demux_seekable()",
          "[mp4][user_box]") {
    mp4::AudioTrack track = ac4_track();
    track.user_boxes = two_boxes();
    const auto file = mp4::mux(track, frames());
    REQUIRE(file.has_value());

    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{5}, std::size_t{64}, file->size()}) {
        INFO("chunk size " << chunk);
        require_same_boxes(read_in_chunks(*file, chunk).user_boxes, track.user_boxes);
    }
    require_same_boxes(read_seekable(*file).user_boxes, track.user_boxes);
}

TEST_CASE("MP4 refuses user boxes beyond the bounds the header names", "[mp4][user_box]") {
    mp4::AudioTrack track = ac4_track();

    SECTION("exactly at the bounds is fine") {
        for (std::size_t i = 0; i < mp4::kMaxUserBoxes; ++i) {
            track.user_boxes.push_back(
                user_box(static_cast<std::uint8_t>(i),
                         bytes_of(i == 0 ? mp4::kMaxUserBoxPayloadBytes : std::size_t{3},
                                  static_cast<std::uint8_t>(i))));
        }
        const auto file = mp4::mux(track, frames());
        REQUIRE(file.has_value());
        const auto out = mp4::demux(*file);
        REQUIRE(out.has_value());
        require_same_boxes(out->track.user_boxes, track.user_boxes);
    }
    SECTION("one box too many") {
        for (std::size_t i = 0; i <= mp4::kMaxUserBoxes; ++i) {
            track.user_boxes.push_back(user_box(static_cast<std::uint8_t>(i), bytes_of(1, 1)));
        }
        REQUIRE(mp4::mux(track, frames()).error() == mp4::MuxError::kInvalidTrack);
        REQUIRE(mp4::fragment(track, frames()).error() == mp4::MuxError::kInvalidTrack);
        REQUIRE(mp4::FragmentWriter::create(track).error() == mp4::MuxError::kInvalidTrack);
    }
    SECTION("a payload one byte too large") {
        track.user_boxes.push_back(user_box(1, bytes_of(mp4::kMaxUserBoxPayloadBytes + 1, 1)));
        REQUIRE(mp4::mux(track, frames()).error() == mp4::MuxError::kInvalidTrack);
        REQUIRE(mp4::fragment(track, frames()).error() == mp4::MuxError::kInvalidTrack);
        REQUIRE(mp4::FragmentWriter::create(track).error() == mp4::MuxError::kInvalidTrack);
    }
}

TEST_CASE("MP4 reader keeps the user boxes it can and ignores the rest", "[mp4][user_box]") {
    const std::vector<mp4::UserBox> base = {user_box(0x31, bytes_of(40, 0x31))};
    const Bytes init = init_segment_with(base);
    {
        const auto plain = mp4::demux(init);
        REQUIRE(plain.has_value());
        require_same_boxes(plain->track.user_boxes, base);
    }

    SECTION("a ninth box in the trak is dropped, the first eight kept") {
        std::vector<mp4::UserBox> eight;
        for (std::size_t i = 0; i < mp4::kMaxUserBoxes; ++i) {
            eight.push_back(user_box(static_cast<std::uint8_t>(0x40 + i),
                                     bytes_of(2, static_cast<std::uint8_t>(i))));
        }
        const Bytes file = grow(init_segment_with(eight), {"moov", "trak"},
                                uuid_box(user_box(0x99, bytes_of(2, 0x99))));
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        require_same_boxes(out->track.user_boxes, eight);
    }
    SECTION("a payload of exactly the bound is kept") {
        const mp4::UserBox big = user_box(0x51, bytes_of(mp4::kMaxUserBoxPayloadBytes, 0x51));
        const Bytes file = grow(init, {"moov", "trak"}, uuid_box(big));
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(out->track.user_boxes.size() == 2);
        require_same_boxes({out->track.user_boxes[1]}, {big});
    }
    SECTION("a payload one byte past the bound is left out, and the file reads on") {
        const mp4::UserBox big = user_box(0x52, bytes_of(mp4::kMaxUserBoxPayloadBytes + 1, 0x52));
        const Bytes file = grow(init, {"moov", "trak"}, uuid_box(big));
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        require_same_boxes(out->track.user_boxes, base);
        REQUIRE(out->track.codec_id == mp4::kCodecAc4);
    }
    SECTION("a box too short to hold a usertype is left out") {
        const Bytes file = grow(init, {"moov", "trak"}, uuid_box_of_body(bytes_of(15, 0x61)));
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        require_same_boxes(out->track.user_boxes, base);
    }
    SECTION("a box of exactly a usertype has an empty payload") {
        const Bytes file = grow(init, {"moov", "trak"}, uuid_box_of_body(bytes_of(16, 0x62)));
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(out->track.user_boxes.size() == 2);
        REQUIRE(out->track.user_boxes[1].payload.empty());
    }
    SECTION("a uuid box that is not a direct child of the trak is not reported") {
        const Bytes stray = uuid_box(user_box(0x71, bytes_of(8, 0x71)));
        // Inside moov, beside the trak.
        const Bytes in_moov = grow(init, {"moov"}, stray);
        // Inside mdia, a level below the trak.
        const Bytes in_mdia = grow(init, {"moov", "trak", "mdia"}, stray);
        for (const Bytes* file : {&in_moov, &in_mdia}) {
            const auto out = mp4::demux(*file);
            REQUIRE(out.has_value());
            require_same_boxes(out->track.user_boxes, base);
        }
    }
    SECTION(
        "a uuid box claiming more bytes than the file has does not make the reader allocate or "
        "crash") {
        Bytes liar = uuid_box(user_box(0x81, bytes_of(8, 0x81)));
        put_u32(liar, 0, 0x7FFFFFF0U);
        const Bytes file = grow(init, {"moov", "trak"}, liar);
        const auto out = mp4::demux(file);
        // Either the file no longer reads or it reads without the box; it never reads with it.
        if (out.has_value()) {
            require_same_boxes(out->track.user_boxes, base);
        }
    }
}
