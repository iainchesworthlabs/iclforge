#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A synthetic singing voice with a known pitch (planning/karaoke-microphone.md, Q1: no recording is
// bundled, so the tests' content is generated and the generator is the record of what is in it).
// A source-filter voice: harmonics whose amplitudes fall with the harmonic number, a vibrato,
// jitter, portamento between notes, three formants and breath noise, shaped by a 20 ms attack and
// release, with room noise at a chosen signal-to-noise ratio. Deterministic for a seed, and
// standard library only.
//
// It is a test signal, not a model of singers: it has no consonants, no sibilance and no children's
// voices, and says so wherever a result is quoted.

namespace iclforge::pitch {

struct VoiceNote {
    double start_s = 0.0;
    double duration_s = 0.0;
    // A fractional MIDI note number (69 is 440 Hz).
    double midi = 69.0;
};

struct VoiceOptions {
    std::uint32_t sample_rate = 16000;
    // Vibrato depth (peak, cents) and rate, ramped in over a quarter of a second of the stream.
    double vibrato_cents = 40.0;
    double vibrato_hz = 5.5;
    // The step of a random walk on the pitch (a fraction), high-passed so that it does not drift.
    double jitter = 0.004;
    // Breath noise level against the voiced signal (dB) and the glide between notes (seconds).
    double breath_db = -22.0;
    double portamento_s = 0.03;
    // Silence before the first note and after the last.
    double lead_s = 0.2;
    double tail_s = 0.3;
    // Room noise: signal-to-noise ratio against the voiced samples (dB); none when unset.
    std::optional<double> snr_db = std::nullopt;
    // 0 is an /a/, 1 an /i/ and 2 an /o/ (three formants each).
    std::uint32_t vowel = 0;
    // The peak amplitude of the result (before the noise is added).
    double amplitude = 0.5;
    std::uint32_t seed = 1;
};

struct Voice {
    std::vector<float> samples;
    // The pitch that was synthesised at each sample, in Hz; 0 between notes.
    std::vector<float> f0;
};

[[nodiscard]] Voice synthesize_voice(std::span<const VoiceNote> notes,
                                     const VoiceOptions& options = {});

// A melody for a test: `count` notes of 0.25 to 0.9 s with gaps of up to 0.4 s, wandering by up to
// four semitones a step inside [low_midi, high_midi]. Deterministic for a seed.
[[nodiscard]] std::vector<VoiceNote> random_melody(double low_midi, double high_midi,
                                                   std::uint32_t count, std::uint32_t seed = 3);

}  // namespace iclforge::pitch
