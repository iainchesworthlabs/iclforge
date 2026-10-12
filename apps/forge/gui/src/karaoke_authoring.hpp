#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"

// What the Karaoke flow decides from the parts an author supplies, Qt-free so
// iclforge-forge-gui-tests holds it (planning/karaoke-design.md, K6; planning/
// karaoke.md, A2, A3 and A8).
//
// A karaoke stream (ETSI TS 102 366 Annex C, Table C.2.1) is a main service
// with bsmod 7 and a coding mode above 1/0 whose channels are not speakers but
// parts:
//
//   3/0  L M R          the backing, and a guide melody where a centre would be
//   2/1  L R V1         one vocal where a surround would be
//   3/1  L M R V1
//   2/2  L R V1 V2      two vocals where the surrounds would be
//   3/2  L M R V1 V2
//
// The author's files are the backing (two channels, L and R), an optional
// melody guide (M, one channel) and one or two vocals (V1, V2, one channel
// each); the coding mode follows from which of them there are. An optional
// LFE part (one channel) is the stream's low-frequency effects channel: it
// does not choose the mode, and the encoder band-limits it as it does any LFE.
// Nothing is
// fixed silently: a part that cannot be used, or that is not what its place
// asks, is a named problem, and a mismatch the encoder will absorb (a rate to
// resample, a length to pad) is a named warning.
//
// Every message is a code and its arguments, not a sentence: the window
// translates, and a sentence built here could not be.

namespace forge_gui {

// The bit rate a karaoke stream of `mode` ("3/0", "2/1", "3/1", "2/2" or "3/2") starts at, in
// kbps: 384 for the five channels of 3/2, as the design sheet showed, and the same rate a
// channel (about 77 kbps) for the modes below it, rounded up to a rate Table 5.18 has. The
// encoder's own default is 192, which is stereo's. 192 for a mode this does not name.
[[nodiscard]] int karaoke_default_bitrate_kbps(const std::string& mode);

// The lyrics file beside `media_path`: the same name in the same folder with the extension `.lrc`
// and, where there is none, `.srt`, `.ass` or `.kar`, matched without regard to case, which is where
// Hearth looks for it (planning/karaoke.md, L1; iclforge::lyrics::find_lyrics_file). Empty when
// there is none.
[[nodiscard]] std::string karaoke_lyrics_beside(const std::string& media_path);

// What became of a lyrics file offered beside a saved stream.
enum class KaraokeLyricsCopy : std::uint8_t {
    // Written beside the stream, under the stream's name with `.lrc`, as UTF-8 LRC whatever the
    // file offered was (iclforge::lyrics::place_lyrics_beside).
    kCopied,
    // It was already there: the file offered is that file.
    kAlreadyThere,
    // A lyrics file of that name was already beside the stream, and was left as it was.
    kExists,
    // The file offered could not be read, or is not lyrics (a code page is named, not guessed at).
    kUnreadable,
    // The write was refused (a read-only folder, a full disk).
    kFailed,
};

// Puts `lyrics_path` beside `stream_path` as the stream's own lyrics file, so that the player finds
// it by name: whichever of `.lrc`, `.srt`, `.ass` and `.kar` it is, written as the one format a
// player reads (UTF-8 LRC, with a duet's singers and the word times the file has). An existing
// lyrics file there is never overwritten. `error`, when given, is set to the reason for
// kUnreadable and kFailed.
[[nodiscard]] KaraokeLyricsCopy karaoke_copy_lyrics(const std::string& lyrics_path,
                                                    const std::string& stream_path,
                                                    std::string* error = nullptr);

// Why `lyrics_path` cannot be used as lyrics, as a sentence for the page, or "" when it can: it is
// read the way karaoke_copy_lyrics reads it, so that a file in a code page is said so when it is
// chosen and not when the stream is saved.
[[nodiscard]] std::string karaoke_lyrics_problem(const std::string& lyrics_path);

enum class KaraokePart : std::uint8_t { kBacking, kMelody, kVocal1, kVocal2, kLfe };

// One file the author supplied, for one part.
struct KaraokeFile {
    KaraokePart part = KaraokePart::kBacking;
    // As the author knows it ("lead.wav"), for the messages.
    std::string name;
    std::size_t channels = 0;
    std::uint32_t sample_rate = 0;
    double seconds = 0.0;
};

// A single multichannel file that names its own speakers (its WAVE_FORMAT_
// EXTENSIBLE mask), the alternative to separate parts.
struct KaraokeSpeakerFile {
    std::string name;
    std::size_t channels = 0;
    std::uint32_t sample_rate = 0;
    double seconds = 0.0;
    // Table E2.5's short names ("L", "C", "R", "Ls", "Rs", "Cs", "LFE") in
    // channel order; empty when the file does not say.
    std::vector<std::string> speakers;
};

enum class KaraokeCheckLevel : std::uint8_t { kOk, kWarning, kProblem };

// "no-backing", "backing-not-stereo", "backing-is-a-bed", "part-not-mono",
// "vocal2-needs-vocal1", "nothing-to-sing", "length-differs", "rate-differs",
// "parts-aligned", "each-part-one-place", "lfe-part", "no-file",
// "speakers-unnamed", "speakers-not-karaoke". `args` are what the sentence names:
// file names, counts, durations as m:ss and rates as kHz.
struct KaraokeCheck {
    KaraokeCheckLevel level = KaraokeCheckLevel::kOk;
    std::string code;
    std::vector<std::string> args;
};

// One coded channel of the stream and where it comes from.
struct KaraokeChannel {
    // The bed position the channel takes - the token EncoderController::
    // setAssignment takes: "L", "C", "R", "Cs" (the one surround of 2/1 and
    // 3/1), "Ls", "Rs" or "LFE".
    std::string position;
    // Table C.2.1's name for what it carries: "L", "M", "R", "V1" or "V2", and
    // "LFE" for the low-frequency channel, which is no part of the table.
    std::string part;
    // The index into the files the plan was made from (always 0 for a
    // multichannel file) and the channel within it.
    std::size_t file = 0;
    std::size_t channel = 0;
};

struct KaraokePlan {
    // Whether a stream can be made: a mode was found and no check is a problem.
    bool buildable = false;
    // The mode, where one was found; meaningful only when `mode` is not empty.
    iclforge::ac3::Acmod acmod = iclforge::ac3::Acmod::k2_0;
    // "3/2", as the encoder's beds are named; empty when none was found.
    std::string mode;
    // What the mode was chosen from.
    bool melody = false;
    int vocals = 0;
    // Whether the stream carries an LFE channel (an optional part of its own).
    bool lfe = false;
    // The coded channels in coded order (L, C, R, then the surround or
    // surrounds, then the LFE); empty when no mode was found.
    std::vector<KaraokeChannel> channels;
    std::vector<KaraokeCheck> checks;

    [[nodiscard]] bool has_problem() const;
};

// The plan for separate parts: `files` holds at most one file for each part.
[[nodiscard]] KaraokePlan plan_karaoke_parts(std::span<const KaraokeFile> files);

// The plan for one file whose speakers name its channels.
[[nodiscard]] KaraokePlan plan_karaoke_speakers(const KaraokeSpeakerFile& file);

// "3:42" for 222.4 seconds, rounded down as the rest of the window prints a
// length.
[[nodiscard]] std::string karaoke_duration_text(double seconds);

// How a karaoke stream is heard (Annex C): the channels as coded, the
// karaoke-aware decoder's reproduction (Table C.2.2) or the karaoke-capable
// one's, with the listener's choice of vocals (Table C.2.3).
enum class KaraokeListening : std::uint8_t { kAsCoded, kAware, kCapable };
enum class KaraokeVocalsChoice : std::uint8_t { kNone, kV1, kV2, kBoth };

// Which parts of the stream reach which speaker of a 5.1 room (L, C, R, Ls,
// Rs), by Table C.2.1's names, for a stream with a melody guide or not and
// with `vocals` (0 to 2) vocals. `folded` is the Lo/Ro stereo fold - L and R
// only, the 2/0 columns of Tables C.2.2 and C.2.3 - which plays every listening
// as the aware column when it is asked for as coded. The page shows this under
// the Hear step, and the tests hold it to what the library's decoder actually
// does.
struct KaraokeOutputs {
    std::vector<std::string> l;
    std::vector<std::string> c;
    std::vector<std::string> r;
    std::vector<std::string> ls;
    std::vector<std::string> rs;
    // The room's LFE: the stream's LFE channel, which every listening passes
    // (a karaoke reproduction is L, C, R and then the LFE) and a stereo fold
    // leaves out unless it is asked to mix it, which the page does not.
    std::vector<std::string> lfe;
};

[[nodiscard]] KaraokeOutputs karaoke_outputs(bool melody, int vocals, KaraokeListening listening,
                                             KaraokeVocalsChoice choice, bool folded = false,
                                             bool lfe = false);

}  // namespace forge_gui
