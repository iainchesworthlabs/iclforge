#include "adm_model.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

#include <adm/adm.hpp>
#include <boost/variant.hpp>

// Clause references below are to Recommendation ITU-R BS.2076-2 (10/2019)
// Annex 1 unless stated otherwise. Every `::adm::` symbol in this file is
// libadm's own (github.com/ebu/libadm); every `iclforge::adm::` symbol - including
// the unqualified ones, since this file lives inside namespace iclforge::adm::detail
// - is this module's own. See adm_model.hpp's header comment for why the two
// must never mix unqualified.

namespace iclforge::adm::detail {

namespace {

double to_seconds(const ::adm::Time& time) {
    return std::chrono::duration<double>(time.asNanoseconds()).count();
}

// libadm declares several coordinate/dimension NamedTypes as NamedType<float, ...> even though
// this module's own equivalents (ac3adm/model.hpp) are double - no precision reason, just how
// libadm declared them. Every plain float->double widening below goes through this helper to
// make the promotion explicit rather than implicit: -Wdouble-promotion is real on GCC/Clang
// (never caught by MSVC - see CONTRIBUTING.md's own multi-compiler verification note) and
// Clang's variant of it - unlike GCC's, which only fires when a float and a double literal have
// to unify inside a ternary - flags every single one of these plain assignments too, confirmed
// directly by building this exact file on both.
double to_double(float value) {
    return static_cast<double>(value);
}

// Table 7/10/20/53's five typeDefinition values, plus UNDEFINED and the
// 0x1000-0xFFFF "User Custom" range libadm folds into neither - see
// ::adm::TypeDescriptor's own doc comment ("valid values are in the range
// [0, 5]"): libadm does not model typeLabel values above 5 as a distinct
// concept, so any TypeDescriptor this project doesn't recognize (there are
// none beyond UNDEFINED/0..5) falls back to kUserCustom.
TypeDefinition to_type_definition(const ::adm::TypeDescriptor& type_descriptor) {
    if (type_descriptor == ::adm::TypeDefinition::DIRECT_SPEAKERS) return TypeDefinition::kDirectSpeakers;
    if (type_descriptor == ::adm::TypeDefinition::MATRIX) return TypeDefinition::kMatrix;
    if (type_descriptor == ::adm::TypeDefinition::OBJECTS) return TypeDefinition::kObjects;
    if (type_descriptor == ::adm::TypeDefinition::HOA) return TypeDefinition::kHoa;
    if (type_descriptor == ::adm::TypeDefinition::BINAURAL) return TypeDefinition::kBinaural;
    if (type_descriptor == ::adm::TypeDefinition::UNDEFINED) return TypeDefinition::kUnknown;
    return TypeDefinition::kUserCustom;
}

// One overload per ADM element type - each type's own `id_type` typedef
// names a distinct libadm class (AudioObjectId, AudioContentId, ...), so a
// single function template keyed on the element type covers every case via
// ordinary overload resolution rather than needing a trait per element.
std::string id_of(const std::shared_ptr<const ::adm::AudioProgramme>& e) {
    return ::adm::formatId(e->get<::adm::AudioProgrammeId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioContent>& e) {
    return ::adm::formatId(e->get<::adm::AudioContentId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioObject>& e) {
    return ::adm::formatId(e->get<::adm::AudioObjectId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioPackFormat>& e) {
    return ::adm::formatId(e->get<::adm::AudioPackFormatId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioChannelFormat>& e) {
    return ::adm::formatId(e->get<::adm::AudioChannelFormatId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioStreamFormat>& e) {
    return ::adm::formatId(e->get<::adm::AudioStreamFormatId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioTrackFormat>& e) {
    return ::adm::formatId(e->get<::adm::AudioTrackFormatId>());
}
std::string id_of(const std::shared_ptr<const ::adm::AudioTrackUid>& e) {
    return ::adm::formatId(e->get<::adm::AudioTrackUidId>());
}

template <typename Element>
std::optional<std::string> id_of_opt(const std::shared_ptr<const Element>& e) {
    if (!e) {
        return std::nullopt;
    }
    return id_of(e);
}

template <typename Range>
std::vector<std::string> ids_of(const Range& range) {
    std::vector<std::string> ids;
    for (const auto& element : range) {
        ids.push_back(id_of(element));
    }
    return ids;
}

// §5.4.3.1/§5.4.3.3, Tables 12/15/16: DirectSpeakers and Objects blocks both
// carry a position, but as different libadm types (SpeakerPosition vs.
// Position) with different sub-getter names - this pulls the common
// azimuth/elevation/distance / X/Y/Z shape out of whichever one a given
// block actually has.
template <typename Spherical, typename Cartesian, typename BlockFormat>
void set_position(const BlockFormat& block, AudioBlockFormat& out) {
    if (block.template has<Cartesian>()) {
        const auto cartesian = block.template get<Cartesian>();
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = to_double(cartesian.template get<::adm::X>().get()),
            .y = to_double(cartesian.template get<::adm::Y>().get()),
            .z = cartesian.template has<::adm::Z>() ? to_double(cartesian.template get<::adm::Z>().get()) : 0.0,
        };
    } else if (block.template has<Spherical>()) {
        const auto spherical = block.template get<Spherical>();
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = to_double(spherical.template get<::adm::Azimuth>().get()),
            .elevation_deg = to_double(spherical.template get<::adm::Elevation>().get()),
            .distance = spherical.template has<::adm::Distance>()
                            ? to_double(spherical.template get<::adm::Distance>().get())
                            : 1.0,
        };
    }
}

// §5.4.3.3, Table 17: Objects' `position` (unlike DirectSpeakers') is
// exposed by libadm as a single `::adm::Position` boost::variant, so this
// takes the more direct isCartesian()/boost::get() path instead of
// set_position()'s has<Cartesian>()/has<Spherical>() probing.
void set_objects_position(const ::adm::AudioBlockFormatObjects& block, AudioBlockFormat& out) {
    if (!block.has<::adm::Position>()) {
        return;
    }
    const ::adm::Position position = block.get<::adm::Position>();
    if (::adm::isCartesian(position)) {
        const auto& cartesian = boost::get<::adm::CartesianPosition>(position);
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = to_double(cartesian.get<::adm::X>().get()),
            .y = to_double(cartesian.get<::adm::Y>().get()),
            .z = cartesian.has<::adm::Z>() ? to_double(cartesian.get<::adm::Z>().get()) : 0.0,
        };
    } else {
        const auto& spherical = boost::get<::adm::SphericalPosition>(position);
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = to_double(spherical.get<::adm::Azimuth>().get()),
            .elevation_deg = to_double(spherical.get<::adm::Elevation>().get()),
            .distance = spherical.has<::adm::Distance>() ? to_double(spherical.get<::adm::Distance>().get()) : 1.0,
        };
    }
}

AudioBlockFormat convert_common(const std::string& id, const ::adm::Rtime& rtime,
                                 const boost::optional<::adm::Duration>& duration, const ::adm::Gain& gain,
                                 const ::adm::Importance& importance) {
    AudioBlockFormat block;
    block.id = id;
    block.rtime_s = to_seconds(rtime.get());
    if (duration) {
        block.has_duration = true;
        block.duration_s = to_seconds(duration->get());
    }
    block.gain = gain.asLinear();
    block.has_importance = true;
    block.importance = importance.get();
    return block;
}

AudioBlockFormat convert(const ::adm::AudioBlockFormatDirectSpeakers& src) {
    auto block = convert_common(::adm::formatId(src.get<::adm::AudioBlockFormatId>()), src.get<::adm::Rtime>(),
                                 src.has<::adm::Duration>() ? boost::optional<::adm::Duration>(src.get<::adm::Duration>())
                                                           : boost::none,
                                 src.get<::adm::Gain>(), src.get<::adm::Importance>());
    set_position<::adm::SphericalSpeakerPosition, ::adm::CartesianSpeakerPosition>(src, block);
    for (const auto& label : src.has<::adm::SpeakerLabels>() ? src.get<::adm::SpeakerLabels>() : ::adm::SpeakerLabels{}) {
        block.speaker_labels.push_back(label.get());
    }
    return block;
}

AudioBlockFormat convert(const ::adm::AudioBlockFormatObjects& src) {
    auto block = convert_common(::adm::formatId(src.get<::adm::AudioBlockFormatId>()), src.get<::adm::Rtime>(),
                                 src.has<::adm::Duration>() ? boost::optional<::adm::Duration>(src.get<::adm::Duration>())
                                                           : boost::none,
                                 src.get<::adm::Gain>(), src.get<::adm::Importance>());
    set_objects_position(src, block);
    block.width = to_double(src.get<::adm::Width>().get());
    block.height = to_double(src.get<::adm::Height>().get());
    block.depth = to_double(src.get<::adm::Depth>().get());
    block.diffuse = to_double(src.get<::adm::Diffuse>().get());
    if (src.has<::adm::ChannelLock>()) {
        const auto channel_lock = src.get<::adm::ChannelLock>();
        block.has_channel_lock = true;
        block.channel_lock = channel_lock.get<::adm::ChannelLockFlag>().get();
        if (channel_lock.has<::adm::MaxDistance>()) {
            block.has_channel_lock_max_distance = true;
            block.channel_lock_max_distance =
                to_double(channel_lock.get<::adm::MaxDistance>().get());
        }
    }
    // §10.5/§10.6: objectDivergence and screenRef/headLocked are DefaultParameters in libadm, so
    // has<>() is always true; isDefault<>() is what says whether the file actually carried them.
    if (!src.isDefault<::adm::ObjectDivergence>()) {
        const auto divergence = src.get<::adm::ObjectDivergence>();
        block.has_object_divergence = true;
        block.object_divergence.value = to_double(divergence.get<::adm::Divergence>().get());
        if (!divergence.isDefault<::adm::AzimuthRange>()) {
            block.object_divergence.has_azimuth_range = true;
            block.object_divergence.azimuth_range_deg =
                to_double(divergence.get<::adm::AzimuthRange>().get());
        }
        if (!divergence.isDefault<::adm::PositionRange>()) {
            block.object_divergence.has_position_range = true;
            block.object_divergence.position_range =
                to_double(divergence.get<::adm::PositionRange>().get());
        }
    }
    block.screen_ref = src.get<::adm::ScreenRef>().get();
    block.head_locked = src.get<::adm::HeadLocked>().get();
    if (src.has<::adm::JumpPosition>()) {
        const auto jump_position = src.get<::adm::JumpPosition>();
        block.has_jump_position = true;
        block.jump_position = jump_position.get<::adm::JumpPositionFlag>().get();
        if (jump_position.has<::adm::InterpolationLength>()) {
            block.has_interpolation_length = true;
            block.interpolation_length_s =
                std::chrono::duration<double>(jump_position.get<::adm::InterpolationLength>().get()).count();
        }
    }
    return block;
}

AudioBlockFormat convert(const ::adm::AudioBlockFormatHoa& src) {
    auto block = convert_common(::adm::formatId(src.get<::adm::AudioBlockFormatId>()), src.get<::adm::Rtime>(),
                                 src.has<::adm::Duration>() ? boost::optional<::adm::Duration>(src.get<::adm::Duration>())
                                                           : boost::none,
                                 src.get<::adm::Gain>(), src.get<::adm::Importance>());
    // §5.4.3.4: order/degree are Required for HOA blocks - libadm's parser
    // already enforces that, so has_hoa_order/has_hoa_degree are set
    // unconditionally rather than guarded by has<>() the way the genuinely
    // optional fields above are.
    block.has_hoa_order = true;
    block.hoa_order = src.get<::adm::Order>().get();
    block.has_hoa_degree = true;
    block.hoa_degree = src.get<::adm::Degree>().get();
    block.hoa_normalization = src.get<::adm::Normalization>().get();
    // nfcRefDist, screenRef and headLocked are DefaultParameters (has<>() is always true), so
    // isDefault<>() says whether the file carried nfcRefDist; the two flags' defaults are false.
    if (!src.isDefault<::adm::NfcRefDist>()) {
        block.has_nfc_ref_dist = true;
        block.nfc_ref_dist = to_double(src.get<::adm::NfcRefDist>().get());
    }
    if (src.has<::adm::Equation>()) {
        block.hoa_equation = src.get<::adm::Equation>().get();
    }
    block.screen_ref = src.get<::adm::ScreenRef>().get();
    block.head_locked = src.get<::adm::HeadLocked>().get();
    return block;
}

// Matrix and Binaural blocks (§5.4.3.2, §5.4.3.5) contribute only the common
// id/rtime/duration/gain/importance fields set by convert_common(). A Binaural
// block has nothing else (Table A1-18). A Matrix block's outputChannelFormatIDRef,
// coefficients and jumpPosition are not libadm parameters at all, so
// scan_matrix_blocks() (adm_xml_extras.cpp) reads them from the axml text and
// parse_axml() attaches them to the block by ID.
AudioBlockFormat convert(const ::adm::AudioBlockFormatMatrix& src) {
    return convert_common(
        ::adm::formatId(src.get<::adm::AudioBlockFormatId>()), src.get<::adm::Rtime>(),
        src.has<::adm::Duration>() ? boost::optional<::adm::Duration>(src.get<::adm::Duration>())
                                   : boost::none,
        src.get<::adm::Gain>(), src.get<::adm::Importance>());
}

AudioBlockFormat convert(const ::adm::AudioBlockFormatBinaural& src) {
    return convert_common(
        ::adm::formatId(src.get<::adm::AudioBlockFormatId>()), src.get<::adm::Rtime>(),
        src.has<::adm::Duration>() ? boost::optional<::adm::Duration>(src.get<::adm::Duration>())
                                   : boost::none,
        src.get<::adm::Gain>(), src.get<::adm::Importance>());
}

AudioChannelFormat convert(const std::shared_ptr<const ::adm::AudioChannelFormat>& src) {
    AudioChannelFormat channel_format;
    channel_format.id = id_of(src);
    channel_format.name = src->get<::adm::AudioChannelFormatName>().get();
    const ::adm::TypeDescriptor type_descriptor = src->get<::adm::TypeDescriptor>();
    channel_format.type = to_type_definition(type_descriptor);

    // §5.3.2: exactly one of these five ranges is non-empty, matching
    // channel_format.type - libadm stores each typeDefinition's blocks in
    // its own internal vector (see AudioChannelFormat::getElements<T>()).
    for (const auto& block : src->getElements<::adm::AudioBlockFormatDirectSpeakers>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<::adm::AudioBlockFormatObjects>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<::adm::AudioBlockFormatHoa>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<::adm::AudioBlockFormatMatrix>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<::adm::AudioBlockFormatBinaural>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    return channel_format;
}

AudioPackFormat convert(const std::shared_ptr<const ::adm::AudioPackFormat>& src) {
    AudioPackFormat pack_format;
    pack_format.id = id_of(src);
    pack_format.name = src->get<::adm::AudioPackFormatName>().get();
    pack_format.type = to_type_definition(src->get<::adm::TypeDescriptor>());
    pack_format.channel_format_refs = ids_of(src->getReferences<::adm::AudioChannelFormat>());
    pack_format.pack_format_refs = ids_of(src->getReferences<::adm::AudioPackFormat>());
    // libadm reads an HOA pack's normalization, nfcRefDist and screenRef as XML attributes; the
    // standard (BS.2076-3 Table A1-25) and the EBU's own renderer have them as sub-elements, which
    // scan_pack_extras() reads and parse_axml() lays over this. Both spellings are accepted.
    if (const auto hoa = std::dynamic_pointer_cast<const ::adm::AudioPackFormatHoa>(src)) {
        if (!hoa->isDefault<::adm::Normalization>()) {
            pack_format.hoa_normalization = hoa->get<::adm::Normalization>().get();
        }
        if (!hoa->isDefault<::adm::NfcRefDist>()) {
            pack_format.has_nfc_ref_dist = true;
            pack_format.nfc_ref_dist = to_double(hoa->get<::adm::NfcRefDist>().get());
        }
        pack_format.screen_ref = hoa->get<::adm::ScreenRef>().get();
    }
    return pack_format;
}

AudioStreamFormat convert(const std::shared_ptr<const ::adm::AudioStreamFormat>& src) {
    AudioStreamFormat stream_format;
    stream_format.id = id_of(src);
    stream_format.name = src->get<::adm::AudioStreamFormatName>().get();
    stream_format.channel_format_ref = id_of_opt(src->getReference<::adm::AudioChannelFormat>());
    stream_format.pack_format_ref = id_of_opt(src->getReference<::adm::AudioPackFormat>());
    // §5.2's audioTrackFormatIDRef is 0..* but AudioTrackFormat is held via
    // weak_ptr on this side of the (cyclic) reference - see
    // AudioStreamFormat::getAudioTrackFormatReferences()'s own doc comment.
    for (const auto& weak_track : src->getAudioTrackFormatReferences()) {
        if (const auto track = weak_track.lock()) {
            stream_format.track_format_refs.push_back(id_of(track));
        }
    }
    return stream_format;
}

AudioTrackFormat convert(const std::shared_ptr<const ::adm::AudioTrackFormat>& src) {
    AudioTrackFormat track_format;
    track_format.id = id_of(src);
    track_format.name = src->get<::adm::AudioTrackFormatName>().get();
    track_format.stream_format_ref = id_of_opt(src->getReference<::adm::AudioStreamFormat>());
    return track_format;
}

AudioTrackUid convert(const std::shared_ptr<const ::adm::AudioTrackUid>& src) {
    AudioTrackUid track_uid;
    track_uid.uid = id_of(src);
    if (src->has<::adm::SampleRate>()) {
        track_uid.has_sample_rate = true;
        track_uid.sample_rate = src->get<::adm::SampleRate>().get();
    }
    if (src->has<::adm::BitDepth>()) {
        track_uid.has_bit_depth = true;
        track_uid.bit_depth = src->get<::adm::BitDepth>().get();
    }
    track_uid.track_format_ref = id_of_opt(src->getReference<::adm::AudioTrackFormat>());
    track_uid.channel_format_ref = id_of_opt(src->getReference<::adm::AudioChannelFormat>());
    track_uid.pack_format_ref = id_of_opt(src->getReference<::adm::AudioPackFormat>());
    return track_uid;
}

AudioObject convert(const std::shared_ptr<const ::adm::AudioObject>& src) {
    AudioObject object;
    object.id = id_of(src);
    object.name = src->get<::adm::AudioObjectName>().get();
    object.start_s = to_seconds(src->get<::adm::Start>().get());
    if (src->has<::adm::Duration>()) {
        object.has_duration = true;
        object.duration_s = to_seconds(src->get<::adm::Duration>().get());
    }
    object.pack_format_refs = ids_of(src->getReferences<::adm::AudioPackFormat>());
    object.track_uid_refs = ids_of(src->getReferences<::adm::AudioTrackUid>());
    object.object_refs = ids_of(src->getReferences<::adm::AudioObject>());
    return object;
}

AudioContent convert(const std::shared_ptr<const ::adm::AudioContent>& src) {
    AudioContent content;
    content.id = id_of(src);
    content.name = src->get<::adm::AudioContentName>().get();
    content.object_refs = ids_of(src->getReferences<::adm::AudioObject>());
    return content;
}

AudioProgramme convert(const std::shared_ptr<const ::adm::AudioProgramme>& src) {
    AudioProgramme programme;
    programme.id = id_of(src);
    programme.name = src->get<::adm::AudioProgrammeName>().get();
    programme.content_refs = ids_of(src->getReferences<::adm::AudioContent>());
    return programme;
}

}  // namespace

namespace {

// The write-side counterpart of to_seconds() above: an iclforge::adm::AudioBlockFormat's *_s fields
// are plain seconds (model.hpp's own convention, chosen so nothing downstream of ac3adm has to know
// libadm's Time/FractionalTime split exists), so every rtime/duration/interpolationLength this
// writer emits goes through this one conversion rather than five ad-hoc ones.
::adm::Time seconds_to_time(double seconds) {
    return ::adm::Time(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds)));
}

// The libadm type descriptor a model TypeDefinition is written as. Nothing for kUnknown and
// kUserCustom: libadm's TypeDescriptor holds the five defined types, so neither has an element to
// become.
bool to_libadm_type(TypeDefinition type, ::adm::TypeDescriptor& out) {
    switch (type) {
        case TypeDefinition::kDirectSpeakers:
            out = ::adm::TypeDefinition::DIRECT_SPEAKERS;
            return true;
        case TypeDefinition::kMatrix:
            out = ::adm::TypeDefinition::MATRIX;
            return true;
        case TypeDefinition::kObjects:
            out = ::adm::TypeDefinition::OBJECTS;
            return true;
        case TypeDefinition::kHoa:
            out = ::adm::TypeDefinition::HOA;
            return true;
        case TypeDefinition::kBinaural:
            out = ::adm::TypeDefinition::BINAURAL;
            return true;
        case TypeDefinition::kUnknown:
        case TypeDefinition::kUserCustom:
            break;
    }
    return false;
}

// The parameters every block type shares (Table A1-8): duration, a gain other than unity and an
// importance other than the schema's default. rtime is each block's own constructor argument.
template <typename Block>
void set_common(Block& out, const AudioBlockFormat& block) {
    if (block.has_duration) {
        out.set(::adm::Duration(seconds_to_time(block.duration_s)));
    }
    if (block.gain != 1.0) {
        out.set(::adm::Gain::fromLinear(block.gain));
    }
    if (block.has_importance && block.importance != 10) {
        out.set(::adm::Importance(block.importance));
    }
}

::adm::SphericalPosition to_libadm_spherical(const PolarPosition& polar) {
    return ::adm::SphericalPosition(::adm::Azimuth(static_cast<float>(polar.azimuth_deg)),
                                    ::adm::Elevation(static_cast<float>(polar.elevation_deg)),
                                    ::adm::Distance(static_cast<float>(polar.distance)));
}

::adm::CartesianPosition to_libadm_cartesian(const CartesianPosition& cartesian) {
    return ::adm::CartesianPosition(::adm::X(static_cast<float>(cartesian.x)),
                                    ::adm::Y(static_cast<float>(cartesian.y)),
                                    ::adm::Z(static_cast<float>(cartesian.z)));
}

::adm::AudioBlockFormatObjects to_libadm_block(const AudioBlockFormat& block) {
    ::adm::AudioBlockFormatObjects out{to_libadm_cartesian({}),
                                       ::adm::Rtime(seconds_to_time(block.rtime_s))};
    // The variant, not the `cartesian` flag, is what says which position this block holds: the flag
    // is for a reader and a hand-built block may leave it either way.
    if (const auto* cartesian = std::get_if<CartesianPosition>(&block.position)) {
        out.set(to_libadm_cartesian(*cartesian));
    } else {
        out.set(to_libadm_spherical(std::get<PolarPosition>(block.position)));
    }
    set_common(out, block);
    out.set(::adm::Width(static_cast<float>(block.width)));
    out.set(::adm::Height(static_cast<float>(block.height)));
    out.set(::adm::Depth(static_cast<float>(block.depth)));
    if (block.diffuse != 0.0) {
        out.set(::adm::Diffuse(static_cast<float>(block.diffuse)));
    }
    if (block.has_channel_lock) {
        ::adm::ChannelLock lock{::adm::ChannelLockFlag(block.channel_lock)};
        if (block.has_channel_lock_max_distance) {
            lock.set(::adm::MaxDistance(static_cast<float>(block.channel_lock_max_distance)));
        }
        out.set(lock);
    }
    if (block.has_object_divergence) {
        ::adm::ObjectDivergence divergence{::adm::Divergence(static_cast<float>(block.object_divergence.value))};
        if (block.object_divergence.has_azimuth_range) {
            divergence.set(::adm::AzimuthRange(static_cast<float>(block.object_divergence.azimuth_range_deg)));
        }
        if (block.object_divergence.has_position_range) {
            divergence.set(::adm::PositionRange(static_cast<float>(block.object_divergence.position_range)));
        }
        out.set(divergence);
    }
    if (block.screen_ref) {
        out.set(::adm::ScreenRef(true));
    }
    if (block.head_locked) {
        out.set(::adm::HeadLocked(true));
    }
    if (block.has_jump_position) {
        ::adm::JumpPosition jump{::adm::JumpPositionFlag(block.jump_position)};
        if (block.has_interpolation_length) {
            jump.set(::adm::InterpolationLength(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(block.interpolation_length_s))));
        }
        out.set(jump);
    }
    return out;
}

::adm::AudioBlockFormatDirectSpeakers to_libadm_direct_speakers_block(
    const AudioBlockFormat& block) {
    ::adm::AudioBlockFormatDirectSpeakers out{::adm::Rtime(seconds_to_time(block.rtime_s))};
    // set(), not the constructor's own named-arg list: SpeakerPosition's two alternatives
    // (Cartesian/Spherical) are read off the same `position` variant AudioBlockFormatObjects
    // above reads, but AudioBlockFormatDirectSpeakers has no matching constructor overload for
    // either - see audio_block_format_direct_speakers.hpp's own set(CartesianSpeakerPosition)/
    // set(SphericalSpeakerPosition).
    if (const auto* cartesian = std::get_if<CartesianPosition>(&block.position)) {
        out.set(::adm::CartesianSpeakerPosition(::adm::X(static_cast<float>(cartesian->x)),
                                                ::adm::Y(static_cast<float>(cartesian->y)),
                                                ::adm::Z(static_cast<float>(cartesian->z))));
    } else {
        const auto& polar = std::get<PolarPosition>(block.position);
        out.set(::adm::SphericalSpeakerPosition(
            ::adm::Azimuth(static_cast<float>(polar.azimuth_deg)),
            ::adm::Elevation(static_cast<float>(polar.elevation_deg)),
            ::adm::Distance(static_cast<float>(polar.distance))));
    }
    set_common(out, block);
    for (const auto& label : block.speaker_labels) {
        out.add(::adm::SpeakerLabel(label));
    }
    if (block.head_locked) {
        out.set(::adm::HeadLocked(true));
    }
    return out;
}

// BS.2076-3 §5.4.3.4: order and degree identify the component and are required, so a block that
// never had them is not a document this writer can describe.
std::expected<::adm::AudioBlockFormatHoa, AdmWriteError> to_libadm_hoa_block(
    const AudioBlockFormat& block) {
    if (!block.has_hoa_order || !block.has_hoa_degree) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
    ::adm::AudioBlockFormatHoa out{::adm::Order(block.hoa_order), ::adm::Degree(block.hoa_degree),
                                   ::adm::Rtime(seconds_to_time(block.rtime_s))};
    set_common(out, block);
    if (!block.hoa_normalization.empty()) {
        out.set(::adm::Normalization(block.hoa_normalization));
    }
    if (block.has_nfc_ref_dist) {
        out.set(::adm::NfcRefDist(static_cast<float>(block.nfc_ref_dist)));
    }
    if (!block.hoa_equation.empty()) {
        out.set(::adm::Equation(block.hoa_equation));
    }
    if (block.screen_ref) {
        out.set(::adm::ScreenRef(true));
    }
    if (block.head_locked) {
        out.set(::adm::HeadLocked(true));
    }
    return out;
}

::adm::AudioBlockFormatBinaural to_libadm_binaural_block(const AudioBlockFormat& block) {
    ::adm::AudioBlockFormatBinaural out{::adm::Rtime(seconds_to_time(block.rtime_s))};
    set_common(out, block);
    return out;
}

// libadm's Matrix block has parameters for rtime, duration, gain and importance only, and its
// formatter writes just the first two; the matrix itself is added to the text afterwards (see
// inject_matrix_extras). The block still goes into the channel so reassignIds() numbers it and
// the formatter writes the element for the text to expand.
::adm::AudioBlockFormatMatrix to_libadm_matrix_block(const AudioBlockFormat& block) {
    ::adm::AudioBlockFormatMatrix out{::adm::Rtime(seconds_to_time(block.rtime_s))};
    set_common(out, block);
    return out;
}

// A small "resolve or fail" helper shared by every *_refs loop below: every reference in an
// AdmModel this writer accepts must resolve within the SAME model (see this file's own
// build_libadm_document doc comment - unlike the read side, there is no partial/best-effort
// tolerance here, since the caller building the model controls every string in it).
template <typename Value>
std::expected<std::reference_wrapper<const std::shared_ptr<Value>>, AdmWriteError> resolve(
    const std::unordered_map<std::string, std::shared_ptr<Value>>& by_id, const std::string& id) {
    const auto it = by_id.find(id);
    if (it == by_id.end()) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
    return std::cref(it->second);
}

// True when following `refs_of` from some element comes back to an element already on the path.
// libadm refuses to add a reference that closes a loop (BS.2076-3 §5.6.7 and §5.5 forbid one) by
// throwing, which would leave write_bw64() as an exception, so the model's own graph is checked
// first. Iterative: a nested model is as deep as its caller made it, and recursion could not say
// how deep that is. References that name nothing are skipped here; resolving them is the loops'
// own job.
template <typename Element, typename RefsOf>
bool has_reference_cycle(const std::vector<Element>& elements, RefsOf refs_of) {
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        index.emplace(elements[i].id, i);
    }
    enum : char { kNew, kOnPath, kDone };
    std::vector<char> state(elements.size(), kNew);
    struct Frame {
        std::size_t element;
        std::size_t next_ref;
    };
    std::vector<Frame> stack;
    for (std::size_t root = 0; root < elements.size(); ++root) {
        if (state[root] != kNew) {
            continue;
        }
        state[root] = kOnPath;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            const auto frame = stack.back();
            const auto& refs = refs_of(elements[frame.element]);
            if (frame.next_ref == refs.size()) {
                state[frame.element] = kDone;
                stack.pop_back();
                continue;
            }
            ++stack.back().next_ref;
            const auto it = index.find(refs[frame.next_ref]);
            if (it == index.end()) {
                continue;
            }
            if (state[it->second] == kOnPath) {
                return true;
            }
            if (state[it->second] == kNew) {
                state[it->second] = kOnPath;
                stack.push_back({it->second, 0});
            }
        }
    }
    return false;
}

std::expected<BuiltDocument, AdmWriteError> build_libadm_document_unchecked(
    const AdmModel& model, std::uint16_t bit_depth) {
    if (has_reference_cycle(model.objects,
                            [](const AudioObject& o) -> const std::vector<std::string>& {
                                return o.object_refs;
                            }) ||
        has_reference_cycle(model.pack_formats,
                            [](const AudioPackFormat& p) -> const std::vector<std::string>& {
                                return p.pack_format_refs;
                            })) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }

    auto document = ::adm::Document::create();
    BuiltDocument built;

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioChannelFormat>> channel_formats_by_id;
    for (const auto& channel_format : model.channel_formats) {
        ::adm::TypeDescriptor type;
        if (!to_libadm_type(channel_format.type, type)) {
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        auto libadm_channel = ::adm::AudioChannelFormat::create(::adm::AudioChannelFormatName(channel_format.name), type);
        for (const auto& block : channel_format.block_formats) {
            switch (channel_format.type) {
                case TypeDefinition::kObjects:
                    libadm_channel->add(to_libadm_block(block));
                    break;
                case TypeDefinition::kDirectSpeakers:
                    libadm_channel->add(to_libadm_direct_speakers_block(block));
                    break;
                case TypeDefinition::kHoa: {
                    auto hoa = to_libadm_hoa_block(block);
                    if (!hoa) {
                        return std::unexpected(hoa.error());
                    }
                    libadm_channel->add(std::move(*hoa));
                    break;
                }
                case TypeDefinition::kBinaural:
                    libadm_channel->add(to_libadm_binaural_block(block));
                    break;
                case TypeDefinition::kMatrix:
                    libadm_channel->add(to_libadm_matrix_block(block));
                    break;
                case TypeDefinition::kUnknown:
                case TypeDefinition::kUserCustom:
                    return std::unexpected(AdmWriteError::kInvalidDocument);
            }
        }
        if (channel_format.type == TypeDefinition::kObjects &&
            std::ranges::any_of(channel_format.block_formats,
                                [](const AudioBlockFormat& b) { return !b.zone_exclusion.empty(); })) {
            ZoneBlockSource source{.channel = libadm_channel, .zones_by_block = {}};
            for (const auto& block : channel_format.block_formats) {
                source.zones_by_block.push_back(block.zone_exclusion);
            }
            built.zone_blocks.push_back(std::move(source));
        }
        if (channel_format.type == TypeDefinition::kMatrix) {
            built.matrix_channels.push_back(
                {.channel = libadm_channel, .blocks = channel_format.block_formats});
        }
        document->add(libadm_channel);
        channel_formats_by_id.emplace(channel_format.id, std::move(libadm_channel));
    }

    // Two passes: a pack may nest one that comes later in the model, so every pack exists before
    // any reference between packs is made.
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioPackFormat>> pack_formats_by_id;
    for (const auto& pack_format : model.pack_formats) {
        ::adm::TypeDescriptor type;
        if (!to_libadm_type(pack_format.type, type)) {
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        // libadm refuses AudioPackFormat::create() for the HOA type: its HOA pack is a subclass
        // with parameters of its own (its parser reads them as attributes; the standard and
        // this writer use sub-elements, see inject_matrix_extras).
        std::shared_ptr<::adm::AudioPackFormat> libadm_pack;
        if (pack_format.type == TypeDefinition::kHoa) {
            libadm_pack =
                ::adm::AudioPackFormatHoa::create(::adm::AudioPackFormatName(pack_format.name));
        } else {
            libadm_pack =
                ::adm::AudioPackFormat::create(::adm::AudioPackFormatName(pack_format.name), type);
        }
        for (const auto& ref : pack_format.channel_format_refs) {
            const auto resolved = resolve(channel_formats_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_pack->addReference(resolved->get());
        }
        document->add(libadm_pack);
        const bool has_matrix_refs = !pack_format.encode_pack_format_refs.empty() ||
                                     !pack_format.decode_pack_format_refs.empty() ||
                                     !pack_format.input_pack_format_ref.empty() ||
                                     !pack_format.output_pack_format_ref.empty();
        const bool has_hoa_defaults = !pack_format.hoa_normalization.empty() ||
                                      pack_format.has_nfc_ref_dist || pack_format.screen_ref;
        if (has_matrix_refs || has_hoa_defaults) {
            built.pack_sources.push_back({.pack = libadm_pack, .model = pack_format});
        }
        pack_formats_by_id.emplace(pack_format.id, std::move(libadm_pack));
    }
    for (const auto& pack_format : model.pack_formats) {
        for (const auto& ref : pack_format.pack_format_refs) {
            const auto resolved = resolve(pack_formats_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            pack_formats_by_id.at(pack_format.id)->addReference(resolved->get());
        }
    }

    // §5.1/§5.2 are skipped in favour of BS.2076-2's plain-PCM shortcut (model.hpp's own
    // AudioTrackUid comment: "the audioTrackUID has to refer to the corresponding
    // audioChannelFormat" when audioTrackFormat/audioStreamFormat are both omitted) - this writer
    // never produces coded/explicit-stream audio, so there is nothing for either element to
    // describe. model.stream_formats/model.track_formats are consequently always empty for a
    // document this writer builds; the loop bodies below exist only so a document built some
    // other way (a future second producer of iclforge::adm::AdmModel) still round-trips correctly.
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioStreamFormat>> stream_formats_by_id;
    for (const auto& stream_format : model.stream_formats) {
        auto libadm_stream =
            ::adm::AudioStreamFormat::create(::adm::AudioStreamFormatName(stream_format.name), ::adm::FormatDefinition::PCM);
        if (stream_format.channel_format_ref) {
            const auto resolved = resolve(channel_formats_by_id, *stream_format.channel_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_stream->setReference(resolved->get());
        }
        document->add(libadm_stream);
        stream_formats_by_id.emplace(stream_format.id, std::move(libadm_stream));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackFormat>> track_formats_by_id;
    for (const auto& track_format : model.track_formats) {
        auto libadm_track =
            ::adm::AudioTrackFormat::create(::adm::AudioTrackFormatName(track_format.name), ::adm::FormatDefinition::PCM);
        if (track_format.stream_format_ref) {
            const auto resolved = resolve(stream_formats_by_id, *track_format.stream_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track->setReference(resolved->get());
        }
        document->add(libadm_track);
        track_formats_by_id.emplace(track_format.id, std::move(libadm_track));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackUid>> track_uids_by_id;
    for (const auto& track_uid : model.track_uids) {
        if (track_uid.track_format_ref && track_uid.channel_format_ref) {
            // An audioTrackUID refers to an audioTrackFormat or, for plain PCM, straight to an
            // audioChannelFormat - not both (model.hpp's AudioTrackUid). libadm's setReference()
            // enforces that by throwing ::adm::error::AudioTrackUidMutuallyExclusiveReferences on
            // the second, which would leave write_bw64() as an exception.
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        auto libadm_track_uid = ::adm::AudioTrackUid::create();
        if (track_uid.has_sample_rate) {
            libadm_track_uid->set(::adm::SampleRate(track_uid.sample_rate));
        }
        // Unconditional, and never track_uid.bit_depth: the caller's value may describe some
        // other file, while this one is always the width write_bw64 writes <fmt > with.
        libadm_track_uid->set(::adm::BitDepth(bit_depth));
        if (track_uid.track_format_ref) {
            const auto resolved = resolve(track_formats_by_id, *track_uid.track_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        if (track_uid.pack_format_ref) {
            const auto resolved = resolve(pack_formats_by_id, *track_uid.pack_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        if (track_uid.channel_format_ref) {
            const auto resolved = resolve(channel_formats_by_id, *track_uid.channel_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        document->add(libadm_track_uid);
        track_uids_by_id.emplace(track_uid.uid, libadm_track_uid);
    }

    // Two passes again: an audioObject may nest one that comes later in the model (§5.6:
    // "audioObjects can be nested"), so each exists before any nesting reference is made.
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioObject>> objects_by_id;
    for (const auto& object : model.objects) {
        auto libadm_object = ::adm::AudioObject::create(::adm::AudioObjectName(object.name));
        if (object.start_s != 0.0) {
            libadm_object->set(::adm::Start(seconds_to_time(object.start_s)));
        }
        for (const auto& ref : object.pack_format_refs) {
            const auto resolved = resolve(pack_formats_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_object->addReference(resolved->get());
        }
        for (const auto& ref : object.track_uid_refs) {
            const auto resolved = resolve(track_uids_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_object->addReference(resolved->get());
        }
        document->add(libadm_object);
        objects_by_id.emplace(object.id, std::move(libadm_object));
    }
    for (const auto& object : model.objects) {
        for (const auto& ref : object.object_refs) {
            const auto resolved = resolve(objects_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            objects_by_id.at(object.id)->addReference(resolved->get());
        }
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioContent>> contents_by_id;
    for (const auto& content : model.contents) {
        auto libadm_content = ::adm::AudioContent::create(::adm::AudioContentName(content.name));
        for (const auto& ref : content.object_refs) {
            const auto resolved = resolve(objects_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_content->addReference(resolved->get());
        }
        document->add(libadm_content);
        contents_by_id.emplace(content.id, std::move(libadm_content));
    }

    for (const auto& programme : model.programmes) {
        auto libadm_programme = ::adm::AudioProgramme::create(::adm::AudioProgrammeName(programme.name));
        for (const auto& ref : programme.content_refs) {
            const auto resolved = resolve(contents_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_programme->addReference(resolved->get());
        }
        document->add(libadm_programme);
    }

    built.document = std::move(document);
    built.track_uids_by_key = std::move(track_uids_by_id);
    built.channels_by_key = std::move(channel_formats_by_id);
    built.packs_by_key = std::move(pack_formats_by_id);
    return built;
}

}  // namespace

std::expected<BuiltDocument, AdmWriteError> build_libadm_document(const AdmModel& model,
                                                                  std::uint16_t bit_depth) {
    // A value libadm's own types refuse (an azimuth past 180 degrees, an importance past 10, a
    // negative width) throws from the constructor or setter that range-checks it. That is the
    // model describing something the standard has no element for, so it is reported as one,
    // rather than leaving write_bw64() as an exception.
    try {
        return build_libadm_document_unchecked(model, bit_depth);
    } catch (const std::exception&) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
}

namespace {

// The seconds a BS.2076 §5.13 timecode names, or nothing for text that is empty or not a timecode.
std::optional<double> timecode_seconds(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        return to_seconds(::adm::parseTimecode(text));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_number(const std::string& text) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::vector<AudioBlockFormat> build_matrix_blocks(const std::vector<MatrixBlockText>& texts) {
    std::vector<AudioBlockFormat> blocks;
    blocks.reserve(texts.size());
    for (const auto& text : texts) {
        AudioBlockFormat block;
        block.id = text.id;
        if (const auto rtime = timecode_seconds(text.rtime)) {
            block.rtime_s = *rtime;
        }
        if (const auto duration = timecode_seconds(text.duration)) {
            block.has_duration = true;
            block.duration_s = *duration;
        }
        if (const auto gain = parse_number(text.gain)) {
            // §5.4.3: gainUnit "dB" or, by default, "linear"; the model always holds linear.
            block.gain = text.gain_unit == "dB" ? std::pow(10.0, *gain / 20.0) : *gain;
        }
        block.has_importance = true;
        if (const auto importance = parse_number(text.importance)) {
            block.importance = static_cast<int>(std::clamp(*importance, 0.0, 10.0));
        }
        block.output_channel_format_ref = text.output_channel_format_ref;
        block.matrix = text.matrix;
        block.has_jump_position = text.has_jump_position;
        block.jump_position = text.jump_position;
        block.has_interpolation_length = text.has_interpolation_length;
        block.interpolation_length_s = text.interpolation_length_s;
        blocks.push_back(std::move(block));
    }
    return blocks;
}

AdmModel build_adm_model(const std::shared_ptr<::adm::Document>& document) {
    AdmModel model;
    if (!document) {
        return model;
    }
    for (const auto& programme : document->getElements<::adm::AudioProgramme>()) {
        model.programmes.push_back(convert(programme));
    }
    for (const auto& content : document->getElements<::adm::AudioContent>()) {
        model.contents.push_back(convert(content));
    }
    for (const auto& object : document->getElements<::adm::AudioObject>()) {
        model.objects.push_back(convert(object));
    }
    for (const auto& pack_format : document->getElements<::adm::AudioPackFormat>()) {
        model.pack_formats.push_back(convert(pack_format));
    }
    for (const auto& channel_format : document->getElements<::adm::AudioChannelFormat>()) {
        model.channel_formats.push_back(convert(channel_format));
    }
    for (const auto& stream_format : document->getElements<::adm::AudioStreamFormat>()) {
        model.stream_formats.push_back(convert(stream_format));
    }
    for (const auto& track_format : document->getElements<::adm::AudioTrackFormat>()) {
        model.track_formats.push_back(convert(track_format));
    }
    for (const auto& track_uid : document->getElements<::adm::AudioTrackUid>()) {
        model.track_uids.push_back(convert(track_uid));
    }
    return model;
}

}  // namespace iclforge::adm::detail
