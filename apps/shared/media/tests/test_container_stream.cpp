#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "byte_source.hpp"
#include "container_input.hpp"
#include "container_stream.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "multi_track_files.hpp"

// container_stream.hpp: a container's audio track read where it lies, a window at a time. The
// streamed reading must give what elementary_stream_from_bytes gives for the same file, so the
// first cases hold the two side by side on files with two tracks, and the last one reads a file
// sixteen times the memory its source is allowed to hand out at once.

namespace {

namespace mt = iclforge::test::multi_track;
using Bytes = std::vector<std::byte>;
using iclforge::apps::CodecSet;
using iclforge::apps::ContainerKind;
using iclforge::apps::TrackCodec;

Bytes read_stream(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    Bytes bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

Bytes joined(const std::vector<std::span<const std::byte>>& units) {
    Bytes out;
    for (const auto& unit : units) {
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

// A source that remembers the largest read it was asked for, and refuses any past `budget`: a
// reader that asks for more than its share of the file has not read it in windows.
class BudgetedSource final : public iclforge::apps::ByteSource {
public:
    BudgetedSource(iclforge::apps::ByteSource& inner, std::size_t budget)
        : inner_(inner), budget_(budget) {}

    [[nodiscard]] std::uint64_t size() const override { return inner_.size(); }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        largest = std::max(largest, out.size());
        ++reads;
        if (out.size() > budget_) {
            ++refused;
            return 0;
        }
        return inner_.read(offset, out);
    }

    std::size_t largest = 0;
    std::size_t refused = 0;
    std::uint64_t reads = 0;

private:
    iclforge::apps::ByteSource& inner_;
    std::size_t budget_;
};

struct Streams {
    Bytes truehd_bytes =
        read_stream(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/truehd_m51_s24.thd");
    Bytes eac3_bytes = read_stream(std::string{ICLFORGE_GOLDEN_AUDIO_DIR} +
                                   "/reference_51_eac3_448k_cplbndstrce0.ec3");
    mt::Track truehd{};
    mt::Track eac3{};
    Bytes truehd_stream{};
    Bytes eac3_stream{};

    Streams() {
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
    Streams(const Streams&) = delete;
    Streams& operator=(const Streams&) = delete;
};

using Build = Bytes (*)(std::span<const mt::Track>);

struct ContainerCase {
    const char* name;
    Build build;
    ContainerKind kind;
};

constexpr ContainerCase kReadInWindows[] = {
    {"Matroska", &mt::matroska_file, ContainerKind::kMatroska},
    {"MP4", &mt::mp4_file, ContainerKind::kMp4},
};

// What the two readings of one file must agree on.
void check_same(const iclforge::apps::StreamedTrack& streamed,
                iclforge::apps::ByteSource& source,
                const iclforge::apps::ElementaryStreamResult& whole) {
    CHECK(streamed.error == whole.error);
    CHECK(streamed.declined_codec == whole.declined_codec);
    CHECK(streamed.codec == whole.codec);
    CHECK(streamed.trim.start == whole.trim.start);
    CHECK(streamed.trim.length == whole.trim.length);
    CHECK(streamed.trim_note == whole.trim_note);
    CHECK(streamed.container.kind == whole.container.kind);
    CHECK(streamed.container.codec_id == whole.container.codec_id);
    CHECK(streamed.container.track == whole.container.track);
    CHECK(streamed.container.language == whole.container.language);
    CHECK(streamed.container.sample_rate == whole.container.sample_rate);
    CHECK(streamed.container.channels == whole.container.channels);
    REQUIRE(streamed.container.other_tracks.size() == whole.container.other_tracks.size());
    for (std::size_t i = 0; i < whole.container.other_tracks.size(); ++i) {
        CHECK(streamed.container.other_tracks[i].id == whole.container.other_tracks[i].id);
        CHECK(streamed.container.other_tracks[i].codec_id ==
              whole.container.other_tracks[i].codec_id);
    }
    if (whole.error.empty()) {
        CHECK(streamed.container.samples == whole.container.samples);
        CHECK(iclforge::apps::read_track_bytes(source, streamed) == whole.bytes);
    }
}

}  // namespace

TEST_CASE("index_container_track finds the track elementary_stream_from_bytes takes",
          "[containers][stream]") {
    const Streams streams;
    const std::array<CodecSet, 4> sets{CodecSet{}, CodecSet{.truehd = true},
                                       CodecSet{.eac3 = false, .truehd = true},
                                       CodecSet{.ac3 = false, .eac3 = false, .ac4 = false,
                                                .truehd = true}};
    for (const ContainerCase& container : kReadInWindows) {
        SECTION(container.name) {
            const std::array<mt::Track, 2> truehd_first{streams.truehd, streams.eac3};
            const std::array<mt::Track, 2> eac3_first{streams.eac3, streams.truehd};
            const std::array<mt::Track, 1> truehd_only{streams.truehd};
            const std::array<mt::Track, 1> eac3_only{streams.eac3};
            const std::vector<Bytes> files{container.build(truehd_first),
                                           container.build(eac3_first),
                                           container.build(truehd_only),
                                           container.build(eac3_only)};
            for (const Bytes& file : files) {
                for (const CodecSet& set : sets) {
                    iclforge::apps::MemoryByteSource source{Bytes{file}};
                    const auto streamed = iclforge::apps::index_container_track(source, set);
                    CHECK(streamed.container.kind == container.kind);
                    check_same(streamed, source, iclforge::apps::elementary_stream_from_bytes(file, set));
                }
            }
        }
    }
}

TEST_CASE("index_container_track reads the same track whatever the window", "[containers][stream]") {
    const Streams streams;
    const std::array<mt::Track, 2> truehd_first{streams.truehd, streams.eac3};
    const Bytes file = mt::matroska_file(truehd_first);
    // A window smaller than a frame, one that cuts frames in two, and one that holds the file.
    for (const std::size_t window : {std::size_t{4096}, std::size_t{5000}, std::size_t{1} << 20}) {
        iclforge::apps::MemoryByteSource source{Bytes{file}};
        const auto streamed = iclforge::apps::index_container_track(
            source, CodecSet{.truehd = true}, {.window_bytes = window});
        REQUIRE(streamed.error.empty());
        CHECK(iclforge::apps::read_track_bytes(source, streamed) == streams.truehd_stream);
    }
}

TEST_CASE("index_container_track leaves a bare stream and a transport stream alone",
          "[containers][stream]") {
    const Streams streams;
    iclforge::apps::MemoryByteSource bare{Bytes{streams.truehd_bytes}};
    const auto bare_track = iclforge::apps::index_container_track(bare, CodecSet{.truehd = true});
    CHECK(bare_track.bare);
    CHECK_FALSE(bare_track.whole_file);
    CHECK(bare_track.extents.size() == 0);

    const std::array<mt::Track, 1> track{streams.truehd};
    iclforge::apps::MemoryByteSource ts{mt::mpegts_file(track)};
    const auto ts_track = iclforge::apps::index_container_track(ts, CodecSet{.truehd = true});
    CHECK(ts_track.whole_file);
    CHECK_FALSE(ts_track.bare);
    CHECK(ts_track.container.kind == ContainerKind::kMpegTs);
}

TEST_CASE("an ExtentMap joins what follows on and reads across its runs", "[containers][stream]") {
    iclforge::apps::ExtentMap map;
    map.add(100, 10);
    map.add(110, 5);  // follows on: one run
    map.add(200, 20);
    map.add(220, 4, false);  // follows on, but kept apart
    CHECK(map.runs() == 3);
    CHECK(map.size() == 39);

    Bytes file(300);
    for (std::size_t i = 0; i < file.size(); ++i) {
        file[i] = static_cast<std::byte>(i & 0xFF);
    }
    iclforge::apps::MemoryByteSource source{file};

    // The whole stream, then a read that starts inside a run and ends inside another.
    Bytes all(39);
    REQUIRE(map.read(source, 0, all) == 39);
    Bytes expected;
    for (const auto [from, count] : {std::pair{100, 15}, std::pair{200, 20}, std::pair{220, 4}}) {
        expected.insert(expected.end(), file.begin() + from, file.begin() + from + count);
    }
    CHECK(all == expected);

    Bytes middle(10);
    REQUIRE(map.read(source, 12, middle) == 10);
    CHECK(middle == Bytes(expected.begin() + 12, expected.begin() + 22));

    // The end of the stream cuts a read short, and nothing lies past it.
    Bytes tail(10);
    CHECK(map.read(source, 35, tail) == 4);
    CHECK(map.read(source, 39, tail) == 0);

    // A file that stops short of what the map says ends the read there.
    iclforge::apps::MemoryByteSource short_file{Bytes(file.begin(), file.begin() + 205)};
    Bytes cut(39);
    CHECK(map.read(short_file, 0, cut) == 20);
}

namespace {

// A Matroska file of `clusters` identical clusters, made on request and never held: a TrueHD
// track and a video track, a block of each in turn. The audio is the units of a real stream, in
// order, repeated.
class GeneratedMatroska final : public iclforge::apps::ByteSource {
public:
    GeneratedMatroska(const Bytes& units_stream, std::size_t clusters, std::size_t groups)
        : clusters_(clusters) {
        using namespace mt::detail;
        const auto units = mt::split_truehd(units_stream);
        units_ = Bytes{units_stream};
        Bytes entries;
        {
            Bytes audio;
            Bytes rate;
            put_u32(rate, std::bit_cast<std::uint32_t>(48000.0F));
            put_bytes(audio, ebml(0xB5, rate));
            put_bytes(audio, ebml_uint(0x9F, 6));
            Bytes entry;
            put_bytes(entry, ebml_uint(0xD7, 1));
            put_bytes(entry, ebml_uint(0x83, 2));
            put_bytes(entry, ebml_text(0x86, "A_TRUEHD"));
            put_bytes(entry, ebml(0xE1, audio));
            put_bytes(entries, ebml(0xAE, entry));
        }
        {
            Bytes entry;
            put_bytes(entry, ebml_uint(0xD7, 2));
            put_bytes(entry, ebml_uint(0x83, 1));  // video
            put_bytes(entry, ebml_text(0x86, "V_MPEG4/ISO/AVC"));
            put_bytes(entries, ebml(0xAE, entry));
        }
        Bytes head;
        put_bytes(head, ebml(0x1A45DFA3, ebml_text(0x4282, "matroska")));
        // A Segment of unknown size, as a live recording writes it.
        put_u32(head, 0x18538067);
        put_u8(head, 0x01);
        for (int i = 0; i < 7; ++i) {
            put_u8(head, 0xFF);
        }
        put_bytes(head, ebml(0x1654AE6B, entries));
        prefix_ = std::move(head);

        Bytes body;
        put_bytes(body, ebml_uint(0xE7, 0));
        for (std::size_t g = 0; g < groups; ++g) {
            const auto unit = units[g % units.size()];
            Bytes audio;
            put_u8(audio, 0x81);
            put_u16(audio, 0);
            put_u8(audio, 0x80);
            put_bytes(audio, unit);
            put_bytes(body, ebml(0xA3, audio));
            audio_in_cluster_ += unit.size();
            Bytes video;
            put_u8(video, 0x82);
            put_u16(video, 0);
            put_u8(video, 0x80);
            video.resize(video.size() + 4000, static_cast<std::byte>(0x5A));
            put_bytes(body, ebml(0xA3, video));
        }
        cluster_ = ebml(0x1F43B675, body);
        groups_ = groups;
        unit_sizes_.reserve(groups);
        for (std::size_t g = 0; g < groups; ++g) {
            unit_sizes_.push_back(units[g % units.size()].size());
        }
        stream_cluster_.reserve(audio_in_cluster_);
        for (std::size_t g = 0; g < groups; ++g) {
            const auto unit = units[g % units.size()];
            stream_cluster_.insert(stream_cluster_.end(), unit.begin(), unit.end());
        }
    }

    [[nodiscard]] std::uint64_t size() const override {
        return prefix_.size() + clusters_ * cluster_.size();
    }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        std::size_t done = 0;
        while (done < out.size() && offset + done < size()) {
            const std::uint64_t at = offset + done;
            if (at < prefix_.size()) {
                const std::size_t n = std::min<std::size_t>(out.size() - done, prefix_.size() - at);
                std::copy_n(prefix_.begin() + static_cast<std::ptrdiff_t>(at), n, out.begin() + static_cast<std::ptrdiff_t>(done));
                done += n;
            } else {
                const std::uint64_t into = (at - prefix_.size()) % cluster_.size();
                const std::size_t n = std::min<std::size_t>(out.size() - done, cluster_.size() - into);
                std::copy_n(cluster_.begin() + static_cast<std::ptrdiff_t>(into), n, out.begin() + static_cast<std::ptrdiff_t>(done));
                done += n;
            }
        }
        return done;
    }

    [[nodiscard]] std::uint64_t audio_bytes() const { return clusters_ * audio_in_cluster_; }
    [[nodiscard]] std::uint64_t frames() const { return clusters_ * groups_; }
    // The track's stream at `offset`, worked out from the pattern.
    [[nodiscard]] Bytes track_at(std::uint64_t offset, std::size_t count) const {
        Bytes out(count);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = stream_cluster_[static_cast<std::size_t>((offset + i) % audio_in_cluster_)];
        }
        return out;
    }

private:
    std::size_t clusters_;
    std::size_t groups_ = 0;
    Bytes units_;
    Bytes prefix_;
    Bytes cluster_;
    Bytes stream_cluster_;
    std::vector<std::size_t> unit_sizes_;
    std::uint64_t audio_in_cluster_ = 0;
};

}  // namespace

// A film's TrueHD track does not fit in memory with the file around it. This file is 160 MiB, which
// the source below would not hand over in one read (its budget is 10 MiB) even if the reader asked,
// so it is read the way a 40 GB one is, and the track comes out whole.
TEST_CASE("a container ten times the source's budget is read in windows", "[containers][stream][large]") {
    const Streams streams;
    constexpr std::size_t kBudget = 10U << 20;
    GeneratedMatroska generated{streams.truehd_bytes, 160, 220};
    REQUIRE(generated.size() > 10 * kBudget);

    BudgetedSource source{generated, kBudget};
    const auto track = iclforge::apps::index_container_track(source, CodecSet{.truehd = true});
    REQUIRE(track.error.empty());
    CHECK(track.codec == TrackCodec::kTruehd);
    CHECK(track.container.samples == generated.frames());
    CHECK(track.extents.size() == generated.audio_bytes());
    // One run for each audio block: the video between them keeps them from joining.
    CHECK(track.extents.runs() == generated.frames());
    CHECK(source.refused == 0);
    CHECK(source.largest <= std::size_t{1} << 20);

    // The track reads back by its own offsets, from anywhere in it, without the file in memory.
    auto shared = std::make_shared<BudgetedSource>(generated, kBudget);
    iclforge::apps::ExtentSource stream{shared, track.extents};
    CHECK(stream.size() == generated.audio_bytes());
    for (const std::uint64_t at : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{70000},
                                    generated.audio_bytes() / 2 + 3,
                                    generated.audio_bytes() - 5000}) {
        Bytes got(5000);
        const std::size_t n = stream.read(at, got);
        got.resize(n);
        CHECK(got == generated.track_at(at, n));
    }
    CHECK(shared->refused == 0);
}
