#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "byte_source.hpp"
#include "container_input.hpp"

// A container's audio track read where it lies, a window at a time, for a file that does not fit
// in memory (planning/truehd-parity-hearth-player.md row 5 and question 9: a two hour remux's
// TrueHD track alone is about 4.5 GB, and elementary_stream_from_bytes holds the file and then a
// copy of the track).
//
// The container readers say where each frame of the track lies in the file. This keeps those places
// (ExtentMap) and nothing of the frames, and ExtentSource reads the track back as one stream by its
// own offsets, as the file's bytes between the frames were not there. An item that plays the track
// reads it through a ByteSource like a bare stream, so everything that reads a bare TrueHD file in
// windows reads a container's track the same way.
//
// Matroska and MP4 are read this way. MPEG-TS is not: its reader rebuilds a PES packet from the
// payloads of many transport packets, so a payload is not one run of bytes in the file, and the
// reader has no offsets to give. index_container_track says so (StreamedTrack::whole_file), and a
// caller that plays a transport stream reads the file whole.

namespace iclforge::apps {

// The runs of a file that make up a track, in order. Frames that follow one another in the file
// are one run, so a track a muxer writes without other tracks in between is a handful of runs and
// an interleaved one is a run for each frame (12 bytes of memory each at most; a TrueHD frame in a
// remux is one access unit or a few).
class ExtentMap {
public:
    // Appends `length` bytes at `file_offset`. With `join`, a run that begins where the last one
    // ended extends it; without, every call is a run of its own (a caller that must tell the
    // frames apart afterwards, as AC-4 in MP4 needs).
    void add(std::uint64_t file_offset, std::size_t length, bool join = true);

    // The bytes of the stream, and the runs they lie in.
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] std::size_t runs() const { return runs_.size(); }

    struct Run {
        std::uint64_t stream_offset = 0;
        std::uint64_t file_offset = 0;
        std::uint32_t length = 0;
    };
    [[nodiscard]] const std::vector<Run>& all() const { return runs_; }

    // Copies up to out.size() bytes of the stream from `offset` out of `file`; fewer only at the
    // end of the stream, or where the file stops short of what it was mapped to.
    [[nodiscard]] std::size_t read(ByteSource& file, std::uint64_t offset,
                                   std::span<std::byte> out) const;

private:
    std::vector<Run> runs_;
    std::uint64_t size_ = 0;
};

// A track's stream, read from the file it lies in.
class ExtentSource final : public ByteSource {
public:
    ExtentSource(std::shared_ptr<ByteSource> file, ExtentMap map)
        : file_(std::move(file)), map_(std::move(map)) {}

    [[nodiscard]] std::uint64_t size() const override { return map_.size(); }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        return map_.read(*file_, offset, out);
    }

private:
    std::shared_ptr<ByteSource> file_;
    ExtentMap map_;
};

struct StreamedTrack {
    // As ElementaryStreamResult's: empty on success, a sentence when the file sniffed as a
    // container this build reads and could not be demuxed, or has audio only in a codec outside
    // the caller's set (then declined_codec is set).
    std::string error;
    std::optional<TrackCodec> declined_codec = std::nullopt;
    StreamTrim trim{};
    std::string trim_note;
    ContainerFacts container{};
    std::optional<TrackCodec> codec = std::nullopt;
    // Where the track lies. Empty for a file that is not a container, and for a transport stream.
    ExtentMap extents;
    // The file is a container this does not read in windows (MPEG-TS): the caller reads it whole
    // and uses elementary_stream_from_bytes.
    bool whole_file = false;
    // The file is not a container at all: its bytes are the stream.
    bool bare = false;
};

struct StreamOptions {
    // How much of the file is held at once while the container is read.
    std::size_t window_bytes = 1U << 20;
};

// Finds the first track of a codec in `codecs` in `file`, as elementary_stream_from_bytes would take
// it from the same bytes, reading the file once from front to back in windows (an MP4 is read by
// its sample table, wherever its moov lies). Memory is one window, the largest element the reader
// holds and the runs of the track.
[[nodiscard]] StreamedTrack index_container_track(ByteSource& file, const CodecSet& codecs = {},
                                                  const StreamOptions& options = {});

// The track's stream in memory, byte for byte what elementary_stream_from_bytes gives for the same
// file (AC-4 samples wrapped in sync frames). For a track that is small enough to hold; a TrueHD
// track is read through an ExtentSource instead.
[[nodiscard]] std::vector<std::byte> read_track_bytes(ByteSource& file, const StreamedTrack& track);

}  // namespace iclforge::apps
