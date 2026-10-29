#include "iclforge/mlp/encoder/atmos.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "iclforge/mlp/decoder/decoder.hpp"

namespace iclforge::mlp {

namespace {

namespace oba = objects::oba;

constexpr int kBedChannels = 8;

// The full-range speakers of the 7.1 bed in 13.1's order, at their positions in TS 103 420's
// room (x 0 left to 1 right, y 0 front to 1 back): L, R, C, Ls and Rs at the sides' midpoints,
// Lrs and Rrs at the back corners. The LFE (channel 3) takes no object.
struct Speaker {
    int channel;
    double x;
    double y;
};
constexpr std::array<Speaker, 7> kSpeakers{{
    {0, 0.0, 0.0},
    {1, 1.0, 0.0},
    {2, 0.5, 0.0},
    {4, 0.0, 0.5},
    {5, 1.0, 0.5},
    {6, 0.0, 1.0},
    {7, 1.0, 1.0},
}};

}  // namespace

std::array<double, 8> render_gains(const oba::DynamicObject& object) {
    std::array<double, 8> gains{};
    if (!object.active) {
        return gains;
    }
    const double x = std::clamp(object.position.x, 0.0, 1.0);
    const double y = std::clamp(object.position.y, 0.0, 1.0);
    double power = 0.0;
    for (const Speaker& s : kSpeakers) {
        const double d = std::hypot(x - s.x, y - s.y);
        const double w = 1.0 / ((d + 0.1) * (d + 0.1));
        gains[static_cast<std::size_t>(s.channel)] = w;
        power += w * w;
    }
    const double scale = std::pow(10.0, object.gain_db / 20.0) / std::sqrt(power);
    for (double& g : gains) {
        g = std::min(1.0, g * scale);
    }
    return gains;
}

oba::Program atmos_program(int objects) {
    oba::Program p;
    p.dynamic_only = false;
    p.bed = oba::bed::kLR | oba::bed::kC | oba::bed::kLfe | oba::bed::kLsRs | oba::bed::kLbRb;
    p.dynamic_objects = objects;
    return p;
}

struct AtmosEncoder::State {
    AtmosConfig config;
    Encoder encoder;
    int spa = 40;
    std::uint64_t frames = 0;              // frames given so far
    std::vector<oba::DynamicObject> last;  // the state in force
    std::vector<std::int32_t> merged;

    std::expected<void, Error> send(std::span<const oba::DynamicObject> state) {
        EvolutionPayload p;
        p.type = static_cast<std::uint8_t>(EvolutionPayloadType::kOamd);
        p.data = oba::build_payload(atmos_program(config.objects), state, spa);
        encoder.attach(std::move(p));
        std::vector<std::array<double, 8>> gains;
        for (const auto& object : state) {
            gains.push_back(render_gains(object));
        }
        encoder.render_objects(gains);
        return {};
    }
};

AtmosEncoder::AtmosEncoder(std::unique_ptr<State> state) : state_(std::move(state)) {}
AtmosEncoder::~AtmosEncoder() = default;
AtmosEncoder::AtmosEncoder(AtmosEncoder&&) noexcept = default;
AtmosEncoder& AtmosEncoder::operator=(AtmosEncoder&&) noexcept = default;

std::expected<AtmosEncoder, Error> AtmosEncoder::create(const AtmosConfig& config) {
    // 15.5: Atmos is 48 kHz only; 15.4: the objects fit the 16-channel presentation.
    if (config.sample_rate != 48000U || config.objects < 1 ||
        config.objects > kMaxChannels - kBedChannels) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    EncoderConfig ec;
    ec.sample_rate = config.sample_rate;
    ec.channels = kBedChannels + config.objects;
    ec.restart_interval = config.restart_interval;
    ec.peak_data_rate = config.peak_data_rate;
    ec.verify = config.verify;
    // 14.2: the 16-channel presentation's description, in (length + 1) = 3 words.
    ExtraChannelMeaning e;
    e.length = 2;
    e.sixteench_channel_count = static_cast<std::uint8_t>(ec.channels);
    e.dyn_object_only = false;
    e.sixteench_content_description = 0x5;  // a channel bed and dynamic objects
    e.sixteench_channel_assignment = atmos_program(config.objects).bed;
    e.sixteench_dynamic_object_count = static_cast<std::uint8_t>(config.objects);
    ec.extra_channel_meaning = e;
    auto encoder = Encoder::create(ec);
    if (!encoder) {
        return std::unexpected(encoder.error());
    }
    auto state = std::make_unique<State>(State{config, std::move(*encoder), 40, 0, {}, {}});
    state->spa = samples_per_access_unit(config.sample_rate);
    return AtmosEncoder(std::move(state));
}

const EncoderStats& AtmosEncoder::stats() const noexcept {
    return state_->encoder.stats();
}

std::expected<std::vector<std::byte>, Error> AtmosEncoder::encode(
    std::span<const std::int32_t> bed, std::span<const std::int32_t> objects,
    std::span<const oba::DynamicObject> state) {
    State& s = *state_;
    const auto n = static_cast<std::size_t>(s.config.objects);
    if (bed.size() % kBedChannels != 0 || objects.size() % n != 0 ||
        bed.size() / kBedChannels != objects.size() / n || (!state.empty() && state.size() != n)) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    for (const auto& object : state) {
        // 15.4: an object's gain is not zero (an inactive object is the way to silence one).
        if (object.active && !std::isfinite(object.gain_db)) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
    }
    if (!state.empty()) {
        s.last.assign(state.begin(), state.end());
        if (auto r = s.send(state); !r) {
            return std::unexpected(r.error());
        }
    }
    const std::size_t frames = bed.size() / kBedChannels;
    const auto interval =
        static_cast<std::uint64_t>(s.config.restart_interval) * static_cast<std::uint64_t>(s.spa);
    std::vector<std::byte> out;
    std::size_t done = 0;
    while (done < frames) {
        // Up to the next restart, where the state in force is sent again for random access.
        const std::uint64_t at = s.frames + done;
        const std::uint64_t next_restart = (at / interval + 1) * interval;
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(frames - done, next_restart - at));
        s.merged.assign(take * (kBedChannels + n), 0);
        for (std::size_t f = 0; f < take; ++f) {
            for (std::size_t c = 0; c < kBedChannels; ++c) {
                s.merged[f * (kBedChannels + n) + c] = bed[(done + f) * kBedChannels + c];
            }
            for (std::size_t o = 0; o < n; ++o) {
                s.merged[f * (kBedChannels + n) + kBedChannels + o] = objects[(done + f) * n + o];
            }
        }
        auto bytes = s.encoder.encode(s.merged);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.insert(out.end(), bytes->begin(), bytes->end());
        done += take;
        if ((s.frames + done) % interval == 0 && !s.last.empty() && done < frames) {
            if (auto r = s.send(s.last); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    s.frames += frames;
    if (s.frames % interval == 0 && !s.last.empty()) {
        if (auto r = s.send(s.last); !r) {
            return std::unexpected(r.error());
        }
    }
    return out;
}

std::expected<std::vector<std::byte>, Error> AtmosEncoder::flush() {
    return state_->encoder.flush();
}

std::expected<AtmosStream, Error> decode_atmos(std::span<const std::byte> stream) {
    auto decoded = decode_stream(stream);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (!decoded->major_sync || decoded->major_sync->num_substreams < 4 ||
        decoded->channels <= kBedChannels) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    AtmosStream out;
    out.sample_rate = decoded->sample_rate;
    out.objects = decoded->channels - kBedChannels;
    const auto ch = static_cast<std::size_t>(decoded->channels);
    const std::size_t frames = decoded->pcm.size() / ch;
    out.bed.resize(frames * kBedChannels);
    out.object_pcm.resize(frames * static_cast<std::size_t>(out.objects));
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < ch; ++c) {
            const std::int32_t v = decoded->pcm[f * ch + c];
            if (c < kBedChannels) {
                out.bed[f * kBedChannels + c] = v;
            } else {
                out.object_pcm[f * static_cast<std::size_t>(out.objects) + c - kBedChannels] = v;
            }
        }
    }
    const auto spa = static_cast<std::uint64_t>(decoded->major_sync->samples_per_access_unit());
    for (std::size_t unit = 0; unit < decoded->evolution.size(); ++unit) {
        for (const EvolutionPayload& p : decoded->evolution[unit]) {
            if (p.type != static_cast<std::uint8_t>(EvolutionPayloadType::kOamd)) {
                continue;
            }
            auto program = oba::parse_payload(p.data);
            // 15.4: the OAMD's channel count is the 16-channel presentation's.
            if (!program || oba::object_count(program->program) != decoded->channels) {
                return std::unexpected(Error::kExtraDataLength);
            }
            out.metadata.push_back({unit * spa, std::move(*program)});
        }
    }
    return out;
}

}  // namespace iclforge::mlp
