#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/containers/export.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// IAMF in ISO-BMFF (AOM IAMF v2.0.0, "ISO-BMFF IAMF Encapsulation"): an `iamf`-branded file whose
// one track holds the IA Sequence. The Descriptors are the configOBUs of the `iacb` box in the
// `iamf` sample entry, and each Temporal Unit is one IA Sample, without a Temporal Delimiter.
//
// write_isobmff() writes a whole file from a Sequence. FragmentedWriter writes the same track as
// an initialization segment followed by movie fragments, one call per group of Temporal Units, for
// output that is produced as it is encoded. read_isobmff() reads both forms.

namespace iclforge::containers::iamf {

struct IsobmffOptions {
    // Written into the handler box's name field.
    std::string writing_app = "iclforge";
    // Track and movie timescale. 0 takes the sample rate of the first Codec Config when it is
    // LPCM, and 48000 otherwise.
    std::uint32_t timescale = 0;
};

struct EditList {
    std::uint64_t segment_duration = 0;  // in the movie timescale, the duration after trimming
    std::int64_t media_time = 0;         // the samples trimmed from the start
};

struct IsobmffInfo {
    std::vector<std::string> brands;  // the major brand, then the compatible brands
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;       // the sum of the sample durations, in the timescale
    std::optional<EditList> edit;     // present when audio samples are trimmed
    bool fragmented = false;
    std::vector<std::uint32_t> sample_durations;
};

struct IsobmffFile {
    Sequence sequence;
    IsobmffInfo info;
};

// Writes a complete file: ftyp, moov, mdat. The duration of an IA Sample counts the samples
// trimmed from its start and not those trimmed from its end. When any Audio Frame carries
// trimming, an edts/elst box records the trim at the start and end of the sequence. Temporal
// Units marked is_not_key_frame are listed as non-sync samples in an stss box.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Bytes, Error> write_isobmff(const Sequence& sequence,
                                                                              const IsobmffOptions& options = {});

// Reads a file written by write_isobmff(), by FragmentedWriter, or by another IAMF muxer that
// follows the same encapsulation. Only the first IA track is read.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<IsobmffFile, Error> read_isobmff(std::span<const std::byte> file);

// A fragmented writer. initialization_segment() holds the ftyp and a moov with an empty sample
// table and an mvex box; each fragment() call returns one moof and mdat for the Temporal Units
// passed, which continue where the last call stopped. The pieces concatenate into a valid file,
// and each fragment can be sent as soon as it is returned.
class ICLFORGE_CONTAINERS_EXPORT FragmentedWriter {
public:
    // `descriptors` supplies the Descriptor OBUs and the parameter definitions; its Temporal Units
    // are not written.
    [[nodiscard]] static std::expected<FragmentedWriter, Error> create(const Sequence& descriptors,
                                                                       const IsobmffOptions& options = {});

    [[nodiscard]] const Bytes& initialization_segment() const { return initialization_segment_; }

    // One movie fragment for `units`. At least one unit is required; every Temporal Unit may
    // carry Parameter Blocks, and the Audio Frames' trimming sets the sample duration as in
    // write_isobmff().
    [[nodiscard]] std::expected<Bytes, Error> fragment(std::span<const TemporalUnit> units);

    [[nodiscard]] std::uint32_t fragments_written() const { return sequence_number_ - 1; }
    // The media time, in the timescale, at which the next fragment starts.
    [[nodiscard]] std::uint64_t next_decode_time() const { return decode_time_; }

private:
    FragmentedWriter() = default;

    Sequence descriptors_;
    Bytes initialization_segment_;
    std::uint32_t default_sample_duration_ = 0;
    std::uint32_t sequence_number_ = 1;
    std::uint64_t decode_time_ = 0;
};

}  // namespace iclforge::containers::iamf
