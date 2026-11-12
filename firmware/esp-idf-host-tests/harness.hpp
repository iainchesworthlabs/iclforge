#pragma once

// Runs a TrueHD stream through the component's own BurstPlayer (burst_player.cpp and
// truehd_stream.cpp, compiled for the host over the stand-ins in shim_runtime.cpp) into a sink that
// keeps what it is given: the player's whole path, from the ring the Sendspin session fills to the
// block the I2S driver would be handed.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "esp_timer.h"
#include "host_control.hpp"
#include "iclforge/burst_player.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

namespace fwhost {

// A sink that keeps every sample it is written, and plays the blocks one after another on a clock of
// its own (the DAC), so the playout above it never finds itself late.
class CaptureSink final : public iclforge::ScheduledSink {
   public:
    CaptureSink(std::uint32_t rate, bool keep) : rate_(rate), keep_(keep) {}

    bool open(std::uint32_t /*rate*/, std::size_t outputs) override {
        outputs_ = outputs;
        return true;
    }
    std::size_t max_outputs() override { return 16; }
    void begin_stream() override {}

    std::optional<iclforge::PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        ++writes_;
        if (keep_) {
            if (samples_.size() < outputs.size()) {
                samples_.resize(outputs.size());
            }
            for (std::size_t o = 0; o < outputs.size(); ++o) {
                samples_[o].insert(samples_[o].end(), outputs[o].begin(), outputs[o].end());
            }
            play_us_.push_back(next_play_us_);
        }
        const iclforge::PlayoutWrite written{.play_us = next_play_us_, .late = false, .gap = false};
        next_play_us_ += (static_cast<std::int64_t>(iclforge::Playout::kBlockFrames) * 1'000'000 + rate_ / 2) / rate_;
        return written;
    }

    void set_origin(std::int64_t play_us) { next_play_us_ = play_us; }
    [[nodiscard]] const std::vector<std::vector<float>>& samples() const { return samples_; }
    [[nodiscard]] const std::vector<std::int64_t>& play_us() const { return play_us_; }
    [[nodiscard]] std::uint64_t writes() const { return writes_; }

   private:
    std::uint32_t rate_;
    bool keep_;
    std::size_t outputs_ = 0;
    std::int64_t next_play_us_ = 0;
    std::vector<std::vector<float>> samples_;
    std::vector<std::int64_t> play_us_;
    std::uint64_t writes_ = 0;
};

// The access units of a raw TrueHD stream, each as a span of it.
[[nodiscard]] inline std::vector<std::span<const std::uint8_t>> split_units(std::span<const std::uint8_t> stream) {
    std::vector<std::span<const std::uint8_t>> units;
    for (std::size_t at = 0; at < stream.size();) {
        const std::size_t length = iclforge::truehd::access_unit_length_bytes(
            std::as_bytes(stream.subspan(at)));
        if (length < 4 || at + length > stream.size()) {
            throw std::runtime_error("the stream is not whole access units");
        }
        units.push_back(stream.subspan(at, length));
        at += length;
    }
    return units;
}

[[nodiscard]] inline std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The levels the meter had published after a chunk.
struct Level {
    std::uint32_t serial = 0;
    bool have = false;
    std::vector<float> peak_db;
    std::vector<float> rms_db;
    friend bool operator==(const Level&, const Level&) = default;
};

struct Scenario {
    std::vector<std::uint8_t> stream;  // raw access units
    std::uint32_t rate = 48000;
    std::string layout = "5.1";
    // BurstPlayerConfig::truehd_block_samples where the player has it; 0 or the unit's length is a
    // block a unit.
    std::size_t block_samples = 256;
    // BurstPlayerConfig::truehd_decode_ahead where the player has it.
    bool decode_ahead = true;
    std::size_t max_channels = 0;
    std::size_t units_per_chunk = 24;
    // The stream played this many times over, end to end (a timing run plays it a few hundred).
    std::size_t passes = 1;
    // Called with the player idle, before chunk `chunk` is sent.
    std::function<void(iclforge::BurstPlayer&, std::size_t chunk)> before_chunk;
    // Keep what the sink is given (a timing run does not).
    bool keep = true;
    bool real_clock = false;
};

struct Outcome {
    // What the sink was written, an output at a time, and when each block plays.
    std::vector<std::vector<float>> samples;
    std::vector<std::int64_t> play_us;
    std::uint64_t writes = 0;
    std::vector<Level> levels;
    iclforge::BurstPlayerStatus status;
    std::size_t chunks = 0;
    std::size_t units = 0;
};

[[nodiscard]] inline std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ p[i]) * 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t samples_digest(const Outcome& o) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& output : o.samples) {
        const std::uint64_t n = output.size();
        h = fnv(h, &n, sizeof n);
        h = fnv(h, output.data(), output.size() * sizeof(float));
    }
    return h;
}

[[nodiscard]] inline std::uint64_t levels_digest(const Outcome& o) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const Level& l : o.levels) {
        h = fnv(h, &l.serial, sizeof l.serial);
        h = fnv(h, l.peak_db.data(), l.peak_db.size() * sizeof(float));
        h = fnv(h, l.rms_db.data(), l.rms_db.size() * sizeof(float));
    }
    return h;
}

// A setting the player of an earlier commit does not have (BurstPlayerConfig::truehd_block_samples,
// TruehdStream::Config::max_block_samples) is left out, so that the same bench builds against it.
template <class Config>
void set_block_samples(Config& config, std::size_t samples) {
    if constexpr (requires { config.truehd_block_samples; }) {
        config.truehd_block_samples = samples;
    } else if constexpr (requires { config.max_block_samples; }) {
        config.max_block_samples = samples;
    }
}

// TruehdStream::Config::max_run_samples: a chunk's worth where the stream has it.
template <class Stream, class Config>
void set_run_samples(Config& config, bool whole_chunk) {
    if constexpr (requires { config.max_run_samples; }) {
        config.max_run_samples = whole_chunk ? Stream::kMaxRunSamples : 0;
    }
}

template <class Config>
void set_decode_ahead(Config& config, bool ahead) {
    if constexpr (requires { config.truehd_decode_ahead; }) {
        config.truehd_decode_ahead = ahead;
    }
}

// Plays the scenario's stream through a BurstPlayer and returns what came out. With the virtual
// clock (the default) every figure is the same however the host's threads were scheduled: the clock
// moves only between chunks, and the sink plays on a clock of its own.
[[nodiscard]] inline Outcome run(const Scenario& sc) {
    namespace ss = iclforge::sendspin;
    namespace ac = iclforge::sendspin::player;
    constexpr std::int64_t kOrigin = 10'000'000;  // where the stream's first frame plays
    constexpr std::int64_t kLead = 40'000;        // how long before it plays a chunk reaches the player
    const std::vector<std::span<const std::uint8_t>> units = split_units(sc.stream);

    iclforge::BurstPlayerConfig config;
    config.sample_rate = sc.rate;
    config.tuning.sample_rate = sc.rate;
    const auto layout = iclforge::render::OutputLayout::parse(sc.layout);
    if (!layout) {
        throw std::runtime_error("not a layout: " + sc.layout);
    }
    config.layout = *layout;
    config.max_truehd_channels = sc.max_channels;
    set_block_samples(config, sc.block_samples);
    set_decode_ahead(config, sc.decode_ahead);

    if (sc.real_clock) {
        use_real_clock();
    } else {
        use_virtual_clock(kOrigin - kLead);
    }
    CaptureSink sink(sc.rate, sc.keep);
    Outcome out;
    {
        iclforge::BurstPlayer player(config, sink);
        if (!player.start()) {
            throw std::runtime_error("the player did not start");
        }
        player.start_bursts(ac::StreamStart{.data_type = ac::DataType::kTruehd,
                                            .sample_rate = static_cast<std::int32_t>(sc.rate)});
        wait_idle();

        const std::size_t per_sample = sc.rate;
        std::uint64_t cumulative = 0;
        std::size_t chunk_index = 0;
        std::vector<std::uint8_t> payload;
        // Where the stream's first frame plays: a fixed time, or a little after now.
        const std::int64_t t0 = sc.real_clock ? esp_timer_get_time() + 200'000 : kOrigin;
        sink.set_origin(t0 - 30'000);
        for (std::size_t pass = 0; pass < sc.passes; ++pass) {
            for (std::size_t first = 0; first < units.size(); first += sc.units_per_chunk) {
                const std::size_t last = std::min(units.size(), first + sc.units_per_chunk);
                payload.clear();
                for (std::size_t u = first; u < last; ++u) {
                    payload.insert(payload.end(), units[u].begin(), units[u].end());
                }
                if (sc.before_chunk) {
                    sc.before_chunk(player, chunk_index);
                }
                const std::int64_t timestamp = static_cast<std::int64_t>(cumulative * 1'000'000ULL / per_sample);
                const std::int64_t local = t0 + timestamp;
                if (!sc.real_clock) {
                    set_virtual_now(local - kLead);
                }
                ss::BurstChunk chunk;
                chunk.chunk.timestamp_us = timestamp;
                chunk.chunk.data = std::span<const std::uint8_t>(payload);
                chunk.pc = static_cast<std::uint16_t>(ss::BurstDataType::kTruehd);
                chunk.pd = static_cast<std::uint16_t>(payload.size());
                player.burst(chunk, local);
                wait_idle();
                ++chunk_index;
                out.units += last - first;
                const std::size_t unit_samples = sc.rate >= 176400 ? 160 : sc.rate >= 88200 ? 80 : 40;
                cumulative += (last - first) * unit_samples;
                if (sc.keep && !sc.real_clock) {
                    const iclforge::BurstPlayerLevels lv = player.levels();
                    out.levels.push_back(Level{.serial = lv.serial,
                                               .have = lv.have,
                                               .peak_db = std::vector<float>(lv.peak_db.begin(), lv.peak_db.begin() + static_cast<std::ptrdiff_t>(lv.outputs)),
                                               .rms_db = std::vector<float>(lv.rms_db.begin(), lv.rms_db.begin() + static_cast<std::ptrdiff_t>(lv.outputs))});
                }
            }
        }
        out.chunks = chunk_index;
        if (!sc.real_clock) {
            set_virtual_now(t0 + static_cast<std::int64_t>(cumulative * 1'000'000ULL / per_sample) + 100'000);
        }
        player.end();
        wait_idle();
        if (!sc.real_clock) {
            // The decode task publishes its status when the clock has moved 100 ms, at the top of its loop.
            set_virtual_now(t0 + static_cast<std::int64_t>(cumulative * 1'000'000ULL / per_sample) + 2'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        out.status = player.status();
        player.stop();
    }
    out.samples = sink.samples();
    out.play_us = sink.play_us();
    out.writes = sink.writes();
    return out;
}

}  // namespace fwhost
