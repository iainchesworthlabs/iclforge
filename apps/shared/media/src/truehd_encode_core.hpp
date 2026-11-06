#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"

// The encode that `forge truehd-encode` and the Forge window's TrueHD encode share: a WAV's PCM,
// coded losslessly by iclforge::truehd::Encoder into a bare stream (apps/forge/cli/src/commands/
// truehd_encode.cpp wraps it in Matroska, MP4 or a transport stream; the window writes the bare
// stream). Nothing here prints; a problem comes back as a sentence the caller says its own way.
//
// Compiled into each application that uses it, like the rest of apps/shared/media/src: it needs
// iclforge::truehd and iclforge::ac3 (the WAV reader), which both applications link.

namespace iclforge::apps::truehd_encode {

// How the WAV's channels go into the stream: the channel assignment (0 for the encoder's default
// for the count) and, for stream channel k, the WAV channel that carries it.
struct Layout {
    std::uint32_t assignment = 0;
    std::vector<std::size_t> wav_index;
    std::vector<std::string> warnings;  // said, not fatal
};

// A file with no channel mask is taken in the order `decode` writes (the default layout of its
// count). One with a mask names its speakers, which are put into the order of the assignment they
// make; a mask that names speakers TrueHD has no channel for, or that is not one assignment, is an
// error (the sentence). A back pair is the surround pair when the file has no side pair, and the
// rear surround pair when it has both: this program's reading (libs/truehd/ERRATA.md, open
// question 16).
[[nodiscard]] std::expected<Layout, std::string> layout_of(const iclforge::ac3::io::WavData& wav);

// ---- several sources, named places ---------------------------------------------------------------
//
// `map=` for truehd-encode, the grammar of the other encoders with TrueHD's channel names for the
// places (planning/truehd-parity-forge.md, question 16, option A): `<source>.<channel>:<place>`,
// comma separated, source and channel counted from 0, the place one of L R C LFE Ls Rs Lvh Rvh Lc Rc
// Lrs Rrs Cs Ts Lsd Rsd Lw Rw Cvh LFE2 (any case). The places named have to make one channel
// assignment (TH spec §13.1: the pairs and singles of `libs/truehd`'s table); the stream's channels
// are then in that assignment's order, whatever the order of the map.

struct Placement {
    std::size_t source = 0;   // 0 is the positional input, 1 the first src=, and so on
    std::size_t channel = 0;  // of that source, from 0
    iclforge::truehd::Channel place{};
};

// The channel a name stands for, in any case; none for anything else.
[[nodiscard]] std::optional<iclforge::truehd::Channel> channel_named(std::string_view name);

// A map= value. The error is a sentence for the user.
[[nodiscard]] std::expected<std::vector<Placement>, std::string> parse_map(std::string_view spec);

struct Assembled {
    iclforge::ac3::io::WavData wav;  // the placed channels, in the stream's order
    Layout layout;                   // its assignment, and the identity order of `wav`
};

// Puts the placed channels of `sources` into one WAV in the stream's order. A source starts
// `offset_samples[i]` samples in (silence ahead of it) and a shorter source is padded with silence
// to the longest. Refused, with a sentence: a place named twice, a source or channel that is not
// there, sources of more than one sample rate, a source none of whose channels is placed, and a
// set of places that is not a channel assignment (the sentence then names the nearest sets).
[[nodiscard]] std::expected<Assembled, std::string> assemble(
    std::span<const iclforge::ac3::io::WavData> sources, std::span<const std::size_t> offset_samples,
    std::span<const Placement> placements);

// `samples` samples of silence ahead of every channel.
void pad_leading(iclforge::ac3::io::WavData& wav, std::size_t samples);


// ---- the encoder's settings ----------------------------------------------------------------------
//
// The `truehd-*` options of `forge truehd-encode` that shape the stream (not the Atmos-only ones),
// and the Forge window's Encode TrueHD options: one place for what each value may be and the
// sentence that says so, so that the window refuses what the command line does. The command line's
// own parser (apps/forge/cli/src/support.cpp) shares the limits below and the channel meaning
// options (set_meaning_option).

inline constexpr int kRestartMaxUnits = 4096;
inline constexpr std::uint32_t kPeakRateMaxKbps = 100000;
inline constexpr std::uint32_t kBitrateMinKbps = 1000;
inline constexpr std::uint32_t kBitrateMaxKbps = 100000;
inline constexpr int kMajorSyncMaxUnits = 65536;
inline constexpr int kFirOrderMax = 8;
inline constexpr double kDownmixGainFloorDb = -60.0;

struct Settings {
    bool standard_downmix = false;                       // truehd-downmix=standard
    std::optional<std::array<double, 4>> downmix_gains;  // linear: C, Ls/Rs, LFE, Lrs/Rrs
    std::optional<int> restart_units;                    // truehd-restart=
    std::optional<std::uint32_t> peak_rate_kbps;         // truehd-peak-rate=
    bool rate_lenient = false;                           // truehd-rate-check=lenient
    std::optional<std::uint32_t> bitrate_kbps;           // truehd-bitrate=
    std::optional<int> major_sync_units;                 // truehd-major-sync=
    std::optional<int> fir_order;                        // truehd-fir-order=
    bool matrix = true;                                  // truehd-matrix=
    unsigned stereo_mode = 0;                            // truehd-stereo-mode=: stereo, ltrt, headphone
    bool verify = false;                                 // verify
    // The channel meaning (TH spec §14.1): the fields the options name, the rest left as the
    // encoder has them (zero, as FFmpeg's streams have them). Provisional, no oracle until the DEE
    // set (libs/truehd/ERRATA.md, questions 51 to 55).
    iclforge::truehd::ChannelMeaningEdit meaning;
};

// ---- the channel meaning -------------------------------------------------------------------------
//
// One option for each field of each presentation (2, 6 and 8 channels), named after it, as `forge
// truehd-encode`, `truehd-atmos` and `metadata` take them: truehd-dialnorm-2ch=, -6ch=, -8ch=;
// truehd-mix-level-*; truehd-source-format-6ch=, -8ch=; truehd-control-* (on or off); and the two DRC
// start-up gains truehd-heavy-drc-start-up-gain= and truehd-drc-start-up-gain=. The ranges are the
// widths of the fields (iclforge::truehd::channel_meaning_problem). Provisional: the specification
// gives no units for most of them.

// The options' names after "truehd-", in the order the command line writes them.
[[nodiscard]] std::span<const std::string_view> meaning_option_names() noexcept;

// Whether `name` (after "truehd-") is one of them.
[[nodiscard]] bool is_meaning_option(std::string_view name) noexcept;

// Sets the field the option `name` names to `value`, as the command line reads it. The error is the
// sentence that says what the option takes, without the value it was given.
[[nodiscard]] std::expected<void, std::string> set_meaning_option(
    iclforge::truehd::ChannelMeaningEdit& edit, std::string_view name, std::string_view value);

// Sets the option `name` (the part after "truehd-", such as "restart") to `value`, as the command line
// reads it; the channel meaning options too. The error is the sentence that says what the option
// takes, without the value it was given: the caller adds that. A name that is not one of these is an
// error too.
[[nodiscard]] std::expected<void, std::string> set_option(Settings& settings, std::string_view name,
                                                          std::string_view value);

// The settings into `config`; the fields they leave alone keep what the caller set. A channel meaning
// the encoder cannot hold comes back as the sentence that names the field (set_option has refused
// those, so this is for a Settings filled in by hand); `config` is then complete but for the meaning.
[[nodiscard]] std::expected<void, std::string> apply(const Settings& settings,
                                                     iclforge::truehd::EncoderConfig& config);

struct Outcome {
    std::vector<std::byte> stream;
    std::uint64_t frames = 0;  // samples per channel
    std::uint64_t access_units = 0;
    int channels = 0;
    int substreams = 0;
    std::uint32_t sample_rate = 0;
    iclforge::truehd::EncoderStats stats{};
};

// Called after each chunk with the samples per channel coded so far and the total; false stops the
// encode (the result is then an error that says it was cancelled).
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

struct Failure {
    iclforge::truehd::EncodeError error;
    bool cancelled = false;  // the progress callback said stop; `error` is then not a fault
};

// Codes the whole WAV. `config` is the caller's, with the sample rate, the channel count, the bit
// depth and the channel assignment filled in here from the WAV and `layout`.
[[nodiscard]] std::expected<Outcome, Failure> encode_wav(
    const iclforge::ac3::io::WavData& wav, const Layout& layout,
    iclforge::truehd::EncoderConfig config, const Progress& progress = {});

}  // namespace iclforge::apps::truehd_encode
