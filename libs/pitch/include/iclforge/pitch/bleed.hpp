#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Is the microphone mostly the speakers? (planning/karaoke-microphone.md, SC5; findings F6.)
//
// A pitch score cannot tell a singer from a loud speaker playing the answer: a silent singer in
// front of speakers that play the guide melody scores near perfect, because the tracker follows
// whichever source is louder and the bleed is the reference. The program knows exactly what it
// played, so it can ask a question the tracker cannot: how much of this stretch of the microphone
// is what the room does to the played signal?
//
// The monitor is given the played signal and the microphone, both mono at one rate. It finds the
// delay between them, then fits the room as a short filter (a few milliseconds of taps, ridge
// regularised) over the last two seconds, and holds that filter fixed while it reads each frame:
// the share of the frame's energy that the filtered played signal predicts, 1 - |m - h*p|^2 /
// |m|^2. A frame the room predicts more than half of is *bleed*: the speakers contribute more than
// the person does, which is also where the tracker starts to follow the speakers, so the two
// boundaries are one.
//
// Why a frozen filter and not the best correlation within a few milliseconds of the delay: a
// singer who is in tune with the guide that is playing correlates with it just as well, because a
// small shift of lag matches any phase, so that every note sung right would look like bleed. The
// room is a single path that does not change from frame to frame; a voice is not predicted by it.
//
// What it does not do is cancel the echo. A frame flagged as bleed is a frame to give no credit
// for, and a person to tell that the speakers are in the microphone; it does not recover the voice.
//
// The delay is found by a coarse search (a quarter of the rate) over a window of the last few
// seconds, refined at the full rate, and searched again every half second together with the path,
// so that a device that changes its buffering is followed. Standard library only.

namespace iclforge::pitch {

struct BleedOptions {
    // The rate of both streams, which are mono; the caller brings them to it. 16 kHz is what the
    // tracker analyses at, and is far more than a delay and a correlation need.
    std::uint32_t sample_rate = 16000;
    // How far behind the played signal the microphone may be, in milliseconds: the output's and the
    // input's delay and the air. The microphone is never ahead of what was played by more than
    // `lead_ms`, which absorbs the error of the two clocks' stamps.
    std::uint32_t max_delay_ms = 400;
    std::uint32_t lead_ms = 20;
    // The frame the share is read over, and the distance between frames.
    std::uint32_t frame_ms = 64;
    std::uint32_t hop_ms = 10;
    // The window the delay and the path are fitted over, and how often.
    std::uint32_t delay_window_ms = 2000;
    std::uint32_t delay_update_ms = 500;
    // The room's path: how many taps (a millisecond at 16 kHz is 16), and the ridge as a share of
    // the mean power of the played signal. More taps follow a more reverberant room and are fooled
    // more by a voice that resembles what is playing.
    std::uint32_t path_taps = 16;
    float path_ridge = 0.01F;
    // A frame the room predicts more than this share of is bleed. Half is where the speakers
    // become the larger part of the microphone; a little more spares a singer who is quiet for a
    // moment, at the cost of some credit for a silent singer behind loud speakers (chosen on
    // simulated rooms; findings F9).
    float explained_min = 0.65F;
    // A frame quieter than this (RMS, dBFS) is silence: there is nothing to attribute to anyone.
    float level_floor_db = -60.0F;
};

// What the monitor answers for one frame of microphone.
struct BleedFrame {
    // The index, in the microphone samples pushed, of the frame's centre sample.
    std::int64_t centre_sample = 0;
    // The share of the frame's energy the room's path predicts from the played signal, 0 to 1; 0
    // while the delay is not known.
    float explained = 0.0F;
    // The level of the microphone in the frame (RMS, dBFS), floored at -120.
    float level_db = -120.0F;
    // The level of what the path predicts against the level of the played signal that fed it, in
    // dB: how loud the speakers are in the microphone; -120 when nothing is predicted.
    float gain_db = -120.0F;
    // The delay was known, so that `explained` is a measurement and not an absence.
    bool known = false;
    // The speakers are mostly what the microphone heard.
    bool bleed = false;
};

class BleedMonitor {
   public:
    explicit BleedMonitor(const BleedOptions& options = {});

    // The played signal, mono, contiguous from sample 0. The microphone's sample `j` is taken to be
    // heard at played sample `j + offset`, with the offset 0: a host that knows how the two clocks
    // relate shifts one stream, and the delay search absorbs the rest. Push the played signal
    // before the microphone samples that can contain it.
    void push_played(std::span<const float> samples);

    // The microphone, mono, contiguous from sample 0. Appends a frame to `out` for every hop
    // completed once a whole frame has been seen.
    void push_microphone(std::span<const float> samples, std::vector<BleedFrame>& out);

    // The delay found, in milliseconds: how far behind the played signal the microphone hears it.
    // Nothing until a delay with a correlation worth the name has been seen.
    [[nodiscard]] std::optional<double> delay_ms() const;

    // The share of the microphone's energy the path predicted over the window it was fitted on, 0
    // to 1: how much of what the microphone has heard lately is the speakers.
    [[nodiscard]] float recent_explained() const { return recent_explained_; }

    // Forgets both streams and the delay.
    void reset();

    [[nodiscard]] const BleedOptions& options() const { return options_; }

   private:
    // The samples are kept in rings addressed by their absolute index.
    struct Ring {
        std::vector<float> data;
        std::int64_t seen = 0;  // samples pushed so far
        void init(std::size_t capacity);
        void push(float v);
        [[nodiscard]] float at(std::int64_t index) const;  // 0 outside what is kept
        void clear();
    };

    void estimate_delay();
    void fit_path(std::int64_t start, std::size_t width);
    [[nodiscard]] BleedFrame analyse(std::int64_t end);

    BleedOptions options_;
    std::size_t frame_ = 0;
    std::size_t hop_ = 0;
    std::size_t max_delay_ = 0;
    std::size_t lead_ = 0;
    std::size_t window_ = 0;
    std::size_t update_ = 0;
    std::size_t taps_ = 0;

    Ring played_;
    Ring mic_;
    std::int64_t next_end_ = 0;          // the microphone index one past the next frame to analyse
    std::int64_t next_update_ = 0;       // the microphone index at which to search the delay again
    std::optional<std::int64_t> delay_;  // in samples, at the full rate
    std::int64_t path_start_ = 0;        // the lag of the path's first tap
    std::vector<double> path_;           // the taps
    float recent_explained_ = 0.0F;
    std::vector<float> coarse_mic_;
    std::vector<float> coarse_played_;
    std::vector<double> coarse_energy_;
};

}  // namespace iclforge::pitch
