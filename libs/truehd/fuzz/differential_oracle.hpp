#pragma once

// The FFmpeg oracle of fuzz_differential_truehd_decode: the same bytes decoded by this library and
// by FFmpeg's strict decoder, and the samples held equal when both decoded them. Modelled on
// libs/ac3/fuzz/differential_oracle.hpp, with the one difference that matters: TrueHD and MLP are
// lossless, so where that header measures an agreement in decibels against a floor it calibrated,
// this one compares samples for equality, and there is no floor to calibrate.
//
// --- What counts as a divergence ----------------------------------------------------------------
//
// The rule is written here so that a finding can be read against it. A divergence is reported
// (the process aborts, so that libFuzzer saves and minimises the input like any crash) exactly
// when both of these hold and the samples differ:
//
//   1. THE LIBRARY DECODED THE INPUT CLEANLY (decode_ours). Its elementary reader read every byte
//      of the input as access units, none skipped and none cut off; the decoder, strict (a failed
//      check nibble, CRC, parity byte or lossless check is an error), returned samples for every
//      unit, none of them an error and none of them "waiting for a restart header"; its
//      counters report no failed check and no failed lossless check; every unit has the sample
//      rate and channel count of the first; and every major sync has the first's kind, format
//      word and substream count. The last is not a check of the stream: a major sync that
//      changes any of them starts a new stream, the library then forgets the codec state, the
//      filter histories included, and FFmpeg's decoder goes on with them (the third finding of
//      this harness, a changed channel assignment word; libs/truehd/ERRATA.md question 42).
//      A unit that fails any of this was refused by the library, and what FFmpeg does with it
//      proves nothing about which of the two is right.
//
//   2. FFMPEG DECODED THE INPUT CLEANLY (ffmpeg_strict_decode). It exited 0, with the invocation
//      the repository uses everywhere as its oracle (`-xerror -err_detect
//      crccheck+bitstream+buffer+explode`, tools/checks/verify_truehd_interop.py), and wrote no
//      line to its standard error at `-v warning`. The second half is the point of reading the
//      stream at all: FFmpeg does not stop on a failed lossless check, it logs "Lossless check
//      failed - expected 00, calculated 8d" as a warning, exits 0 and writes the samples
//      (libs/truehd/ERRATA.md, entry 20), and `-xerror` does not change that. Samples written
//      beside a failed check are not what the stream encodes, so any warning at all takes the
//      case out of the comparison: not only that one, because the text of FFmpeg's other
//      warnings is not something this repository has read and one it did not anticipate must
//      not become a false finding. Anything on the benign list below is the exception.
//
// When both hold, the codec is lossless and nothing is left to interpret: the two must agree on
// the sample rate, the channel count, the number of samples and every sample, at the word length
// FFmpeg writes. That is the stream's: an MLP stream whose major sync declares 16-bit words
// (group 1's word length code 0) comes out of FFmpeg as 16-bit samples (ffprobe says s16 and
// bits_per_raw_sample 16), every other stream, TrueHD at any rate and MLP at 20 or 24 bits, as 24
// (s32 and 24). The library returns the 24-bit value the stream decodes to, whatever the declared
// width. A stream that is what it declares never shows the difference, because a 16-bit stream's
// samples have eight zero bits below them, but a mutation of the audio of one does: the first
// finding of this harness was a 16-bit MLP stream whose residual change left bit 7 of a
// channel set, which FFmpeg dropped (80256 against 80128, in a stream that declared 16 bits and
// carried 17). So for an MLP stream of 16-bit words the library's samples are compared with the
// eight bits below the declared width cleared; libs/truehd/ERRATA.md question 40 asks whether the
// library should do that itself. FFmpeg returns the
// channels of a 7 or 8 channel stream in its own order (verify_truehd_interop.py puts them back),
// so a channel-for-channel mismatch is then retried with each decoder's channels sorted by their
// samples, and only a difference that remains is a divergence: which channel is which is a
// question of layout, not of what a lossless decode returns, and no input has two decoders
// agreeing on every sample of a different set of channels.
//
// Everything else is not comparable and is skipped without a word: FFmpeg's exit status is not 0,
// it warns, it writes nothing, it does not finish in a few seconds, or the two disagree about
// what is in the stream at all: if FFmpeg decodes a stream the library reads whole and finds a
// different sample rate or channel count, that is "not comparable" (counted in measure mode,
// ICLFORGE_DIFF_MEASURE_ONLY, where it can be read), because two decoders that disagree on the
// shape of a stream both accepted have a framing question that this oracle does not decide. With
// the same rate and channel count, a different number of samples is a divergence like a different
// sample.
//
// Two environment variables change the behaviour for a human investigating an input; neither is
// set by fuzz.yml or by a normal tools/fuzz/run.sh:
//   ICLFORGE_DIFF_MEASURE_ONLY  prints the outcome of every input and never aborts.
//   ICLFORGE_DIFF_DEBUG         keeps the scratch files and prints their paths.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

extern char** environ;

namespace iclforge::truehd_fuzzdiff {

// An input above this is not offered to FFmpeg: the cost of a process is the same, the cost of the
// comparison grows, and a stream this long adds nothing a shorter one does not reach.
inline constexpr std::size_t kMaxInputBytes = 128 * 1024;
// libFuzzer's -timeout (tools/fuzz/run.sh) is 10 s, FFmpeg is given less so that a slow oracle is
// "not comparable" and never a hang finding.
inline constexpr int kFfmpegTimeoutMs = 6000;

// What FFmpeg prints at -v warning on a stream it decoded cleanly. None is known; a line added
// here is a decision to read that warning as harmless, and needs the evidence in its comment.
inline constexpr std::array<std::string_view, 0> kBenignWarnings{};

// A decode: interleaved, `channels` per sample, each value a 24-bit sample sign-extended into the
// int32 (the library's own form; FFmpeg's 32-bit words are shifted right by 8 to match).
struct Pcm {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    // The stream's major sync is MLP's, which FFmpeg reads as "mlp", not "truehd".
    bool mlp = false;
    // The width FFmpeg writes the samples at (see the rule at the top of this file).
    int word_bits = 24;
    std::vector<std::int32_t> samples;
};

// /dev/shm when the container provides it, /tmp otherwise (the convention fuzz_wav_read and the
// AC-3 oracle use).
inline const char* scratch_dir() {
    static const char* const dir = (::access("/dev/shm", W_OK) == 0) ? "/dev/shm" : "/tmp";
    return dir;
}

// A fresh path ending in `suffix`: FFmpeg wants a real file name, as everywhere else in the
// repository. Empty on failure.
inline std::string make_scratch_path(const char* suffix) {
    const std::string pattern = std::string(scratch_dir()) + "/iclforge-thd-diff-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        return {};
    }
    ::close(fd);
    const std::string base(buf.data());
    const std::string path = base + suffix;
    if (::rename(base.c_str(), path.c_str()) != 0) {
        ::unlink(base.c_str());
        return {};
    }
    return path;
}

inline bool write_file(const std::string& path, std::span<const std::byte> bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

inline std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return bytes;
    }
    std::uint8_t chunk[4096];
    while (const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file)) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    return bytes;
}

// --- The library's side (rule 1) ---

inline std::optional<Pcm> decode_ours(std::span<const std::byte> bytes) {
    using namespace iclforge::truehd;
    ElementaryReader reader(bytes);
    Decoder decoder;  // strict, the best presentation: what `forge decode` does
    Pcm pcm;
    MajorSync first_sync;
    std::size_t units = 0;
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        if (!decoded || !decoded->has_value()) {
            return std::nullopt;  // an error, or no restart header yet
        }
        const DecodedAccessUnit& out = **decoded;
        if (out.channels <= 0 || out.samples < 0) {
            return std::nullopt;
        }
        const MajorSync& sync = *decoder.major_sync();
        if (units == 0) {
            pcm.sample_rate = out.sample_rate;
            pcm.channels = out.channels;
            first_sync = sync;
        } else if (out.sample_rate != pcm.sample_rate || out.channels != pcm.channels) {
            return std::nullopt;  // the format changed: one raw PCM stream cannot say so
        } else if (sync.kind != first_sync.kind || sync.format_info != first_sync.format_info ||
                   sync.num_substreams != first_sync.num_substreams) {
            return std::nullopt;  // a new stream, which the two decoders treat differently
        }
        pcm.samples.insert(pcm.samples.end(), out.pcm.begin(), out.pcm.end());
        ++units;
    }
    const DecoderCounters& counters = decoder.counters();
    pcm.mlp = decoder.major_sync() && decoder.major_sync()->kind == StreamKind::kMlp;
    if (pcm.mlp && mlp_word_bits(decoder.major_sync()->group1_bits_code()) == 16) {
        pcm.word_bits = 16;
    }
    if (units == 0 || reader.skipped_bytes() != 0 || reader.offset() != bytes.size() ||
        counters.check_failures != 0 || counters.lossless_check_failures != 0 ||
        counters.units_waiting != 0 || counters.units_decoded != units) {
        return std::nullopt;
    }
    return pcm;
}

// --- FFmpeg's side (rule 2) ---

struct FfmpegRun {
    bool clean = false;
    std::string notes;  // why it is not clean, for measure mode
    Pcm pcm;
};

// The channels, the rate and the 32-bit samples of the WAV FFmpeg wrote, shifted down to the
// library's 24-bit form. False for anything that is not a PCM WAV of 32-bit words.
inline bool parse_wav(const std::vector<std::uint8_t>& wav, Pcm& out) {
    const auto u16 = [&](std::size_t at) {
        return static_cast<unsigned>(wav[at]) | (static_cast<unsigned>(wav[at + 1]) << 8U);
    };
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(u16(at)) |
               (static_cast<std::uint32_t>(u16(at + 2)) << 16U);
    };
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
        std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_format = false;
    std::size_t at = 12;
    while (at + 8 <= wav.size()) {
        const std::size_t size = u32(at + 4);
        const std::size_t body = at + 8;
        if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && body + 16 <= wav.size()) {
            out.channels = static_cast<int>(u16(body + 2));
            out.sample_rate = u32(body + 4);
            have_format = u16(body + 14) == 32;
        } else if (std::memcmp(wav.data() + at, "data", 4) == 0) {
            const std::size_t available = std::min(size, wav.size() - body);
            if (!have_format || out.channels <= 0) {
                return false;
            }
            out.samples.clear();
            for (std::size_t i = 0; i + 4 <= available; i += 4) {
                out.samples.push_back(static_cast<std::int32_t>(u32(body + i)) >> 8);
            }
            return true;
        }
        at = body + size + (size & 1U);
    }
    return false;
}

inline bool is_benign(std::string_view line) {
    for (const std::string_view benign : kBenignWarnings) {
        if (line.find(benign) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// FFmpeg's strict decode of `coded_path` (the elementary stream, `format` "truehd" or "mlp") to a
// 32-bit WAV. posix_spawn rather than fork: this process is sanitizer-instrumented and a fork of it
// is weight the fuzzer should not pay per input. Standard error goes to a file the caller reads.
inline FfmpegRun ffmpeg_strict_decode(const std::string& coded_path, const char* format,
                                      const std::string& wav_path, const std::string& log_path) {
    FfmpegRun run;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log_path.c_str(), O_WRONLY | O_TRUNC,
                                     0600);

    // argv entries are char*, not const char*: copy what is not a literal.
    std::vector<std::string> args = {
        "ffmpeg", "-hide_banner", "-v",          "warning",
        "-y",     "-xerror",      "-err_detect", "crccheck+bitstream+buffer+explode",
        "-f",     format,         "-i",          coded_path,
        "-c:a",   "pcm_s32le",    wav_path};
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    ::pid_t pid = 0;
    const int spawned = ::posix_spawnp(&pid, "ffmpeg", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        run.notes = "ffmpeg not started";
        return run;
    }
    int status = 0;
    bool exited = false;
    for (int waited = 0; waited < kFfmpegTimeoutMs; ++waited) {
        const ::pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            exited = true;
            break;
        }
        if (done < 0) {
            run.notes = "waitpid failed";
            return run;
        }
        ::timespec nap{0, 1000000};
        ::nanosleep(&nap, nullptr);
    }
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        run.notes = "ffmpeg timed out";
        return run;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        run.notes =
            "ffmpeg exit status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return run;
    }
    const std::vector<std::uint8_t> log = read_file(log_path);
    std::string_view text(reinterpret_cast<const char*>(log.data()), log.size());
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && !is_benign(line)) {
            run.notes = "ffmpeg warned: " + std::string(line);
            return run;
        }
    }
    if (!parse_wav(read_file(wav_path), run.pcm) || run.pcm.samples.empty()) {
        run.notes = "ffmpeg wrote no samples";
        return run;
    }
    run.clean = true;
    return run;
}

// --- The comparison ---

// Channel `c` of an interleaved decode.
inline std::vector<std::int32_t> channel_of(const Pcm& pcm, int c) {
    std::vector<std::int32_t> out;
    const std::size_t stride = static_cast<std::size_t>(pcm.channels);
    for (std::size_t i = static_cast<std::size_t>(c); i < pcm.samples.size(); i += stride) {
        out.push_back(pcm.samples[i]);
    }
    return out;
}

enum class Verdict : std::uint8_t {
    kEqual,
    kEqualInAnotherChannelOrder,
    kNotComparable,  // the two disagree about the shape of the stream
    kDiffer,
};

[[nodiscard]] inline Verdict compare(const Pcm& ours, const Pcm& theirs) {
    if (ours.sample_rate != theirs.sample_rate || ours.channels != theirs.channels ||
        ours.samples.size() % static_cast<std::size_t>(ours.channels) != 0) {
        return Verdict::kNotComparable;
    }
    // The library's samples at the width FFmpeg writes: the bits below it cleared.
    const std::int32_t keep = ~((std::int32_t{1} << (24 - ours.word_bits)) - 1);
    Pcm narrowed = ours;
    for (std::int32_t& sample : narrowed.samples) {
        sample &= keep;
    }
    if (narrowed.samples == theirs.samples) {
        return Verdict::kEqual;
    }
    if (ours.samples.size() != theirs.samples.size()) {
        return Verdict::kDiffer;
    }
    std::vector<std::vector<std::int32_t>> a;
    std::vector<std::vector<std::int32_t>> b;
    for (int c = 0; c < ours.channels; ++c) {
        a.push_back(channel_of(narrowed, c));
        b.push_back(channel_of(theirs, c));
    }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b ? Verdict::kEqualInAnotherChannelOrder : Verdict::kDiffer;
}

// A divergence has to become a process crash for libFuzzer to catch, minimise and save it like a
// memory-safety bug. std::abort(), not assert(): the fuzz build defines NDEBUG.
[[noreturn]] inline void report_divergence(const Pcm& ours, const Pcm& theirs) {
    std::fprintf(stderr,
                 "iclforge differential fuzzer: the TrueHD decode differs from FFmpeg's strict "
                 "decode of the SAME bytes, and both decoded them cleanly. The codec is "
                 "lossless, so one of them is wrong. library: %u Hz, %d channels, %zu samples; "
                 "FFmpeg: %u Hz, %d channels, %zu samples. See libs/truehd/fuzz/"
                 "differential_oracle.hpp for the rule and tools/fuzz/README.md for what to do.\n",
                 ours.sample_rate, ours.channels, ours.samples.size(), theirs.sample_rate,
                 theirs.channels, theirs.samples.size());
    std::fflush(stderr);
    std::abort();
}

// The whole pipeline for one input; returns quietly in every case but a divergence.
inline void run_differential(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxInputBytes || bytes.size() < 8) {
        return;
    }
    const std::optional<Pcm> ours = decode_ours(bytes);
    const bool measure = ::getenv("ICLFORGE_DIFF_MEASURE_ONLY") != nullptr;
    if (!ours) {
        if (measure) {
            std::fprintf(stderr, "ICLFORGE_DIFF_MEASURE_ONLY: library refused\n");
        }
        return;
    }
    const bool mlp = ours->mlp;
    const std::string coded = make_scratch_path(mlp ? ".mlp" : ".thd");
    const std::string wav = make_scratch_path(".wav");
    const std::string log = make_scratch_path(".log");
    FfmpegRun theirs;
    if (!coded.empty() && !wav.empty() && !log.empty() && write_file(coded, bytes)) {
        theirs = ffmpeg_strict_decode(coded, mlp ? "mlp" : "truehd", wav, log);
    }
    Verdict verdict = Verdict::kNotComparable;
    if (theirs.clean) {
        verdict = compare(*ours, theirs.pcm);
    }
    if (measure) {
        static const char* const names[] = {"equal", "equal in another channel order",
                                            "not comparable", "DIFFER"};
        std::fprintf(stderr, "ICLFORGE_DIFF_MEASURE_ONLY: library clean, ffmpeg %s%s%s: %s\n",
                     theirs.clean ? "clean" : "not clean", theirs.notes.empty() ? "" : " (",
                     theirs.notes.empty() ? "" : (theirs.notes + ")").c_str(),
                     names[static_cast<std::size_t>(verdict)]);
    }
    if (::getenv("ICLFORGE_DIFF_DEBUG") == nullptr) {
        for (const std::string& path : {coded, wav, log}) {
            if (!path.empty()) {
                ::unlink(path.c_str());
            }
        }
    } else {
        std::fprintf(stderr, "ICLFORGE_DIFF_DEBUG: coded=%s wav=%s log=%s\n", coded.c_str(),
                     wav.c_str(), log.c_str());
    }
    if (verdict == Verdict::kDiffer && !measure) {
        report_divergence(*ours, theirs.pcm);
    }
}

}  // namespace iclforge::truehd_fuzzdiff
