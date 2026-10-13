#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sink/decoding_output.hpp"

// A test sink's output for _iclforge_player@v1 (planning/hearth-sendspin-extension.md): each
// stream's bursts decoded, AC-3 or E-AC-3 with any object layer, or AC-4, and rendered to the
// sink's speaker layout by iclforge::render, to a float WAV file with one channel per slot and a
// play-time log beside it. The log's lines are `local_time_us,first_frame,frames` for each burst,
// first_frame being the burst's first decoded sample counted from the stream's start, and
// `clear,<frames>` where stream/clear dropped what was buffered, as WavOutput's are for player@v1.
// With no directory it decodes and counts.
//
// The decode is libs/sink's (iclforge::sink::DecodingBurstOutput, which says how and what it is
// held to); what is here is where its samples go.

namespace iclforge::hearth::testsink {

// The test sink's PcmSink: a float WAV file and a play-time log per stream, named
// `<prefix>-<stream number>.wav` and `.times.csv`. With no directory it takes nothing and keeps
// no file.
class WavPcmSink final : public sink::PcmSink {
   public:
    // `prefix` starts every file's name.
    WavPcmSink(std::filesystem::path directory, std::string prefix);
    ~WavPcmSink() override;

    bool open(std::uint32_t sample_rate, std::uint16_t channels) override;
    [[nodiscard]] bool taking() const override { return writer_.is_open(); }
    void burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t frames) override;
    void write(std::span<const float> interleaved) override;
    void clear(std::uint64_t frames) override;
    void close() override;
    [[nodiscard]] const std::filesystem::path& file() const override { return file_; }

   private:
    std::filesystem::path directory_;
    std::string prefix_;
    ac3::io::WavStreamWriter writer_;
    std::ofstream log_;
    std::filesystem::path file_;
    std::uint32_t streams_ = 0;
};

// The decode with that sink.
class BurstOutput final : public sink::DecodingBurstOutput {
   public:
    // `prefix` starts every file's name.
    BurstOutput(std::filesystem::path directory, std::string prefix, const render::OutputLayout& layout);
};

using sink::ac4_bed;
using sink::ac4_placeable;

}  // namespace iclforge::hearth::testsink
