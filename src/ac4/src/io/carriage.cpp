#include "iclforge/ac4/io/carriage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <string>

namespace iclforge::ac4 {

// --- Carriage (AC-4 bitstream inspector's separable slice) -------------------------------

namespace {

// Annex E's DSI fields are written MSB-first into whole bytes, the same
// bit-packing discipline the parser reads with - small enough here that a
// local accumulator beats pulling a writer dependency into a module whose
// whole identity is depending on nothing.
class DsiWriter {
   public:
    void put(std::uint32_t value, int bits) {
        for (int bit = bits - 1; bit >= 0; --bit) {
            accumulator_ = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(accumulator_) << 1) | ((value >> bit) & 1u));
            if (++filled_ == 8) {
                bytes_.push_back(static_cast<std::byte>(accumulator_));
                accumulator_ = 0;
                filled_ = 0;
            }
        }
    }
    void byte_align() {
        while (filled_ != 0) {
            put(0, 1);
        }
    }
    [[nodiscard]] std::vector<std::byte> take() {
        byte_align();
        return std::move(bytes_);
    }

   private:
    std::vector<std::byte> bytes_;
    std::uint8_t accumulator_ = 0;
    int filled_ = 0;
};

// Annex E.7's ac4_bitrate_dsi(): the mode wait_frames implies, the rate unknown.
void put_bitrate_dsi(DsiWriter& w, const Toc& toc) {
    std::uint32_t mode = 3;
    if (toc.wait_frames == 0) {
        mode = 1;
    } else if (toc.wait_frames && *toc.wait_frames >= 1 && *toc.wait_frames <= 6) {
        mode = 2;
    }
    w.put(mode, 2);
    w.put(0, 32);            // bit_rate: unknown
    w.put(0xFFFFFFFFu, 32);  // bit_rate_precision: unknown
}

// Annex E.10.3's audio channel groups of a channel mode (Table A.27's right
// column, group g at bit g). Pseudocode E.3 as printed sets no LFE, above
// ch_mode 10 no L/R or Ls/Rs and the centre as group 2, never the 9.X layouts'
// Lscr/Rscr, and 22.2's Tsl/Tsr only for one top pair; the groups here are the
// ones Table A.27 gives each mode, which is what DEE's muxer writes
// (src/ac4/ERRATA.md).
std::uint32_t channel_groups(int ch_mode, bool centre, bool four_back, int top_pairs) {
    std::uint32_t groups = 0;
    const auto set = [&groups](int g) { groups |= 1u << g; };
    const bool lfe = ch_mode == 4 || ch_mode == 6 || ch_mode == 8 || ch_mode == 10 || ch_mode == 12 ||
                     ch_mode == 14 || ch_mode == 15;
    if (ch_mode == 15) {
        for (const int g : {17, 15, 14, 13, 12, 11, 10, 9, 7, 5, 4, 3, 1}) {
            set(g);
        }
    }
    if (ch_mode == 13 || ch_mode == 14) {
        set(16);  // Lscr Rscr
    }
    if (ch_mode >= 11) {
        set(0);  // L R
        set(2);  // Ls Rs
        if (top_pairs == 1) {
            set(7);  // Tsl Tsr
        }
        if (top_pairs == 2) {
            set(4);  // Tfl Tfr
            set(5);  // Tbl Tbr
        }
        if (four_back) {
            set(3);  // Lb Rb
        }
        if (centre) {
            set(1);  // C
        }
    }
    if (ch_mode >= 2 && ch_mode <= 10) {
        if (ch_mode >= 3) {
            set(2);  // Ls Rs
        }
        if (ch_mode == 5 || ch_mode == 6) {
            set(3);  // Lb Rb
        }
        if (ch_mode == 7 || ch_mode == 8) {
            set(17);  // Lw Rw
        }
        if (ch_mode == 9 || ch_mode == 10) {
            set(4);  // Tfl Tfr
        }
        set(0);  // L R
        set(1);  // C
    }
    if (ch_mode == 1) {
        set(0);
    }
    if (ch_mode == 0) {
        set(1);
    }
    if (lfe) {
        set(6);
    }
    return groups;
}

// A reason build_dac4() writes nothing, a string literal (dac4_refusal()).
using Refusal = std::string_view;

// Part 2 clause 6.3.3.1.27's superset() over the channel modes, by the channels
// each mode holds in full: its channel groups with the centre, the four back
// channels and both top pairs present. The lowest mode holding every channel
// of both; -1 identity, superset(0, 1) is 1 as the clause says, and -1 where no
// mode holds both, as src/ac4/ERRATA.md ("The presentation substream") reads
// the six pairs the clause leaves without one.
[[nodiscard]] int superset(int a, int b) {
    if (a < 0) {
        return b;
    }
    if (b < 0) {
        return a;
    }
    if ((a == 0 && b == 1) || (a == 1 && b == 0)) {
        return 1;
    }
    const std::uint32_t wanted =
        channel_groups(a, true, true, 2) | channel_groups(b, true, true, 2);
    for (int mode = 0; mode <= 15; ++mode) {
        if ((channel_groups(mode, true, true, 2) & wanted) == wanted) {
            return mode;
        }
    }
    return -1;
}

// The same over Table 71's core modes 3 to 6 (5.0, 5.1, 5.0.2 and 5.1.2),
// each the one before with an LFE or a top pair added.
[[nodiscard]] int superset_core(int a, int b) {
    if (a < 0) {
        return b;
    }
    if (b < 0) {
        return a;
    }
    const bool lfe = a == 4 || a == 6 || b == 4 || b == 6;
    const bool top = a >= 5 || b >= 5;
    return 3 + (lfe ? 1 : 0) + (top ? 2 : 0);
}

// What Pseudocodes 25 and 26 and clauses 6.3.3.1.29 to 6.3.3.1.30 derive from
// every substream of the substream groups a presentation's specifiers name,
// each group once, as the decoder takes them (src/ac4/ERRATA.md,
// "presentation_config 1 and 4 read more specifiers than n_substream_groups").
struct PresentationShape {
    int ch_mode = -1;  // pres_ch_mode
    int core = -1;     // pres_ch_mode_core
    bool four_back = false;
    bool centre = false;
    int top_pairs = 0;
    // Whether every substream sends b_bitrate_info, and one at least does.
    bool bitrate_info = false;
};

std::expected<PresentationShape, Refusal> shape_of(const Toc& toc, const PresentationInfoV1& pres) {
    PresentationShape shape;
    bool objects = false;
    bool adaptive = false;
    bool any = false;
    bool every_rate = true;
    std::vector<bool> counted(toc.substream_groups.size(), false);
    for (const int ref : pres.group_refs) {
        if (ref < 0 || static_cast<std::size_t>(ref) >= toc.substream_groups.size()) {
            return std::unexpected(
                "a substream group the table of contents does not carry (b_multi_pid puts it in "
                "another elementary stream)");
        }
        const auto index = static_cast<std::size_t>(ref);
        if (counted[index]) {
            continue;
        }
        counted[index] = true;
        for (const GroupSubstream& s : toc.substream_groups[index].substreams) {
            any = true;
            if (s.kind == GroupSubstream::Kind::kChan && s.chan) {
                if (!s.chan->ch_mode) {
                    return std::unexpected("a substream of a channel mode the text reserves");
                }
                const int mode = *s.chan->ch_mode;
                shape.ch_mode = superset(shape.ch_mode, mode);
                // Table 71: the channel-coded rows.
                const int mode_core =
                    (mode == 11 || mode == 13) ? 5 : ((mode == 12 || mode == 14) ? 6 : -1);
                shape.core = superset_core(shape.core, mode_core);
                if (s.chan->original_content) {
                    const OriginalContent& content = *s.chan->original_content;
                    shape.four_back = shape.four_back || content.b_4_back_channels_present;
                    shape.centre = shape.centre || content.b_centre_present;
                    // Table 72, 2 winning where both rows hold.
                    const int pairs = content.top_channels_present == 3
                                          ? 2
                                          : (content.top_channels_present > 0 ? 1 : 0);
                    shape.top_pairs = std::max(shape.top_pairs, pairs);
                }
                every_rate = every_rate && s.chan->brate_ind.has_value();
            } else if (s.kind == GroupSubstream::Kind::kAjoc && s.ajoc) {
                objects = true;
                if (s.ajoc->b_static_dmx) {
                    shape.core = superset_core(shape.core, s.ajoc->b_lfe ? 4 : 3);
                } else {
                    adaptive = true;
                }
                every_rate = every_rate && s.ajoc->brate_ind.has_value();
            } else if (s.kind == GroupSubstream::Kind::kObj && s.obj) {
                objects = true;
                adaptive = true;
                every_rate = every_rate && s.obj->brate_ind.has_value();
            } else {
                return std::unexpected("a substream its group does not describe");
            }
        }
    }
    if (objects) {
        shape.ch_mode = -1;
    }
    if (adaptive) {
        shape.core = -1;
    }
    if (shape.core == shape.ch_mode) {
        shape.core = -1;
    }
    // Nothing contributes to a presentation without audio substreams, so it
    // sends no rate (src/ac4/ERRATA.md, "The bit rate and the indicators").
    shape.bitrate_info = any && every_rate;
    return shape;
}

// Part 1 Table E.5d: 0 at the base rate, 1 for twice it, 2 for four times.
[[nodiscard]] std::uint32_t dsi_sf_multiplier(const std::optional<int>& sf_multiplier) {
    return sf_multiplier ? static_cast<std::uint32_t>(*sf_multiplier + 1) : 0U;
}

// ac4_substream_group_dsi() (Annex E.11).
std::optional<Refusal> put_group_dsi(DsiWriter& w, const SubstreamGroupInfo& group) {
    if (group.substreams.size() > 255) {
        return "a substream group of more substreams than n_substreams' eight bits count";
    }
    if (group.content_type && group.content_type->serialized_language_tag) {
        return "a language tag sent in chunks, which one table of contents does not hold whole";
    }
    w.put(group.b_substreams_present ? 1U : 0U, 1);
    w.put(group.b_hsf_ext ? 1U : 0U, 1);
    w.put(group.b_channel_coded ? 1U : 0U, 1);
    w.put(static_cast<std::uint32_t>(group.substreams.size()), 8);
    for (const GroupSubstream& s : group.substreams) {
        std::optional<int> sf_multiplier;
        std::optional<int> brate_ind;
        if (group.b_channel_coded) {
            if (s.kind != GroupSubstream::Kind::kChan || !s.chan || !s.chan->ch_mode) {
                return "a channel-coded group whose substream is not a channel-coded one";
            }
            sf_multiplier = s.chan->sf_multiplier;
            brate_ind = s.chan->brate_ind;
        } else if (s.kind == GroupSubstream::Kind::kAjoc && s.ajoc) {
            sf_multiplier = s.ajoc->sf_multiplier;
            brate_ind = s.ajoc->brate_ind;
        } else if (s.kind == GroupSubstream::Kind::kObj && s.obj) {
            sf_multiplier = s.obj->sf_multiplier;
            brate_ind = s.obj->brate_ind;
        } else {
            return "an object-coded group whose substream is not an object one";
        }
        w.put(dsi_sf_multiplier(sf_multiplier), 2);
        w.put(brate_ind ? 1U : 0U, 1);  // b_substream_bitrate_indicator
        if (brate_ind) {
            w.put(static_cast<std::uint32_t>(*brate_ind), 5);
        }
        if (group.b_channel_coded) {
            // The channel groups of the substream's original content (NOTE 2).
            const OriginalContent content = s.chan->original_content.value_or(OriginalContent{});
            const int top_pairs =
                content.top_channels_present == 0 ? 0 : (content.top_channels_present == 3 ? 2 : 1);
            w.put(0, 6);  // reserved_zero
            w.put(channel_groups(*s.chan->ch_mode, content.b_centre_present,
                                 content.b_4_back_channels_present, top_pairs),
                  18);
            continue;
        }
        bool bed = false;
        bool dynamic = false;
        bool isf = false;
        w.put(s.ajoc ? 1U : 0U, 1);  // b_ajoc
        if (s.ajoc) {
            const AjocSubstreamInfo& ajoc = *s.ajoc;
            if (ajoc.n_fullband_dmx_signals < 1 || ajoc.n_fullband_dmx_signals > 16 ||
                ajoc.n_fullband_upmix_signals < 1 || ajoc.n_fullband_upmix_signals > 64) {
                return "an A-JOC substream of more upmix objects than six bits count";
            }
            w.put(ajoc.b_static_dmx ? 1U : 0U, 1);
            if (!ajoc.b_static_dmx) {
                // n_dmx_objects_minus1
                w.put(static_cast<std::uint32_t>(ajoc.n_fullband_dmx_signals - 1), 4);
            }
            // n_umx_objects_minus1
            w.put(static_cast<std::uint32_t>(ajoc.n_fullband_upmix_signals - 1), 6);
            // The upmix's objects (Table E.15): bed_dyn_obj_assignment() lists
            // its bed and ISF objects, and the signals it does not list are
            // dynamic (src/ac4/ERRATA.md, "An A-JOC substream's objects").
            int listed = 0;
            for (const ObjectEntry& object : ajoc.upmix_objects) {
                bed = bed || object.kind == ObjectKind::kBed;
                isf = isf || object.kind == ObjectKind::kIsf;
                listed += object.kind == ObjectKind::kDyn ? 0 : 1;
            }
            dynamic = ajoc.n_fullband_upmix_signals > listed;
        } else {
            // What ac4_substream_info_obj() sends.
            bed = s.obj->static_kind == ObjSubstreamInfo::Static::kBed;
            dynamic = s.obj->b_dynamic_objects;
            isf = s.obj->static_kind == ObjSubstreamInfo::Static::kIsf;
        }
        w.put(bed ? 1U : 0U, 1);      // b_substream_contains_bed_objects
        w.put(dynamic ? 1U : 0U, 1);  // b_substream_contains_dynamic_objects
        w.put(isf ? 1U : 0U, 1);      // b_substream_contains_ISF_objects
        w.put(0, 1);                  // reserved
    }
    w.put(group.content_type ? 1U : 0U, 1);  // b_content_type
    if (group.content_type) {
        w.put(static_cast<std::uint32_t>(group.content_type->content_classifier), 3);
        const auto& tag = group.content_type->language_tag;
        w.put(tag ? 1U : 0U, 1);  // b_language_indicator
        if (tag) {
            w.put(static_cast<std::uint32_t>(tag->size()), 6);
            for (const std::byte b : *tag) {
                w.put(std::to_integer<std::uint32_t>(b), 8);
            }
        }
    }
    return std::nullopt;
}

// The DSI's closing byte (E.10.1): de_indicator, immersive_audio_indicator and
// an extended presentation_id, written where the Toc carries the indicators.
void put_indicators(DsiWriter& w, const PresentationInfoV1& pres) {
    const int id = pres.presentation_id.value_or(0);
    w.put(pres.de_indicator.value_or(false) ? 1U : 0U, 1);
    w.put(pres.immersive_audio_indicator.value_or(false) ? 1U : 0U, 1);
    w.put(0, 4);                  // reserved
    w.put(id > 31 ? 1U : 0U, 1);  // b_extended_presentation_id
    w.put(id > 31 ? static_cast<std::uint32_t>(id) : 0U, id > 31 ? 9 : 1);
}

[[nodiscard]] bool has_indicators(const PresentationInfoV1& pres) {
    return pres.de_indicator.has_value() || pres.immersive_audio_indicator.has_value();
}

std::optional<Refusal> put_add_emdf(DsiWriter& w, const std::vector<EmdfVersionKey>& add_emdf) {
    if (add_emdf.size() > 127) {
        return "more additional EMDF substreams than n_add_emdf_substreams' seven bits count";
    }
    w.put(static_cast<std::uint32_t>(add_emdf.size()), 7);
    for (const EmdfVersionKey& emdf : add_emdf) {
        if (emdf.emdf_version < 0 || emdf.emdf_version > 31 || emdf.key_id < 0 ||
            emdf.key_id > 1023) {
            return "an EMDF version or key_id past the DSI's five or ten bits";
        }
        w.put(static_cast<std::uint32_t>(emdf.emdf_version), 5);
        w.put(static_cast<std::uint32_t>(emdf.key_id), 10);
    }
    return std::nullopt;
}

// ac4_presentation_v1_dsi() (Annex E.10) for one presentation of the table of
// contents.
std::expected<std::vector<std::byte>, Refusal> presentation_v1_dsi(const Toc& toc,
                                                                   const PresentationInfoV1& pres) {
    DsiWriter w;
    if (pres.presentation_config == 6) {
        // EMDF payloads alone: presentation_config_v1 6 implies
        // b_add_emdf_substreams, and there is no substream to describe.
        w.put(6, 5);
        if (const auto refused = put_add_emdf(w, pres.add_emdf)) {
            return std::unexpected(*refused);
        }
        w.put(0, 1);  // b_presentation_bitrate_info: nothing contributes
        w.put(0, 1);  // b_alternative
        w.byte_align();
        if (has_indicators(pres)) {
            put_indicators(w, pres);
        }
        return w.take();
    }
    // Table 53's configurations, each with the substream groups its
    // specifiers name; unset for a single substream group.
    std::size_t groups = 1;
    if (pres.presentation_config) {
        const int config = *pres.presentation_config;
        if (config < 0 || config > 5) {
            return std::unexpected(
                "a presentation_config the text reserves, whose presentation_config_ext_info() "
                "the table of contents skips");
        }
        groups = config <= 2 ? 2 : (config <= 4 ? 3 : pres.group_refs.size());
        if (config == 5 && (groups < 2 || groups > 9)) {
            return std::unexpected(
                "more substream groups than n_substream_groups_minus2's three bits count");
        }
    }
    if (pres.group_refs.size() != groups) {
        return std::unexpected("a presentation whose substream groups did not all read");
    }
    const std::expected<PresentationShape, Refusal> shape = shape_of(toc, pres);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    const int id = pres.presentation_id.value_or(0);
    if (id < 0 || id > 511) {
        return std::unexpected("a presentation_id past extended_presentation_id's nine bits");
    }
    if (id > 31 && !has_indicators(pres)) {
        return std::unexpected(
            "a presentation_id above 31, which the DSI carries only beside the indicators, "
            "and the table of contents carries no indicators");
    }
    if (pres.emdf.emdf_version < 0 || pres.emdf.emdf_version > 31 || pres.emdf.key_id < 0 ||
        pres.emdf.key_id > 1023) {
        return std::unexpected("an EMDF version or key_id past the DSI's five or ten bits");
    }
    if (pres.b_alternative && !pres.alternative_info) {
        return std::unexpected(
            "an alternative presentation, whose name and targets its presentation substream "
            "carries, and the table of contents does not");
    }

    const std::uint32_t config_v1 =
        pres.presentation_config ? static_cast<std::uint32_t>(*pres.presentation_config) : 0x1FU;
    w.put(config_v1, 5);  // presentation_config_v1
    w.put(static_cast<std::uint32_t>(pres.md_compat.value_or(0)), 3);
    w.put(pres.presentation_id ? 1U : 0U, 1);  // b_presentation_id
    if (pres.presentation_id) {
        w.put(static_cast<std::uint32_t>(id & 0x1F), 5);
    }
    // Tables E.12 and E.13, from the factor and fraction the TOC gave.
    const int index = toc.frame_rate_index;
    std::uint32_t multiply = 0;
    if ((index >= 2 && index <= 4) || index == 0 || index == 1 || (index >= 7 && index <= 9)) {
        multiply = pres.frame_rate_factor == 2 ? 1U : (pres.frame_rate_factor == 4 ? 2U : 0U);
    }
    std::uint32_t fraction = 0;
    if (index >= 5 && index <= 12) {
        fraction = pres.frame_rate_fraction == 2 ? 1U : (pres.frame_rate_fraction == 4 ? 2U : 0U);
    }
    w.put(multiply, 2);
    w.put(fraction, 2);
    w.put(static_cast<std::uint32_t>(pres.emdf.emdf_version), 5);
    w.put(static_cast<std::uint32_t>(pres.emdf.key_id), 10);

    // The presentation's channel mode (Pseudocode 25) and its channel groups
    // (Pseudocode E.3, as Table A.27 gives them: src/ac4/ERRATA.md).
    w.put(shape->ch_mode >= 0 ? 1U : 0U, 1);  // b_presentation_channel_coded
    if (shape->ch_mode >= 0) {
        w.put(static_cast<std::uint32_t>(shape->ch_mode), 5);
        if (shape->ch_mode >= 11 && shape->ch_mode <= 14) {
            w.put(shape->four_back ? 1U : 0U, 1);
            w.put(static_cast<std::uint32_t>(shape->top_pairs), 2);
        }
        w.put(0, 6);  // reserved_zero
        w.put(channel_groups(shape->ch_mode, shape->centre, shape->four_back, shape->top_pairs),
              18);
    }
    // b_presentation_core_differs where the core mode (Pseudocode 26) is not
    // -1 (Table E.11 prints "is -1"; src/ac4/ERRATA.md), and Table E.14's
    // code for it.
    w.put(shape->core >= 0 ? 1U : 0U, 1);
    if (shape->core >= 0) {
        w.put(1, 1);  // b_presentation_core_channel_coded
        w.put(static_cast<std::uint32_t>(shape->core - 3), 2);
    }
    w.put(pres.enable_presentation ? 1U : 0U, 1);  // b_presentation_filter
    if (pres.enable_presentation) {
        w.put(*pres.enable_presentation ? 1U : 0U, 1);
        w.put(0, 8);  // n_filter_bytes
    }
    if (pres.presentation_config) {
        w.put(pres.b_multi_pid ? 1U : 0U, 1);
        if (*pres.presentation_config == 5) {
            w.put(static_cast<std::uint32_t>(groups - 2), 3);  // n_substream_groups_minus2
        }
    }
    // A group named twice is described twice, as its specifiers name it.
    for (const int ref : pres.group_refs) {
        if (const auto refused =
                put_group_dsi(w, toc.substream_groups[static_cast<std::size_t>(ref)])) {
            return std::unexpected(*refused);
        }
    }

    w.put(pres.b_pre_virtualized ? 1U : 0U, 1);
    w.put(pres.b_add_emdf_substreams ? 1U : 0U, 1);
    if (pres.b_add_emdf_substreams) {
        if (const auto refused = put_add_emdf(w, pres.add_emdf)) {
            return std::unexpected(*refused);
        }
    }
    w.put(shape->bitrate_info ? 1U : 0U, 1);  // b_presentation_bitrate_info
    if (shape->bitrate_info) {
        put_bitrate_dsi(w, toc);
    }
    w.put(pres.b_alternative ? 1U : 0U, 1);
    if (pres.b_alternative) {
        // alternative_info() (E.12): the name's bytes without the 0 the
        // presentation substream closes it with, and each target's level and
        // device categories, Table 67's four Booleans above the four bits
        // tdc_extension would add (src/ac4/ERRATA.md, "An alternative
        // presentation's dac4").
        const AlternativeInfo& alternative = *pres.alternative_info;
        if (alternative.name.size() > 0xFFFF || alternative.targets.empty() ||
            alternative.targets.size() > 31) {
            return std::unexpected(
                "an alternative presentation's name or targets past alternative_info()'s fields");
        }
        w.byte_align();
        w.put(static_cast<std::uint32_t>(alternative.name.size()), 16);
        for (const char c : alternative.name) {
            w.put(static_cast<std::uint32_t>(static_cast<unsigned char>(c)), 8);
        }
        w.put(static_cast<std::uint32_t>(alternative.targets.size()), 5);
        for (const AlternativeTarget& target : alternative.targets) {
            if (target.md_compat < 0 || target.md_compat > 7 || target.device_category < 0 ||
                target.device_category > 15) {
                return std::unexpected("an alternative presentation's target past its fields");
            }
            w.put(static_cast<std::uint32_t>(target.md_compat), 3);
            w.put(static_cast<std::uint32_t>(target.device_category) << 4U, 8);
        }
    }
    w.byte_align();
    if (has_indicators(pres)) {
        put_indicators(w, pres);
    }
    return w.take();
}

std::expected<std::vector<std::byte>, Refusal> dac4_of(const Toc& toc) {
    if (toc.bitstream_version < 2) {
        return std::unexpected(
            "a bitstream_version 0 or 1 table of contents, whose presentations Part 1 Annex E.4a's "
            "ac4_presentation_v0_dsi() describes");
    }
    if (toc.n_presentations < 0 || toc.n_presentations > 511) {
        return std::unexpected("more presentations than n_presentations' nine bits count");
    }
    if (static_cast<std::size_t>(toc.n_presentations) != toc.presentations_v1.size()) {
        return std::unexpected("a table of contents whose presentations did not all read");
    }
    DsiWriter w;
    // ac4_dsi_v1 (Annex E.6).
    w.put(1, 3);  // ac4_dsi_version
    w.put(static_cast<std::uint32_t>(toc.bitstream_version), 7);
    w.put(toc.sample_rate_hz == 48000 ? 1U : 0U, 1);  // fs_index (Table 82)
    w.put(static_cast<std::uint32_t>(toc.frame_rate_index), 4);
    w.put(static_cast<std::uint32_t>(toc.n_presentations), 9);
    // The program identifier, copied from the table of contents.
    w.put(toc.short_program_id ? 1U : 0U, 1);  // b_program_id
    if (toc.short_program_id) {
        w.put(static_cast<std::uint32_t>(*toc.short_program_id), 16);
        w.put(toc.program_uuid ? 1U : 0U, 1);  // b_uuid
        if (toc.program_uuid) {
            for (const std::byte b : *toc.program_uuid) {
                w.put(std::to_integer<std::uint32_t>(b), 8);
            }
        }
    }
    put_bitrate_dsi(w, toc);
    w.byte_align();

    for (const PresentationInfoV1& pres : toc.presentations_v1) {
        // A version 2 presentation's DSI is a skip area to this annex; DEE's
        // muxer fills it with the version 1 structure, and so does this.
        if (pres.presentation_version < 1 || pres.presentation_version > 255) {
            return std::unexpected(
                "a presentation_version 0 presentation in a version 2 table of contents");
        }
        const std::expected<std::vector<std::byte>, Refusal> body = presentation_v1_dsi(toc, pres);
        if (!body) {
            return std::unexpected(body.error());
        }
        if (body->size() > 255 + 0xFFFF) {
            return std::unexpected(
                "a presentation longer than pres_bytes and add_pres_bytes count");
        }
        w.put(static_cast<std::uint32_t>(pres.presentation_version), 8);
        if (body->size() >= 255) {
            w.put(255, 8);
            w.put(static_cast<std::uint32_t>(body->size() - 255), 16);
        } else {
            w.put(static_cast<std::uint32_t>(body->size()), 8);  // pres_bytes
        }
        for (const std::byte b : *body) {
            w.put(std::to_integer<std::uint32_t>(b), 8);
        }
    }
    return w.take();
}

}  // namespace

std::vector<std::byte> build_dac4(const Toc& toc) {
    std::expected<std::vector<std::byte>, Refusal> dac4 = dac4_of(toc);
    return dac4 ? std::move(*dac4) : std::vector<std::byte>{};
}

std::string_view dac4_refusal(const Toc& toc) {
    const std::expected<std::vector<std::byte>, Refusal> dac4 = dac4_of(toc);
    return dac4 ? std::string_view{} : dac4.error();
}

std::string_view cmaf_refusal(const Toc& toc) {
    // Part 2 Annex H.1.2.1's constraints, as one table of contents shows them.
    if (toc.bitstream_version != 2) {
        return "a bitstream_version other than 2";
    }
    if (toc.n_presentations > 64) {
        return "more than 64 presentations";
    }
    std::vector<int> ids;
    for (const PresentationInfoV1& pres : toc.presentations_v1) {
        if (pres.presentation_version != 1) {
            return "a presentation_version other than 1";
        }
        // 6.2.1.3 reads no b_presentation_id for an EMDF-only presentation.
        if (pres.presentation_config == 6) {
            return "a presentation of configuration 6, EMDF payloads alone, which has no field for "
                   "the presentation_id every presentation needs";
        }
        if (!pres.presentation_id) {
            return "a presentation without a presentation_id";
        }
        if (std::ranges::find(ids, *pres.presentation_id) != ids.end()) {
            return "two presentations with one presentation_id";
        }
        ids.push_back(*pres.presentation_id);
    }
    return {};
}

std::optional<std::uint32_t> samples_per_frame(const Toc& toc) {
    // Table 83/84. At 44,1 kHz only the 2048-sample frame exists; at 48 kHz
    // the 1000/1001-family entries with a NON-integer sample count per frame
    // (29,97 / 59,94 / 119,88 fps - the frame length alternates) have no
    // single answer and yield nullopt.
    if (toc.sample_rate_hz == 44100) {
        return toc.frame_rate_index == 13 ? std::optional<std::uint32_t>{2048} : std::nullopt;
    }
    switch (toc.frame_rate_index) {
        case 0: return 2002;   // 23,976 fps
        case 1: return 2000;   // 24
        case 2: return 1920;   // 25
        case 3: return std::nullopt;  // 29,97: 1601,6 - alternating
        case 4: return 1600;   // 30
        case 5: return 1001;   // 47,952
        case 6: return 1000;   // 48
        case 7: return 960;    // 50
        case 8: return std::nullopt;  // 59,94: 800,8 - alternating
        case 9: return 800;    // 60
        case 10: return 480;   // 100
        case 11: return std::nullopt;  // 119,88: 400,4 - alternating
        case 12: return 400;   // 120
        case 13: return 2048;  // the sample-rate-locked frame
        default: return std::nullopt;
    }
}

std::optional<MediaTiming> media_timing(const Toc& toc) {
    if (const auto samples = samples_per_frame(toc)) {
        return MediaTiming{.timescale = static_cast<std::uint32_t>(toc.sample_rate_hz),
                           .sample_delta = *samples};
    }
    if (toc.sample_rate_hz != 48000) {
        return std::nullopt;
    }
    switch (toc.frame_rate_index) {
        case 3: return MediaTiming{.timescale = 240000, .sample_delta = 8008};   // 29,97 fps
        case 8: return MediaTiming{.timescale = 240000, .sample_delta = 4004};   // 59,94
        case 11: return MediaTiming{.timescale = 240000, .sample_delta = 2002};  // 119,88
        default: return std::nullopt;
    }
}

std::optional<FrameRate> frame_rate(const Toc& toc) {
    const std::optional<MediaTiming> timing = media_timing(toc);
    if (!timing || toc.frame_rate_index < 0 || toc.frame_rate_index > 13) {
        return std::nullopt;
    }
    // Table 83's frame lengths at the internal rate (frame_len_base), index 13
    // being Table 84's 2 048 at either rate.
    constexpr std::array<int, 14> kFrameLength = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                                  1024, 768,  768,  512,  384,  384, 2048};
    FrameRate rate;
    rate.frames_per_second =
        static_cast<double>(timing->timescale) / static_cast<double>(timing->sample_delta);
    rate.frame_length = kFrameLength[static_cast<std::size_t>(toc.frame_rate_index)];
    rate.internal_rate_hz = static_cast<double>(rate.frame_length) * rate.frames_per_second;
    return rate;
}

std::string rfc6381_codec_string(const Toc& toc) {
    // Annex E.13: two lowercase hex digits per field, the presentation's two
    // from the one a manifest describes the track by. The two Toc presentation
    // lists only ever have one populated (the struct's own comment).
    constexpr std::string_view kHex = "0123456789abcdef";
    const auto pair = [&](int value) {
        std::string out;
        out.push_back(kHex[static_cast<std::size_t>((value >> 4) & 0xF)]);
        out.push_back(kHex[static_cast<std::size_t>(value & 0xF)]);
        return out;
    };
    int version = 0;
    int md_compat = 0;
    if (const std::optional<std::size_t> index = signalled_presentation(toc)) {
        if (!toc.presentations_v1.empty()) {
            version = toc.presentations_v1[*index].presentation_version;
            md_compat = toc.presentations_v1[*index].md_compat.value_or(0);
        } else {
            version = toc.presentations_v0[*index].presentation_version;
            md_compat = toc.presentations_v0[*index].md_compat.value_or(0);
        }
    }
    return "ac-4." + pair(toc.bitstream_version) + "." + pair(version) + "." + pair(md_compat);
}

std::optional<std::size_t> signalled_presentation(const Toc& toc) {
    // Annex G.2.3's widest compatibility: the level fewest decoders fall
    // short of, the lowest md_compat (Part 2 Table 55, Part 1 Table 86), among
    // the presentations a decoder may select.
    std::optional<std::size_t> best;
    int best_level = 0;
    const auto consider = [&](std::size_t index, bool audio, std::optional<int> md_compat) {
        const int level = md_compat.value_or(0);
        if (audio && (!best || level < best_level)) {
            best = index;
            best_level = level;
        }
    };
    if (!toc.presentations_v1.empty()) {
        for (std::size_t i = 0; i < toc.presentations_v1.size(); ++i) {
            const PresentationInfoV1& p = toc.presentations_v1[i];
            // Configuration 6 carries EMDF payloads alone.
            const bool audio = p.presentation_config != 6 && !p.group_refs.empty();
            consider(i, audio && p.enable_presentation.value_or(true), p.md_compat);
        }
        return best.has_value() ? best : std::optional<std::size_t>{0};
    }
    if (toc.presentations_v0.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < toc.presentations_v0.size(); ++i) {
        const PresentationInfoV0& p = toc.presentations_v0[i];
        consider(i, !p.substreams.empty(), p.md_compat);
    }
    return best.has_value() ? best : std::optional<std::size_t>{0};
}

namespace {

// Table A.27: how many speakers each audio channel group holds, group 8 being
// the deprecated pair group 7 replaces.
constexpr std::array<int, 18> kGroupSpeakers = {2, 1, 2, 2, 2, 2, 1, 2, 2,
                                                1, 1, 1, 1, 2, 1, 1, 2, 2};

// Table G.1: presentation_v1_channel_groups[], group g at bit g, and the value
// of urn:mpeg:mpegB:cicp:ChannelConfiguration it maps to.
struct CicpRow {
    std::uint32_t groups;
    int value;
};
// clang-format off
constexpr std::array<CicpRow, 27> kCicp = {{
    {0x000002, 1},  {0x000001, 2},  {0x000003, 3},  {0x008003, 4},  {0x000007, 5},
    {0x000047, 6},  {0x020047, 7},  {0x008001, 9},  {0x000005, 10}, {0x008047, 11},
    {0x00004F, 12}, {0x02FF7F, 13}, {0x06FF6F, 13}, {0x000057, 14}, {0x040047, 14},
    {0x00145F, 15}, {0x04144F, 15}, {0x000077, 16}, {0x040067, 16}, {0x000A77, 17},
    {0x040A67, 17}, {0x000A7F, 18}, {0x040A6F, 18}, {0x00007F, 19}, {0x04006F, 19},
    {0x01007F, 20}, {0x05006F, 20},
}};
// clang-format on

// What the manifest functions read of signalled_presentation(): its audio
// channel groups as build_dac4() writes them (Annex E.10.3), or that it is
// object audio. Nothing for a bitstream_version below 2, or a presentation
// whose substreams the table of contents does not describe whole.
struct SignalledChannels {
    bool objects = false;
    std::uint32_t groups = 0;
};

std::optional<SignalledChannels> signalled_channels(const Toc& toc) {
    const std::optional<std::size_t> index = signalled_presentation(toc);
    if (toc.bitstream_version < 2 || !index || *index >= toc.presentations_v1.size()) {
        return std::nullopt;
    }
    const PresentationInfoV1& pres = toc.presentations_v1[*index];
    const std::expected<PresentationShape, Refusal> shape = shape_of(toc, pres);
    if (!shape) {
        return std::nullopt;
    }
    if (shape->ch_mode >= 0) {
        return SignalledChannels{.objects = false,
                                 .groups = channel_groups(shape->ch_mode, shape->centre,
                                                          shape->four_back, shape->top_pairs)};
    }
    // Pseudocode 25 leaves no channel mode for object audio, and for a
    // presentation without audio substreams, which is not object audio.
    for (const int ref : pres.group_refs) {
        for (const GroupSubstream& s :
             toc.substream_groups[static_cast<std::size_t>(ref)].substreams) {
            if (s.kind != GroupSubstream::Kind::kChan) {
                return SignalledChannels{.objects = true, .groups = 0};
            }
        }
    }
    return std::nullopt;
}

// Six hexadecimal digits, as G.3.3.2's examples write them.
std::string hex6(std::uint32_t value) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out(6, '0');
    for (int digit = 5; digit >= 0; --digit) {
        out[static_cast<std::size_t>(digit)] = kHex[value & 0xFU];
        value >>= 4;
    }
    return out;
}

// A substream's sf_multiplier, whichever kind it is.
std::optional<int> sf_multiplier_of(const GroupSubstream& s) {
    if (s.chan) {
        return s.chan->sf_multiplier;
    }
    if (s.ajoc) {
        return s.ajoc->sf_multiplier;
    }
    return s.obj ? s.obj->sf_multiplier : std::nullopt;
}

// A content_type()'s language tag's primary subtag: its bytes up to the first
// hyphen.
std::string primary_subtag(const std::vector<std::byte>& tag) {
    std::string out;
    for (const std::byte b : tag) {
        const char c = static_cast<char>(std::to_integer<unsigned char>(b));
        if (c == '-') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::optional<ManifestDescriptor> dash_channel_configuration(const Toc& toc) {
    const std::optional<SignalledChannels> channels = signalled_channels(toc);
    if (!channels) {
        return std::nullopt;
    }
    constexpr std::string_view kDolby = "tag:dolby.com,2015:dash:audio_channel_configuration:2015";
    if (channels->objects) {
        return ManifestDescriptor{.scheme_id_uri = std::string{kDolby}, .value = hex6(0x800000U)};
    }
    for (const CicpRow& row : kCicp) {
        if (row.groups == channels->groups) {
            return ManifestDescriptor{.scheme_id_uri = "urn:mpeg:mpegB:cicp:ChannelConfiguration",
                                      .value = std::to_string(row.value)};
        }
    }
    return ManifestDescriptor{.scheme_id_uri = std::string{kDolby},
                              .value = hex6(channels->groups)};
}

std::vector<ManifestDescriptor> dash_supplemental_properties(const Toc& toc) {
    std::vector<ManifestDescriptor> out;
    // G.3.2's frame rate, reduced: Table E.1's time scale over its
    // sample_delta, 240 000 / 8 008 being 30 000 / 1 001.
    if (const std::optional<MediaTiming> timing = media_timing(toc)) {
        const std::uint32_t divisor = std::gcd(timing->timescale, timing->sample_delta);
        const std::uint32_t num = timing->timescale / divisor;
        const std::uint32_t den = timing->sample_delta / divisor;
        out.push_back(ManifestDescriptor{
            .scheme_id_uri = "tag:dolby.com,2017:dash:audio_frame_rate:2017",
            .value =
                den == 1 ? std::to_string(num) : std::to_string(num) + "/" + std::to_string(den)});
    }
    const std::optional<std::size_t> index = signalled_presentation(toc);
    const bool virtualized =
        index.has_value() &&
        (!toc.presentations_v1.empty() ? toc.presentations_v1[*index].b_pre_virtualized
                                       : toc.presentations_v0[*index].b_pre_virtualized);
    if (virtualized) {
        out.push_back(ManifestDescriptor{
            .scheme_id_uri = "tag:dolby.com,2016:dash:virtualized_content:2016", .value = "1"});
    }
    return out;
}

std::optional<int> presentation_channel_count(const Toc& toc) {
    const std::optional<SignalledChannels> channels = signalled_channels(toc);
    if (!channels || channels->objects) {
        return std::nullopt;
    }
    int count = 0;
    for (std::size_t g = 0; g < kGroupSpeakers.size(); ++g) {
        if (((channels->groups >> g) & 1U) != 0) {
            count += kGroupSpeakers[g];
        }
    }
    return count;
}

std::string_view configuration_difference(const Toc& a, const Toc& b) {
    // Annex H.1.2.4's parameters, in its order.
    if (a.frame_rate_index != b.frame_rate_index) {
        return "frame_rate_index";
    }
    if (a.sample_rate_hz != b.sample_rate_hz) {
        return "fs_index";
    }
    if (a.n_presentations != b.n_presentations ||
        a.presentations_v1.size() != b.presentations_v1.size() ||
        a.presentations_v0.size() != b.presentations_v0.size()) {
        return "n_presentations";
    }
    for (std::size_t i = 0; i < a.presentations_v1.size(); ++i) {
        if (a.presentations_v1[i].presentation_config !=
            b.presentations_v1[i].presentation_config) {
            return "a presentation's b_single_substream_group or presentation_config";
        }
    }
    for (std::size_t i = 0; i < a.presentations_v0.size(); ++i) {
        if (a.presentations_v0[i].presentation_config !=
            b.presentations_v0[i].presentation_config) {
            return "a presentation's b_single_substream_group or presentation_config";
        }
    }
    if (a.substream_groups.size() != b.substream_groups.size()) {
        return "the substream groups";
    }
    for (std::size_t j = 0; j < a.substream_groups.size(); ++j) {
        const SubstreamGroupInfo& ga = a.substream_groups[j];
        const SubstreamGroupInfo& gb = b.substream_groups[j];
        if (ga.content_type.has_value() != gb.content_type.has_value()) {
            return "a substream group's content_type()";
        }
        if (ga.content_type) {
            const ContentType& ca = *ga.content_type;
            const ContentType& cb = *gb.content_type;
            if (ca.content_classifier != cb.content_classifier) {
                return "a substream group's content_classifier";
            }
            const bool language_a = ca.language_tag.has_value() || ca.serialized_language_tag;
            const bool language_b = cb.language_tag.has_value() || cb.serialized_language_tag;
            if (language_a != language_b ||
                ca.serialized_language_tag != cb.serialized_language_tag ||
                (ca.language_tag && cb.language_tag &&
                 primary_subtag(*ca.language_tag) != primary_subtag(*cb.language_tag))) {
                return "a substream group's language";
            }
        }
        if (ga.substreams.size() != gb.substreams.size()) {
            return "a substream group's substreams";
        }
        for (std::size_t k = 0; k < ga.substreams.size(); ++k) {
            const GroupSubstream& sa = ga.substreams[k];
            const GroupSubstream& sb = gb.substreams[k];
            if (sa.kind != sb.kind ||
                (sa.chan && sb.chan && sa.chan->channel_mode != sb.chan->channel_mode)) {
                return "a substream's channel_mode";
            }
            if (sf_multiplier_of(sa) != sf_multiplier_of(sb)) {
                return "a substream's sf_multiplier";
            }
        }
    }
    return {};
}

}  // namespace iclforge::ac4
