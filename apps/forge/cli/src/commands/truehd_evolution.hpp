#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "iclforge/truehd/io/access_unit.hpp"

namespace iclforge::apps {
class JsonSink;
}  // namespace iclforge::apps

// What `forge probe` says about the evolution frames in the extra data of a TrueHD stream's access
// units: how many units carry one, whether the extra data's check nibble, framing and parity pass,
// which payload ids the frames hold, what the object audio metadata (payload id 11) says through
// iclforge::objects' reader (the object count, the bed and the dynamic objects), and how often a
// frame comes. Metadata only: no payload byte is printed except the first four of the first frame,
// which tell a bit-level container from a Thrift one.
//
// PROVISIONAL, owner to confirm (libs/truehd/ERRATA.md, "Atmos, spec v3.6", question 20). The
// frames are read as TH spec v3.6 §15.4 under both readings the library has (the EMDF container's
// and the letter of §15.4), and the report says which one read each. Nothing is verified: the
// protection bytes (§15.4) and any signature (§16.3, §16.6) are returned by the library and not
// checked, and the report says so. This is the check to run on a real Atmos track, and its output
// is what the owner is asked to send back.

namespace forge_cli::commands {

class EvolutionSurvey {
   public:
    // One parsed access unit, `unit_index` counting the units that parsed, from 0.
    void add(std::uint64_t unit_index, const iclforge::truehd::AccessUnit& unit);

    // True when at least one unit has an extra data region.
    [[nodiscard]] bool any() const noexcept { return regions_ != 0; }

    // The value of the "evolution" member: an object.
    void write_json(iclforge::apps::JsonSink& json) const;
    // The lines under the "evolution" label of the text report, none when there is no region.
    void print_text() const;

   private:
    struct ReadingTally {
        std::uint64_t frames = 0;  // regions that read as evo_frame()
        std::uint64_t clean = 0;  // of them, with a payload and only zero bits after the protection
        std::map<std::string, std::uint64_t> errors{};  // the failures, by kind
    };
    struct ProgramTally {
        std::uint64_t object_count = 0;
        bool dynamic_only = false;
        bool lfe = false;
        std::uint64_t bed_assignment = 0;  // Table 12's flags as iclforge::objects holds them
        bool bed_nonstandard = false;      // the first bed instance used Table 13's 17 flags
        std::uint64_t extra_bed_instances = 0;
        std::vector<std::string> bed_channels{};
        std::int64_t isf_index = -1;
        std::uint64_t dynamic_objects = 0;
        std::uint64_t update_blocks = 0;
        std::uint64_t frames = 0;
    };

    std::uint64_t regions_ = 0;
    std::uint64_t check_ok_ = 0;
    std::uint64_t framing_ok_ = 0;
    std::uint64_t parity_ok_ = 0;
    std::uint64_t reserved_zero_ = 0;
    std::uint64_t length_fits_ = 0;
    std::array<ReadingTally, 2> readings_{};  // EMDF, then the letter of §15.4
    std::uint64_t frames_read_ = 0;           // by either
    std::map<std::uint64_t, std::uint64_t> payload_ids_{};
    std::map<std::uint64_t, std::uint64_t> primary_codes_{};
    std::map<std::uint64_t, std::uint64_t> secondary_codes_{};
    std::uint64_t oamd_min_bytes_ = 0;
    std::uint64_t oamd_max_bytes_ = 0;
    std::uint64_t oamd_payloads_ = 0;
    std::uint64_t oamd_unread_ = 0;
    std::uint64_t oamd_other_programs_ = 0;
    std::vector<ProgramTally> programs_{};
    std::uint64_t first_unit_ = 0;
    std::uint64_t last_unit_ = 0;
    std::uint64_t gap_min_ = 0;
    std::uint64_t gap_max_ = 0;
    std::string lead_hex_{};
};

}  // namespace forge_cli::commands
