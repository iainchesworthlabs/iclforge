#include "iclforge/adm/bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/adm/coordinates.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/adm/ac3adm.hpp"

namespace iclforge::adm {

std::string_view describe(BridgeError error) {
    switch (error) {
        case BridgeError::kNoProgramme: return "the ADM model has no audioProgramme";
        case BridgeError::kProgrammeNotFound: return "no audioProgramme with the requested ID";
        case BridgeError::kUnresolvedReference: return "an ID reference did not resolve";
        case BridgeError::kObjectReferenceCycle: return "nested audioObject references form a loop";
        case BridgeError::kUnsupportedType:
            return "a resolved audioPackFormat is not DirectSpeakers or Objects, its type "
                   "disagrees with a sibling pack, or it nests further audioPackFormats";
        case BridgeError::kChannelTrackMismatch:
            return "audioTrackUIDRef count does not match the resolved channel count";
        case BridgeError::kNoAudioForTrack: return "a resolved chna track has no PCM audio";
        case BridgeError::kEmptyBlockSequence: return "an audioChannelFormat has no audioBlockFormat";
        case BridgeError::kTooManyChannels: return "more channels than AtmosEncoder supports";
        case BridgeError::kEmptyInput: return "write() input had no channels, or a dynamic object had no updates";
        case BridgeError::kEmptyIabStream: return "build_iab() input had no frames";
        case BridgeError::kUnsupportedIabChannel:
            return "a Bed ChannelID has no iclforge::oba::BedLabel equivalent";
        case BridgeError::kNoIabEssenceForChannel:
            return "a channel's non-zero AudioDataID never resolved to AudioDataPCM or AudioDataDLC essence";
        case BridgeError::kBadIabAudio:
            return "an AudioDataDLC element in an IAB frame failed to decode";
    }
    return "unknown iclforge::adm::BridgeError";
}

namespace {

// A real Dirac/instantaneous jump has no representation in KeyframePath's piecewise-linear model
// (two keyframes cannot share one time_s - see
// iclforge::objects::oba::PathError::kDuplicateTimestamp). This is the same resolution
// tests/ac3/oba/test_atmos_motion.cpp's own make_holds() helper relies on implicitly: every caller
// in this codebase samples ObjectPath::evaluate() once per encoded frame
// (iclforge::ac3::kSamplesPerFrame = 1536 samples, 32 ms at 48 kHz - see
// iclforge::ac3::oba::AtmosEncoder:: encode_frame's own doc comment, "one placement per frame"), so
// any transition faster than one frame period is already indistinguishable from instantaneous at
// the resolution that actually reaches the bitstream. 1 microsecond is roughly 1/20 of one 48 kHz
// sample and about six orders of magnitude below a 32 ms frame - far too small for any real frame
// boundary to land inside it, and far smaller than any interpolationLength BS.2076-2 §10.3 itself
// expects a producer to author ("It is recommended that audioBlockFormat sizes are chosen to be
// small enough to avoid the use of the interpolationLength parameter for smoothly moving objects" -
// i.e. this parameter is meant for short but audible crossfades, not zero).
constexpr double kInstantJumpEpsilon = 1.0e-6;

// The non-positional, non-gain part of a Keyframe that comes from one audioBlockFormat.
struct Rendering {
    iclforge::objects::oba::ObjectSize size{};
    bool snap = false;
    iclforge::objects::oba::ZoneConstraint zone = iclforge::objects::oba::ZoneConstraint::kNone;
    bool enable_elevation = true;
    double divergence = 0.0;
    bool screen_reference = false;
};

}  // namespace

std::expected<iclforge::objects::oba::ObjectPath, BridgeError> build_channel_path(
    const iclforge::adm::AudioChannelFormat& channel, double object_start_s, bool force_lfe) {
    if (channel.block_formats.empty()) {
        return std::unexpected(BridgeError::kEmptyBlockSequence);
    }

    // §10.3's interpolatable parameters bundled into one Keyframe per breakpoint - position and
    // gain move together, governed by the same block's own jumpPosition/interpolationLength (see
    // build_channel_path's own header comment). force_lfe discards the block's real data
    // entirely: an LFE bed channel has no direction to pin (ac3/oba/atmos.hpp: "Objects never
    // reach the LFE by panning"), so it reaches the bed only via lfe_send.
    const auto placement_of = [&](const iclforge::adm::AudioBlockFormat& block)
        -> std::pair<iclforge::objects::oba::Position, double> {
        if (force_lfe) {
            return {iclforge::objects::oba::Position{}, 0.0};
        }
        return {adm_position_to_room(block.position), block.gain};
    };
    // Everything but position and gain that a block says about HOW the object is rendered, in the
    // form Keyframe carries it. An LFE bed channel has no direction, so it gets none of it.
    //
    // Width/height/depth (BS.2076-2 Tables 15-17) are the same normalized [0, 1] extents TS 103 420
    // §5.6.1.2 codes, on the same three axes: a rename, not a conversion.
    //
    // channelLock (§10.2) and b_object_snap (§5.6.1.5.1) are the same idea under two names: render
    // to the nearest speaker instead of panning. maxDistance has no image - OAMD b_object_snap is
    // one bit, with no distance to condition it on - so a conditioned channelLock maps to an
    // unconditioned snap, the closest thing the syntax can say.
    //
    // zoneExclusion (§10.4) maps through TS 103 420 Annex B.2.6 (see coordinates.hpp); a zone list
    // that is not one of Table B.18's presets keeps whatever part of it did map.
    //
    // objectDivergence (§10.5) value maps to OAMD's object_divergence (§5.2.7, Tables 40 to 42): both
    // are the share of the object's energy moved into two objects spread along X, 0 to 1. Annex B
    // does not print the correspondence; src/adm/ERRATA.md has the reading. screenRef (§10.6)
    // maps to b_object_use_screen_ref with a full screen_factor and depth_factor (see
    // Keyframe), since ADM's flag is all or nothing.
    const auto rendering_of = [&](const iclforge::adm::AudioBlockFormat& block) -> Rendering {
        if (force_lfe) {
            return {};
        }
        const auto zones = adm_zone_exclusion_to_constraint(block.zone_exclusion);
        return {.size = {.width = std::clamp(block.width, 0.0, 1.0),
                         .depth = std::clamp(block.depth, 0.0, 1.0),
                         .height = std::clamp(block.height, 0.0, 1.0)},
                .snap = block.has_channel_lock && block.channel_lock,
                .zone = zones.zone,
                .enable_elevation = zones.enable_elevation,
                .divergence = block.has_object_divergence ? std::clamp(block.object_divergence.value, 0.0, 1.0) : 0.0,
                .screen_reference = block.screen_ref};
    };
    const double lfe_send = force_lfe ? 1.0 : 0.0;

    std::vector<iclforge::objects::oba::Keyframe> keyframes;
    keyframes.reserve(channel.block_formats.size() * 2);

    // Monotonically-increasing insertion with a minimum spacing of kInstantJumpEpsilon - the one
    // mechanism that turns every §10.3 case (an instant jump, a jump immediately following a
    // zero-length "make a position jump" block per Fig. 10, or simply two blocks abutting at the
    // same nominal time) into a valid, strictly-increasing keyframe sequence without needing a
    // separate branch for each. See kInstantJumpEpsilon's own comment for why this is inaudible
    // at the resolution that reaches the bitstream.
    const auto push_keyframe = [&](double time_s, iclforge::objects::oba::Position position,
                                   double gain, const Rendering& rendering) {
        if (!keyframes.empty() && time_s <= keyframes.back().time_s) {
            time_s = keyframes.back().time_s + kInstantJumpEpsilon;
        }
        keyframes.push_back({.time_s = time_s,
                             .position = position,
                             .gain = gain,
                             .lfe_send = lfe_send,
                             .size = rendering.size,
                             .snap = rendering.snap,
                             .zone = rendering.zone,
                             .enable_elevation = rendering.enable_elevation,
                             .divergence = rendering.divergence,
                             .screen_reference = rendering.screen_reference});
    };

    if (channel.block_formats.size() == 1) {
        // §5.4.1: "If there is only one audioBlockFormat within an audioChannelFormat, the
        // characteristics of the parent audioChannelFormat are considered to be static over
        // time" - one keyframe, which iclforge::objects::oba::KeyframePath already holds
        // everywhere.
        const auto& block = channel.block_formats.front();
        const auto [position, gain] = placement_of(block);
        push_keyframe(object_start_s + block.rtime_s, position, gain, rendering_of(block));
    } else {
        for (std::size_t i = 0; i < channel.block_formats.size(); ++i) {
            const auto& block = channel.block_formats[i];
            const double t_start = object_start_s + block.rtime_s;
            const bool has_end = block.has_duration;
            const double t_end = t_start + block.duration_s;
            const auto [position, gain] = placement_of(block);
            const auto rendering = rendering_of(block);

            if (i == 0) {
                // §10.3: "the position specified in the first block covers the entire length of
                // the block (regardless of the jumpPosition and interpolationLength properties)".
                push_keyframe(t_start, position, gain, rendering);
                // A non-final block omitting duration is legal (§5.4.1 only "should" - not
                // "must" - pair rtime with duration once a channel has more than one block) but
                // discouraged, and its true end is the NEXT block's own start, not "forever":
                // channel.block_formats[1] always exists here (this whole branch only runs when
                // size() > 1). Without this, the hold-keyframe below would be skipped entirely
                // and block 1's own ramp/jump would be computed relative to this single
                // keyframe, silently stretching its interpolation span back to object_start_s
                // instead of block 1's own duration.
                const double effective_end =
                    has_end ? t_end : object_start_s + channel.block_formats[1].rtime_s;
                if (effective_end > t_start) {
                    push_keyframe(effective_end, position, gain, rendering);
                }
                continue;
            }

            if (!block.jump_position) {
                // Ramp across the FULL block duration, continuous from whatever the previous
                // block's own last keyframe already pinned at this same t_start.
                push_keyframe(has_end ? t_end : t_start, position, gain, rendering);
                continue;
            }

            // jumpPosition = 1: a ramp of interpolationLength (default/absent = as close to
            // instant as this representation allows) at the block's start, then a hold.
            const double length =
                block.has_interpolation_length ? std::max(block.interpolation_length_s, 0.0) : 0.0;
            const double ramp_end = has_end ? std::min(t_start + length, t_end) : t_start + length;
            push_keyframe(ramp_end, position, gain, rendering);
            if (has_end && t_end > ramp_end) {
                push_keyframe(t_end, position, gain, rendering);
            }
        }
    }

    auto created = iclforge::objects::oba::KeyframePath::create(std::move(keyframes));
    if (!created.has_value()) {
        // Unreachable in practice - push_keyframe's own monotonic nudge guarantees a strictly
        // increasing sequence, and it is never called with an empty channel.block_formats (the
        // kEmptyBlockSequence check above already rejected that) - kept as a real, checked error
        // rather than an assert because it is cheap to check and this boundary parses untrusted
        // file-derived data.
        return std::unexpected(BridgeError::kEmptyBlockSequence);
    }
    return iclforge::objects::oba::ObjectPath(std::move(*created));
}

namespace {

template <typename T>
const T* find_by_id(const std::vector<T>& elements, std::string_view id) {
    const auto it = std::ranges::find_if(elements, [&](const T& e) { return e.id == id; });
    return it == elements.end() ? nullptr : &*it;
}

// BS.2076-2 Table 12's own speakerLabel values for a low-frequency-effects channel - "LFE" for a
// single-LFE bed, "LFE1"/"LFE2" for a dual-LFE one (the standard's own 9.1/7.1.2 worked examples
// use both forms).
bool is_lfe_label(const std::string& label) {
    return label == "LFE" || label == "LFE1" || label == "LFE2";
}

bool channel_is_lfe(const iclforge::adm::AudioChannelFormat& channel) {
    for (const auto& block : channel.block_formats) {
        if (std::ranges::any_of(block.speaker_labels, is_lfe_label)) {
            return true;
        }
    }
    return false;
}

// What a channel's blocks ask for that this bridge does not carry into the Atmos encode. Each
// name appears once however many blocks use it. An LFE bed channel is routed by lfe_send alone,
// so nothing it carries is lost.
std::vector<std::string> unmapped_features(const iclforge::adm::AudioChannelFormat& channel, bool force_lfe) {
    std::vector<std::string> out;
    if (force_lfe) {
        return out;
    }
    const auto any_block = [&](auto&& predicate) {
        return std::ranges::any_of(channel.block_formats, predicate);
    };
    const auto note = [&](bool present, std::string_view name) {
        if (present) {
            out.emplace_back(name);
        }
    };
    // §10.5 objectDivergence's value maps (see rendering_of); its azimuthRange and positionRange,
    // which say where the two objects go, have no OAMD field.
    note(any_block([](const auto& b) {
             return b.has_object_divergence && b.object_divergence.value > 0.0 &&
                    (b.object_divergence.has_azimuth_range || b.object_divergence.has_position_range);
         }),
         "objectDivergence range");
    note(any_block([](const auto& b) { return b.head_locked; }), "headLocked");
    note(any_block([](const auto& b) { return b.diffuse > 0.0; }), "diffuse");
    note(any_block([](const auto& b) { return b.has_channel_lock_max_distance; }),
         "channelLock maxDistance");
    note(any_block([](const auto& b) { return !adm_zone_exclusion_to_constraint(b.zone_exclusion).exact; }),
         "zoneExclusion (not a TS 103 420 Table B.18 preset)");
    return out;
}

struct ClassifiedObject {
    bool is_bed = false;
    std::vector<const iclforge::adm::AudioChannelFormat*> channels;
};

// Resolves one audioObject's pack_format_refs down to an ordered list of audioChannelFormats and
// classifies it as a bed (every resolved pack is DirectSpeakers) or a dynamic object (every
// resolved pack is Objects) - see bridge.hpp's own top comment for what happens to every other
// TypeDefinition and to nested audioPackFormats.
std::expected<ClassifiedObject, BridgeError> classify_object(
    const iclforge::adm::AdmModel& model, const iclforge::adm::AudioObject& object) {
    ClassifiedObject result;
    std::optional<iclforge::adm::TypeDefinition> agreed_type;
    for (const auto& pack_ref : object.pack_format_refs) {
        const auto* pack = find_by_id(model.pack_formats, pack_ref);
        if (pack == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        if (!pack->pack_format_refs.empty()) {
            // Nested audioPackFormat (§5.5's own pack_format_refs sub-reference - Matrix decode/
            // HOA layouts nest packs this way) - out of this phase's scope, see bridge.hpp's own
            // top comment.
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        if (pack->type != iclforge::adm::TypeDefinition::kDirectSpeakers &&
            pack->type != iclforge::adm::TypeDefinition::kObjects) {
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        if (agreed_type.has_value() && *agreed_type != pack->type) {
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        agreed_type = pack->type;
        for (const auto& channel_ref : pack->channel_format_refs) {
            const auto* channel = find_by_id(model.channel_formats, channel_ref);
            if (channel == nullptr) {
                return std::unexpected(BridgeError::kUnresolvedReference);
            }
            result.channels.push_back(channel);
        }
    }
    result.is_bed = (agreed_type == iclforge::adm::TypeDefinition::kDirectSpeakers);
    return result;
}

// Depth-first walk from one audioProgramme's own audioContents down through every audioObject
// they reference, following nested audioObject::object_refs (§5.6: "AudioObjects can be nested
// and so they can refer to other audioObjects") and collecting every object that carries its own
// pack_format_refs (a "leaf" with actual audio to place - a pure container object that only
// nests further objects contributes nothing of its own). `visiting` guards against a reference
// cycle (§5.6.7: "An audioObject element should not reference itself, nor can a loop of
// references be used").
std::expected<std::vector<const iclforge::adm::AudioObject*>, BridgeError> collect_leaf_objects(
    const iclforge::adm::AdmModel& model, const iclforge::adm::AudioProgramme& programme) {
    std::vector<const iclforge::adm::AudioObject*> objects;
    std::vector<std::string> visiting;

    const std::function<std::expected<void, BridgeError>(const std::string&)> visit =
        [&](const std::string& object_id) -> std::expected<void, BridgeError> {
        if (std::ranges::contains(visiting, object_id)) {
            return std::unexpected(BridgeError::kObjectReferenceCycle);
        }
        const auto* object = find_by_id(model.objects, object_id);
        if (object == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        if (!object->pack_format_refs.empty()) {
            objects.push_back(object);
        }
        visiting.push_back(object_id);
        for (const auto& child_id : object->object_refs) {
            if (const auto child_result = visit(child_id); !child_result.has_value()) {
                return child_result;
            }
        }
        visiting.pop_back();
        return {};
    };

    for (const auto& content_ref : programme.content_refs) {
        const auto* content = find_by_id(model.contents, content_ref);
        if (content == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        for (const auto& object_ref : content->object_refs) {
            if (const auto result = visit(object_ref); !result.has_value()) {
                return std::unexpected(result.error());
            }
        }
    }
    return objects;
}

// TS 103 420 §8.3.2.2 caps complexity_index_type_a (== object_count(program), see ac3/oba/
// oamd.hpp) at 16 total, with AtmosEncoder's own implicit LFE bookkeeping (see atmos.cpp's
// AtmosEncoder::AtmosEncoder: `Program{.dynamic_only = true, .lfe = true, ...}`) always taking
// one of those 16 regardless of what this bridge produces - the same cap and the same reasoning
// apps/cli/main.cpp's run_atmos_encode/run_atmos_path already apply to the `objects` constructor
// parameter, reused here rather than re-derived.
constexpr std::size_t kMaxChannels = 15;

}  // namespace

std::expected<BridgeResult, BridgeError> build(const iclforge::adm::AdmDocument& document,
                                                std::string_view programme_id) {
    const auto& model = document.model;
    if (model.programmes.empty()) {
        return std::unexpected(BridgeError::kNoProgramme);
    }

    const iclforge::adm::AudioProgramme* programme = nullptr;
    if (programme_id.empty()) {
        // §5.8: IDs are formatted strings (APR_wwww, fixed-width hex), so a lexicographic
        // compare over them is a numeric-ID compare too.
        programme = &*std::ranges::min_element(model.programmes, std::ranges::less{},
                                               &iclforge::adm::AudioProgramme::id);
    } else {
        programme = find_by_id(model.programmes, programme_id);
        if (programme == nullptr) {
            return std::unexpected(BridgeError::kProgrammeNotFound);
        }
    }

    const auto leaf_objects = collect_leaf_objects(model, *programme);
    if (!leaf_objects.has_value()) {
        return std::unexpected(leaf_objects.error());
    }

    BridgeResult out;
    out.sample_rate = document.audio.sample_rate;

    for (const auto* object : *leaf_objects) {
        const auto classified = classify_object(model, *object);
        if (!classified.has_value()) {
            return std::unexpected(classified.error());
        }
        if (classified->channels.size() != object->track_uid_refs.size()) {
            return std::unexpected(BridgeError::kChannelTrackMismatch);
        }

        for (std::size_t i = 0; i < classified->channels.size(); ++i) {
            const auto* channel = classified->channels[i];
            const auto& track_uid_ref = object->track_uid_refs[i];

            const auto chna_it = std::ranges::find(document.chna, track_uid_ref,
                                                    &iclforge::adm::ChnaEntry::uid);
            if (chna_it == document.chna.end()) {
                return std::unexpected(BridgeError::kUnresolvedReference);
            }
            if (chna_it->track_index == 0 ||
                static_cast<std::size_t>(chna_it->track_index) > document.audio.channels.size()) {
                return std::unexpected(BridgeError::kNoAudioForTrack);
            }

            const bool is_lfe = classified->is_bed && channel_is_lfe(*channel);
            auto path = build_channel_path(*channel, object->start_s, is_lfe);
            if (!path.has_value()) {
                return std::unexpected(path.error());
            }

            out.channel_ids.push_back(channel->id);
            out.unmapped.push_back(unmapped_features(*channel, is_lfe));
            out.is_bed.push_back(classified->is_bed);
            out.is_lfe.push_back(is_lfe);
            out.paths.push_back(std::move(*path));
            out.pcm.emplace_back(
                document.audio.channels[static_cast<std::size_t>(chna_it->track_index) - 1]);
        }
    }

    if (out.paths.size() > kMaxChannels) {
        return std::unexpected(BridgeError::kTooManyChannels);
    }
    return out;
}

namespace {

// One dynamic object's audioBlockFormat sequence from its flattened OAMD update timeline. TS 103
// 420's own per-block model - a value takes effect at `sample_offset`, reached over
// `ramp_duration` samples, then held - is already, block for block, exactly BS.2076-2 §10.3's
// jumpPosition=1 + interpolationLength case (see WriteObjectUpdate's own doc comment in
// bridge.hpp), so unlike build_channel_path()'s read-direction state machine above this needs no
// case analysis: every update but the first becomes one audioBlockFormat with jumpPosition set;
// the first becomes one plain hold, per §10.3's "the first block covers its entire length
// regardless of jumpPosition" rule - the same rule build_channel_path()'s own i==0 branch reads
// off the read direction.
//
// Non-increasing sample_offsets (two updates landing on the same absolute sample, or a caller
// bug) are folded together - the later one overwrites the earlier rather than producing a
// zero-or-negative-duration audioBlockFormat, mirroring build_channel_path()'s own
// kInstantJumpEpsilon nudge for the equivalent read-direction case.
std::vector<iclforge::adm::AudioBlockFormat> build_block_formats(std::span<const WriteObjectUpdate> updates,
                                                          double total_duration_s, std::uint32_t sample_rate) {
    std::vector<WriteObjectUpdate> distinct;
    distinct.reserve(updates.size());
    for (const auto& update : updates) {
        if (!distinct.empty() && update.sample_offset <= distinct.back().sample_offset) {
            distinct.back() = update;
            continue;
        }
        distinct.push_back(update);
    }

    const auto time_of = [sample_rate](std::uint64_t sample) {
        return static_cast<double>(sample) / static_cast<double>(sample_rate);
    };
    const auto place = [](iclforge::adm::AudioBlockFormat& block,
                          const iclforge::objects::oba::DynamicObject& state) {
        block.cartesian = true;
        block.position = room_to_adm_cartesian(state.position);
        block.gain = std::pow(10.0, state.gain_db / 20.0);
        block.width = state.size.width;
        block.height = state.size.height;
        block.depth = state.size.depth;
        if (state.snap) {
            block.has_channel_lock = true;
            block.channel_lock = true;
        }
        // TS 103 420 Annex B.2.6: zone constraints go out as a zoneExclusion.
        block.zone_exclusion = constraint_to_adm_zone_exclusion(state.zone, state.enable_elevation);
        // The divergence value (Table 42) and the screen reference. ADM's screenRef is all or nothing, so
        // a screen_factor below one half reads as room-anchored.
        if (state.divergence > 0.0) {
            block.has_object_divergence = true;
            block.object_divergence.value = std::clamp(state.divergence, 0.0, 1.0);
        }
        block.screen_ref = state.screen_reference && state.screen_factor >= 0.5;
    };

    std::vector<iclforge::adm::AudioBlockFormat> blocks;
    blocks.reserve(distinct.size());

    iclforge::adm::AudioBlockFormat first;
    first.rtime_s = time_of(distinct.front().sample_offset);
    place(first, distinct.front().state);
    if (distinct.size() > 1) {
        first.has_duration = true;
        first.duration_s = time_of(distinct[1].sample_offset) - first.rtime_s;
    }
    blocks.push_back(std::move(first));

    for (std::size_t i = 1; i < distinct.size(); ++i) {
        iclforge::adm::AudioBlockFormat block;
        block.rtime_s = time_of(distinct[i].sample_offset);
        place(block, distinct[i].state);
        block.has_duration = true;
        const bool has_next = i + 1 < distinct.size();
        block.duration_s = (has_next ? time_of(distinct[i + 1].sample_offset) : total_duration_s) - block.rtime_s;
        block.has_jump_position = true;
        block.jump_position = true;
        const auto ramp_samples = distinct[i].ramp_duration_samples;
        if (ramp_samples > 0) {
            block.has_interpolation_length = true;
            block.interpolation_length_s =
                std::min(static_cast<double>(ramp_samples) / static_cast<double>(sample_rate), block.duration_s);
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

}  // namespace

std::expected<iclforge::adm::AdmDocument, BridgeError> write(const WriteInput& input) {
    if (input.channels.empty()) {
        return std::unexpected(BridgeError::kEmptyInput);
    }

    iclforge::adm::AdmDocument document;
    document.audio.sample_rate = input.sample_rate;
    // The width iclforge::adm::write_bw64 stores this PCM at, which every audioTrackUID below
    // states as its bitDepth too - so the document already describes the master it becomes.
    document.audio.bits_per_sample = iclforge::adm::kWriteBitDepth;

    auto& model = document.model;
    model.channel_formats.reserve(input.channels.size());
    model.pack_formats.reserve(input.channels.size());
    model.stream_formats.reserve(input.channels.size());
    model.track_formats.reserve(input.channels.size());
    model.track_uids.reserve(input.channels.size());
    model.objects.reserve(input.channels.size());
    document.chna.reserve(input.channels.size());
    document.audio.channels.reserve(input.channels.size());

    for (std::size_t i = 0; i < input.channels.size(); ++i) {
        const auto& channel = input.channels[i];
        // A correlation key only, never written literally - see ac3adm.hpp's own write_bw64 doc
        // comment on why iclforge::adm::write_bw64 lets libadm's reassignIds() assign the real IDs.
        const std::string key = std::to_string(i);

        iclforge::adm::AudioChannelFormat channel_format;
        channel_format.id = "chan_" + key;
        channel_format.name = channel.name;

        iclforge::adm::AudioPackFormat pack_format;
        pack_format.id = "pack_" + key;
        pack_format.name = channel.name;
        pack_format.channel_format_refs = {channel_format.id};

        if (channel.bed_label.has_value()) {
            channel_format.type = iclforge::adm::TypeDefinition::kDirectSpeakers;
            pack_format.type = iclforge::adm::TypeDefinition::kDirectSpeakers;

            iclforge::adm::AudioBlockFormat block;
            block.cartesian = true;
            block.position = room_to_adm_cartesian(iclforge::objects::oba::bed_label_position(*channel.bed_label));
            block.speaker_labels = {
                std::string(iclforge::objects::oba::describe(*channel.bed_label))};
            channel_format.block_formats.push_back(std::move(block));
        } else {
            if (channel.updates.empty()) {
                return std::unexpected(BridgeError::kEmptyInput);
            }
            channel_format.type = iclforge::adm::TypeDefinition::kObjects;
            pack_format.type = iclforge::adm::TypeDefinition::kObjects;
            const double total_duration_s = static_cast<double>(channel.pcm.size()) / input.sample_rate;
            channel_format.block_formats = build_block_formats(channel.updates, total_duration_s, input.sample_rate);
        }

        // §5.1/§5.2's full PCM chain (audioStreamFormat -> audioTrackFormat), not BS.2076-2's
        // "plain PCM" direct-to-channel shortcut: libadm's own reassignIds() zeroes out any
        // audioChannelFormat that no audioStreamFormat references ("get an Id with the value
        // zero and are thereby marked as ADM elements which should be ignored" - id_assignment.hpp's
        // own doc comment), which the shortcut alone triggers - every audioChannelFormat this
        // writer produces collapsed to the same "AC_00000000" id until this chain was added
        // (caught by tests/adm/test_adm_bridge_write.cpp's own round trip, not by
        // construction here). This is also the exact wiring libadm's own
        // adm/utilities/object_creation.cpp uses for its Objects-type helper.
        iclforge::adm::AudioStreamFormat stream_format;
        stream_format.id = "stream_" + key;
        stream_format.name = channel.name;
        stream_format.channel_format_ref = channel_format.id;

        iclforge::adm::AudioTrackFormat track_format;
        track_format.id = "track_" + key;
        track_format.name = channel.name;
        track_format.stream_format_ref = stream_format.id;

        iclforge::adm::AudioTrackUid track_uid;
        track_uid.uid = "atu_" + key;
        track_uid.has_sample_rate = true;
        track_uid.sample_rate = input.sample_rate;
        // The Dolby Atmos Master ADM Profile expects bitDepth on every audioTrackUID, matching
        // <fmt >. write_bw64 writes kWriteBitDepth there whatever this field says (see its doc
        // comment); it is set here as well so a caller inspecting this document sees the same.
        track_uid.has_bit_depth = true;
        track_uid.bit_depth = iclforge::adm::kWriteBitDepth;
        track_uid.track_format_ref = track_format.id;
        track_uid.pack_format_ref = pack_format.id;

        iclforge::adm::AudioObject object;
        object.id = "obj_" + key;
        object.name = channel.name;
        object.pack_format_refs = {pack_format.id};
        object.track_uid_refs = {track_uid.uid};

        // track_ref/pack_ref explicitly empty: write_bw64 derives the real ones itself from the
        // resolved AudioTrackUid's own references (see ac3adm.hpp's own write_bw64 doc comment) -
        // GCC's -Wmissing-field-initializers still wants every ChnaEntry field named here, even
        // ones this writer never populates.
        document.chna.push_back({.track_index = static_cast<std::uint16_t>(i + 1),
                                 .uid = track_uid.uid,
                                 .track_ref = {},
                                 .pack_ref = {}});
        document.audio.channels.emplace_back(channel.pcm.begin(), channel.pcm.end());

        model.channel_formats.push_back(std::move(channel_format));
        model.pack_formats.push_back(std::move(pack_format));
        model.stream_formats.push_back(std::move(stream_format));
        model.track_formats.push_back(std::move(track_format));
        model.track_uids.push_back(std::move(track_uid));
        model.objects.push_back(std::move(object));
    }

    iclforge::adm::AudioContent content;
    content.id = "content";
    content.name = "Programme";
    for (const auto& object : model.objects) {
        content.object_refs.push_back(object.id);
    }
    model.contents.push_back(std::move(content));

    iclforge::adm::AudioProgramme programme;
    programme.id = "programme";
    programme.name = "Programme";
    programme.content_refs = {model.contents.back().id};
    model.programmes.push_back(std::move(programme));

    return document;
}

}  // namespace iclforge::adm
