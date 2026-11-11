#include "truehd_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/layout.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/truehd/encoder/bed_pan.hpp"
#include "truehd_objects.hpp"

namespace forge_cli::commands {

namespace {

namespace oba = iclforge::objects::oba;
using Location = iclforge::base::Location;

constexpr double kFullScale = 8388608.0;  // 2^23

// The Table E2.5 location of a bed label the layout renderer places by location; none for the two
// top-back labels, which it places by their room position as an object.
std::optional<Location> location_of(oba::BedLabel label) {
    switch (label) {
        case oba::BedLabel::kL:
            return Location::kLeft;
        case oba::BedLabel::kR:
            return Location::kRight;
        case oba::BedLabel::kC:
            return Location::kCentre;
        case oba::BedLabel::kLfe:
            return Location::kLfe;
        case oba::BedLabel::kLs:
            return Location::kLeftSurround;
        case oba::BedLabel::kRs:
            return Location::kRightSurround;
        case oba::BedLabel::kLb:
            return Location::kLrs;
        case oba::BedLabel::kRb:
            return Location::kRrs;
        case oba::BedLabel::kTfl:
            return Location::kVhl;
        case oba::BedLabel::kTfr:
            return Location::kVhr;
        case oba::BedLabel::kTsl:
            return Location::kLts;
        case oba::BedLabel::kTsr:
            return Location::kRts;
        case oba::BedLabel::kLw:
            return Location::kLw;
        case oba::BedLabel::kRw:
            return Location::kRw;
        case oba::BedLabel::kLfe2:
            return Location::kLfe2;
        case oba::BedLabel::kTbl:
        case oba::BedLabel::kTbr:
            break;
    }
    return std::nullopt;
}

// The row of the stream's 7.1 bed (L R C LFE Ls Rs Lrs Rrs) each slot of the layout is, when the
// layout is those eight speakers, each once, in any order: the name 7.1 lists them as L C R Ls Rs
// Lrs Rrs LFE, a list may give the stream's own order. None for any other layout.
std::optional<std::array<std::size_t, 8>> stream_bed_rows(
    const iclforge::render::OutputLayout& layout) {
    constexpr std::array<Location, 8> bed{Location::kLeft,         Location::kRight,
                                           Location::kCentre,       Location::kLfe,
                                           Location::kLeftSurround, Location::kRightSurround,
                                           Location::kLrs,          Location::kRrs};
    if (layout.slots() != bed.size()) {
        return std::nullopt;
    }
    std::array<std::size_t, 8> rows{};
    std::array<bool, 8> taken{};
    for (std::size_t slot = 0; slot < bed.size(); ++slot) {
        const auto& speaker = layout.slot(slot);
        std::size_t row = bed.size();
        for (std::size_t r = 0; r < bed.size(); ++r) {
            const bool lfe = bed[r] == Location::kLfe;
            if (lfe ? speaker.kind == iclforge::render::Speaker::Kind::kLfe
                    : (speaker.kind != iclforge::render::Speaker::Kind::kLfe && speaker.location &&
                       *speaker.location == bed[r])) {
                row = r;
                break;
            }
        }
        if (row == bed.size() || taken[row]) {
            return std::nullopt;
        }
        taken[row] = true;
        rows[slot] = row;
    }
    return rows;
}

}  // namespace

TrueHdObjectRenderer::TrueHdObjectRenderer(const iclforge::render::OutputLayout& layout,
                                           std::uint32_t sample_rate, bool limit)
    : layout_(layout),
      renderer_(layout, sample_rate),
      slots_(layout.slots()),
      limit_(limit) {
    if (const auto rows = stream_bed_rows(layout)) {
        bed_rows_ = *rows;
        measured_ = true;
    }
    // Only the renderer's gains are used, never its render(), so its LFE delay line is not either.
    for (std::size_t slot = 0; slot < slots_; ++slot) {
        const auto& speaker = layout_.slot(slot);
        names_ += slot == 0 ? "" : " ";
        names_ += speaker.location
                      ? std::string(iclforge::base::name(*speaker.location))
                      : (speaker.kind == iclforge::render::Speaker::Kind::kLfe ? "LFE" : "-");
    }
}

TrueHdObjectRenderer::Gains TrueHdObjectRenderer::bed_gains(oba::BedLabel label) {
    Gains out{};
    if (const auto location = location_of(label)) {
        iclforge::base::Layout bed;
        bed.items[0] = *location;
        bed.count = 1;
        renderer_.set_bed(bed);
        for (std::size_t slot = 0; slot < slots_; ++slot) {
            out[slot] = static_cast<double>(renderer_.bed_gain(0, slot));
        }
        return out;
    }
    iclforge::objects::oba::DisplayObject object;
    object.position = oba::bed_label_position(label);
    renderer_.set_objects(std::span<const oba::DisplayObject>(&object, 1));
    for (std::size_t slot = 0; slot < slots_; ++slot) {
        out[slot] = static_cast<double>(renderer_.object_gain(0, slot));
    }
    return out;
}

void TrueHdObjectRenderer::set_plan(const ObjectPlan& plan) {
    tracks_.assign(plan.channels.size(), Track{});
    for (std::size_t i = 0; i < plan.channels.size(); ++i) {
        const ObjectChannel& channel = plan.channels[i];
        Track& track = tracks_[i];
        if ((channel.kind == ObjectChannelKind::kBed || channel.kind == ObjectChannelKind::kLfe) &&
            channel.label) {
            track.gains = bed_gains(*channel.label);
            track.placed = true;
        } else if (channel.kind == ObjectChannelKind::kDynamic) {
            track.dynamic = true;
        }
    }
}

void TrueHdObjectRenderer::add_update(std::size_t channel, std::uint64_t sample, int ramp,
                                      const oba::DynamicObject& state) {
    if (channel >= tracks_.size() || !tracks_[channel].dynamic) {
        return;
    }
    Event event;
    event.sample = sample;
    event.ramp = std::max(ramp, 0);
    if (measured_) {
        const auto shares = iclforge::truehd::bed_pan_gains(iclforge::truehd::bed_position_code(
            state.position.x, state.position.y, state.position.z));
        const double level = state.active ? std::pow(10.0, state.gain_db / 20.0) : 0.0;
        for (std::size_t slot = 0; slot < slots_ && slot < bed_rows_.size(); ++slot) {
            event.target[slot] = level * shares[bed_rows_[slot]];
        }
        tracks_[channel].events.push_back(event);
        return;
    }
    oba::DisplayObject object;
    object.position = state.position;
    object.size = state.size;
    object.gain_db = state.gain_db;
    object.snap = state.snap;
    object.active = state.active;
    object.zone = state.zone;
    object.enable_elevation = state.enable_elevation;
    renderer_.set_objects(std::span<const oba::DisplayObject>(&object, 1));
    for (std::size_t slot = 0; slot < slots_; ++slot) {
        event.target[slot] = static_cast<double>(renderer_.object_gain(0, slot));
    }
    tracks_[channel].events.push_back(event);
}

void TrueHdObjectRenderer::render(std::span<const std::int32_t> pcm, std::size_t channels,
                                  std::size_t samples, std::vector<std::int32_t>& out) {
    out.assign(samples * slots_, 0);
    std::vector<double> mixed(samples * slots_, 0.0);
    std::vector<double> mix(slots_);
    for (std::size_t i = 0; i < samples; ++i) {
        const std::uint64_t now = position_ + i;
        std::ranges::fill(mix, 0.0);
        for (std::size_t ch = 0; ch < channels && ch < tracks_.size(); ++ch) {
            Track& track = tracks_[ch];
            if (track.dynamic) {
                while (!track.events.empty() && track.events.front().sample <= now) {
                    const Event& event = track.events.front();
                    track.target = event.target;
                    if (!track.primed || event.ramp == 0) {
                        track.gains = event.target;
                        track.left = 0;
                        track.primed = true;
                    } else {
                        for (std::size_t slot = 0; slot < slots_; ++slot) {
                            track.step[slot] = (event.target[slot] - track.gains[slot]) /
                                               static_cast<double>(event.ramp);
                        }
                        track.left = event.ramp;
                    }
                    track.events.pop_front();
                }
                if (!track.primed) {
                    continue;
                }
            } else if (!track.placed) {
                continue;
            }
            const double x = static_cast<double>(pcm[i * channels + ch]) / kFullScale;
            for (std::size_t slot = 0; slot < slots_; ++slot) {
                mix[slot] += track.gains[slot] * x;
            }
            if (track.left > 0) {
                for (std::size_t slot = 0; slot < slots_; ++slot) {
                    track.gains[slot] += track.step[slot];
                }
                if (--track.left == 0) {
                    track.gains = track.target;
                }
            }
        }
        std::ranges::copy(mix, mixed.begin() + static_cast<std::ptrdiff_t>(i * slots_));
    }
    // The sum of the objects under the ceiling, one gain for all the slots, then to the 24 bit grid.
    if (limit_) {
        limited_ += iclforge::truehd::limit_bed_peaks(mixed, static_cast<int>(slots_));
    }
    for (std::size_t i = 0; i < samples * slots_; ++i) {
        const double scaled = std::nearbyint(mixed[i] * kFullScale);
        double limited = scaled;
        if (scaled > kFullScale - 1.0) {
            limited = kFullScale - 1.0;
            // the largest 24 bit value is 1 / 2^23 under the ceiling the limiter holds: not a clip
            if (mixed[i] > 1.0) {
                ++clipped_;
            }
        } else if (scaled < -kFullScale) {
            limited = -kFullScale;
            ++clipped_;
        }
        out[i] = static_cast<std::int32_t>(limited);
    }
    position_ += samples;
}

}  // namespace forge_cli::commands
