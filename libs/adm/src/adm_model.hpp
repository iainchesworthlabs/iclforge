#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adm_xml_extras.hpp"
#include "iclforge/adm/ac3adm.hpp"
#include "iclforge/adm/model.hpp"

// Forward-declared rather than #include <adm/document.hpp> here: libadm's
// own headers (and the Boost headers they pull in) are an implementation
// detail of this translation-unit pair (adm_model.hpp/.cpp) plus adm.cpp -
// nothing else in ac3adm, and nothing in its public ac3adm/ac3adm.hpp or
// ac3adm/model.hpp headers, needs to know libadm's types exist. See
// ac3adm/model.hpp's own header comment for why the two libraries'
// identically-named classes (::adm::AudioObject vs. iclforge::adm::AudioObject, ...)
// must never appear unqualified in the same header.
namespace adm {
class Document;
class AudioTrackUid;
class AudioChannelFormat;
class AudioPackFormat;
}  // namespace adm

namespace iclforge::adm::detail {

// Translates a fully-parsed libadm `::adm::Document` (see `::adm::parseXml()`)
// into this module's own ADM object graph (iclforge::adm::AdmModel), per
// Recommendation ITU-R BS.2076-2 (10/2019) Annex 1. libadm already enforces
// the schema's Required/Default/Optional rules while parsing, so this
// function trusts a successfully-returned Document to have every Required
// field present - it does not re-validate them.
[[nodiscard]] AdmModel build_adm_model(const std::shared_ptr<::adm::Document>& document);

// A Matrix channel's blocks, from the text scan (adm_xml_extras.hpp's MatrixBlockText): libadm
// reads none of them. Times come out through libadm's own timecode parser, so the two time
// formats BS.2076 §5.13 allows mean here what they mean everywhere else; gain and importance get
// the schema's defaults (linear 1.0 and 10) when the text is absent or not a number.
[[nodiscard]] std::vector<AudioBlockFormat> build_matrix_blocks(
    const std::vector<MatrixBlockText>& texts);

// The write-side inverse: builds a libadm `::adm::Document` from `model`, ready for
// `::adm::reassignIds()` and `::adm::writeXml()` (both called by adm.cpp's write_bw64, not here -
// this function only builds the graph). Every audioTrackUID gets bitDepth = `bit_depth`, the width
// write_bw64 stores <data> at, and never the model's own `bit_depth` (see write_bw64's doc comment
// in ac3adm.hpp for why). `track_uids_by_key` maps each `AdmModel::AudioTrackUid::uid`
// string to the `::adm::AudioTrackUid` it became, keyed by that SAME correlation string (see
// ac3adm.hpp's write_bw64 doc comment on why these are correlation keys, not real ADM IDs) - so
// write_bw64 can resolve `AdmDocument::chna` entries (which name a track_uids[].uid) back to the
// libadm element they describe, after reassignIds() has given it its real, final AudioTrackUidId.
// One Objects audioChannelFormat whose blocks carry a zoneExclusion. libadm cannot write that
// element, so write_bw64 adds it to the XML text afterwards, and needs the channel's final block
// IDs (known only once reassignIds() has run) to say where. `zones_by_block[i]` belongs to the
// channel's i-th block.
struct ZoneBlockSource {
    std::shared_ptr<::adm::AudioChannelFormat> channel;
    std::vector<std::vector<ExclusionZone>> zones_by_block;
};

// A Matrix channel. libadm's formatter writes each of its blocks as an empty element with only an
// ID, rtime and duration, so write_bw64 expands them from `blocks` (the model's own, in order, so
// blocks[i] belongs to the channel's i-th block) once reassignIds() has given them final IDs. The
// references inside still use the model's correlation keys; they are translated through
// BuiltDocument::channels_by_key at that point, when the final channel IDs exist.
struct MatrixChannelSource {
    std::shared_ptr<::adm::AudioChannelFormat> channel;
    std::vector<AudioBlockFormat> blocks;
};

// A pack that carries something libadm cannot write: a Matrix pack's references to other packs, or
// an HOA pack's defaults. `model` is the model's own pack, its references still in correlation
// keys (BuiltDocument::packs_by_key translates them).
struct PackSource {
    std::shared_ptr<::adm::AudioPackFormat> pack;
    AudioPackFormat model;
};

struct BuiltDocument {
    std::shared_ptr<::adm::Document> document;
    std::vector<ZoneBlockSource> zone_blocks;
    std::vector<MatrixChannelSource> matrix_channels;
    std::vector<PackSource> pack_sources;
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackUid>> track_uids_by_key;
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioChannelFormat>> channels_by_key;
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioPackFormat>> packs_by_key;
};

[[nodiscard]] std::expected<BuiltDocument, AdmWriteError> build_libadm_document(const AdmModel& model,
                                                                              std::uint16_t bit_depth);

}  // namespace iclforge::adm::detail
