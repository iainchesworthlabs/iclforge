// A singer's pitch for the browser decode demo (planning/karaoke-microphone.md, M7; D16): the same
// tracker, reference melody and scorer Hearth uses (libs/pitch), compiled into the demo's module so
// that the page follows a microphone with the one implementation and not a second one in
// JavaScript.
//
//   new PitchTracker(inputRate)        tracker.push(Float32Array) -> flat frames, tracker.reset()
//   new ReferenceBuilder(rate, total)  builder.push(Float32Array) ... builder.finish() -> contour
//   contour.notes()                    the melody as flat start, end, MIDI note number triples
//   new PitchScorer(contour, cents)    scorer.add(itemMs, hz), .total(), .tally(from, to), .lagMs()
//
// Nothing here knows a clock or a microphone: the page says where in the item each frame of the
// singer's voice was sung (the Web Audio clock, the output latency, the microphone's own), which is
// the one decision about time. The tracker is standard library only; the page's worklet hands it
// 128-sample blocks, and the reference is read from the guide melody's channel in slices so that
// the page stays alive while it is.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/scorer.hpp"
#include "iclforge/pitch/tracker.hpp"

namespace {

namespace pit = iclforge::pitch;

// A Float32Array of the page's, copied into the module's memory once: `set` on a view of a vector.
std::vector<float> floats_of(const emscripten::val& js) {
    const auto length = js["length"].as<std::size_t>();
    std::vector<float> out(length);
    if (length > 0) {
        emscripten::val view(emscripten::typed_memory_view(length, out.data()));
        view.call<void>("set", js);
    }
    return out;
}

class PitchTracker {
   public:
    explicit PitchTracker(double input_rate) {
        const auto rate = static_cast<std::uint32_t>(std::max(0.0, std::round(input_rate)));
        auto made = pit::Tracker::make(pit::TrackerConfig::for_input_rate(rate));
        if (made) {
            hop_ms_ = static_cast<double>(made->config().hop_ms);
            tracker_ = std::move(*made);
        }
    }

    // False when the input rate cannot be tracked: zero, or one the analysis rate does not divide.
    // The rates a browser's audio runs at (44.1, 48, 96 kHz) are all followed.
    bool valid() const { return tracker_.has_value(); }
    double hopMs() const { return hop_ms_; }

    // Four numbers a frame, flat: the frame's centre sample in the stream pushed, the pitch in Hz
    // (0 when not voiced), the clarity and the level in dBFS.
    emscripten::val push(const emscripten::val& samples) {
        auto out = emscripten::val::array();
        if (!tracker_) {
            return out;
        }
        const std::vector<float> block = floats_of(samples);
        frames_.clear();
        tracker_->push(block, frames_);
        std::size_t at = 0;
        for (const pit::Frame& frame : frames_) {
            out.set(at++, static_cast<double>(frame.centre_sample));
            out.set(at++, static_cast<double>(frame.hz));
            out.set(at++, static_cast<double>(frame.clarity));
            out.set(at++, static_cast<double>(frame.level_db));
        }
        return out;
    }

    void reset() {
        if (tracker_) {
            tracker_->reset();
        }
    }

   private:
    std::optional<pit::Tracker> tracker_;
    std::vector<pit::Frame> frames_;
    double hop_ms_ = 10.0;
};

class ReferenceContour {
   public:
    explicit ReferenceContour(pit::Contour contour) : contour_(std::move(contour)) {}

    int hopMs() const { return static_cast<int>(contour_.hop_ms); }
    int length() const { return static_cast<int>(contour_.hz.size()); }
    int voicedFrames() const { return static_cast<int>(contour_.voiced_frames()); }
    double durationMs() const { return static_cast<double>(contour_.duration_ms()); }

    // The tune's pitch at a position, in Hz; 0 where it has no note.
    double hzAt(double ms) const {
        if (contour_.hop_ms == 0 || ms < 0.0) {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(
            (static_cast<std::int64_t>(ms) + static_cast<std::int64_t>(contour_.hop_ms) / 2) /
            static_cast<std::int64_t>(contour_.hop_ms));
        return index < contour_.hz.size() ? static_cast<double>(contour_.hz[index]) : 0.0;
    }

    // The melody as notes, flat: start ms, end ms, MIDI note number.
    emscripten::val notes() const {
        auto out = emscripten::val::array();
        std::size_t at = 0;
        for (const pit::Note& note : pit::segment_notes(contour_)) {
            out.set(at++, static_cast<double>(note.start_ms));
            out.set(at++, static_cast<double>(note.end_ms));
            out.set(at++, note.midi);
        }
        return out;
    }

    const pit::Contour& contour() const { return contour_; }

   private:
    pit::Contour contour_;
};

// The reference read from one channel of audio, a slice at a time. `total_samples` is the length
// of the channel, which sizes the grid; the result is the one pit::contour_of_signal() gives.
class ReferenceBuilder {
   public:
    ReferenceBuilder(double sample_rate, double total_samples)
        : rate_(static_cast<std::uint32_t>(std::max(0.0, std::round(sample_rate)))) {
        pit::ContourOptions options;
        pit::TrackerConfig config = pit::TrackerConfig::for_input_rate(rate_);
        config.hop_ms = options.hop_ms;
        config.min_hz = options.min_hz;
        config.max_hz = options.max_hz;
        config.clarity_min = options.clarity_min;
        config.level_floor_db = options.level_floor_db;
        contour_.hop_ms = options.hop_ms;
        auto made = pit::Tracker::make(config);
        if (made && rate_ > 0 && total_samples > 0.0) {
            tracker_ = std::move(*made);
            const auto total_ms =
                static_cast<std::int64_t>(total_samples) * 1000 / static_cast<std::int64_t>(rate_);
            contour_.hz.assign(static_cast<std::size_t>(total_ms / options.hop_ms) + 1U, 0.0F);
        }
    }

    bool valid() const { return tracker_.has_value(); }

    void push(const emscripten::val& samples) {
        if (!tracker_) {
            return;
        }
        const std::vector<float> block = floats_of(samples);
        frames_.clear();
        tracker_->push(block, frames_);
        for (const pit::Frame& frame : frames_) {
            if (!frame.voiced()) {
                continue;
            }
            const std::int64_t ms = frame.centre_sample * 1000 / static_cast<std::int64_t>(rate_);
            const auto hop = static_cast<std::int64_t>(contour_.hop_ms);
            const std::int64_t index = (ms + hop / 2) / hop;
            if (index >= 0 && static_cast<std::size_t>(index) < contour_.hz.size()) {
                contour_.hz[static_cast<std::size_t>(index)] = frame.hz;
            }
        }
    }

    // The contour; the builder is spent.
    ReferenceContour finish() { return ReferenceContour(std::move(contour_)); }

   private:
    std::uint32_t rate_;
    std::optional<pit::Tracker> tracker_;
    pit::Contour contour_;
    std::vector<pit::Frame> frames_;
};

class PitchScorer {
   public:
    PitchScorer(const ReferenceContour& reference, double tolerance_cents)
        : scorer_(reference.contour(), options_of(tolerance_cents)) {}

    // The singer's pitch at a position in the item; hz 0 is "not voiced".
    void add(double item_ms, double hz) {
        scorer_.add(static_cast<std::int64_t>(std::llround(item_ms)), static_cast<float>(hz));
    }
    void clear() { scorer_.clear(); }

    emscripten::val total() const { return describe(scorer_.total()); }
    emscripten::val tally(double start_ms, double end_ms) const {
        return describe(scorer_.tally(static_cast<std::int64_t>(std::llround(start_ms)),
                                      static_cast<std::int64_t>(std::llround(end_ms))));
    }

    // How early (negative) or late (positive) the singer is, in ms; null until there is enough.
    emscripten::val lagMs() const {
        const auto lag = scorer_.best_lag_ms();
        return lag ? emscripten::val(static_cast<double>(*lag)) : emscripten::val::null();
    }

   private:
    static pit::ScoreOptions options_of(double tolerance_cents) {
        pit::ScoreOptions options;
        options.tolerance_cents = tolerance_cents > 0.0 ? tolerance_cents : 100.0;
        return options;
    }

    static emscripten::val describe(const pit::Tally& tally) {
        auto out = emscripten::val::object();
        out.set("counted", static_cast<double>(tally.counted));
        out.set("sung", static_cast<double>(tally.sung));
        out.set("hit", static_cast<double>(tally.hit));
        out.set("scored", tally.scored());
        out.set("fraction", tally.fraction());
        out.set("coverage", tally.coverage());
        out.set("meanCents", tally.mean_cents);
        return out;
    }

    pit::Scorer scorer_;
};

}  // namespace

EMSCRIPTEN_BINDINGS(iclforge_wasm_pitch) {
    emscripten::class_<PitchTracker>("PitchTracker")
        .constructor<double>()
        .function("valid", &PitchTracker::valid)
        .function("hopMs", &PitchTracker::hopMs)
        .function("push", &PitchTracker::push)
        .function("reset", &PitchTracker::reset);
    emscripten::class_<ReferenceContour>("ReferenceContour")
        .function("hopMs", &ReferenceContour::hopMs)
        .function("length", &ReferenceContour::length)
        .function("voicedFrames", &ReferenceContour::voicedFrames)
        .function("durationMs", &ReferenceContour::durationMs)
        .function("hzAt", &ReferenceContour::hzAt)
        .function("notes", &ReferenceContour::notes);
    emscripten::class_<ReferenceBuilder>("ReferenceBuilder")
        .constructor<double, double>()
        .function("valid", &ReferenceBuilder::valid)
        .function("push", &ReferenceBuilder::push)
        .function("finish", &ReferenceBuilder::finish);
    emscripten::class_<PitchScorer>("PitchScorer")
        .constructor<const ReferenceContour&, double>()
        .function("add", &PitchScorer::add)
        .function("clear", &PitchScorer::clear)
        .function("total", &PitchScorer::total)
        .function("tally", &PitchScorer::tally)
        .function("lagMs", &PitchScorer::lagMs);
}
