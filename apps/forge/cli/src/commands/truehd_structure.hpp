#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

namespace iclforge::apps {
class JsonSink;
}  // namespace iclforge::apps

// `forge probe detail=structure`: the stream-level syntax facts that the owner's specification
// leaves open and a disc settles (libs/truehd/ERRATA.md, open questions 2, 3, 10 and 11), as counts
// and field values from the headers, never audio and never content.
//
//   - The major syncs, distinct ones only: the channel meaning's extension and the size the CRC
//   found
//     for it (question 2), `substream_info` and `extended_substream_info`.
//   - The restart headers of each substream: the sync, the 16 bits that end the fixed part (where
//     heavy DRC would sit, question 10) as a histogram, the noise shift, the data check flag and
//     the ranges of the fields the decoder does not need.
//   - A decode of the whole stream (every substream, lenient): whether each restart interval's
//     lossless check passes, how many units fail or wait, how many matrices send LSBs through
//     lsb_bypass (question 11: if the reading of the bypass is wrong, the checks fail), and what
//     follows the last block of each substream's segments (question 3: terminatorA, terminatorB and
//     the end-of-stream marker).
//
// The decode is the work of `forge decode`; its samples are discarded.

namespace forge_cli::commands {

class StructureSurvey {
   public:
    // One parsed access unit (the light pass: major syncs and restart headers).
    void add(std::uint64_t unit_index, const iclforge::truehd::AccessUnit& unit);
    // The decode pass over the whole stream.
    void decode(std::span<const std::byte> stream);

    void write_json(iclforge::apps::JsonSink& json) const;
    void print_text() const;

   private:
    struct MajorSyncSummary {
        std::string description;
        std::uint64_t count = 0;
        std::uint64_t first_unit = 0;
        bool extra_present = false;
        std::uint64_t size_bytes = 0;
        std::uint64_t extra_length = 0;
        std::uint64_t substream_info = 0;
        std::uint64_t extended_substream_info = 0;
        std::uint64_t substreams = 0;
        std::uint64_t sample_rate = 0;
        std::uint64_t channel_count = 0;
        std::uint64_t dyn_object_only = 0;
        std::uint64_t lfe_present = 0;
        std::uint64_t content_description = 0;
        std::uint64_t channel_assignment = 0;
        std::uint64_t isf = 0;
        std::uint64_t dynamic_object_count = 0;
    };
    struct Range {
        std::uint64_t min = 0;
        std::uint64_t max = 0;
        bool seen = false;
        void add(std::uint64_t value) {
            min = seen ? (value < min ? value : min) : value;
            max = seen ? (value > max ? value : max) : value;
            seen = true;
        }
    };
    struct RestartTally {
        std::uint64_t count = 0;
        std::map<std::string, std::uint64_t> failures{};
        std::map<std::uint16_t, std::uint64_t> syncs{};
        std::map<std::uint16_t, std::uint64_t> tails{};
        std::uint64_t noise_shift_nonzero = 0;
        std::uint64_t data_check_present = 0;
        Range min_chan{};
        Range max_chan{};
        Range max_matrix_chan{};
        Range max_shift{};
        Range max_lsbs{};
        Range max_bits{};
        Range max_bits_repeat{};
    };

    std::vector<MajorSyncSummary> major_syncs_{};
    std::uint64_t major_syncs_other_ = 0;
    std::array<RestartTally, iclforge::truehd::kMaxSubstreams> restarts_{};
    bool decoded_ = false;
    iclforge::truehd::DecoderCounters counters_{};
    std::map<std::string, std::uint64_t> decode_errors_{};
    std::uint64_t samples_ = 0;
    std::uint64_t padding_samples_ = 0;
};

}  // namespace forge_cli::commands
