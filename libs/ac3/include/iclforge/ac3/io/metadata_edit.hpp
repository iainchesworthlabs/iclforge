#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/export.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

// Changing a stream's bsi metadata WITHOUT re-encoding the audio.
//
// dialnorm, compr, bsmod and dsurmod are all delivery decisions - what a
// receiver is told the dialogue level is, how hard to compress on an RF
// output, what kind of service this is, whether the surrounds were matrixed.
// Every one of them lands in bsi, ahead of the first audblk, and none of them
// changes a single coded coefficient. Re-encoding a programme to correct one
// costs a whole generation of lossy coding for no reason; this rewrites the
// bits in place and re-stamps the frame's CRCs instead, so the audio comes
// back out of a decoder bit-identical to what went in.
//
// The CRCs are the part that is not obvious. crc2 is an ordinary trailing
// CRC - recompute over the covered region and store it. crc1 is not: A/52
// §7.10.1 puts it BEFORE the region it protects and requires the register to
// read zero once the first 5/8 of the syncframe has been shifted through, so
// it has to be SOLVED rather than computed. iclforge::ac3::solve_leading_crc
// (ac3/core/crc16.hpp) does that with a GF(2) polynomial inverse, and is the
// same function the encoder itself uses - see its own comment.
//
// Scope, stated as limits rather than left to be discovered:
//
//   * edit_frame_metadata and edit_stream_metadata change only fields ALREADY
//     ON THE WIRE, in place: compr lives behind compre, bsmod and dsurmod
//     behind E-AC-3's infomdate, and a frame that did not transmit one has no
//     bits to overwrite. Asking for such a field is kFieldAbsent.
//   * insert_frame_metadata and insert_stream_metadata go further for E-AC-3:
//     the missing field is INSERTED, which moves every bit after it, so the
//     syncframe grows. E-AC-3 can take that - frmsiz is a free 11-bit length,
//     and the padding that makes up a whole 16-bit word is auxbits, which the
//     encoder itself pads with - where AC-3 cannot: its frame size is a code
//     (frmsizecod) that fixes the bit rate, and finding room inside it would
//     take a bit-accurate walk of all six audio blocks to where the audio ends.
//     An AC-3 frame lacking the field stays kFieldAbsent.
//     An insert is refused (kCannotInsert) rather than guessed at on a frame
//     that has block start information (blkstrtinfo holds block offsets from
//     the frame start and is as wide as frmsiz, so every one would be wrong),
//     that carries auxiliary data (it sits at the end and padding must not
//     come between it and the tail), or that is not an independent substream
//     (a dependent's compre is not a compression word, and mixing metadata is
//     the independent substream's).
//     The audio blocks are copied bit for bit; what changes is the length of
//     the frames that gained a field, and so the stream's bit rate by about
//     16 bits a frame at the most.
//   * A DEPENDENT E-AC-3 substream reports no compr at all, whatever its
//     compre bit says: §E3.8.5 repurposes compre there to mark the last
//     dependent of the programme rather than to announce a compression word
//     (see decoder.hpp's DecodedSubstream::compr). Its 8 bits are still on
//     the wire and are still skipped correctly; they are simply not a compr
//     word, so this refuses to write one into them.
//   * strmtyp 2 (a "convertible" substream, §E2.3.1.1: previously coded in
//     AC-3) is edited as an independent substream is. Its bsi swaps convsync
//     for blkid and frmsizecod, which are walked rather than rewritten, and
//     it sends neither the programme-mixing group nor the converter strategy
//     elements (Table E1.2 and E1.3 gate those on strmtyp 0). strmtyp 3 is
//     reserved and refused.

namespace iclforge::ac3::io {

enum class EditError : std::uint8_t {
    kBadSyncWord,
    kTruncated,        // the span is shorter than the syncframe's own declared size
    kUnsupportedBsid,  // not AC-3 (<= 10) or E-AC-3 (16)
    kReservedValue,    // a reserved fscod/frmsizecod, or strmtyp 3
    kFieldAbsent,      // asked to change a field this frame does not transmit
    kOutOfRange,       // a value the field cannot hold
    kCannotInsert,     // insert_*: this frame cannot take the field without re-framing it
};

[[nodiscard]] ICLFORGE_AC3_EXPORT std::string_view describe(EditError error);

// E-AC-3's mixmdate group (Table E1.2), as transmitted. Every member is
// optional because acmod and lfeon decide which of them are sent at all -
// std::nullopt means "not on the wire", never "sent as zero".
//
// Read, not applied: this exists so a transcode can carry a DD+ stream's
// downmix intent across to the two coarse levels AC-3 has room for. See
// iclforge::ac3::meta (ac3/meta/mixing.hpp) for what the values mean.
// Every member below carries an explicit `= std::nullopt`, even though
// std::optional's own default constructor already produces one: a caller
// naming only some of these in a designated initializer trips GCC's
// -Wmissing-field-initializers (on under -Wextra, and this project builds
// -Werror) for every member without one. The same reason the CLI's own
// QcProgrammeResult spells its defaults out.
struct WireMixMetadata {
    std::optional<meta::DownmixMode> dmixmod = std::nullopt;
    std::optional<meta::MixLevel> ltrtcmixlev = std::nullopt;
    std::optional<meta::MixLevel> lorocmixlev = std::nullopt;
    std::optional<meta::MixLevel> ltrtsurmixlev = std::nullopt;
    std::optional<meta::MixLevel> lorosurmixlev = std::nullopt;
    std::optional<int> lfemixlevcod = std::nullopt;  // Table E1.3, 0..31
};

// One syncframe's rewritable metadata, and enough of its shape to know which
// of those fields exist. std::nullopt on any of dialnorm2/compr/compr2/bsmod/
// dsurmod means the frame does not transmit it.
struct FrameMetadata {
    StreamKind kind = StreamKind::kAc3;
    int bsid = 0;
    std::size_t bytes = 0;  // the whole syncframe, from its own size field
    SampleRate sample_rate = SampleRate::k48000;
    // E-AC-3 only. AC-3 reports strmtyp 0, substreamid 0 and numblkscod 3,
    // which is what an AC-3 syncframe amounts to in Annex E's vocabulary.
    int strmtyp = 0;
    int substreamid = 0;
    int numblkscod = 3;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;

    int dialnorm = 31;
    std::optional<std::uint8_t> compr = std::nullopt;
    std::optional<int> dialnorm2 = std::nullopt;  // 1+1 dual mono only
    std::optional<std::uint8_t> compr2 = std::nullopt;
    std::optional<int> bsmod = std::nullopt;
    std::optional<int> dsurmod = std::nullopt;

    // AC-3's two bsi downmix levels (§5.4.2.4/§5.4.2.5), present only when
    // acmod brought the channels they describe.
    std::optional<meta::CentreMixLevel> cmixlev = std::nullopt;
    std::optional<meta::SurroundMixLevel> surmixlev = std::nullopt;
    // E-AC-3's richer group, when mixmdate was set. AC-3 never has one.
    std::optional<WireMixMetadata> mix = std::nullopt;
};

// What to change. Every field is optional; an unset one is left exactly as it
// was, so an empty edit is a no-op that still re-stamps the CRCs identically.
// Explicit `= std::nullopt` on every member, for WireMixMetadata's reason
// above - and it matters most here, since naming ONE field is exactly how
// this struct is meant to be used.
struct MetadataEdit {
    std::optional<int> dialnorm = std::nullopt;         // 1..31 (§5.4.2.8)
    std::optional<int> dialnorm2 = std::nullopt;        // 1..31, 1+1 only
    std::optional<std::uint8_t> compr = std::nullopt;   // §7.7.2's word, needs compre set
    std::optional<std::uint8_t> compr2 = std::nullopt;  // ditto, Ch2's own
    std::optional<int> bsmod = std::nullopt;            // 0..7 (Table 5.5)
    std::optional<int> dsurmod = std::nullopt;          // 0..3 (Table 5.11), acmod 2/0 only
};

// Reads one syncframe's metadata without changing anything. `frame` may be
// longer than the syncframe (the trailing bytes are ignored) but not shorter.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<FrameMetadata, EditError> read_frame_metadata(
    std::span<const std::byte> frame);

// Applies `edit` to one syncframe in place and re-stamps its CRC word(s).
// Returns the frame's metadata AFTER the edit. Nothing outside the named
// fields and the CRCs is touched: the audblks, the aux bits and every other
// bsi field keep their exact bits.
//
// Fails without modifying anything when a named field is not on the wire
// (kFieldAbsent) or a value is out of range (kOutOfRange) - a partially
// applied edit would leave a frame claiming metadata nobody asked for.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<FrameMetadata, EditError> edit_frame_metadata(
    std::span<std::byte> frame, const MetadataEdit& edit);

// The result of an edit that may lengthen a syncframe.
struct InsertedFrame {
    std::vector<std::byte> bytes;  // the whole syncframe, as edited
    FrameMetadata metadata;        // after the edit
    bool grew = false;             // bytes.size() is not the frame's original size
};

// edit_frame_metadata for a caller that wants a field the frame lacks added
// rather than refused. A field that is on the wire is overwritten as before; one
// that is not is inserted if the frame can take it (an E-AC-3 independent
// substream: compre/compr2e or infomdate cleared, no block start information,
// no auxiliary data) and is kFieldAbsent where it cannot be asked of the frame
// at all (a dependent's compr, dsurmod outside acmod 2/0, anything missing from
// an AC-3 frame). Inserted fields take the values named; everything else
// infomdate brings along is written as the encoder's own defaults - copyrightb
// 0, origbs 1, dheadphonmod, dsurexmod and the audio production fields not
// indicated - and bsmod, if only dsurmod was asked for, is 0 (complete main).
// Fails without producing anything when any named field cannot be taken.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<InsertedFrame, EditError> insert_frame_metadata(
    std::span<const std::byte> frame, const MetadataEdit& edit);

// Re-stamps crc1 (AC-3 only) and crc2 for one syncframe, for a caller that
// changed bsi bits itself. edit_frame_metadata already does this; this is
// exposed because the CRCs are the non-obvious half of any in-place rewrite
// and a caller doing its own (iclforge::ac3::signing::sign_atmos_frame is the
// in-project precedent) should not have to reimplement crc1's solve.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<void, EditError> restamp_crc(
    std::span<std::byte> frame);

struct EditSummary {
    std::size_t syncframes = 0;
    // Syncframes whose bytes actually changed. An edit that asks for the
    // value a frame already carries leaves it identical, CRCs included.
    std::size_t changed = 0;
};

// Applies `edit` to every syncframe of a whole elementary stream, in place.
//
// Every substream's own dialnorm is rewritten, because Table E1.2 gives each
// one its own and a decoder reads the one belonging to whichever substream it
// is decoding. Every other field is applied to the syncframes that carry it
// and skipped on those that do not - compr and compr2 belong to the
// independent substream alone (§E3.8.5), dialnorm2 to a 1+1 programme,
// dsurmod to acmod 2/0 - so a stream whose substreams differ is rewritten
// correctly rather than refused.
//
// A field named in `edit` that NO syncframe in the stream carries is
// kFieldAbsent, checked before anything is written: the stream is either
// fully rewritten or left byte-for-byte alone, and a metadata option that
// silently did nothing is indistinguishable from one that does not work.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<EditSummary, EditError> edit_stream_metadata(
    std::span<std::byte> stream, const MetadataEdit& edit);

struct InsertedStream {
    std::vector<std::byte> bytes;
    EditSummary summary;
    // Syncframes that gained bits, and the bytes the stream is longer by.
    std::size_t grown = 0;
    std::size_t added_bytes = 0;
};

// edit_stream_metadata for a stream that may need fields added: the same two
// passes and the same per-substream narrowing (compr and compr2 are the
// independent substream's alone, dsurmod belongs to acmod 2/0, and so on), with
// each syncframe that should carry a named field and does not getting it
// inserted by insert_frame_metadata. Only independent E-AC-3 substreams are
// grown - a dependent that lacks a field is left as it is - and a field that no
// frame of the stream can carry or take is kFieldAbsent, checked before
// anything is written. Returns a new buffer; `stream` is not modified.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<InsertedStream, EditError> insert_stream_metadata(
    std::span<const std::byte> stream, const MetadataEdit& edit);

}  // namespace iclforge::ac3::io
