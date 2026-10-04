#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/export.hpp"

// Roadmap item IM1 phase 2 of 3 (): minimal SMPTE ST 336:2017 KLV extraction for an
// IAB Track File, the way a real IMF/Dolby Atmos cinema master actually delivers ST 2098-2's
// Immersive Audio Bitstream - a bare elementary `.iab` file is the exception, not the rule.
//
// SMPTE ST 2098-2 itself has no MXF content at all; the wrapping is defined by a separate, much
// shorter standard, SMPTE ST 2067-201:2021 ("IMF - Immersive Audio Bitstream Level 0 Plug-in"),
// which in turn references the base MXF standards (ST 377-1 file format, ST 379-1/-2 Generic/
// Constrained Generic Container, ST 336 KLV/BER encoding). Every citation in mxf_reader.cpp names
// one of those five documents' own clause/table numbers - all five are free from
// https://pub.smpte.org, confirming the IAB MXF planning record.
//
// The one fact that makes this "minimal" rather than "a general MXF library": ST 2067-201 §5.5
// clip-wraps the Immersive Audio Bitstream - "the entire duration of the essence container shall
// be contained within a single Content Package... comprised of a single KLV" (ST 379-2 §8.4.2).
// An IAB Track File's audio essence therefore lives in exactly ONE Generic Container KLV triplet,
// and that KLV's Value is byte-identical to ST 2098-2 Clause 7's IABitstream syntax - the same
// `while(true){Preamble;IAFrame;}` run an elementary `.iab` file already has. So this reader does
// not need Index Tables (legally absent per ST 377-1 §11.5.3, and only useful for random-access
// seeking inside that one KLV even when present - ST 379-2 §8.4.4 Table 1), a System Item (never
// required by ST 2067-201), or any of Header Metadata's Preface/ContentStorage/Package object
// graph (locating essence is a KLV-Key matter, not an object-graph one) - it only has to walk
// top-level KLV triplets from the start of the file, skip everything whose Key does not match ST
// 2067-201 Table 4.2's registered IAB Essence Element Key, and hand the one KLV that does match
// straight to iclforge::iab::parse_iabitstream(std::istream&) unmodified (see that function's own
// updated doc comment in ac3iab.hpp) - zero duplication of the Preamble/IAFrame framing logic phase
// 1 already implements.
//
// Deliberately out of scope, since ST 2067-201 §5.3-5.5 already constrains a compliant IAB Track
// File to exactly one Essence Track and one Sound Element, closing off most of what ST 377-1
// otherwise permits: multi-Package Header Metadata graphs, Operational Pattern logic, external or
// segmented essence (BodySID = 0), the Footer Partition's repeated Header Metadata, and a nonzero
// Run-In before the Header Partition Pack (ST 377-1 §7.2.1's own "default case of a Run-In
// sequence length of zero" is assumed; a file that does not open with a KLV Key at byte 0 is
// reported as kMxfBadKlv rather than scanned for one, matching this reader's "minimal" scope).
//
// Consulted DTSProAudio/iab-validator (MIT) as an external oracle per usual - it turned out to
// have no MXF-related code or sample .mxf files at all, so there was nothing to cross-check this
// specific piece against beyond one corroborating doc comment (IABParserAPI.h, on its own
// no-stream frame-buffer entry point: "A typical application example is from MXF-unwrapped
// frames") confirming the same "external MXF-unwrap hands a frame buffer to the existing parser"
// split this reader uses.

namespace iclforge::iab {

// Reads an IAB Track File (SMPTE ST 2067-201) and returns the same IABitstreamFrame sequence
// parse_iabitstream() returns for a bare elementary `.iab` file - see this header's own top
// comment for why the two converge on identical output. kMxfNoIabEssence: no top-level KLV's Key
// matched ST 2067-201 Table 4.2's registered value. kMxfBadKlv: a Length field violated ST 336's
// BER encoding rules. kTruncated: fewer bytes remained than a Key or a declared Length/Value
// needed.
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError>
parse_mxf_iab(const std::string& path);
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError>
parse_mxf_iab(std::istream& in);

// --- Writing ---------------------------------------------------------------------------------
//
// write_mxf_iab() wraps a parsed or constructed IABitstream sequence as an IMF IAB Track File: the
// structure ST 2067-5 gives an Essence Component and ST 2067-201 gives IAB. It writes
//
//   header partition   Primer Pack, Header Metadata, 8 KiB of KLV fill  (closed and complete)
//   body partition     the Index Table Segments                          (IndexSID 2)
//   body partition     the clip-wrapped essence, one KLV                  (BodySID 1)
//   footer partition   no repeated metadata
//   Random Index Pack
//
// KAG size 1, OP1a (single item, single package, uni-track, stream, internal essence). The
// Header Metadata holds one Material Package and one File Package, each with a timecode track and
// the sound track; the IAB Essence Descriptor with an IAB Soundfield Label SubDescriptor and one
// IAB Channel SubDescriptor per bed channel (ST 2067-201 Annexes C and E); and the Preface's
// ConformsToSpecifications entry for IMF IAB Track File Level 0.
//
// Edit Rate is the IAB frame rate and an Edit Unit is one Preamble + IAFrame segment pair, so the
// Index Table has one entry per frame. Following ST 2067-201 5.7.2 (a deliberate deviation from
// ST 377-1 11.1.4) each entry's Stream Offset includes the essence KLV's key and length, so the
// first Edit Unit is at offset 25.
//
// ST 2067-201 constrains the bitstream more tightly than ST 2098-2 does, and the writer refuses
// what it forbids rather than writing a non-conformant file: 24-bit audio, AudioDataPCM only (no
// AudioDataDLC), no BedRemap and no child elements of a BedDefinition or ObjectDefinition, no
// conditional elements unless their UseCase is 0xFF, and a SampleRate, BitDepth and FrameRate that
// do not change.
enum class MxfWriteError : std::uint8_t {
    kNoFrames,             // there are no frames to wrap
    kInconsistentFrames,   // SampleRate, BitDepth or FrameRate differs between frames (5.6.1), or a
                           // BedDefinition / ObjectDefinition does not keep its constant fields (5.6.3)
    kBadBitDepth,          // the bit depth is not 24 (5.6.2)
    kDlcNotAllowed,        // a frame carries AudioDataDLC (5.6.2)
    kBedRemapNotAllowed,   // a BedRemap element (5.6.3.3)
    kChildElement,         // a child of a BedDefinition or ObjectDefinition (5.6.3.2)
    kConditionalElement,   // a conditional element whose UseCase is not 0xFF (5.6.3.4)
    kBitstream,            // write_iabitstream() refused a frame; see its own WriteError
    kTooLarge,             // a value does not fit the field that holds it
    kCannotOpen,           // the output path could not be opened
};

[[nodiscard]] ICLFORGE_IAB_EXPORT std::string_view describe(MxfWriteError error);

struct MxfWriteOptions {
    // Identification Set: who wrote the file.
    std::string company_name = "iclforge";
    std::string product_name = "iclforge IAB track file writer";
    std::string version_string = "ST 2067-201";

    // IAB Soundfield Label SubDescriptor (Annex C, Table 7). The strings are written as given; MCA
    // Content and MCA Use Class take the values of ST 377-41 Subclauses 5.4 and 5.5, which are not
    // checked here. A spoken language is an RFC 5646 tag and is left out when empty.
    std::optional<std::string> title;
    std::optional<std::string> title_version;
    std::optional<std::string> spoken_language;
    std::optional<std::string> content;
    std::optional<std::string> use_class;

    // ST 2067-2 Annex E items, both "should be present". When the edit rate is not set it defaults
    // to the IAB frame rate for the rates that are also picture rates (24, 25, 30 and 24000/1001)
    // and is left out for the others. The alignment level is in dBFS.
    std::optional<std::pair<std::int32_t, std::int32_t>> reference_image_edit_rate;
    std::optional<std::int8_t> reference_audio_alignment_level = -20;

    // false leaves out the IAB Channel SubDescriptors (Annex E; "should").
    bool channel_sub_descriptors = true;

    // Times written to the Preface, Identification and both Packages. Now when unset.
    std::optional<std::chrono::sys_seconds> timestamp;

    // Seeds the UUIDs and the package identifier. The same seed and timestamp give the same
    // bytes; unset draws one from std::random_device.
    std::optional<std::uint64_t> uid_seed;
};

[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<std::byte>, MxfWriteError> write_mxf_iab(
    std::span<const IABitstreamFrame> frames, const MxfWriteOptions& options = {});

[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<void, MxfWriteError> write_mxf_iab(
    const std::string& path, std::span<const IABitstreamFrame> frames, const MxfWriteOptions& options = {});

}  // namespace iclforge::iab
