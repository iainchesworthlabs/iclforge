#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../adm/decode_adm.hpp"
#include "../support.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"

// The object layer of a TrueHD Atmos stream, read from substream 3 and the evolution frames
// (TH spec v3.9 §15). PROVISIONAL, OWNER TO CONFIRM (libs/truehd/ERRATA.md entry 61, questions 61
// and 62): no stream here has been checked against a licensed decoder.
//
// Substream 3 gives N channels. The object audio metadata of the stream's evolution frames
// (payload id 11, TS 103 420 clause 5.5) describes N objects in the order of TS 103 420 §5.6.4.8,
// beds first, then intermediate spatial format objects, then dynamic objects, and channel i of the
// substream is object i (TH spec §15.6, §15.16). A dynamic-object-only programme with an LFE has
// the LFE as object 0.

namespace forge_cli::commands {

enum class ObjectChannelKind : std::uint8_t {
    kBed,       // a speaker-anchored channel with a Table 12 label
    kLfe,       // the LFE, a bed channel
    kDynamic,   // a room-anchored object; its position comes from the OAMD update blocks
    kUnplaced,  // an intermediate spatial format object, or a bed the labels do not name
};

struct ObjectChannel {
    ObjectChannelKind kind = ObjectChannelKind::kUnplaced;
    std::optional<iclforge::objects::oba::BedLabel> label{};
    std::size_t dynamic_index = 0;  // kDynamic: the index into UpdateBlock::objects
};

// How the `channels` of substream 3 map onto the objects of `program`. `problem` says why the plan
// is not exact (the programme has another number of objects than the substream has channels, or
// uses intermediate spatial format, several bed instances or a non-standard bed assignment); the
// channels it could name are still named, the others are kUnplaced.
struct ObjectPlan {
    std::vector<ObjectChannel> channels;
    std::string problem;
};

[[nodiscard]] ObjectPlan plan_object_channels(const iclforge::objects::oba::Program& program,
                                              int channels);

class TrueHdObjectRenderer;

// Takes the decoded access units of substream 3 in order and writes what `decode objects_dir=` and
// `adm_out=` ask for: each channel as a mono WAV `object_NN.wav` (NN the channel, from 0), and a
// Dolby Atmos Master ADM BW64 with the beds as DirectSpeakers and the dynamic objects as Objects
// whose position blocks are the OAMD update blocks, timed from the access unit that carried them.
class TrueHdObjectSink {
   public:
    // With a `layout` the objects are also placed on its speakers (rendered(),
    // TrueHdObjectRenderer).
    TrueHdObjectSink(std::string_view objects_dir, std::string_view adm_out,
                     const std::optional<iclforge::render::OutputLayout>& layout = std::nullopt);
    ~TrueHdObjectSink();
    TrueHdObjectSink(TrueHdObjectSink&&) noexcept;
    TrueHdObjectSink& operator=(TrueHdObjectSink&&) noexcept;
    TrueHdObjectSink(const TrueHdObjectSink&) = delete;
    TrueHdObjectSink& operator=(const TrueHdObjectSink&) = delete;

    // False when a file could not be written (the message is on stderr).
    [[nodiscard]] bool add(const iclforge::truehd::DecodedAccessUnit& unit);

    // Closes the files, writes the ADM master and says what was found on `status` (null: nothing).
    // The exit code.
    [[nodiscard]] int finish(FILE* status);
    void abort();

    // The unit last added, rendered to the layout's speakers: interleaved 24 bit values, the slots
    // named by rendered_names(). Empty and 0 slots when no layout was given.
    [[nodiscard]] bool rendering() const noexcept { return renderer_ != nullptr; }
    [[nodiscard]] std::size_t rendered_slots() const noexcept;
    [[nodiscard]] const std::string& rendered_names() const noexcept;
    [[nodiscard]] std::span<const std::int32_t> rendered() const noexcept { return rendered_; }

    [[nodiscard]] std::uint64_t oamd_payloads() const noexcept { return oamd_payloads_; }
    [[nodiscard]] const ObjectPlan& plan() const noexcept { return plan_; }

   private:
    bool open_files(const iclforge::truehd::DecodedAccessUnit& unit);
    void read_metadata(const iclforge::truehd::DecodedAccessUnit& unit);

    std::string objects_dir_;
    std::string adm_out_;
    std::vector<PlanarWavSink> sinks_;
    bool opened_ = false;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t samples_ = 0;  // per channel, before the unit being added
    std::optional<iclforge::objects::oba::Program> program_;
    ObjectPlan plan_;
    bool plan_said_ = false;
    std::uint64_t oamd_payloads_ = 0;
    std::uint64_t oamd_unread_ = 0;
    std::uint64_t program_changes_ = 0;
    AdmMasterInput adm_;
    std::unique_ptr<TrueHdObjectRenderer> renderer_;
    std::vector<std::int32_t> rendered_;
};

// The ADM master of a presentation's bed alone: each channel a DirectSpeakers channel at the room
// position of its label (TH spec §15.20; no object decode). The presentation has to name its
// speakers, and every one has to have a Table 12 label (the 7.1 bed does: L R C LFE Ls Rs Lb Rb).
class TrueHdBedSink {
   public:
    explicit TrueHdBedSink(std::string_view adm_out) : adm_out_(adm_out) {}

    // False when the channels cannot be named (the message is on stderr).
    [[nodiscard]] bool add(const iclforge::truehd::DecodedAccessUnit& unit);
    [[nodiscard]] int finish(FILE* status);

   private:
    std::string adm_out_;
    AdmMasterInput adm_;
};

// The Table 12 label of a TrueHD channel (TH spec §13.1), if it has one.
[[nodiscard]] std::optional<iclforge::objects::oba::BedLabel> bed_label_of(
    iclforge::truehd::Channel channel) noexcept;

}  // namespace forge_cli::commands
