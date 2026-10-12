#include "microphone_session.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iclforge::hearth {

namespace {

constexpr std::int64_t kStallNs = 3'000'000'000LL;
constexpr std::int64_t kClipHoldNs = 1'000'000'000LL;
// The meter falls this many dB a second.
constexpr double kMeterReleaseDbPerSecond = 40.0;

}  // namespace

MicrophoneSession::MicrophoneSession(std::unique_ptr<MicrophoneInput> input)
    : input_(std::move(input)) {}

PerformanceOptions MicrophoneSession::performance_options() const {
    PerformanceOptions options;
    options.input_rate = input_->sample_rate();
    options.input_delay_ms = input_delay_ms_;
    options.offset_ms = offset_ms_;
    options.score = score_options_;
    return options;
}

void MicrophoneSession::rebuild_performance() {
    performance_.reset();
    if (!open_ || !reference_) {
        return;
    }
    performance_ = Performance::make(*reference_, performance_options());
    if (performance_ && anchor_) {
        performance_->anchor(anchor_->position_ms, anchor_->at_ns, anchor_->speed);
    }
}

std::expected<void, std::string> MicrophoneSession::start(const std::string& device_id) {
    stop();
    auto started = input_->start(device_id);
    if (!started) {
        return started;
    }
    open_ = true;
    level_db_ = -120.0F;
    ever_clipped_ = false;
    silent_ = false;
    silent_seconds_ = 0.0;
    last_audio_ns_ = 0;
    timing_estimated_ = false;
    rebuild_performance();
    return {};
}

void MicrophoneSession::stop() {
    if (open_) {
        input_->stop();
    }
    open_ = false;
    performance_.reset();
    level_db_ = -120.0F;
    ever_clipped_ = false;
    silent_ = false;
    silent_seconds_ = 0.0;
}

void MicrophoneSession::set_reference(std::optional<pitch::Contour> reference) {
    reference_ = std::move(reference);
    rebuild_performance();
}

void MicrophoneSession::anchor(std::int64_t position_ms, bool playing, std::int64_t now_ns) {
    anchor_ = Anchor{position_ms, playing ? 1.0 : 0.0, now_ns};
    if (performance_) {
        performance_->anchor(position_ms, now_ns, playing ? 1.0 : 0.0);
    }
}

void MicrophoneSession::set_input_delay_ms(std::int64_t ms) {
    input_delay_ms_ = ms;
    if (performance_) {
        performance_->set_input_delay_ms(ms);
    }
}

void MicrophoneSession::set_offset_ms(std::int64_t ms) {
    offset_ms_ = ms;
    if (performance_) {
        performance_->set_offset_ms(ms);
    }
}

void MicrophoneSession::set_score_options(const pitch::ScoreOptions& options) {
    score_options_ = options;
    if (performance_) {
        performance_->set_score_options(options);
    }
}

MicrophoneHealth MicrophoneSession::pump(std::int64_t now_ns) {
    if (!open_) {
        return MicrophoneHealth::kNotOpen;
    }
    if (last_audio_ns_ == 0) {
        last_audio_ns_ = now_ns;  // the stall clock starts when the stream does
    }
    if (!input_->running()) {
        return MicrophoneHealth::kGone;
    }
    while (input_->read(block_)) {
        last_audio_ns_ = now_ns;
        timing_estimated_ = block_.time_estimated;
        const std::size_t channels = std::max<std::size_t>(input_->channels(), 1);
        const std::size_t frames = block_.samples.size() / channels;
        if (frames == 0) {
            continue;
        }
        // The level, and whether anything reached full scale.
        double energy = 0.0;
        float peak = 0.0F;
        bool any_nonzero = false;
        for (std::size_t i = 0; i < frames; ++i) {
            double mono = 0.0;
            for (std::size_t c = 0; c < channels; ++c) {
                mono += static_cast<double>(block_.samples[i * channels + c]);
            }
            mono /= static_cast<double>(channels);
            energy += mono * mono;
            peak = std::max(peak, static_cast<float>(std::fabs(mono)));
            any_nonzero = any_nonzero || mono != 0.0;
        }
        const double rms = std::sqrt(energy / static_cast<double>(frames));
        const float block_db = static_cast<float>(std::max(-120.0, 20.0 * std::log10(rms + 1e-9)));
        const double since_s =
            last_level_ns_ == 0 ? 0.0 : static_cast<double>(now_ns - last_level_ns_) / 1e9;
        last_level_ns_ = now_ns;
        // The meter falls at a fixed rate; the arithmetic is in double and narrowed once.
        const double released = static_cast<double>(level_db_) - kMeterReleaseDbPerSecond * since_s;
        level_db_ = std::max(block_db, static_cast<float>(released));
        if (peak >= 0.99F) {
            ever_clipped_ = true;
            clipped_at_ns_ = now_ns;
        }
        if (any_nonzero) {
            silent_ = false;
            silent_seconds_ = 0.0;
        } else {
            if (!silent_) {
                silent_ = true;
                silent_since_ns_ = now_ns;
            }
            silent_seconds_ = static_cast<double>(now_ns - silent_since_ns_) / 1e9;
        }
        if (performance_) {
            performance_->feed_interleaved(block_.samples, static_cast<std::uint16_t>(channels),
                                           block_.first_time_ns);
        }
    }
    if (now_ns - last_audio_ns_ > kStallNs) {
        return MicrophoneHealth::kStalled;
    }
    return MicrophoneHealth::kOk;
}

MicrophoneSnapshot MicrophoneSession::snapshot() const {
    MicrophoneSnapshot s;
    s.open = open_;
    s.level_db = level_db_;
    s.clipping = ever_clipped_ && last_level_ns_ - clipped_at_ns_ < kClipHoldNs;
    s.silent_seconds = silent_seconds_;
    s.headphones = headphones_;
    s.score_withheld = !headphones_;
    s.input_delay_ms = input_delay_ms_;
    s.offset_ms = offset_ms_;
    s.timing_estimated = timing_estimated_;
    s.period_frames = open_ ? input_->period_frames() : 0;
    s.sample_rate = open_ ? input_->sample_rate() : 0;
    if (performance_) {
        s.live = performance_->live();
        s.total = performance_->total();
        s.lag_ms = performance_->lag_ms();
        s.key_offset_cents = performance_->key_offset_cents();
        s.unplaced_frames = performance_->unplaced_frames();
    }
    return s;
}

}  // namespace iclforge::hearth
