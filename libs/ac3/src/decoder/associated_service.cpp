#include "iclforge/ac3/decoder/associated_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

namespace iclforge::ac3 {

namespace {

using eac3::chanmap::Location;

constexpr double kQuarterTurn = std::numbers::pi / 2.0;

// The five horizontal seats Tables E3.15-E3.17 name. Everything wider reduces
// to these, the way eac3_seat_fold.hpp reduces a wide layout to an acmod.
enum class Seat : std::uint8_t { kLeft, kCentre, kRight, kLeftSurround, kRightSurround };
constexpr std::size_t kSeatCount = 5;
using SeatValues = std::array<double, kSeatCount>;

[[nodiscard]] constexpr std::size_t index_of(Seat seat) {
    return static_cast<std::size_t>(seat);
}

// Which seats a location belongs to: none for the LFEs and the heights, which a
// pan has no table for; one for most; both surrounds for a mono rear surround.
struct SeatSet {
    std::array<Seat, 2> seats{};
    std::size_t count = 0;
};

[[nodiscard]] constexpr SeatSet seats_of(Location location) {
    switch (location) {
        case Location::kLeft:
        case Location::kLc:
        case Location::kLw:
            return {{Seat::kLeft, Seat::kLeft}, 1};
        case Location::kCentre:
            return {{Seat::kCentre, Seat::kCentre}, 1};
        case Location::kRight:
        case Location::kRc:
        case Location::kRw:
            return {{Seat::kRight, Seat::kRight}, 1};
        case Location::kLeftSurround:
        case Location::kLrs:
        case Location::kLsd:
            return {{Seat::kLeftSurround, Seat::kLeftSurround}, 1};
        case Location::kRightSurround:
        case Location::kRrs:
        case Location::kRsd:
            return {{Seat::kRightSurround, Seat::kRightSurround}, 1};
        case Location::kCs:
            return {{Seat::kLeftSurround, Seat::kRightSurround}, 2};
        case Location::kTs:
        case Location::kVhl:
        case Location::kVhr:
        case Location::kVhc:
        case Location::kLts:
        case Location::kRts:
        case Location::kLfe2:
        case Location::kLfe:
            break;
    }
    return {};
}

// Table E3.15: a mono service over a stereo main. The amplitudes are the
// table's, panmean 0..239; the centre, at 0, is both at -3 dB, and from 30 to
// 150 degrees the service sits in the right speaker alone.
[[nodiscard]] SeatValues stereo_amplitudes(int panmean) {
    SeatValues out{};
    auto& left = out[index_of(Seat::kLeft)];
    auto& right = out[index_of(Seat::kRight)];
    if (panmean <= 19) {
        const double angle = kQuarterTurn * (panmean + 20) / 40.0;
        left = std::cos(angle);
        right = std::sin(angle);
    } else if (panmean <= 99) {
        right = 1.0;
    } else if (panmean <= 139) {
        const double angle = kQuarterTurn * (panmean - 100) / 40.0;
        left = std::sin(angle);
        right = std::cos(angle);
    } else if (panmean <= 219) {
        left = 1.0;
    } else {
        const double angle = kQuarterTurn * (panmean - 220) / 40.0;
        left = std::cos(angle);
        right = std::sin(angle);
    }
    return out;
}

// Tables E3.16 (L, C, R) and E3.17 (Ls, Rs): a mono service over a 5.1 main,
// the LFE excluded. Each row is a constant-power crossfade between the two
// speakers the index lies between - the centre at 0, right at 20 (30 degrees),
// right surround at 73, left surround at 167, left at 220 - so the five
// amplitudes square to one at every index.
[[nodiscard]] SeatValues five_one_amplitudes(int panmean) {
    SeatValues out{};
    auto& left = out[index_of(Seat::kLeft)];
    auto& centre = out[index_of(Seat::kCentre)];
    auto& right = out[index_of(Seat::kRight)];
    auto& left_surround = out[index_of(Seat::kLeftSurround)];
    auto& right_surround = out[index_of(Seat::kRightSurround)];
    if (panmean <= 19) {
        const double angle = kQuarterTurn * panmean / 20.0;
        centre = std::cos(angle);
        right = std::sin(angle);
    } else if (panmean <= 72) {
        const double angle = kQuarterTurn * (panmean - 20) / 53.0;
        right = std::cos(angle);
        right_surround = std::sin(angle);
    } else if (panmean <= 166) {
        const double angle = kQuarterTurn * (panmean - 73) / 94.0;
        left_surround = std::sin(angle);
        right_surround = std::cos(angle);
    } else if (panmean <= 219) {
        const double angle = kQuarterTurn * (panmean - 167) / 53.0;
        left = std::sin(angle);
        left_surround = std::cos(angle);
    } else {
        const double angle = kQuarterTurn * (panmean - 220) / 20.0;
        left = std::cos(angle);
        centre = std::sin(angle);
    }
    return out;
}

// Carry `power` (the square of each seat's amplitude) onto the channels of a
// main that has `locations`. A seat the main has no channel in is folded into
// the nearest it does have, by power so that the service keeps its level; the
// channels sharing a seat split the seat's power evenly.
[[nodiscard]] std::vector<double> distribute(SeatValues power,
                                             std::span<const Location> locations) {
    std::array<std::vector<std::size_t>, kSeatCount> members;
    for (std::size_t i = 0; i < locations.size(); ++i) {
        const auto set = seats_of(locations[i]);
        for (std::size_t k = 0; k < set.count; ++k) {
            members[index_of(set.seats[k])].push_back(i);
        }
    }
    const auto occupied = [&](Seat seat) { return !members[index_of(seat)].empty(); };
    const auto fold = [&](Seat from, Seat to, double share) {
        power[index_of(to)] += power[index_of(from)] * share;
        power[index_of(from)] = 0.0;
    };
    // A surround with no speaker goes to the front on its side; a missing
    // centre to the two fronts, half each; a missing front pair to the centre.
    if (!occupied(Seat::kLeftSurround)) {
        fold(Seat::kLeftSurround, Seat::kLeft, 1.0);
    }
    if (!occupied(Seat::kRightSurround)) {
        fold(Seat::kRightSurround, Seat::kRight, 1.0);
    }
    if (!occupied(Seat::kCentre) && (occupied(Seat::kLeft) || occupied(Seat::kRight))) {
        if (occupied(Seat::kLeft) && occupied(Seat::kRight)) {
            const double half = power[index_of(Seat::kCentre)] * 0.5;
            power[index_of(Seat::kLeft)] += half;
            power[index_of(Seat::kRight)] += half;
            power[index_of(Seat::kCentre)] = 0.0;
        } else {
            fold(Seat::kCentre, occupied(Seat::kLeft) ? Seat::kLeft : Seat::kRight, 1.0);
        }
    }
    if (!occupied(Seat::kLeft)) {
        fold(Seat::kLeft, Seat::kCentre, 1.0);
    }
    if (!occupied(Seat::kRight)) {
        fold(Seat::kRight, Seat::kCentre, 1.0);
    }

    // A main whose only channels sit at no seat - heights and LFEs - has
    // nowhere to put the service, and the weights stay zero.
    std::vector<double> weights(locations.size(), 0.0);
    double placed = 0.0;
    for (std::size_t s = 0; s < kSeatCount; ++s) {
        if (!members[s].empty()) {
            placed += power[s];
        }
    }
    for (std::size_t s = 0; s < kSeatCount; ++s) {
        if (members[s].empty()) {
            continue;
        }
        // Nothing reached any occupied seat (a pan to a corner the main lacks
        // and the folds above could not follow): share it out evenly rather
        // than lose the service.
        const double seat_power = placed > 0.0 ? power[s] : 1.0;
        const double each = seat_power / static_cast<double>(members[s].size());
        for (const auto i : members[s]) {
            weights[i] += each;
        }
    }
    for (double& w : weights) {
        w = std::sqrt(w);
    }
    return weights;
}

[[nodiscard]] SeatValues squared(SeatValues amplitudes) {
    for (double& a : amplitudes) {
        a *= a;
    }
    return amplitudes;
}

[[nodiscard]] bool is_lfe(Location location) {
    return location == Location::kLfe || location == Location::kLfe2;
}

// §E2.3.1.13 and §E2.3.1.17: absent means 0 dB.
[[nodiscard]] double scale_gain(const std::optional<int>& code) {
    return code ? meta::pgm_scale_gain(*code) : 1.0;
}

[[nodiscard]] double to_db(double gain) {
    return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
}

// Whether a unit's channels are a downmix of a wider programme: the fold the
// decoder was told to make, and a programme that was wider than the fold's
// channels to begin with. A 2.0 programme through a Lo/Ro fold is still its
// own two channels.
[[nodiscard]] bool downmixed(const DecodedAccessUnit& unit, DownmixTarget fold,
                             std::size_t output_channels) {
    if (fold == DownmixTarget::kAsCoded || unit.acmod == Acmod::kDualMono) {
        return false;
    }
    std::size_t coded = 0;
    for (int i = 0; i < unit.layout.count; ++i) {
        if (!is_lfe(unit.layout[i])) {
            ++coded;
        }
    }
    return coded > output_channels;
}

// What the OTHER programme's metadata does to each channel of the programme
// whose channels sit at `locations`: the per-channel trim of §E3.10.6, or
// dmixscl (§E3.10.7) in its place when `folded`. 1.0 where it says nothing.
//
// The two auxiliary scales name "channels with locations that can only be
// indicated using the chanmap parameter": the first and second channel of
// the programme at a location outside L, C, R, Ls, Rs and the LFE, in coded
// order.
[[nodiscard]] std::vector<double> external_trims(const meta::MixMetadata* owner,
                                                 std::span<const Location> locations, bool folded) {
    std::vector<double> out(locations.size(), 1.0);
    if (owner == nullptr || owner->mixing.mixdef != meta::MixDefinition::kExtended ||
        !owner->mixing.external.has_value()) {
        return out;
    }
    const meta::ExternalScales& scales = *owner->mixing.external;
    if (folded) {
        if (scales.dmixscl) {
            std::ranges::fill(out, meta::external_scale_gain(*scales.dmixscl));
        }
        return out;
    }
    std::size_t aux = 0;
    for (std::size_t i = 0; i < locations.size(); ++i) {
        const std::optional<int>* code = nullptr;
        switch (locations[i]) {
            case Location::kLeft:
                code = &scales.left;
                break;
            case Location::kCentre:
                code = &scales.centre;
                break;
            case Location::kRight:
                code = &scales.right;
                break;
            case Location::kLeftSurround:
                code = &scales.left_surround;
                break;
            case Location::kRightSurround:
                code = &scales.right_surround;
                break;
            case Location::kLfe:
                code = &scales.lfe;
                break;
            default:
                if (scales.auxiliary.has_value() && aux < scales.auxiliary->size()) {
                    code = &(*scales.auxiliary)[aux];
                }
                ++aux;
                break;
        }
        if (code != nullptr && code->has_value()) {
            out[i] = meta::external_scale_gain(**code);
        }
    }
    return out;
}

// One channel of the associated service, and what is to be done with it.
struct Source {
    std::size_t channel = 0;
    Location location = Location::kCentre;
    // A mono service is placed by its pan; anything with a soundfield of its
    // own goes to the channel at its own location.
    bool mono = false;
    int panmean = 0;
    double gain = 1.0;  // the service's own scale, before per-channel trims
};

}  // namespace

std::string_view describe(MixError error) {
    switch (error) {
        case MixError::kSampleRateMismatch:
            return "the main and associated programmes are at different sample rates";
        case MixError::kFrameLengthMismatch:
            return "the main and associated programmes carry a different number of samples";
        case MixError::kChannelCountMismatch:
            return "a programme's channels do not match its layout and fold";
        case MixError::kUnmixableMain:
            return "a 1+1 dual-mono programme is two programmes, not a main to mix against";
        case MixError::kUnmixableAssociated:
            return "the associated programme has no channels to mix";
    }
    return "unknown mix error";
}

std::vector<Location> output_locations(const DecodedAccessUnit& unit, DownmixTarget fold) {
    if (unit.acmod == Acmod::kDualMono) {
        return {};
    }
    switch (fold) {
        case DownmixTarget::kLoRo:
        case DownmixTarget::kLtRt:
            return {Location::kLeft, Location::kRight};
        case DownmixTarget::kMono:
            return {Location::kCentre};
        case DownmixTarget::kAsCoded:
            break;
    }
    return {unit.layout.begin(), unit.layout.end()};
}

std::vector<double> pan_weights(int panmean, std::span<const Location> locations) {
    if (panmean < 0 || panmean > meta::kPanMeanMax) {
        panmean = 0;
    }
    bool horizontal = false;
    std::array<bool, kSeatCount> occupied{};
    for (const auto location : locations) {
        const auto set = seats_of(location);
        for (std::size_t k = 0; k < set.count; ++k) {
            occupied[index_of(set.seats[k])] = true;
            horizontal = true;
        }
    }
    if (!horizontal) {
        return std::vector<double>(locations.size(), 0.0);
    }
    // Table E3.15 is for a stereo main exactly; folding the 5.1 tables onto
    // two speakers would add the surround's share to the front's in amplitude
    // and overshoot it.
    const bool stereo = occupied[index_of(Seat::kLeft)] && occupied[index_of(Seat::kRight)] &&
                        !occupied[index_of(Seat::kCentre)] &&
                        !occupied[index_of(Seat::kLeftSurround)] &&
                        !occupied[index_of(Seat::kRightSurround)];
    const auto amplitudes = stereo ? stereo_amplitudes(panmean) : five_one_amplitudes(panmean);
    return distribute(squared(amplitudes), locations);
}

void AssociatedServiceMixer::reset() {
    main_gains_.clear();
    source_gains_.clear();
    main_channels_ = 0;
    sources_ = 0;
    primed_ = false;
}

std::expected<AssociatedServiceMixResult, MixError> AssociatedServiceMixer::mix(
    DecodedAccessUnit& main, const DecodedAccessUnit& associated) {
    if (main.sample_rate != associated.sample_rate) {
        return std::unexpected(MixError::kSampleRateMismatch);
    }
    if (main.acmod == Acmod::kDualMono) {
        return std::unexpected(MixError::kUnmixableMain);
    }
    if (associated.channels.empty()) {
        return std::unexpected(MixError::kUnmixableAssociated);
    }
    const auto main_locations = output_locations(main, config_.main_fold);
    if (main.channels.empty() || main.channels.size() != main_locations.size()) {
        return std::unexpected(MixError::kChannelCountMismatch);
    }
    const bool dual_mono = associated.acmod == Acmod::kDualMono;
    const auto associated_locations = output_locations(associated, config_.associated_fold);
    if (dual_mono ? associated.channels.size() != 2
                  : associated.channels.size() != associated_locations.size()) {
        return std::unexpected(MixError::kChannelCountMismatch);
    }
    const std::size_t samples = main.channels.front().size();
    const auto same_length = [samples](const std::vector<float>& channel) {
        return channel.size() == samples;
    };
    if (!std::ranges::all_of(main.channels, same_length) ||
        !std::ranges::all_of(associated.channels, same_length)) {
        return std::unexpected(MixError::kFrameLengthMismatch);
    }

    const meta::MixMetadata* main_mix = main.mixing ? &*main.mixing : nullptr;
    const meta::MixMetadata* associated_mix = associated.mixing ? &*associated.mixing : nullptr;
    const std::size_t main_count = main.channels.size();

    // The main, channel by channel: its own pgmscl, the associated service's
    // extpgmscl, and that service's trim for the channel.
    const double main_all = scale_gain(main_mix ? main_mix->pgmscl : std::nullopt) *
                            scale_gain(associated_mix ? associated_mix->extpgmscl : std::nullopt);
    const bool main_folded = downmixed(main, config_.main_fold, main_count);
    const auto main_trims = external_trims(associated_mix, main_locations, main_folded);
    std::vector<double> main_gains(main_count);
    for (std::size_t c = 0; c < main_count; ++c) {
        main_gains[c] = main_all * main_trims[c];
    }

    // The associated service, one source per channel it carries.
    const double trim = std::pow(10.0, config_.associated_trim_db / 20.0);
    const double associated_common =
        scale_gain(main_mix ? main_mix->extpgmscl : std::nullopt) * trim;
    std::vector<Source> sources;
    if (dual_mono) {
        // 1+1: two services in one syncframe, each with its own scale and pan.
        for (std::size_t ch = 0; ch < 2; ++ch) {
            Source source;
            source.channel = ch;
            source.mono = true;
            const auto& scale = ch == 0 ? (associated_mix ? associated_mix->pgmscl : std::nullopt)
                                        : (associated_mix ? associated_mix->pgmscl2 : std::nullopt);
            const auto& pan = ch == 0 ? (associated_mix ? associated_mix->pan : std::nullopt)
                                      : (associated_mix ? associated_mix->pan2 : std::nullopt);
            source.panmean = pan ? pan->panmean : 0;
            source.gain = scale_gain(scale) * associated_common;
            sources.push_back(source);
        }
    } else {
        const bool lone_centre =
            associated_locations.size() == 1 && associated_locations.front() == Location::kCentre;
        const double own = scale_gain(associated_mix ? associated_mix->pgmscl : std::nullopt);
        for (std::size_t ch = 0; ch < associated_locations.size(); ++ch) {
            if (is_lfe(associated_locations[ch])) {
                continue;  // §E3.10.8: the LFE is not part of the service's pan
            }
            Source source;
            source.channel = ch;
            source.location = associated_locations[ch];
            source.mono = lone_centre;
            source.panmean = lone_centre && associated_mix && associated_mix->pan
                                 ? associated_mix->pan->panmean
                                 : 0;
            source.gain = own * associated_common;
            sources.push_back(source);
        }
    }
    if (sources.empty()) {
        return std::unexpected(MixError::kUnmixableAssociated);
    }
    const bool associated_folded =
        downmixed(associated, config_.associated_fold, associated.channels.size());
    const auto associated_trims = external_trims(
        main_mix, dual_mono ? std::vector<Location>(2, Location::kCentre) : associated_locations,
        associated_folded);

    // The sources' weights into each main channel, row per source.
    std::vector<double> source_gains(sources.size() * main_count, 0.0);
    for (std::size_t s = 0; s < sources.size(); ++s) {
        const Source& source = sources[s];
        std::vector<double> route;
        if (source.mono) {
            route = pan_weights(source.panmean, main_locations);
        } else {
            route.assign(main_count, 0.0);
            const auto exact = std::ranges::find(main_locations, source.location);
            if (exact != main_locations.end()) {
                route[static_cast<std::size_t>(exact - main_locations.begin())] = 1.0;
            } else {
                // The main has no such channel: seat it where it would sit.
                const auto set = seats_of(source.location);
                if (set.count > 0) {
                    SeatValues power{};
                    for (std::size_t k = 0; k < set.count; ++k) {
                        power[index_of(set.seats[k])] += 1.0 / static_cast<double>(set.count);
                    }
                    route = distribute(power, main_locations);
                }
            }
        }
        const double gain = source.gain * associated_trims[source.channel];
        for (std::size_t c = 0; c < main_count; ++c) {
            source_gains[s * main_count + c] = gain * route[c];
        }
    }

    // A change of gain ramps over the first block of the unit; the first unit,
    // or one whose shape differs from the last, has nothing to ramp from.
    const bool ramped = primed_ && main_channels_ == main_count && sources_ == sources.size();
    const std::vector<double>& previous_main = ramped ? main_gains_ : main_gains;
    const std::vector<double>& previous_sources = ramped ? source_gains_ : source_gains;
    const std::size_t ramp = std::min<std::size_t>(kSamplesPerBlock, samples);
    for (std::size_t c = 0; c < main_count; ++c) {
        float* out = main.channels[c].data();
        const double main_from = previous_main[c];
        const double main_delta = main_gains[c] - main_from;
        for (std::size_t n = 0; n < samples; ++n) {
            const double t =
                n < ramp ? static_cast<double>(n + 1) / static_cast<double>(ramp) : 1.0;
            double acc = (main_from + main_delta * t) * static_cast<double>(out[n]);
            for (std::size_t s = 0; s < sources.size(); ++s) {
                const double from = previous_sources[s * main_count + c];
                const double to = source_gains[s * main_count + c];
                if (from == 0.0 && to == 0.0) {
                    continue;
                }
                acc += (from + (to - from) * t) *
                       static_cast<double>(associated.channels[sources[s].channel][n]);
            }
            out[n] = static_cast<float>(acc);
        }
    }

    main_gains_ = std::move(main_gains);
    source_gains_ = std::move(source_gains);
    main_channels_ = main_count;
    sources_ = sources.size();
    primed_ = true;

    AssociatedServiceMixResult result;
    result.main_gain_db = to_db(main_all);
    result.associated_gain_db = to_db(sources.front().gain);
    if (sources.front().mono) {
        result.panmean = sources.front().panmean;
    }
    return result;
}

}  // namespace iclforge::ac3
