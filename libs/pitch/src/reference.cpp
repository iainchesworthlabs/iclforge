#include "iclforge/pitch/reference.hpp"

#include <algorithm>
#include <cmath>

namespace iclforge::pitch {

std::size_t Contour::voiced_frames() const {
    return static_cast<std::size_t>(
        std::count_if(hz.begin(), hz.end(), [](float v) { return v > 0.0F; }));
}

Contour contour_of_signal(std::span<const float> mono, std::uint32_t sample_rate,
                          const ContourOptions& options) {
    Contour contour;
    contour.hop_ms = options.hop_ms;
    if (mono.empty()) {
        return contour;
    }
    TrackerConfig config = TrackerConfig::for_input_rate(sample_rate);
    config.hop_ms = options.hop_ms;
    config.min_hz = options.min_hz;
    config.max_hz = options.max_hz;
    config.clarity_min = options.clarity_min;
    config.level_floor_db = options.level_floor_db;
    auto tracker = Tracker::make(config);
    if (!tracker || sample_rate == 0 || options.hop_ms == 0) {
        return contour;
    }
    const std::int64_t total_ms =
        static_cast<std::int64_t>(mono.size()) * 1000 / static_cast<std::int64_t>(sample_rate);
    const std::size_t grid =
        static_cast<std::size_t>(total_ms / static_cast<std::int64_t>(options.hop_ms)) + 1U;
    contour.hz.assign(grid, 0.0F);

    std::vector<Frame> frames;
    frames.reserve(512);
    constexpr std::size_t kChunk = 8192;
    for (std::size_t at = 0; at < mono.size(); at += kChunk) {
        frames.clear();
        tracker->push(mono.subspan(at, std::min(kChunk, mono.size() - at)), frames);
        for (const Frame& frame : frames) {
            if (!frame.voiced()) {
                continue;
            }
            const std::int64_t ms =
                frame.centre_sample * 1000 / static_cast<std::int64_t>(sample_rate);
            const std::int64_t index = (ms + static_cast<std::int64_t>(options.hop_ms) / 2) /
                                       static_cast<std::int64_t>(options.hop_ms);
            if (index >= 0 && static_cast<std::size_t>(index) < contour.hz.size()) {
                contour.hz[static_cast<std::size_t>(index)] = frame.hz;
            }
        }
    }
    return contour;
}

Contour contour_of_notes(std::span<const Note> notes, std::uint32_t hop_ms,
                         std::int64_t duration_ms) {
    Contour contour;
    contour.hop_ms = hop_ms == 0 ? 10U : hop_ms;
    std::int64_t end = duration_ms;
    for (const Note& note : notes) {
        end = std::max(end, note.end_ms);
    }
    if (end <= 0) {
        return contour;
    }
    const std::int64_t hop = static_cast<std::int64_t>(contour.hop_ms);
    contour.hz.assign(static_cast<std::size_t>(end / hop) + 1U, 0.0F);
    // Applied in start order, so that a later note replaces what it overlaps from its own start.
    std::vector<Note> ordered(notes.begin(), notes.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Note& a, const Note& b) { return a.start_ms < b.start_ms; });
    for (const Note& note : ordered) {
        if (note.end_ms <= note.start_ms) {
            continue;
        }
        const float hz = static_cast<float>(hz_from_midi(note.midi));
        const std::int64_t first = (std::max<std::int64_t>(note.start_ms, 0) + hop - 1) / hop;
        const std::int64_t last =
            (note.end_ms - 1) / hop;  // the last grid point before the note ends
        for (std::int64_t i = first; i <= last && static_cast<std::size_t>(i) < contour.hz.size();
             ++i) {
            contour.hz[static_cast<std::size_t>(i)] = hz;
        }
    }
    return contour;
}

std::vector<Note> segment_notes(const Contour& contour, const NoteOptions& options) {
    std::vector<Note> notes;
    if (contour.hz.empty() || contour.hop_ms == 0) {
        return notes;
    }
    const std::int64_t hop = static_cast<std::int64_t>(contour.hop_ms);
    const std::size_t bridge =
        static_cast<std::size_t>(std::max<std::int64_t>(options.bridge_ms / hop, 0));

    std::vector<double> run;  // the midi values of the note being built
    std::size_t run_first = 0;
    std::size_t run_last = 0;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const std::size_t mid = v.size() / 2;
        return v.size() % 2 == 1 ? v[mid] : 0.5 * (v[mid - 1U] + v[mid]);
    };
    auto close_run = [&]() {
        if (run.empty()) {
            return;
        }
        const std::int64_t start = static_cast<std::int64_t>(run_first) * hop;
        const std::int64_t end = static_cast<std::int64_t>(run_last + 1U) * hop;
        if (end - start >= options.min_ms) {
            notes.push_back({start, end, median(run)});
        }
        run.clear();
    };

    std::size_t gap = 0;
    for (std::size_t i = 0; i < contour.hz.size(); ++i) {
        const float hz = contour.hz[i];
        if (hz <= 0.0F) {
            ++gap;
            if (!run.empty() && gap > bridge) {
                close_run();
            }
            continue;
        }
        const double midi = midi_from_hz(static_cast<double>(hz));
        if (!run.empty()) {
            const double centre = median(run);
            if (std::fabs(midi - centre) * 100.0 > options.split_cents) {
                close_run();
            }
        }
        if (run.empty()) {
            run_first = i;
        }
        run.push_back(midi);
        run_last = i;
        gap = 0;
    }
    close_run();
    return notes;
}

}  // namespace iclforge::pitch
