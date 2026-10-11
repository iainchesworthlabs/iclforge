#include "iclforge/ac3/encoder/plan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/drc.hpp"
#include "iclforge/ac3/meta/mixing.hpp"
#include "iclforge/render/spatial.hpp"

namespace iclforge::ac3::plan {

namespace {

using Location = eac3::chanmap::Location;

[[nodiscard]] bool is_lfe(Location location) {
    return location == Location::kLfe || location == Location::kLfe2;
}

// The independent substream's channels, in AC-3 coded order (Table 5.8) with
// the LFE last. This IS acmod_map/expand: Table E2.5's first five bits agree
// with Table 5.8's coded order by construction (acmod_map's own static_assert
// already proves the count agrees for every acmod), so a bed never needs its
// own hand-written location list.
[[nodiscard]] std::vector<Location> bed_locations(const ChannelPlan& plan) {
    const auto expanded =
        eac3::chanmap::expand(eac3::chanmap::acmod_map(plan.bed_acmod, plan.bed_lfe));
    return {expanded.begin(), expanded.end()};
}

// What a decoder ends up with: the bed, with each dependent's channels either
// replacing a bed channel of the same location or appended as a new one. The
// LFE is kept last, where every layout in this project puts it.
[[nodiscard]] std::vector<Location> rendered_locations(const ChannelPlan& plan) {
    auto out = bed_locations(plan);
    const bool lfe = !out.empty() && is_lfe(out.back());
    if (lfe) {
        out.pop_back();
    }
    for (const auto mask : plan.dependents) {
        const auto expanded = eac3::chanmap::expand(mask);
        for (const auto location : expanded) {
            if (std::ranges::find(out, location) == out.end()) {
                out.push_back(location);
            }
        }
    }
    if (lfe) {
        out.push_back(Location::kLfe);
    }
    return out;
}

// The named-layout callers below only ever want channel_plan_for(id)'s
// answer; keeping this overload spares them writing that out.
[[nodiscard]] std::vector<Location> rendered_locations(LayoutId id) {
    return rendered_locations(channel_plan_for(id));
}

// --- geometry ---------------------------------------------------------------
//
// The height-aware azimuth/elevation panner (Direction, direction_of,
// PanTargets, pan_targets, pan_direction) used to live here alone; it is now
// iclforge::spatial's, promoted so IO12's object-based loudness measurement can
// pan an object by its own position with the identical geometry this
// renderer uses to move a bed's channels between layouts. Aliased back in
// rather than qualified at every call site below.
using Direction = spatial::Direction;
using spatial::direction_of;
using PanTargets = spatial::PanTargets;
using spatial::pan_targets;
using spatial::pan_direction;
using spatial::kNegligibleGain;

// --- source layouts ---------------------------------------------------------

// Standard WAVEFORMATEXTENSIBLE speaker order, as far as it and Table E2.5
// overlap. Three E-AC-3 locations (Lw/Rw, Lsd/Rsd, LFE2) have no slot in that
// order at all; they follow in bitstream order rather than being dropped.
constexpr std::array<Location, 17> kWavSpeakerOrder = {
    Location::kLeft,           // FL
    Location::kRight,          // FR
    Location::kCentre,         // FC
    Location::kLfe,            // LFE
    Location::kLrs,            // BL
    Location::kRrs,            // BR
    Location::kLc,             // FLC
    Location::kRc,             // FRC
    Location::kCs,             // BC
    Location::kLeftSurround,   // SL
    Location::kRightSurround,  // SR
    Location::kTs,             // TC
    Location::kVhl,            // TFL
    Location::kVhc,            // TFC
    Location::kVhr,            // TFR
    Location::kLts,            // TBL
    Location::kRts,            // TBR
};

// A set of locations sorted into the order a WAV file interleaves them.
[[nodiscard]] std::vector<Location> in_wav_order(std::span<const Location> locations) {
    std::vector<Location> out;
    out.reserve(locations.size());
    for (const auto index : wav_order(locations)) {
        out.push_back(locations[index]);
    }
    return out;
}

// What a WAV of this width most likely holds, when its width does not match
// the layout being encoded. The counts that are ambiguous - eight channels is
// 7.1 or 5.1.2, ten is 5.1.4 or 7.1.2 - resolve to the commoner delivery
// layout; a source that really is the other one only has to be encoded to the
// matching target, which takes the exact-match path above instead.
[[nodiscard]] std::optional<std::vector<Location>> generic_wav_layout(std::size_t channels) {
    switch (channels) {
        case 1: return std::vector<Location>{Location::kCentre};
        case 2: return std::vector<Location>{Location::kLeft, Location::kRight};
        case 3:
            return std::vector<Location>{Location::kLeft, Location::kRight, Location::kCentre};
        case 4:
            return std::vector<Location>{Location::kLeft, Location::kRight,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 5:
            return std::vector<Location>{Location::kLeft, Location::kRight, Location::kCentre,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 6:
            return std::vector<Location>{Location::kLeft,  Location::kRight,
                                         Location::kCentre, Location::kLfe,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 8: return in_wav_order(rendered_locations(LayoutId::k71));
        case 10: return in_wav_order(rendered_locations(LayoutId::k514));
        case 12: return in_wav_order(rendered_locations(LayoutId::k714));
        default: return std::nullopt;
    }
}

// The Table E2.5 map bit a location belongs to. Both halves of a pair share
// one, which is why a mask built from these can say a pair is present without
// saying that BOTH of its halves are (channel_mask_of() is what checks that).
[[nodiscard]] constexpr std::uint16_t map_bit(Location location) {
    namespace cm = eac3::chanmap;
    switch (location) {
        case Location::kLeft: return cm::kLeftBit;
        case Location::kCentre: return cm::kCentreBit;
        case Location::kRight: return cm::kRightBit;
        case Location::kLeftSurround: return cm::kLeftSurroundBit;
        case Location::kRightSurround: return cm::kRightSurroundBit;
        case Location::kLc:
        case Location::kRc: return cm::kLcRcBit;
        case Location::kLrs:
        case Location::kRrs: return cm::kLrsRrsBit;
        case Location::kCs: return cm::kCsBit;
        case Location::kTs: return cm::kTsBit;
        case Location::kLsd:
        case Location::kRsd: return cm::kLsdRsdBit;
        case Location::kLw:
        case Location::kRw: return cm::kLwRwBit;
        case Location::kVhl:
        case Location::kVhr: return cm::kVhlVhrBit;
        case Location::kVhc: return cm::kVhcBit;
        case Location::kLts:
        case Location::kRts: return cm::kLtsRtsBit;
        case Location::kLfe2: return cm::kLfe2Bit;
        case Location::kLfe: return cm::kLfeBit;
    }
    return 0;
}

// --- fold-down --------------------------------------------------------------

// Folding a wide source into one or two channels has a specified answer
// (§7.8), and it is not what a panner would do: Lo/Ro sends each surround to
// its own side only, while a pairwise pan bleeds it across both. So the spec's
// coefficients are used wherever they are defined - which is wherever the
// source fits an acmod - and the panner handles everything else.
[[nodiscard]] bool fold_down(std::span<const Location> source, const ChannelPlan& target,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev,
                             Routing& out) {
    // Fold-down only has an answer (§7.8) when the whole target IS the bed
    // and the bed is mono or stereo - a target with any dependent, or an LFE
    // channel neither mono nor stereo layouts ever carried, has no entry.
    if (!target.dependents.empty() || target.bed_lfe) {
        return false;
    }
    const bool mono = target.bed_acmod == Acmod::k1_0;
    const bool stereo = target.bed_acmod == Acmod::k2_0;
    if (!mono && !stereo) {
        return false;
    }
    // The source's own acmod, read off WHICH locations it holds rather than
    // how many: three channels are 3/0 as L R C and 2/1 as L R Cs, and §7.8's
    // coefficients are different for the two (the lone surround is a surround
    // at -3 dB, not a centre). A source whose locations are not exactly one
    // Table 5.8 mode (with or without LFE) has no entry here.
    std::uint16_t held = 0;
    for (const auto location : source) {
        held = static_cast<std::uint16_t>(held | map_bit(location));
    }
    // Searched rather than asked of acmod_for_chanmap(), which breaks a tie
    // between modes of one width by a fixed preference (3/0 over 2/1) because
    // it answers for a dependent's channel map, where only the count matters.
    std::optional<Acmod> found;
    for (const auto candidate : {Acmod::k1_0, Acmod::k2_0, Acmod::k3_0, Acmod::k2_1, Acmod::k3_1,
                                 Acmod::k2_2, Acmod::k3_2}) {
        for (const bool lfe : {false, true}) {
            if (eac3::chanmap::acmod_map(candidate, lfe) == held) {
                found = candidate;
            }
        }
    }
    if (!found.has_value()) {
        return false;  // §7.8 is defined per acmod; a wider source has no entry
    }
    const Acmod source_acmod = *found;
    const int fbw = fullbw_channel_count(source_acmod);
    if (fbw <= (mono ? 1 : 2)) {
        return false;  // nothing to fold; the panner's identity is fine
    }
    // Coded order (Table 5.8) is Table E2.5's bit order for every acmod, so
    // the k-th location of the mode's own map is its k-th coded channel; its
    // slot in the source is wherever the file put that location.
    const auto coded = eac3::chanmap::expand(eac3::chanmap::acmod_map(source_acmod, false));
    std::array<std::size_t, 5> slot{};
    for (int k = 0; k < fbw; ++k) {
        const auto at = std::ranges::find(source, coded[k]);
        slot[static_cast<std::size_t>(k)] =
            static_cast<std::size_t>(std::distance(source.begin(), at));
    }

    const double c = meta::coefficient(clev);
    const double s = meta::coefficient(slev);
    if (mono) {
        const auto mono_gains = meta::mono_downmix(source_acmod, c, s);
        for (int k = 0; k < fbw; ++k) {
            out.gain[slot[static_cast<std::size_t>(k)]] = mono_gains[static_cast<std::size_t>(k)];
        }
        return true;
    }
    const auto stereo_gains = meta::stereo_downmix(source_acmod, c, s);
    for (int k = 0; k < fbw; ++k) {
        const auto wav = slot[static_cast<std::size_t>(k)];
        out.gain[wav] = stereo_gains.left[static_cast<std::size_t>(k)];
        out.gain[static_cast<std::size_t>(out.source_channels) + wav] =
            stereo_gains.right[static_cast<std::size_t>(k)];
    }
    return true;
}

}  // namespace

// --- layouts ----------------------------------------------------------------

std::optional<LayoutId> parse_layout(std::string_view name) {
    for (const auto& info : kLayouts) {
        if (info.name == name) {
            return info.id;
        }
    }
    return std::nullopt;
}

std::optional<LayoutId> layout_for_source(std::size_t wav_channels) {
    switch (wav_channels) {
        case 1: return LayoutId::kMono;
        case 2: return LayoutId::kStereo;
        // 3, 4 and 5 are legal acmods with no layout entry of their own. 5.1
        // holds all of them, and route() leaves the positions they do not fill
        // silent - which is what a source without a centre or an LFE means.
        case 3:
        case 4:
        case 5:
        case 6: return LayoutId::k51;
        case 8: return LayoutId::k71;
        case 10: return LayoutId::k514;
        case 12: return LayoutId::k714;
        default: return std::nullopt;
    }
}

std::optional<std::uint16_t> channel_mask_of(std::span<const Location> locations) {
    std::array<bool, eac3::chanmap::kMaxChannels> seen{};
    std::uint16_t mask = 0;
    for (const auto location : locations) {
        auto& slot = seen[static_cast<std::size_t>(location)];
        if (slot) {
            return std::nullopt;  // named twice
        }
        slot = true;
        mask = static_cast<std::uint16_t>(mask | map_bit(location));
    }
    // A pair's bit stands for BOTH members, so a bit whose expansion is not
    // entirely present means one half was named alone.
    for (int bit = 0; bit < 16; ++bit) {
        const auto one = static_cast<std::uint16_t>(0x8000u >> bit);
        if ((mask & one) == 0) {
            continue;
        }
        for (const auto member : eac3::chanmap::expand(one)) {
            if (!seen[static_cast<std::size_t>(member)]) {
                return std::nullopt;
            }
        }
    }
    return mask;
}

std::optional<LayoutId> layout_for_locations(std::uint16_t locations) {
    for (const auto& info : kLayouts) {
        if (info.id == LayoutId::kDualMono) {
            continue;
        }
        const auto cp = channel_plan_for(info.id);
        auto occupied = eac3::chanmap::acmod_map(cp.bed_acmod, cp.bed_lfe);
        for (const auto dependent : cp.dependents) {
            occupied = static_cast<std::uint16_t>(occupied | dependent);
        }
        if (occupied == locations) {
            return info.id;
        }
    }
    return std::nullopt;
}

namespace {

// ksmedia.h's SPEAKER_* bits.
constexpr std::uint32_t kSpkFrontLeft = 0x1;
constexpr std::uint32_t kSpkFrontRight = 0x2;
constexpr std::uint32_t kSpkFrontCentre = 0x4;
constexpr std::uint32_t kSpkLowFrequency = 0x8;
constexpr std::uint32_t kSpkBackLeft = 0x10;
constexpr std::uint32_t kSpkBackRight = 0x20;
constexpr std::uint32_t kSpkFrontLeftOfCentre = 0x40;
constexpr std::uint32_t kSpkFrontRightOfCentre = 0x80;
constexpr std::uint32_t kSpkBackCentre = 0x100;
constexpr std::uint32_t kSpkSideLeft = 0x200;
constexpr std::uint32_t kSpkSideRight = 0x400;
constexpr std::uint32_t kSpkTopCentre = 0x800;
constexpr std::uint32_t kSpkTopFrontLeft = 0x1000;
constexpr std::uint32_t kSpkTopFrontCentre = 0x2000;
constexpr std::uint32_t kSpkTopFrontRight = 0x4000;
constexpr std::uint32_t kSpkTopBackLeft = 0x8000;
constexpr std::uint32_t kSpkTopBackCentre = 0x10000;  // no Table E2.5 location
constexpr std::uint32_t kSpkTopBackRight = 0x20000;
constexpr std::uint32_t kSpkNamed = 0x3FFFF;

// One SPEAKER_* bit and the location it names. SPEAKER_BACK_LEFT/RIGHT are the
// surrounds of a 5.1 ring on their own and the rear surrounds beside the
// sides, so those two carry the location they take in that company as well.
struct SpeakerSlot {
    std::uint32_t bit;
    Location location;
    Location beside_sides;
};

// Ascending bit order, which is the interleave order of a file with the mask.
constexpr std::array<SpeakerSlot, 17> kSpeakerSlots = {{
    {kSpkFrontLeft, Location::kLeft, Location::kLeft},
    {kSpkFrontRight, Location::kRight, Location::kRight},
    {kSpkFrontCentre, Location::kCentre, Location::kCentre},
    {kSpkLowFrequency, Location::kLfe, Location::kLfe},
    {kSpkBackLeft, Location::kLeftSurround, Location::kLrs},
    {kSpkBackRight, Location::kRightSurround, Location::kRrs},
    {kSpkFrontLeftOfCentre, Location::kLc, Location::kLc},
    {kSpkFrontRightOfCentre, Location::kRc, Location::kRc},
    {kSpkBackCentre, Location::kCs, Location::kCs},
    {kSpkSideLeft, Location::kLeftSurround, Location::kLeftSurround},
    {kSpkSideRight, Location::kRightSurround, Location::kRightSurround},
    {kSpkTopCentre, Location::kTs, Location::kTs},
    {kSpkTopFrontLeft, Location::kVhl, Location::kVhl},
    {kSpkTopFrontCentre, Location::kVhc, Location::kVhc},
    {kSpkTopFrontRight, Location::kVhr, Location::kVhr},
    {kSpkTopBackLeft, Location::kLts, Location::kLts},
    {kSpkTopBackRight, Location::kRts, Location::kRts},
}};

}  // namespace

std::optional<std::vector<Location>> wav_mask_locations(std::uint32_t channel_mask,
                                                        std::size_t channels) {
    if (channel_mask == 0 || channels == 0 || (channel_mask & ~kSpkNamed) != 0 ||
        (channel_mask & kSpkTopBackCentre) != 0 ||
        static_cast<std::size_t>(std::popcount(channel_mask)) != channels) {
        return std::nullopt;
    }
    const bool sides = (channel_mask & (kSpkSideLeft | kSpkSideRight)) != 0;
    std::vector<Location> out;
    out.reserve(channels);
    for (const auto& slot : kSpeakerSlots) {
        if ((channel_mask & slot.bit) != 0) {
            out.push_back(sides ? slot.beside_sides : slot.location);
        }
    }
    return out;
}

std::uint32_t wav_channel_mask(std::span<const Location> in_wav_order) {
    if (in_wav_order.size() < 3) {
        return 0;
    }
    const auto holds = [&](Location location) {
        return std::ranges::find(in_wav_order, location) != in_wav_order.end();
    };
    // The surrounds take the back pair of a 5.1 ring, unless the rears are
    // there too, when they move to the sides - wav_mask_locations() reads
    // exactly this back.
    const bool rears = holds(Location::kLrs) || holds(Location::kRrs);
    std::uint32_t mask = 0;
    std::uint32_t previous = 0;
    for (const auto location : in_wav_order) {
        std::uint32_t bit = 0;
        switch (location) {
            case Location::kLeft: bit = kSpkFrontLeft; break;
            case Location::kRight: bit = kSpkFrontRight; break;
            case Location::kCentre: bit = kSpkFrontCentre; break;
            case Location::kLfe: bit = kSpkLowFrequency; break;
            case Location::kLeftSurround: bit = rears ? kSpkSideLeft : kSpkBackLeft; break;
            case Location::kRightSurround: bit = rears ? kSpkSideRight : kSpkBackRight; break;
            case Location::kLrs: bit = kSpkBackLeft; break;
            case Location::kRrs: bit = kSpkBackRight; break;
            case Location::kLc: bit = kSpkFrontLeftOfCentre; break;
            case Location::kRc: bit = kSpkFrontRightOfCentre; break;
            case Location::kCs: bit = kSpkBackCentre; break;
            case Location::kTs: bit = kSpkTopCentre; break;
            case Location::kVhl: bit = kSpkTopFrontLeft; break;
            case Location::kVhc: bit = kSpkTopFrontCentre; break;
            case Location::kVhr: bit = kSpkTopFrontRight; break;
            case Location::kLts: bit = kSpkTopBackLeft; break;
            case Location::kRts: bit = kSpkTopBackRight; break;
            case Location::kLsd:
            case Location::kRsd:
            case Location::kLw:
            case Location::kRw:
            case Location::kLfe2: return 0;  // WAVEFORMATEXTENSIBLE has no speaker for it
        }
        if (bit <= previous) {
            return 0;  // repeated, or out of the order the file will carry them in
        }
        previous = bit;
        mask |= bit;
    }
    return mask;
}

std::optional<SourceLayout> source_layout(Codec codec, std::span<const Location> locations) {
    if (codec == Codec::kAc4) {
        return std::nullopt;
    }
    const auto mask = channel_mask_of(locations);
    if (!mask.has_value()) {
        return std::nullopt;
    }
    const auto allocated = eac3::chanmap::allocate(*mask);
    if (!allocated.has_value() || (codec == Codec::kAc3 && !allocated->dependents.empty())) {
        return std::nullopt;
    }
    if (const auto id = layout_for_locations(*mask)) {
        return SourceLayout{.layout = *id, .custom_locations = std::nullopt};
    }
    return SourceLayout{.layout = std::nullopt, .custom_locations = *mask};
}

std::string layout_names(Codec codec) {
    std::string out;
    for (const auto& info : kLayouts) {
        if (!carries(codec, info.id)) {
            continue;
        }
        if (!out.empty()) {
            out += " | ";
        }
        out += info.name;
    }
    return out;
}

ChannelPlan channel_plan_for(LayoutId id) {
    switch (id) {
        case LayoutId::kMono:
            return {.bed_acmod = Acmod::k1_0, .bed_lfe = false, .dependents = {}};
        case LayoutId::kStereo:
            return {.bed_acmod = Acmod::k2_0, .bed_lfe = false, .dependents = {}};
        case LayoutId::kDualMono:
            // No LFE, ever: 1+1 has no soundfield for a subwoofer to sit in,
            // and Table 5.8 never pairs acmod 0 with one in practice.
            return {.bed_acmod = Acmod::kDualMono, .bed_lfe = false, .dependents = {}};
        case LayoutId::k51:
            return {.bed_acmod = Acmod::k3_2, .bed_lfe = true, .dependents = {}};
        case LayoutId::k71:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k71Rear}};
        case LayoutId::k512:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k512Height}};
        case LayoutId::k514:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::kTopQuad}};
        case LayoutId::k714:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k71Rear, eac3::chanmap::kTopQuad}};
    }
    return {};
}

std::optional<std::uint16_t> parse_channels(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<Location> wanted;
    while (!text.empty()) {
        const auto split = text.find(',');
        // A trailing separator with nothing after it means a location was
        // meant and did not survive whatever produced the string - the same
        // reasoning parse_tools already applies to '+'.
        if (split != std::string_view::npos && split + 1 == text.size()) {
            return std::nullopt;
        }
        const auto token = text.substr(0, split);
        if (token.empty()) {
            return std::nullopt;
        }
        const auto location = eac3::chanmap::parse_location(token);
        if (!location.has_value()) {
            return std::nullopt;
        }
        wanted.push_back(*location);
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }

    // Every Table E2.5 bit, so a pair location can be checked as a whole:
    // both members present sets the bit, one alone is rejected rather than
    // silently dropped or silently completed.
    constexpr std::array<std::uint16_t, 16> kBits = {
        eac3::chanmap::kLeftBit,     eac3::chanmap::kCentreBit,   eac3::chanmap::kRightBit,
        eac3::chanmap::kLeftSurroundBit, eac3::chanmap::kRightSurroundBit, eac3::chanmap::kLcRcBit,
        eac3::chanmap::kLrsRrsBit,   eac3::chanmap::kCsBit,       eac3::chanmap::kTsBit,
        eac3::chanmap::kLsdRsdBit,   eac3::chanmap::kLwRwBit,     eac3::chanmap::kVhlVhrBit,
        eac3::chanmap::kVhcBit,      eac3::chanmap::kLtsRtsBit,   eac3::chanmap::kLfe2Bit,
        eac3::chanmap::kLfeBit,
    };
    std::uint16_t mask = 0;
    for (const auto bit : kBits) {
        const auto expanded = eac3::chanmap::expand(bit);
        const auto present = static_cast<int>(std::ranges::count_if(
            expanded, [&](Location loc) { return std::ranges::find(wanted, loc) != wanted.end(); }));
        if (present == 0) {
            continue;
        }
        if (present != expanded.count) {
            return std::nullopt;  // named one half of a pair location, not both
        }
        mask = static_cast<std::uint16_t>(mask | bit);
    }
    // Catches a name repeated in `text`: it would otherwise vanish silently,
    // since the bit it belongs to is already accounted for by its first
    // appearance.
    if (eac3::chanmap::channel_count(mask) != static_cast<int>(wanted.size())) {
        return std::nullopt;
    }
    return mask;
}

std::string format_channels(std::uint16_t locations) {
    std::string out;
    for (const auto location : eac3::chanmap::expand(locations)) {
        if (!out.empty()) {
            out += ',';
        }
        out += eac3::chanmap::name(location);
    }
    return out;
}

std::vector<CodedChannel> coded_channels(const ChannelPlan& plan) {
    std::vector<CodedChannel> out;
    for (const auto location : bed_locations(plan)) {
        out.push_back({.location = location, .bed = true, .substream = 0});
    }
    int substream = 1;
    for (const auto mask : plan.dependents) {
        for (const auto location : eac3::chanmap::expand(mask)) {
            out.push_back({.location = location, .bed = false, .substream = substream});
        }
        ++substream;
    }
    return out;
}

std::vector<CodedChannel> coded_channels(LayoutId id) {
    return coded_channels(channel_plan_for(id));
}

std::vector<std::string> coded_channel_names(const ChannelPlan& plan) {
    const auto coded = coded_channels(plan);
    std::vector<std::string> out;
    out.reserve(coded.size());
    for (const auto& channel : coded) {
        std::string name{eac3::chanmap::name(channel.location)};
        // A bed channel a dependent overwrites still exists, still carries
        // audio and still reaches a 5.1 decoder - but a display that showed
        // "Ls" twice with different levels would give no way to tell which of
        // the two a reading belonged to.
        const bool replaced =
            channel.bed && std::ranges::any_of(coded, [&](const CodedChannel& other) {
                return !other.bed && other.location == channel.location;
            });
        if (replaced) {
            name += " (bed)";
        }
        out.push_back(std::move(name));
    }
    return out;
}

std::vector<std::string> coded_channel_names(LayoutId id) {
    return coded_channel_names(channel_plan_for(id));
}

Acmod bed_acmod(LayoutId id) { return channel_plan_for(id).bed_acmod; }

bool bed_lfe(LayoutId id) { return channel_plan_for(id).bed_lfe; }

int rendered_channel_count(const ChannelPlan& plan) {
    std::uint16_t occupied = eac3::chanmap::acmod_map(plan.bed_acmod, plan.bed_lfe);
    for (const auto mask : plan.dependents) {
        occupied |= mask;
    }
    return eac3::chanmap::expand(occupied).count;
}

std::vector<std::size_t> wav_order(std::span<const eac3::chanmap::Location> locations) {
    std::vector<std::size_t> out;
    out.reserve(locations.size());
    std::vector<bool> placed(locations.size(), false);
    for (const auto speaker : kWavSpeakerOrder) {
        const auto at = std::ranges::find(locations, speaker);
        if (at == locations.end()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(std::distance(locations.begin(), at));
        out.push_back(index);
        placed[index] = true;
    }
    for (std::size_t i = 0; i < locations.size(); ++i) {
        if (!placed[i]) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<std::size_t> monitor_order(std::span<const eac3::chanmap::Location> locations,
                                       std::size_t channel_count) {
    if (locations.empty()) {
        std::vector<std::size_t> identity(channel_count);
        std::iota(identity.begin(), identity.end(), std::size_t{0});
        return identity;
    }
    return wav_order(locations);
}

// --- tools ------------------------------------------------------------------

namespace {

[[nodiscard]] int parse_index(std::string_view text, int limit) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() ||
        value > static_cast<unsigned>(limit)) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace

bool parse_tools(std::string_view text, Tools& out) {
    if (text.empty() || text == "none") {
        return true;
    }
    while (!text.empty()) {
        const auto split = text.find('+');
        // A separator with nothing after it means a token was meant and did
        // not survive whatever produced the string. Accepting it would turn a
        // truncated selection into a silently smaller one.
        if (split != std::string_view::npos && split + 1 == text.size()) {
            return false;
        }
        const auto token = text.substr(0, split);
        if (token.starts_with("cpl:")) {
            // Pinning the band edge is how a coupling-frequency question gets
            // answered by experiment rather than by argument.
            out.coupling = true;
            out.cplbegf = parse_index(token.substr(4), 15);
            if (out.cplbegf < 0) {
                return false;
            }
        } else if (token.starts_with("spx:")) {
            out.spx = true;
            out.spxbegf = parse_index(token.substr(4), 7);
            if (out.spxbegf < 0) {
                return false;
            }
        } else if (token == "auto") {
            out.auto_tools = true;
        } else if (token == "cpl") {
            out.coupling = true;
        } else if (token == "ecpl") {
            out.enhanced = true;
        } else if (token == "spx") {
            out.spx = true;
        } else if (token == "noatten") {
            out.spx_atten = false;  // spectral extension without its seam notch
        } else if (token.starts_with("atten:")) {
            out.spxattencod = parse_index(token.substr(6), 31);
            if (out.spxattencod < 0) {
                return false;
            }
        } else if (token.starts_with("aht:")) {
            // "aht:0" is AHT with gain-adaptive quantization switched off,
            // which is how GAQ's own contribution gets measured.
            out.aht = true;
            out.gaqmod = parse_index(token.substr(4), 3);
            if (out.gaqmod < 0) {
                return false;
            }
        } else if (token == "aht") {
            out.aht = true;
        } else if (token == "tpn") {
            out.transient_prenoise = true;
        } else if (token == "fastmdct") {
            // The opt-in spelling from when the fast path was off by default,
            // kept so a recorded command line from that era still parses; it
            // now names what already happens.
            out.fast_mdct = true;
        } else if (token == "nofastmdct") {
            out.fast_mdct = false;  // the direct §8.2.3.2 reference form
        } else if (token == "nodither") {
            out.dither = false;  // dithflag pinned at 0, not content-decided
        } else if (token == "nodelta") {
            out.delta = false;  // no §7.2.2.6 segments, and no second fit to weigh them
        } else if (token.starts_with("numblkscod:")) {
            out.numblkscod = parse_index(token.substr(11), 3);
            if (out.numblkscod < 0) {
                return false;
            }
        } else if (token == "all") {
            out.coupling = true;
            out.spx = true;
            out.aht = true;
        } else {
            return false;
        }
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }
    return true;
}

std::string format_tools(const Tools& tools) {
    std::string out;
    const auto add = [&out](std::string_view token) {
        if (!out.empty()) {
            out += '+';
        }
        out += token;
    };
    // `auto` decides the on/off tokens rather than sitting alongside them, so
    // it prints instead of them - but the band-edge pins it still honours
    // print as usual, since those are the part a caller kept control of.
    if (tools.auto_tools) {
        add("auto");
        if (tools.cplbegf >= 0) {
            add("cpl:" + std::to_string(tools.cplbegf));
        }
        if (tools.spxbegf >= 0) {
            add("spx:" + std::to_string(tools.spxbegf));
        }
        if (tools.gaqmod >= 0) {
            add("aht:" + std::to_string(tools.gaqmod));
        }
        if (tools.numblkscod != 3) {
            add("numblkscod:" + std::to_string(tools.numblkscod));
        }
        if (!tools.fast_mdct) {
            add("nofastmdct");
        }
        if (!tools.dither) {
            add("nodither");
        }
        if (!tools.delta) {
            add("nodelta");
        }
        return out;
    }
    if (tools.coupling) {
        add(tools.cplbegf >= 0 ? "cpl:" + std::to_string(tools.cplbegf) : std::string{"cpl"});
        if (tools.enhanced) {
            add("ecpl");
        }
    }
    if (tools.spx) {
        add(tools.spxbegf >= 0 ? "spx:" + std::to_string(tools.spxbegf) : std::string{"spx"});
        if (!tools.spx_atten) {
            add("noatten");
        } else if (tools.spxattencod >= 0) {
            add("atten:" + std::to_string(tools.spxattencod));
        }
    }
    if (tools.aht) {
        add(tools.gaqmod >= 0 ? "aht:" + std::to_string(tools.gaqmod) : std::string{"aht"});
    }
    if (tools.transient_prenoise) {
        add("tpn");
    }
    // Like noatten above, only the non-default state is worth a token: the
    // fast MDCT is what every stream does now, so formatting it would put
    // "fastmdct" on every command line while saying nothing. Same reasoning
    // for numblkscod's default of 3 (six blocks, this encoder's original and
    // still ordinary profile).
    if (tools.numblkscod != 3) {
        add("numblkscod:" + std::to_string(tools.numblkscod));
    }
    if (!tools.fast_mdct) {
        add("nofastmdct");
    }
    if (!tools.dither) {
        add("nodither");
    }
    if (!tools.delta) {
        add("nodelta");
    }
    return out.empty() ? std::string{"none"} : out;
}

// --- variable bit rate -------------------------------------------------------

namespace {

[[nodiscard]] bool parse_unit_double(std::string_view text, double& out) {
    // Not std::from_chars: its floating-point overload is absent from some
    // libc++ builds this project targets (NDK r26's bundled libc++ only
    // implements <charconv>'s integer overloads, not double - see
    // docs/platforms/android.md), so this hand-rolls the same
    // locale-independent, reject-all-trailing-garbage contract with strtod
    // instead, for the floating-point case only (parse_kbps below keeps
    // std::from_chars - the integer overload IS available everywhere).
    // strtod itself is locale-sensitive; nothing in this codebase ever
    // calls setlocale, so the process locale stays "C" for its entire
    // lifetime and this is safe in practice, not just in theory.
    if (text.empty()) {
        return false;
    }
    // strtod needs a NUL-terminated buffer; text is a view into someone
    // else's storage (typically a CLI argument), not necessarily one.
    const std::string buffer(text);
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size() || errno == ERANGE || value < 0.0 || value > 1.0) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_kbps(std::string_view text, std::uint32_t& out) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    // 0 kbps is not a legal bound in either direction - frame_words() gives
    // it zero words, which is not a syncframe at all.
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

bool parse_vbr(std::string_view text, std::optional<eac3::VbrConfig>& out) {
    if (text.empty() || text == "off") {
        out = std::nullopt;
        return true;
    }
    // Two rate controls, two leading tokens. "q:" is plain VBR - a fixed
    // quality, the rate follows. "avg:" is average-rate mode - the offset is
    // steered to hold a rate, so a quality would be a number the encoder
    // never reads (see eac3::AbrConfig). Asking for both names two different
    // things at once, so the pair is refused rather than one half silently
    // winning.
    eac3::VbrConfig vbr;
    const bool abr = text.starts_with("avg:");
    if (!abr) {
        if (!text.starts_with("q:")) {
            return false;
        }
        text = text.substr(2);
        const auto comma = text.find(',');
        if (!parse_unit_double(text.substr(0, comma), vbr.quality)) {
            return false;
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    while (!text.empty()) {
        const auto split = text.find(',');
        // A separator with nothing after it means a field was meant and did
        // not survive whatever produced the string - the same rule
        // parse_tools applies to its own trailing '+'.
        if (split != std::string_view::npos && split + 1 == text.size()) {
            return false;
        }
        const auto token = text.substr(0, split);
        std::uint32_t kbps = 0;
        if (token.starts_with("min:")) {
            if (!parse_kbps(token.substr(4), kbps)) {
                return false;
            }
            vbr.min_kbps = kbps;
        } else if (token.starts_with("max:")) {
            if (!parse_kbps(token.substr(4), kbps)) {
                return false;
            }
            vbr.max_kbps = kbps;
        } else if (token.starts_with("avg:")) {
            // Only the LEADING token may turn ABR on: "q:0.5,avg:192" would
            // otherwise reach here and quietly discard a quality the caller
            // did type, and a second "avg:" would leave which of the two
            // rates was meant unanswerable.
            if (!abr || vbr.abr.has_value()) {
                return false;
            }
            if (!parse_kbps(token.substr(4), kbps)) {
                return false;
            }
            // window_frames keeps AbrConfig's own default; "win:" below is
            // the only thing that moves it.
            vbr.abr = eac3::AbrConfig{.target_kbps = kbps};
        } else if (token.starts_with("win:")) {
            // Meaningless without an average to size. Since "avg:" can only
            // lead, a "win:" reaching here with no AbrConfig built is a
            // window around nothing rather than a reordering.
            if (!vbr.abr.has_value()) {
                return false;
            }
            // Same rule parse_kbps enforces for a rate: a zero-frame window
            // is not a window, it is a missing one.
            if (!parse_kbps(token.substr(4), kbps)) {
                return false;
            }
            vbr.abr->window_frames = kbps;
        } else {
            return false;
        }
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }
    if (vbr.min_kbps.has_value() && vbr.max_kbps.has_value() && *vbr.min_kbps > *vbr.max_kbps) {
        return false;
    }
    // Bounds that exclude the average make it unreachable by construction;
    // the encoder's own validate() refuses the same pair, so catching it here
    // means the CLI reports the syntax rather than a frame-encode failure
    // several hundred frames in.
    if (vbr.abr && ((vbr.min_kbps && *vbr.min_kbps > vbr.abr->target_kbps) ||
                    (vbr.max_kbps && *vbr.max_kbps < vbr.abr->target_kbps))) {
        return false;
    }
    out = vbr;
    return true;
}

std::string format_vbr(const std::optional<eac3::VbrConfig>& vbr) {
    if (!vbr.has_value()) {
        return "off";
    }
    // ABR leads with avg: and never prints a quality - the encoder does not
    // read one, so showing it would describe a knob that does nothing.
    std::string out;
    if (vbr->abr.has_value()) {
        out = "avg:" + std::to_string(vbr->abr->target_kbps);
        // The window is written only when it is not the default, so a plain
        // avg: round-trips as the plain avg: the caller typed - the same rule
        // the optional min:/max: fields already follow by not appearing.
        if (vbr->abr->window_frames != eac3::kAbrDefaultWindowFrames) {
            out += ",win:" + std::to_string(vbr->abr->window_frames);
        }
    } else {
        out = "q:" + std::to_string(vbr->quality);
    }
    if (vbr->min_kbps.has_value()) {
        out += ",min:" + std::to_string(*vbr->min_kbps);
    }
    if (vbr->max_kbps.has_value()) {
        out += ",max:" + std::to_string(*vbr->max_kbps);
    }
    return out;
}

// --- metadata ---------------------------------------------------------------

namespace {

// The two coarse AC-3 levels have no exact 3-bit twin for every value, but
// each one they do have is the same coefficient, so an E-AC-3 stream asked for
// "-4.5 dB centre" gets the level a listener would measure either way.
[[nodiscard]] meta::MixLevel widen(meta::CentreMixLevel value) {
    switch (value) {
        case meta::CentreMixLevel::kMinus3dB: return meta::MixLevel::kMinus3dB;
        case meta::CentreMixLevel::kMinus4_5dB: return meta::MixLevel::kMinus4_5dB;
        case meta::CentreMixLevel::kMinus6dB: return meta::MixLevel::kMinus6dB;
    }
    return meta::MixLevel::kMinus4_5dB;
}

[[nodiscard]] meta::MixLevel widen(meta::SurroundMixLevel value) {
    switch (value) {
        case meta::SurroundMixLevel::kMinus3dB: return meta::MixLevel::kMinus3dB;
        case meta::SurroundMixLevel::kMinus6dB: return meta::MixLevel::kMinus6dB;
        case meta::SurroundMixLevel::kSilent: return meta::MixLevel::kSilent;
    }
    return meta::MixLevel::kMinus6dB;
}

}  // namespace

meta::MixMetadata mix_metadata(const Metadata& options) {
    // Everything past the five levels comes from mixdepth verbatim - the
    // programme scale factors, the mixing-parameter block, the pan info and
    // the per-block configuration have nothing to derive from.
    meta::MixMetadata out = options.mixdepth;
    out.dmixmod = options.dmixmod;
    // Lt/Rt folds down into a matrix that will be re-decoded, so the centre
    // traditionally sits 1.5 dB hotter there than in Lo/Ro. An explicit
    // override wins over both that convention and the widening below.
    out.ltrtcmixlev = options.ltrtcmixlev.value_or(meta::MixLevel::kMinus3dB);
    out.lorocmixlev = options.lorocmixlev.value_or(widen(options.cmixlev));
    out.ltrtsurmixlev = options.ltrtsurmixlev.value_or(meta::MixLevel::kMinus3dB);
    out.lorosurmixlev = options.lorosurmixlev.value_or(widen(options.surmixlev));
    out.lfemixlevcod = options.lfemix;
    return out;
}

meta::AlternateBsi alternate_bsi(const Metadata& options) {
    meta::AlternateBsi out;
    // xbsi1 is the same five quantities mix_metadata() derives. The rest of
    // the MixMetadata it returns has no Annex D field and is simply not read
    // by the AC-3 writer - see MixMetadata's own comment.
    out.mix = mix_metadata(options);
    // dsurexmod and dheadphonmod are stated once, on `info`, because E-AC-3
    // carries the same two fields in infomdat - one source, two homes.
    out.extended = meta::ExtendedBsi{.dsurexmod = options.info.dsurexmod,
                                     .dheadphonmod = options.info.dheadphonmod,
                                     .adconvtyp = options.adconvtyp,
                                     .encinfo = options.encinfo};
    return out;
}

// --- configs ----------------------------------------------------------------

std::string_view describe(PlanError error) {
    switch (error) {
        case PlanError::kLayoutNeedsEac3:
            return "that channel selection needs dependent substreams, which only E-AC-3 has "
                   "(AC-3 codes nothing wider than 3/2 + LFE)";
        case PlanError::kBitrateNotLegal:
            return "AC-3 takes only the 19 nominal rates of Table 5.18";
        case PlanError::kBitrateNotFramable:
            return "E-AC-3 signals the frame size in frmsiz, which is 11 bits, so a syncframe "
                   "holds 1 to 2048 words - at 48 kHz that is 1 to 1024 kbit/s, less at a lower "
                   "sample rate, and a layout with dependent substreams gives each of them half "
                   "the rate";
        case PlanError::kNoSourceLayout:
            return "no standard speaker layout has that many channels";
        case PlanError::kInvalidChannels:
            return "that channel selection is not one A/52 Annex E can express";
        case PlanError::kSampleRateNeedsEac3:
            return "24, 22.05 and 16 kHz (fscod2) only exist in E-AC-3; AC-3 has no such field";
        case PlanError::kVbrNeedsEac3:
            return "variable bit rate needs E-AC-3 - AC-3's frame size indexes Table 5.18 "
                   "and cannot vary freely, and AC-4 has rate modes of its own";
        case PlanError::kTimecodeNeedsBsid8:
            return "Annex D's alternate syntax reuses the two time code fields (§D1), so a "
                   "bsid-6 stream cannot carry a time code as well";
        case PlanError::kLayoutNotInAc4:
            return "the AC-4 encoder takes mono, stereo, 5.0 and 5.1 from a plan: it has no dual "
                   "mono, 7.0 and 7.1 are ac4-encode's experimental option, and it does not "
                   "encode immersive layouts yet";
        case PlanError::kSampleRateNotInAc4:
            return "the AC-4 encoder takes 48 or 44.1 kHz (ETSI TS 103 190-1 Table 82)";
        case PlanError::kUnknownCodec:
            return "a codec this build does not know";
    }
    return "";
}

namespace {

// A Codec::kAc4 plan: the layouts carries() gives AC-4, or a channel list that
// is one of the channel modes the AC-4 encoder takes (ETSI TS 103 190-1 Table
// 88's mono, stereo, 5.0 and 5.1), at one of the rates it takes. The rate and
// everything else the encoder decides for itself (its least frame, the codec
// mode) are iclforge::ac4::Encoder::refusal_reason()'s to name.
std::optional<PlanError> validate_ac4(const Plan& plan) {
    if (plan.custom_locations.has_value()) {
        const auto allocated = eac3::chanmap::allocate(*plan.custom_locations);
        if (!allocated.has_value()) {
            return PlanError::kInvalidChannels;
        }
        const bool narrow =
            (allocated->bed_acmod == Acmod::k1_0 || allocated->bed_acmod == Acmod::k2_0) &&
            !allocated->bed_lfe;
        const bool five = allocated->bed_acmod == Acmod::k3_2;
        if (!allocated->dependents.empty() || !(narrow || five)) {
            return PlanError::kLayoutNotInAc4;
        }
    } else if (!carries(Codec::kAc4, plan.layout)) {
        return PlanError::kLayoutNotInAc4;
    }
    if (plan.sample_rate != SampleRate::k48000 && plan.sample_rate != SampleRate::k44100) {
        return PlanError::kSampleRateNotInAc4;
    }
    if (plan.vbr.has_value()) {
        return PlanError::kVbrNeedsEac3;
    }
    return std::nullopt;
}

// AC-3's and E-AC-3's plans.
std::optional<PlanError> validate_a52(const Plan& plan) {
    if (plan.custom_locations.has_value()) {
        const auto allocated = eac3::chanmap::allocate(*plan.custom_locations);
        if (!allocated.has_value()) {
            return PlanError::kInvalidChannels;
        }
        if (plan.codec == Codec::kAc3 && !allocated->dependents.empty()) {
            return PlanError::kLayoutNeedsEac3;
        }
    } else if (!carries(plan.codec, plan.layout)) {
        return PlanError::kLayoutNeedsEac3;
    }
    // AC-3 indexes Table 5.18 and cannot say anything else. E-AC-3 signals
    // frmsiz directly, so any rate its 11 bits can hold is expressible there -
    // which is a range of its own, checked at the end of this function.
    if (plan.codec == Codec::kAc3 && !is_valid_bitrate(plan.bitrate_kbps)) {
        return PlanError::kBitrateNotLegal;
    }
    // fscod2 is an Annex E field with no AC-3 counterpart at all.
    if (plan.codec == Codec::kAc3 && is_reduced_rate(plan.sample_rate)) {
        return PlanError::kSampleRateNeedsEac3;
    }
    if (plan.vbr.has_value() && plan.codec == Codec::kAc3) {
        return PlanError::kVbrNeedsEac3;
    }
    // Only AC-3 has an alternate syntax to choose, and only AC-3 has a time
    // code field for it to displace - E-AC-3 has neither, so `annexd` there is
    // inert rather than in conflict with anything.
    if (plan.codec == Codec::kAc3 && plan.meta.annexd &&
        (plan.meta.info.timecod1 || plan.meta.info.timecod2)) {
        return PlanError::kTimecodeNeedsBsid8;
    }
    // The E-AC-3 counterpart of the Table 5.18 check above. frmsiz carries a
    // free word count rather than a table index, but it is only 11 bits
    // (§E2.3.1.3), so a syncframe still has a range: 1 to kMaxFrameWords
    // words, and a rate outside it cannot be signalled at all.
    //
    // The rate that has to fit is a SUBSTREAM's, not the plan's - eac3_config()
    // gives the independent substream the whole rate and each dependent half
    // of it, so both ends are reachable from one plan (1 kbit/s stereo is
    // fine; 1 kbit/s 7.1.4 leaves its dependents with a frame of no words at
    // all). Asking eac3_config() for the configs it will really build is what
    // keeps this from drifting away from that split.
    //
    // Without this the verdict exists only inside the frame encoder, which
    // reaches it too late to be reported: a rejected config leaves
    // AccessUnitEncoder with no substreams and therefore no channels, and a
    // front end that sized its buffers from the plan disagrees with it before
    // the first frame is encoded.
    if (plan.codec == Codec::kEac3) {
        // Under VBR the content decides the word count and bitrate_kbps is
        // only a tool heuristic, so there is nothing fixed to check - the same
        // exemption eac3_frame.cpp's own validate() makes, per substream
        // because halve_vbr_bounds() gives dependents their own VBR config.
        const auto framable = [](const eac3::FrameConfig& sub) {
            if (sub.vbr.has_value()) {
                return true;
            }
            const auto words = eac3::frame_words(sub.sample_rate, sub.bitrate_kbps,
                                                 eac3::blocks_per_syncframe(sub.numblkscod));
            return words >= 1 && words <= eac3::kMaxFrameWords;
        };
        const auto config = eac3_config(plan);
        if (!framable(config.independent) ||
            !std::ranges::all_of(config.dependents, framable)) {
            return PlanError::kBitrateNotFramable;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<PlanError> validate(const Plan& plan) {
    switch (plan.codec) {
        case Codec::kAc3:
        case Codec::kEac3:
            return validate_a52(plan);
        case Codec::kAc4:
            return validate_ac4(plan);
    }
    return PlanError::kUnknownCodec;
}

ChannelPlan resolve(const Plan& plan) {
    if (plan.custom_locations.has_value()) {
        return eac3::chanmap::allocate(*plan.custom_locations).value_or(ChannelPlan{});
    }
    return channel_plan_for(plan.layout);
}

EncoderConfig ac3_config(const Plan& plan) {
    const auto cp = resolve(plan);
    return {.sample_rate = plan.sample_rate,
            .bitrate_kbps = plan.bitrate_kbps,
            .dialnorm = plan.meta.dialnorm,
            .dialnorm2 = cp.bed_acmod == Acmod::kDualMono
                            ? std::optional<int>(plan.meta.dialnorm2)
                            : std::nullopt,
            // -1 here is EncoderConfig's own "encoder chooses", which for
            // AC-3 is the measured rate curve - so the default reaches the
            // same behaviour it had before this field was plumbed.
            .fgaincod = plan.tools.fgaincod,
            .acmod = cp.bed_acmod,
            .lfe = cp.bed_lfe,
            // Coupling shares coefficients between full-bandwidth channels
            // (§7.4), so a mono programme has nothing to share it with.
            .coupling = plan.tools.coupling && fullbw_channel_count(cp.bed_acmod) >= 2,
            .cplbegf = plan.tools.cplbegf,
            .fast_mdct = plan.tools.fast_mdct,
            .dither = plan.tools.dither,
            .delta_allocation = plan.tools.delta,
            .drc = plan.meta.drc,
            .heavy = plan.meta.heavy,
            .drc2 = cp.bed_acmod == Acmod::kDualMono
                        ? plan.meta.drc2
                        : std::optional<meta::Profile>(std::nullopt),
            .heavy2 = cp.bed_acmod == Acmod::kDualMono
                          ? plan.meta.heavy2
                          : std::optional<meta::HeavyConfig>(std::nullopt),
            .cmixlev = plan.meta.cmixlev,
            .surmixlev = plan.meta.surmixlev,
            .info = plan.meta.info,
            // Annex D and the time code occupy the same 28 bits, so asking for
            // bsid 6 drops whatever timecode the plan carried rather than
            // handing the encoder a config it would refuse.
            .alternate_bsi = plan.meta.annexd
                                 ? std::optional<meta::AlternateBsi>(alternate_bsi(plan.meta))
                                 : std::nullopt,
            .search = plan.tools.search};
}

namespace {

void apply_tools(const Tools& tools, eac3::FrameConfig& config) {
    config.auto_tools = tools.auto_tools;
    config.coupling = tools.coupling && fullbw_channel_count(config.acmod) >= 2;
    config.cplbegf = tools.cplbegf;
    config.enhanced = tools.enhanced;
    config.spx = tools.spx;
    config.spxbegf = tools.spxbegf;
    config.spx_atten = tools.spx_atten;
    config.spxattencod = tools.spxattencod;
    config.aht = tools.aht;
    config.gaqmod = tools.gaqmod;
    config.transient_prenoise = tools.transient_prenoise;
    config.fast_mdct = tools.fast_mdct;
    config.dither = tools.dither;
    config.delta_allocation = tools.delta;
    config.numblkscod = tools.numblkscod;
    // EQ13: CBR only - see FrameConfig::search's own comment for what
    // search=distortion/perceptual actually do here, and for how the two
    // axes it now moves are priced against each other.
    config.search = tools.search;
    // fgaincod: -1 leaves Table E1.4's implied 0x4 and writes no element, which
    // is byte-for-byte the frame this encoder emitted before the field
    // existed; 0..7 pins the code and pays for the per-block fgaincode
    // element in all six blocks.
    config.fgaincod = tools.fgaincod;
}

// A dependent's share of the plan's VBR bounds, halved the same way its
// bitrate_kbps already is below - substreams occupy one frame period, not
// one frame, so each gets its own slice of whatever rate range the plan
// asked for. quality is not a rate quantity, so it carries over unchanged.
eac3::VbrConfig halve_vbr_bounds(eac3::VbrConfig vbr) {
    if (vbr.min_kbps.has_value()) {
        *vbr.min_kbps /= 2;
    }
    if (vbr.max_kbps.has_value()) {
        *vbr.max_kbps /= 2;
    }
    if (vbr.nominal_kbps.has_value()) {
        *vbr.nominal_kbps /= 2;
    }
    // The ABR target is a rate too, and the plan's target is what the WHOLE
    // access unit is contracted to average - so each substream holds half of
    // it, or the two together would deliver twice what was asked. The window
    // is a count of frames, not a rate, so it carries over unchanged: both
    // substreams cover the same 1536 samples and therefore the same span of
    // time.
    if (vbr.abr.has_value()) {
        vbr.abr->target_kbps /= 2;
    }
    return vbr;
}

}  // namespace

eac3::AccessUnitConfig eac3_config(const Plan& plan) {
    auto programme = eac3_programme(plan);
    // FrameConfig is trivially copyable; only the dependent vector is worth
    // moving.
    return {.independent = programme.independent,
            .dependents = std::move(programme.dependents),
            .additional = {}};
}

eac3::ProgrammeConfig eac3_programme(const Plan& plan) {
    const auto cp = resolve(plan);
    eac3::ProgrammeConfig out;
    auto& independent = out.independent;
    independent.sample_rate = plan.sample_rate;
    independent.bitrate_kbps = plan.bitrate_kbps;
    independent.acmod = cp.bed_acmod;
    independent.lfe = cp.bed_lfe;
    independent.dialnorm = plan.meta.dialnorm;
    if (cp.bed_acmod == Acmod::kDualMono) {
        independent.dialnorm2 = plan.meta.dialnorm2;
    }
    independent.drc = plan.meta.drc;
    independent.heavy = plan.meta.heavy;
    if (cp.bed_acmod == Acmod::kDualMono) {
        independent.drc2 = plan.meta.drc2;
        independent.heavy2 = plan.meta.heavy2;
    }
    if (plan.meta.mixmeta) {
        independent.mixing = mix_metadata(plan.meta);
    }
    if (plan.meta.infomdat) {
        independent.info = plan.meta.info;
        // Annex E's audprodie carries adconvtyp as a third field where AC-3's
        // stops at roomtyp; Metadata states it once, outside audprod, so this
        // is where it reaches the wire on this side.
        if (independent.info->audprod.has_value()) {
            independent.info->audprod->adconvtyp = plan.meta.adconvtyp;
        }
    }
    apply_tools(plan.tools, independent);
    independent.vbr = plan.vbr;

    // A dependent gets its own slice of the rate rather than a share of the
    // independent's - substreams occupy one frame period, not one frame.
    const std::uint32_t dependent_kbps = plan.bitrate_kbps / 2;
    for (const auto mask : cp.dependents) {
        eac3::FrameConfig dependent{};
        dependent.sample_rate = plan.sample_rate;
        dependent.bitrate_kbps = dependent_kbps;
        dependent.chanmap = mask;
        // `mask` came from a ChannelPlan that channel_plan_for/allocate()
        // already built to satisfy exactly one (acmod, lfeon) - it cannot
        // fail here.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto fit = *eac3::chanmap::acmod_for_chanmap(mask);
        dependent.acmod = fit.first;
        dependent.lfe = fit.second;
        apply_tools(plan.tools, dependent);
        if (plan.vbr.has_value()) {
            dependent.vbr = halve_vbr_bounds(*plan.vbr);
        }
        out.dependents.push_back(dependent);
    }
    return out;
}

// --- routing ----------------------------------------------------------------

bool Routing::is_permutation() const {
    if (source_channels != coded_channels) {
        return false;
    }
    // Compared with a tolerance rather than exactly: a pan that lands on a
    // speaker's own direction solves to unity through a 2x2 system, and the
    // last bit of that is not something a caller should have to reason about.
    std::vector<int> used(static_cast<std::size_t>(source_channels), 0);
    for (int coded = 0; coded < coded_channels; ++coded) {
        int taken = -1;
        for (int source = 0; source < source_channels; ++source) {
            const double g = at(coded, source);
            if (g <= kNegligibleGain) {
                continue;
            }
            if (std::abs(g - 1.0) > kNegligibleGain || taken >= 0) {
                return false;
            }
            taken = source;
        }
        if (taken < 0 || used[static_cast<std::size_t>(taken)]++ != 0) {
            return false;
        }
    }
    return true;
}

std::optional<Routing> route(const ChannelPlan& target, std::size_t wav_channels,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev) {
    if (wav_channels == 0) {
        return std::nullopt;
    }
    // Dual mono has no soundstage to pan into - Ch1 and Ch2 are unrelated
    // programmes, not directions - so the direction-based machinery below,
    // built entirely around Table E2.5 locations, does not apply at all. The
    // only sensible routing is the identity: source channel i is coded
    // channel i, always. The caller is responsible for having assembled
    // `wav_channels == 2` worth of source PCM as Ch1 then Ch2, whether that
    // came from one two-channel file or two mono ones.
    if (target.bed_acmod == Acmod::kDualMono) {
        if (wav_channels != 2) {
            return std::nullopt;
        }
        return Routing{.source_channels = 2, .coded_channels = 2, .gain = {1.0, 0.0, 0.0, 1.0}};
    }
    // A source exactly as wide as the target is taken to BE the target, in WAV
    // speaker order. Nothing else can distinguish 7.1 from 5.1.2 at eight
    // channels, and a user who picked 7.1 for an eight-channel file has said
    // which one it is.
    std::vector<Location> source;
    if (wav_channels == static_cast<std::size_t>(rendered_channel_count(target))) {
        source = in_wav_order(rendered_locations(target));
    } else {
        const auto generic = generic_wav_layout(wav_channels);
        if (!generic.has_value()) {
            return std::nullopt;
        }
        source = *generic;
    }
    return route(target, std::span<const Location>{source}, clev, slev);
}

std::optional<Routing> route(const ChannelPlan& target, std::span<const Location> source_locations,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev) {
    if (source_locations.empty()) {
        return std::nullopt;
    }
    // Dual mono again, for a source that states its locations: Ch1 and Ch2 are
    // not directions, so there is nothing for a location to say about either,
    // and the identity over exactly two channels is the only routing it has.
    if (target.bed_acmod == Acmod::kDualMono) {
        if (source_locations.size() != 2) {
            return std::nullopt;
        }
        return Routing{.source_channels = 2, .coded_channels = 2, .gain = {1.0, 0.0, 0.0, 1.0}};
    }
    // A location named twice is not a layout: which of the two channels is "the"
    // centre has no answer, and a panner would quietly sum them.
    for (std::size_t i = 0; i < source_locations.size(); ++i) {
        const auto rest = source_locations.subspan(i + 1);
        if (std::ranges::find(rest, source_locations[i]) != rest.end()) {
            return std::nullopt;
        }
    }
    const std::vector<Location> source(source_locations.begin(), source_locations.end());

    const auto coded = coded_channels(target);
    Routing out{.source_channels = static_cast<int>(source.size()),
                .coded_channels = static_cast<int>(coded.size()),
                .gain = std::vector<double>(source.size() * coded.size(), 0.0)};

    // The LFE is not a direction and never takes part in a pan: §7.8 makes its
    // downmix optional and objects reach it only by an explicit send.
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (!is_lfe(coded[c].location)) {
            continue;
        }
        for (std::size_t s = 0; s < source.size(); ++s) {
            if (is_lfe(source[s])) {
                out.gain[c * source.size() + s] = 1.0;
            }
        }
    }

    if (fold_down(source, target, clev, slev, out)) {
        return out;
    }

    // Two target sets, because a bed channel and a dependent channel of the
    // same name need not carry the same thing.
    //
    // A dependent either REPLACES a bed channel (7.1's side surrounds) or ADDS
    // one (the heights), and which it does decides what the bed underneath is
    // fed:
    //
    //   Replaced - the bed channel is fed a rendering of the BED layout, so a
    //   7.1 source's sides and rears both fold into the 5.1 surround a legacy
    //   decoder plays. Nothing is heard twice, because a full decoder throws
    //   that channel away and takes the dependent's instead (§E3.8.2).
    //
    //   Not replaced - the bed channel is fed a rendering of the FULL layout,
    //   so height content lands only on the height speakers. Folding it into
    //   the bed as well would have a 5.1.4 decoder play it twice, once from
    //   above and once from the ring.
    //
    // The cost of the second rule is that a 5.1 decoder does not hear the
    // height layer at all. That is what a channel-based height extension is:
    // carrying it in both places is not an option, and the object path (JOC,
    // TS 103 420) is what exists for the case where it has to survive.
    const auto bed = pan_targets(bed_locations(target));
    const auto rendered = pan_targets(rendered_locations(target));

    std::vector<bool> replaced(coded.size(), false);
    for (std::size_t c = 0; c < coded.size(); ++c) {
        replaced[c] = coded[c].bed &&
                      std::ranges::any_of(coded, [&](const CodedChannel& other) {
                          return !other.bed && other.location == coded[c].location;
                      });
    }

    std::vector<double> bed_gains(bed.directions.size());
    std::vector<double> rendered_gains(rendered.directions.size());
    const bool source_has_rears = std::ranges::find(source, Location::kLrs) != source.end();
    const bool source_has_side_discrete = std::ranges::find(source, Location::kLsd) != source.end();

    for (std::size_t s = 0; s < source.size(); ++s) {
        if (is_lfe(source[s])) {
            continue;
        }
        const auto direction =
            direction_of(source[s], source_has_rears, source_has_side_discrete);
        pan_direction(direction, bed.directions, bed_gains);
        pan_direction(direction, rendered.directions, rendered_gains);

        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (is_lfe(coded[c].location)) {
                continue;
            }
            const auto& set = replaced[c] ? bed : rendered;
            const auto& gains = replaced[c] ? bed_gains : rendered_gains;
            const int index = set.index_of(coded[c].location);
            if (index >= 0) {
                out.gain[c * source.size() + s] = gains[static_cast<std::size_t>(index)];
            }
        }
    }

    // §7.8.1's normalisation: where several source channels land on one
    // speaker their coefficients sum, and a sum above unity clips. A 5.1.4
    // source rendered into 7.1.4 folds the ceiling into the bed's surrounds
    // and measured +1.5 dBFS before this existed.
    //
    // Applied per SUBSTREAM rather than across the whole access unit. §7.8's
    // "attenuating all downmix coefficients equally" is about one downmix, and
    // the independent substream is the only downmix here - the dependents
    // carry the discrete rendering, at unity, with nothing folded into them.
    // Scaling everything together would drop that rendering by as much as
    // 12 dB to protect a fallback nobody with a wide decoder ever hears, which
    // is a worse answer than the clipping it prevents.
    //
    // Within the bed it IS one factor rather than one per speaker: scaling
    // only the oversubscribed speaker would change the balance between
    // speakers, which is a different mix rather than a quieter one.
    int substreams = 0;
    for (const auto& channel : coded) {
        substreams = std::max(substreams, channel.substream);
    }
    for (int substream = 0; substream <= substreams; ++substream) {
        double loudest = 1.0;
        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (coded[c].substream != substream) {
                continue;
            }
            double sum = 0.0;
            for (std::size_t s = 0; s < source.size(); ++s) {
                sum += std::abs(out.gain[c * source.size() + s]);
            }
            loudest = std::max(loudest, sum);
        }
        if (loudest <= 1.0) {
            continue;
        }
        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (coded[c].substream != substream) {
                continue;
            }
            for (std::size_t s = 0; s < source.size(); ++s) {
                out.gain[c * source.size() + s] /= loudest;
            }
        }
    }
    return out;
}

std::optional<Routing> route(LayoutId target, std::size_t wav_channels,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev) {
    return route(channel_plan_for(target), wav_channels, clev, slev);
}

void render(const Routing& routing, std::span<const std::span<const float>> source,
            std::span<const std::span<float>> coded, std::size_t samples) {
    for (int c = 0; c < routing.coded_channels; ++c) {
        auto out = coded[static_cast<std::size_t>(c)];
        std::fill_n(out.begin(), samples, 0.0f);
        for (int s = 0; s < routing.source_channels; ++s) {
            const double gain = routing.at(c, s);
            if (gain == 0.0) {
                continue;
            }
            const auto in = source[static_cast<std::size_t>(s)];
            const auto n = std::min(samples, in.size());
            for (std::size_t i = 0; i < n; ++i) {
                out[i] += static_cast<float>(gain * static_cast<double>(in[i]));
            }
        }
    }
}

}  // namespace iclforge::ac3::plan
