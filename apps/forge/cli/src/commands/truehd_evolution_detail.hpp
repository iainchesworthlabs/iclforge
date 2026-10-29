#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/evolution.hpp"

namespace iclforge::apps {
class JsonSink;
}  // namespace iclforge::apps

// `forge probe detail=evolution`: the structure of the evolution frames of a TrueHD stream, in
// enough detail to settle the open questions about them from the output alone
// (libs/truehd/ERRATA.md, "Atmos, spec v3.6", questions 20 to 22 and 35). For the first frames, one
// by one: the extra data's check nibble, framing and parity, and the container's fields (version,
// key id, each payload's id, size and configuration flags, the protection length codes). For every
// distinct shape of frame (the list of payload ids, the protection length codes, the reading that
// parsed it): how many frames have it, where they are and how large their payloads are, which is
// what shows whether a signature comes with every frame or once. For a payload that is not the
// object audio metadata (id 11): a walk of it as a Thrift Binary Protocol struct with the
// repository's own reader (iclforge::base::crypto:: thrift), printing field ids, types and lengths
// only.
//
// Metadata only. No payload byte, certificate, digest, signature or protection byte is printed, and
// no value of a Thrift field: the walk gives the shape of the struct, not what is in it.
//
// PROVISIONAL, owner to confirm: the readings are the library's two (ERRATA question 21), and where
// the Thrift struct starts inside a payload is not known (question 20), so the walk tries offsets 0
// to 16 and reports the first at which a whole struct reads.

namespace forge_cli::commands {

class EvolutionDetail {
   public:
    // One unit's extra data. `frame` is the frame the survey chose (null when neither reading read
    // one) and `reading` the name of the reading that read it.
    void add(std::uint64_t unit_index, const iclforge::truehd::ExtraData& extra,
             const iclforge::truehd::EvolutionRead& emdf,
             const iclforge::truehd::EvolutionRead& letter,
             const iclforge::truehd::EvolutionFrame* frame, const char* reading);

    // The value of the "detail" member of the "evolution" object: an object.
    void write_json(iclforge::apps::JsonSink& json) const;
    // The lines of the text report (indented under the evolution label).
    void print_text() const;

    struct ThriftNode {
        std::int64_t id = 0;
        std::string type;
        std::string element;  // a list's or a set's element type, a map's value type
        std::string key;      // a map's key type
        bool has_count = false;
        std::uint64_t count = 0;  // a string's length, a list's or map's element count
        std::vector<ThriftNode>
            children{};  // a struct's fields; a list of structs: the first one's
    };
    struct ThriftOutline {
        bool found = false;
        std::size_t offset = 0;
        std::size_t consumed = 0;
        std::size_t payload_bytes = 0;
        bool capped = false;
        std::vector<ThriftNode> fields{};
    };
    struct PayloadShape {
        std::uint64_t id = 0;
        std::size_t bytes = 0;
        iclforge::truehd::EvolutionPayloadConfig config{};
        std::optional<ThriftOutline> thrift{};
    };

   private:
    struct FrameRecord {
        std::uint64_t unit = 0;
        std::uint64_t extra_data_length = 0;
        std::uint64_t evo_frame_byte_length = 0;
        bool check_ok = false;
        bool framing_ok = false;
        bool parity_ok = false;
        bool reserved_zero = false;
        bool length_fits = false;
        std::string reading;  // "emdf", "spec36" or "none"
        std::string emdf_error;
        std::string letter_error;
        std::uint64_t version = 0;
        std::uint64_t key_id = 0;
        std::vector<PayloadShape> payloads{};
        std::uint64_t primary_code = 0;
        std::uint64_t secondary_code = 0;
        std::size_t bits_used = 0;
        bool trailing_zero = true;
    };
    struct Shape {
        std::string reading;
        std::string unread;  // the errors of both readings when neither read the frame
        std::vector<std::uint64_t> ids{};
        std::uint64_t primary_code = 0;
        std::uint64_t secondary_code = 0;
        std::uint64_t frames = 0;
        std::uint64_t first_unit = 0;
        std::uint64_t last_unit = 0;
        std::vector<std::size_t> size_min{};  // per payload, in the frame's order
        std::vector<std::size_t> size_max{};
        std::vector<PayloadShape> example{};  // the first frame of the shape, with its walks
    };

    std::vector<FrameRecord> frames_{};
    std::vector<Shape> shapes_{};
    std::uint64_t frames_total_ = 0;
    std::uint64_t other_shape_frames_ = 0;
};

}  // namespace forge_cli::commands
