// hearth-testserver: a Sendspin server for Hearth's board and CI tests
// (planning/hearth-reference-player.md, B3 and B4).
//
// It dials each player it is given, pairs it, sends it settings, plays one AC-3 or E-AC-3
// programme to all of them as a group over _iclforge_player@v1, and reports what each said: its
// counters, its decoder's findings and its levels, and for a board, what its GET /status said
// while it played - above all where it put the programme's first frame on the server's clock,
// which is how two boards are held to playing within a millisecond of each other.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/number_parse.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/handshake.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/json.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/server_host.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "server_store.hpp"
#include "sink.hpp"

// Last: on Windows it brings windows.h, whose macros (small, among others) would break the
// headers above.
#include <httplib.h>

namespace {

namespace fs = std::filesystem;
namespace ss = iclforge::sendspin;
namespace m = ss::messages;
namespace ac = ss::player;
namespace json = ss::json;
namespace testsink = iclforge::hearth::testsink;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

constexpr std::string_view kUsage =
    R"(usage: hearth-testserver [options] --play FILE --player URL [player options]...

A Sendspin server for Hearth's tests. It dials each player, pairs it, sends it
settings, plays one AC-3, E-AC-3 or AC-4 programme to all of them as a group over
_iclforge_player@v1, and writes a report of what each said about it.

  --player URL         a player to dial, such as ws://192.168.1.40:8928/sendspin;
                       the options below apply to the last one given
    --label TEXT       what the report calls it (default: the name it gives)
    --token SP:0...    its pairing token, from its console
    --code-log FILE    pair by the code it shows, read from the first
                       "PAIRING CODE" line written to FILE after asking
    --layout LAYOUT    settings with this speaker layout before the play
    --trim-db LIST     and these trims, one per output in dB, separated by
                       commas: -6,-6 plays a two-output board 6 dB down
    --status URL       its GET /status, read once a second while it plays,
                       such as http://192.168.1.40/status
    --hold             only connect, and stay connected unpaired, as a
                       second server such as Music Assistant does: it is not
                       paired or played to, and may be displaced
    --pcm              play it PCM over player@v1, not bursts over
                       _iclforge_player@v1: it is paired, then moved to the
                       role (needs --tone, and takes no --layout)
    --max-chunk-bytes N  with --pcm, at most N bytes of audio in a chunk to
                       it (a board takes a few KB a message); default 20 ms
    --opus             with --pcm, in Opus (20 ms packets) where the player
                       lists it, a twelfth of PCM's bytes and lossy

  --play FILE          the programme, an .ac3, .ec3, .ac4 or .thd file (a TrueHD stream is sent
                       as access units, 24 to a chunk; AC-4 is sent as one
                       sync frame a burst, at 48 kHz, and a player that lists no `ac4`
                       refuses it)
  --tone               instead of a file, two tones at 48 kHz stereo for
                       --seconds (default 10); PCM only, so every player
                       played takes --pcm
  --tone-bits N        the tone's depth, 16 or 24 (default 24); a player is
                       sent the first format it lists, rescaled
  --lyrics FILE        also send each player that takes lyrics (_iclforge_player@v1
                       lists the lyrics command) this .lrc document for the
                       programme, and the playhead as anchors every two seconds and
                       at each pass, as Hearth's engine sends them
  --title TEXT         also tell each player that lists metadata@v1 (the P4X
                       panel's title card) what is playing, as the specification's
                       whole server/state object: this title, with --artist,
                       --album, --year and --track-seconds (the song's length,
                       default the programme's) for the rest, and the position
                       from the play's start
  --artist TEXT        (see --title)
  --album TEXT         (see --title)
  --year N             (see --title)
  --track-seconds N    (see --title)
  --then-after N       N seconds into the play, say the next song: --then-title
                       with --then-artist and nothing else, which the whole
                       object makes a song with no album or year
  --then-title TEXT    (see --then-after)
  --then-artist TEXT   (see --then-after)
  --seconds N          play the file over and over for at least N seconds
                       (default: once through)
  --stall-after N      N seconds into the play, send nothing for --stall-ms, then
                       send what fell due in the time at once: a network stall,
                       for what a player does with chunks that arrive late. The
                       group reads a buffered source about 1.85 s ahead of play,
                       so a stall shorter than that is absorbed and a longer one
                       makes the first chunk after it late by what is over
  --stall-ms N         how long the stall lasts, in milliseconds (default 2500)
  --sink DIRECTORY     also play to a test sink in this process, which writes
                       its WAV and play-time log to DIRECTORY/out
  --sink-layout LAYOUT the test sink's speaker layout (default 2.0)
  --state DIRECTORY    the server's identity and pairing records
                       (default ./hearth-testserver-state)
  --name NAME          the name players show (default "Hearth test server")
  --timeout SECONDS    how long a player may take to connect, pair and take
                       its settings (default 60)
  --report FILE        the report, as JSON (default: standard output)

The exit status is 0 when every player played the whole programme, 1 for a
command line or a file that cannot be used, 2 when a player did not connect,
pair or take its settings, and 3 when a player went away, or reported an
error, during the play.
)";

constexpr int kExitUsage = 1;
constexpr int kExitSetup = 2;
constexpr int kExitPlay = 3;

constexpr std::uint32_t kSampleRate = 48000;
// How long after its last burst has played a programme ends: past the players' output queues.
constexpr std::int64_t kEndMarginUs = 500'000;

std::mutex g_log_mutex;
const SteadyClock::time_point g_started = SteadyClock::now();

void note(std::string_view text) {
    const std::lock_guard lock(g_log_mutex);
    const double seconds = std::chrono::duration<double>(SteadyClock::now() - g_started).count();
    std::array<char, 16> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%9.3f", seconds);
    std::cerr << stamp.data() << " " << text << std::endl;
}

struct PlayerSpec {
    std::string url{};
    std::string label{};
    std::optional<std::string> token{};
    std::optional<fs::path> code_log{};
    std::optional<std::string> layout{};
    std::optional<std::vector<double>> trim_db{};
    std::optional<std::string> status_url{};
    bool hold = false;
    bool in_process = false;
    // Played PCM over player@v1 (ServerHost::use_pcm()) rather than bursts over the extension
    // role, in chunks of at most max_chunk_bytes of audio (0: 20 ms).
    bool pcm = false;
    std::size_t max_chunk_bytes = 0;
    // With pcm: in Opus, where the player lists it (ServerHost::use_pcm's codec).
    bool opus = false;
};

// What one board's GET /status said, sampled once a second.
struct StatusSample {
    double seconds = 0.0;
    std::optional<std::int64_t> origin_server_us{};
    std::string playing{};
    std::int64_t underruns = 0;
    std::int64_t late = 0;
    std::int64_t dropped = 0;
    std::int64_t invalid = 0;
    std::int64_t resyncs = 0;
    std::int64_t error_us = 0;
    std::int64_t worst_error_us = 0;
    std::int64_t decode_stack_free = 0;
    std::int64_t server_stack_free = 0;
};

// What was played, for the report: a file, or the generated tone.
struct ProgrammeInfo {
    std::string file{};
    std::string codec{};
    int channels = 0;
};

struct PlayerRun {
    PlayerSpec spec{};
    std::string client_id{};
    std::string name{};
    std::string dialect{};
    std::string paired_by{};
    std::int64_t settings_revision = 0;
    std::optional<std::string> failure{};
    std::optional<ss::ClientView> last_view{};
    // What the player reported while it played: its decoder's findings, and its levels.
    std::optional<ac::DecoderReport> decoder{};
    std::vector<ac::Level> levels{};
    std::vector<StatusSample> samples{};
    std::optional<std::string> status_error{};
    // The last "sendspin.stream=" line a board's GET /log holds, once the play is over: its own
    // totals for the stream, such as the frames it skipped for being late.
    std::optional<std::string> board_summary{};
};

class Events final : public ss::ServerHostEvents {
   public:
    void on_client(const ss::ClientView& client) override {
        const std::lock_guard lock(mutex_);
        if (!clients_.contains(client.client_id)) {
            note("client " + client.client_id.substr(0, 8) + " (" + client.name + ") from " +
                (client.url.empty() ? client.peer : client.url));
        }
        clients_[client.client_id] = client;
    }
    void on_client_gone(const std::string& client_id) override {
        note("client " + client_id.substr(0, 8) + " gone");
        const std::lock_guard lock(mutex_);
        gone_.push_back(client_id);
    }
    void on_pairing_code_wanted(const std::string& client_id) override {
        note("client " + client_id.substr(0, 8) + " waits for its code");
    }
    void on_paired(const std::string& client_id) override { note("client " + client_id.substr(0, 8) + " paired"); }
    void on_pairing_ended(const std::string& client_id, std::optional<ss::pairing_messages::AbortReason> reason) override {
        note("client " + client_id.substr(0, 8) + " pairing ended" + (reason ? " (aborted)" : ""));
        const std::lock_guard lock(mutex_);
        pairing_failed_.push_back(client_id);
    }
    void on_log(std::string_view line) override { note(line); }

    [[nodiscard]] bool gone(const std::string& client_id) {
        const std::lock_guard lock(mutex_);
        return std::find(gone_.begin(), gone_.end(), client_id) != gone_.end();
    }
    [[nodiscard]] bool pairing_failed(const std::string& client_id) {
        const std::lock_guard lock(mutex_);
        return std::find(pairing_failed_.begin(), pairing_failed_.end(), client_id) != pairing_failed_.end();
    }

   private:
    std::mutex mutex_;
    std::map<std::string, ss::ClientView> clients_;
    std::vector<std::string> gone_;
    std::vector<std::string> pairing_failed_;
};

class SinkLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override { note("sink: " + std::string(text)); }
};

[[nodiscard]] std::optional<std::vector<std::byte>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

[[nodiscard]] std::optional<long> parse_number(std::string_view text) {
    long value = 0;
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return value;
}

// The burst's two little-endian words at `at` in IEC 61937 carrier bytes.
[[nodiscard]] std::uint16_t word_at(const std::vector<std::byte>& words, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(words[at]) |
                                      (std::to_integer<unsigned>(words[at + 1]) << 8U));
}

// The programme's bursts, made as they are needed: `passes` times through the file's access units
// on one timeline, each burst with the Pc and Pd iclforge::containers::iec61937 writes, the units it carries, and the
// programme frame of its first sample.
class BurstSource {
   public:
    BurstSource(const iclforge::ac3::io::ScannedStream& stream, int passes)
        : stream_(&stream), passes_(passes), pass_samples_(iclforge::ac3::io::stream_duration_samples(stream)) {}

    [[nodiscard]] std::optional<ss::Group::Burst> next(std::string& error) {
        const bool eac3 = stream_->kind != iclforge::ac3::io::StreamKind::kAc3;
        payload_.clear();
        std::optional<std::int64_t> frame;
        while (pass_ < passes_) {
            if (unit_ >= stream_->access_units.size()) {
                unit_ = 0;
                ++pass_;
                continue;
            }
            const std::span<const std::byte> unit = stream_->access_units[unit_];
            const std::optional<iclforge::ac3::io::AccessUnitTiming> timing = iclforge::ac3::io::access_unit_timing(*stream_, unit_);
            ++unit_;
            if (!timing) {
                error = "an access unit has no timing";
                return std::nullopt;
            }
            if (!frame) {
                frame = static_cast<std::int64_t>((pass_samples_ * static_cast<std::uint64_t>(pass_)) + timing->start_sample);
            }
            std::transform(unit.begin(), unit.end(), std::back_inserter(payload_),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            if (!eac3) {
                const auto burst = iclforge::containers::iec61937::wrap_frame(unit);
                if (!burst) {
                    error = "an AC-3 frame could not be wrapped";
                    return std::nullopt;
                }
                return ss::Group::Burst{.pc = word_at(*burst, 4), .pd = word_at(*burst, 6), .payload = payload_, .frame = *frame};
            }
            const auto burst = packer_.push(unit);
            if (!burst) {
                error = "an E-AC-3 access unit could not be packed";
                return std::nullopt;
            }
            if (*burst) {
                return ss::Group::Burst{.pc = word_at(**burst, 4), .pd = word_at(**burst, 6), .payload = payload_, .frame = *frame};
            }
        }
        return std::nullopt;
    }

   private:
    const iclforge::ac3::io::ScannedStream* stream_;
    int passes_;
    std::uint64_t pass_samples_;
    int pass_ = 0;
    std::size_t unit_ = 0;
    iclforge::containers::iec61937::Eac3BurstPacker packer_;
    std::vector<std::uint8_t> payload_;
};

// An AC-4 programme (planning/ac4.md, I6): its sync frames, each one burst, on the timeline of the
// data-burst periods IEC 61937-14 writes them with. Packed once, so that every pass has the same
// bursts and the next starts where the last ended.
struct Ac4Programme {
    // The data type the frames are sent as: AC-4's sync frames, or a TrueHD programme's chunks of
    // access units (read_truehd_programme()), which are packed the same way for this tool's purposes.
    ac::DataType data_type = ac::DataType::kAc4;
    struct Frame {
        // The sync frame whole, the CRC word included: a view into the file's bytes.
        std::span<const std::byte> sync_frame;
        std::uint16_t pc = 0;
        std::uint16_t pd = 0;
        // HBR16's payload runs on to a whole 8-byte unit, with zeros.
        std::size_t payload_bytes = 0;
        std::uint64_t period = 0;
    };
    std::vector<Frame> frames;
    std::uint64_t pass_samples = 0;
    std::uint32_t sample_rate = 0;
};

// Whether `bytes` opens with a TrueHD or MLP access unit: a check nibble and a length, a timing word,
// and a major sync (TH spec section 4.2).
[[nodiscard]] bool starts_truehd(std::span<const std::byte> bytes) {
    return bytes.size() >= 8 && bytes[4] == std::byte{0xF8} && bytes[5] == std::byte{0x72} &&
           bytes[6] == std::byte{0x6F} && (bytes[7] == std::byte{0xBA} || bytes[7] == std::byte{0xBB});
}

// A TrueHD programme (planning/truehd-parity-hearth-player.md, H5): the stream's access units, 24 to a
// chunk (sendspin::kTruehdUnitsPerChunk), each chunk a burst of Pc 22 and a Pd in bytes, on the
// timeline of the units' samples. The chunk's payload is a view of the file: its units are
// contiguous there. Shaped as an Ac4Programme so that the same source sends it.
[[nodiscard]] std::optional<Ac4Programme> read_truehd_programme(std::span<const std::byte> bytes,
                                                                std::string& error) {
    const auto major = iclforge::truehd::parse_major_sync(bytes.subspan(4));
    if (!major) {
        error = "its first access unit's major sync does not read";
        return std::nullopt;
    }
    if (major->kind != iclforge::truehd::StreamKind::kTrueHd) {
        error = "it is MLP, which has no data type on the network";
        return std::nullopt;
    }
    Ac4Programme out;
    out.data_type = ac::DataType::kTruehd;
    out.sample_rate = major->sample_rate();
    const std::uint64_t unit_samples = 40U * (out.sample_rate / (out.sample_rate % 44100U == 0 ? 44100U : 48000U));
    std::size_t at = 0;
    std::size_t chunk_start = 0;
    std::size_t units = 0;
    const auto close_chunk = [&](std::size_t end) {
        if (units == 0) {
            return;
        }
        const std::span<const std::byte> payload = bytes.subspan(chunk_start, end - chunk_start);
        out.frames.push_back({.sync_frame = payload,
                              .pc = static_cast<std::uint16_t>(ss::BurstDataType::kTruehd),
                              .pd = static_cast<std::uint16_t>(payload.size()),
                              .payload_bytes = payload.size(),
                              .period = units * unit_samples});
        out.pass_samples += units * unit_samples;
        units = 0;
        chunk_start = end;
    };
    while (at + 4 <= bytes.size()) {
        const std::size_t words = ((std::to_integer<std::size_t>(bytes[at]) & 0x0FU) << 8U) |
                                  std::to_integer<std::size_t>(bytes[at + 1]);
        const std::size_t size = words * 2;
        if (size < 4 || at + size > bytes.size()) {
            break;
        }
        at += size;
        if (++units == ss::kTruehdUnitsPerChunk) {
            close_chunk(at);
        }
    }
    close_chunk(at);
    if (out.frames.empty()) {
        error = "it holds no whole access unit";
        return std::nullopt;
    }
    return out;
}

// Whether `bytes` opens with an AC-4 sync word, 0xAC40 or 0xAC41.
[[nodiscard]] bool starts_ac4(std::span<const std::byte> bytes) {
    return bytes.size() >= 2 && bytes[0] == std::byte{0xAC} &&
           (bytes[1] == std::byte{0x40} || bytes[1] == std::byte{0x41});
}

[[nodiscard]] std::optional<Ac4Programme> read_ac4_programme(std::span<const std::byte> bytes,
                                                             std::string& error) {
    namespace iec = iclforge::containers::iec61937;
    const iclforge::ac4::ScanResult scanned = iclforge::ac4::scan(bytes);
    if (scanned.frames.empty() || scanned.frames.front().offset != 0) {
        error = "it holds no AC-4 sync frame at its start";
        return std::nullopt;
    }
    const auto first = iclforge::ac4::parse_raw_frame(scanned.frames.front().raw_ac4_frame);
    if (!first) {
        error = "its first AC-4 frame's table of contents does not read";
        return std::nullopt;
    }
    Ac4Programme out;
    out.sample_rate = static_cast<std::uint32_t>(first->toc.sample_rate_hz);
    std::vector<std::span<const std::byte>> frames;
    std::size_t largest = 0;
    for (const iclforge::ac4::SyncFrame& frame : scanned.frames) {
        const auto raw_end = static_cast<std::size_t>(frame.raw_ac4_frame.data() - bytes.data()) +
                             frame.raw_ac4_frame.size();
        const std::size_t end = raw_end + (frame.sync_word == 0xAC41 ? 2U : 0U);
        frames.push_back(bytes.subspan(frame.offset, end - frame.offset));
        largest = std::max(largest, frames.back().size());
    }
    const std::optional<iec::BurstDataType> type = iec::ac4_burst_type_for(
        largest, first->toc.sample_rate_hz == 44100 ? 0 : 1, first->toc.frame_rate_index);
    if (!type) {
        error = "its largest AC-4 frame fits no burst type";
        return std::nullopt;
    }
    iec::Ac4BurstPacker packer{*type};
    for (const std::span<const std::byte> sync_frame : frames) {
        if (!packer.push(sync_frame)) {
            error = "an AC-4 frame could not be packed";
            return std::nullopt;
        }
        const iec::Ac4BurstPacker::Packed& last = *packer.last();
        out.frames.push_back({.sync_frame = sync_frame,
                              .pc = last.pc,
                              .pd = last.pd,
                              .payload_bytes = last.payload_bytes,
                              .period = last.period});
        out.pass_samples += last.period;
    }
    return out;
}

// The programme's bursts: `passes` times through the frames on one timeline.
class Ac4BurstSource {
   public:
    Ac4BurstSource(const Ac4Programme& programme, int passes)
        : programme_(&programme), passes_(passes) {}

    [[nodiscard]] std::optional<ss::Group::Burst> next(std::string&) {
        if (pass_ >= passes_) {
            return std::nullopt;
        }
        if (unit_ >= programme_->frames.size()) {
            unit_ = 0;
            if (++pass_ >= passes_) {
                return std::nullopt;
            }
        }
        const Ac4Programme::Frame& frame = programme_->frames[unit_++];
        payload_.assign(frame.payload_bytes, 0);
        std::transform(frame.sync_frame.begin(), frame.sync_frame.end(), payload_.begin(),
                       [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
        const ss::Group::Burst burst{.pc = frame.pc,
                                     .pd = frame.pd,
                                     .payload = payload_,
                                     .frame = static_cast<std::int64_t>(next_frame_),
                                     .frames = static_cast<std::int64_t>(frame.period)};
        next_frame_ += frame.period;
        return burst;
    }

   private:
    const Ac4Programme* programme_;
    int passes_;
    int pass_ = 0;
    std::size_t unit_ = 0;
    std::uint64_t next_frame_ = 0;
    std::vector<std::uint8_t> payload_;
};

// The digits of the first "PAIRING CODE" line in `path` past byte `from`.
[[nodiscard]] std::optional<std::string> code_in(const fs::path& path, std::uintmax_t from) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(from));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t at = line.find("PAIRING CODE ");
        if (at == std::string::npos) {
            continue;
        }
        std::string digits;
        for (const char c : line.substr(at + 13)) {
            if (c >= '0' && c <= '9') {
                digits.push_back(c);
            } else if (c != '-' && c != ' ') {
                break;
            }
        }
        if (digits.size() >= 6) {
            return digits;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uintmax_t size_of(const fs::path& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    return error ? 0 : size;
}

[[nodiscard]] std::string psk_text(ss::handshake::PskCategory psk) {
    switch (psk) {
        case ss::handshake::PskCategory::kLongTerm:
            return "long-term";
        case ss::handshake::PskCategory::kPairing:
            return "pairing";
        case ss::handshake::PskCategory::kSentinel:
            return "sentinel";
    }
    return "unknown";
}

// One GET /status, and what its "sendspin" object says.
[[nodiscard]] std::optional<StatusSample> read_status(const std::string& url, double seconds, std::string& error) {
    const std::size_t scheme = url.find("://");
    const std::size_t path_at = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    const std::string origin = path_at == std::string::npos ? url : url.substr(0, path_at);
    const std::string path = path_at == std::string::npos ? "/status" : url.substr(path_at);
    httplib::Client client(origin);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    const httplib::Result result = client.Get(path);
    if (!result) {
        error = "no answer from " + url;
        return std::nullopt;
    }
    if (result->status != 200) {
        error = url + " answered " + std::to_string(result->status);
        return std::nullopt;
    }
    json::Document document;
    std::vector<json::Token> tokens;
    if (!document.parse(result->body, tokens, 16384)) {
        error = url + " sent no JSON";
        return std::nullopt;
    }
    const json::Value sendspin = document.root()["sendspin"];
    if (!sendspin.is_object()) {
        error = url + " has no Sendspin player";
        return std::nullopt;
    }
    StatusSample sample;
    sample.seconds = seconds;
    sample.origin_server_us = sendspin["origin_server_us"].as_int();
    sample.playing = sendspin["playing"].as_string().value_or("");
    sample.underruns = sendspin["underruns"].as_int().value_or(0);
    sample.late = sendspin["late"].as_int().value_or(0);
    sample.dropped = sendspin["dropped"].as_int().value_or(0);
    sample.invalid = sendspin["invalid"].as_int().value_or(0);
    sample.resyncs = sendspin["resyncs"].as_int().value_or(0);
    sample.error_us = sendspin["error_us"].as_int().value_or(0);
    sample.worst_error_us = sendspin["worst_error_us"].as_int().value_or(0);
    sample.decode_stack_free = sendspin["decode_stack_free"].as_int().value_or(0);
    sample.server_stack_free = sendspin["server_stack_free"].as_int().value_or(0);
    return sample;
}

// The last "sendspin.stream=" line in a board's GET /log, which the board writes as a stream ends:
// its totals for the play, among them the frames it left out for being late and the silence it
// wrote. The log is read from the same origin as the status URL.
[[nodiscard]] std::optional<std::string> read_board_summary(const std::string& status_url) {
    const std::size_t scheme = status_url.find("://");
    const std::size_t path_at = status_url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    const std::string origin =
        path_at == std::string::npos ? status_url : status_url.substr(0, path_at);
    httplib::Client client(origin);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(4, 0);
    const httplib::Result result = client.Get("/log");
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    std::optional<std::string> last;
    std::size_t at = 0;
    while (at < result->body.size()) {
        const std::size_t end = std::min(result->body.find('\n', at), result->body.size());
        const std::string line = result->body.substr(at, end - at);
        if (line.find("sendspin.stream=") != std::string::npos) {
            last = line;
        }
        at = end + 1;
    }
    return last;
}

// Connects, pairs and configures one player; its client_id once it plays the extension role with
// the settings asked for, or a failure.
[[nodiscard]] bool bring_up(ss::ServerHost& host, Events& events, PlayerRun& run, SteadyClock::time_point deadline) {
    const auto fail = [&](std::string why) {
        run.failure = std::move(why);
        note(run.spec.label + ": " + *run.failure);
        return false;
    };
    // Connected and said hello.
    std::optional<ss::ClientView> view;
    while (!view) {
        for (const ss::ClientView& client : host.clients()) {
            if (client.url == run.spec.url) {
                view = client;
            }
        }
        if (!view) {
            if (SteadyClock::now() >= deadline) {
                return fail("did not connect");
            }
            std::this_thread::sleep_for(50ms);
        }
    }
    run.client_id = view->client_id;
    run.name = view->name;
    if (run.spec.label.empty()) {
        run.spec.label = view->name;
    }
    run.dialect = view->dialect == ss::Dialect::kAiosendspin911 ? "aiosendspin 9.1.1" : "specification";
    if (run.spec.hold) {
        note(run.spec.label + ": connected, and held without pairing (" + psk_text(view->psk) + ")");
        return true;
    }
    run.paired_by = run.spec.token ? "token" : view->psk == ss::handshake::PskCategory::kLongTerm ? "record" : "";

    // Paired: by its record, by its token (entered before dialling), or by the code it shows. A
    // player asked for PCM (--pcm) is moved to player@v1 once it is paired, and is playing when it
    // has taken that role.
    const auto playing = [&](const ss::ClientView& client) {
        if (run.spec.pcm) {
            return client.playing && !client.bursts && client.available &&
                   client.psk == ss::handshake::PskCategory::kLongTerm &&
                   client.player_state.has_value();
        }
        return client.playing && client.bursts && client.available &&
               client.psk == ss::handshake::PskCategory::kLongTerm && client.iclforge_state.has_value();
    };
    bool asked_for_pcm = false;
    bool stated_noted = false;
    if (view->psk != ss::handshake::PskCategory::kLongTerm && !run.spec.token) {
        if (!run.spec.code_log) {
            return fail("is not paired, and has no --token or --code-log");
        }
        const std::uintmax_t from = size_of(*run.spec.code_log);
        // main() asked for this pairing at the dial (dial_to_pair), so that it is the connection's
        // first activation. Asking again here would leave a request behind that the host honours
        // once the player is paired, and sends it back to pair a second time.
        run.paired_by = "code";
        note(run.spec.label + ": asked to show a code; reading " + run.spec.code_log->string());
        std::optional<std::string> code;
        while (!code) {
            code = code_in(*run.spec.code_log, from);
            if (!code) {
                if (SteadyClock::now() >= deadline) {
                    return fail("showed no code in " + run.spec.code_log->string());
                }
                std::this_thread::sleep_for(100ms);
            }
        }
        note(run.spec.label + ": entering the code it shows");
        bool entered = false;
        while (!entered) {
            entered = host.enter_code(run.client_id, *code);
            if (!entered) {
                if (SteadyClock::now() >= deadline || events.pairing_failed(run.client_id)) {
                    return fail("did not take its code");
                }
                std::this_thread::sleep_for(50ms);
            }
        }
    }
    while (true) {
        const std::optional<ss::ClientView> now = host.client(run.client_id);
        if (!stated_noted && now && now->iclforge_support && now->iclforge_state) {
            // What the sink says of itself: the chunk limit and the layout it renders to.
            stated_noted = true;
            note(run.spec.label + ": states a chunk limit of " +
                 std::to_string(now->iclforge_support->max_chunk_bytes) + " bytes and layout " +
                 now->iclforge_state->layout.value_or("(none)"));
            // And what it lists of each data type: its widest frame and fastest rate, 0 for none.
            for (const ac::DataType type : now->iclforge_support->data_types) {
                note(run.spec.label + ": lists " + std::string(ac::data_type_name(type)) +
                     ", up to " +
                     std::to_string(now->iclforge_support->max_coded_channels_of(type)) +
                     " channels and " +
                     std::to_string(now->iclforge_support->max_frame_rate_of(type)) +
                     " frames a second (0 states none)");
            }
        }
        if (run.spec.pcm && !asked_for_pcm && now &&
            now->psk == ss::handshake::PskCategory::kLongTerm && now->available) {
            // Paired and playing its first role: now it is moved to PCM, and chunked to its limit.
            if (!host.use_pcm(
                    run.client_id, true, run.spec.max_chunk_bytes, 0,
                    run.spec.opus ? std::optional<m::Codec>{m::Codec::kOpus} : std::nullopt)) {
                return fail("cannot be asked to play PCM");
            }
            asked_for_pcm = true;
        }
        if (now && playing(*now)) {
            view = now;
            break;
        }
        if (events.pairing_failed(run.client_id) && run.paired_by == "code") {
            return fail("pairing failed");
        }
        if (SteadyClock::now() >= deadline) {
            if (!now) {
                return fail("went away before it played");
            }
            return fail(std::string("is not playing _iclforge_player@v1 on a long-term PSK (psk ") + psk_text(now->psk) +
                        (now->playing ? ", playing" : "") + (now->bursts ? ", bursts" : "") +
                        (now->available ? ", available" : "") + ")");
        }
        std::this_thread::sleep_for(50ms);
    }
    if (run.spec.pcm) {
        note(run.spec.label + ": playing player@v1's " + (run.spec.opus ? "Opus" : "PCM") + " (" +
             run.dialect + ")" +
             (run.spec.max_chunk_bytes != 0
                  ? ", chunks of at most " + std::to_string(run.spec.max_chunk_bytes) +
                        " bytes of audio"
                  : std::string{}));
        if (run.spec.layout) {
            return fail(
                "takes no layout: player@v1 renders nothing, and --layout is for the extension "
                "role");
        }
        return true;
    }
    note(run.spec.label + ": playing _iclforge_player@v1 (" + run.dialect + ")");

    // Settings, for a player that takes them.
    if (run.spec.layout) {
        const std::vector<ac::Command>& commands = view->iclforge_state->supported_commands;
        if (std::find(commands.begin(), commands.end(), ac::Command::kSettings) == commands.end()) {
            return fail("does not take settings");
        }
        const std::int64_t revision =
            std::max<std::int64_t>(view->iclforge_state->settings_revision + 1,
                                   std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
        ac::CommandMessage command;
        command.command = ac::Command::kSettings;
        command.settings.revision = revision;
        command.settings.layout = *run.spec.layout;
        command.settings.trim_db = run.spec.trim_db;
        if (!host.iclforge_command(run.client_id, command)) {
            return fail("refused settings for layout " + *run.spec.layout + " before sending them");
        }
        while (true) {
            const std::optional<ss::ClientView> now = host.client(run.client_id);
            if (now && now->iclforge_state) {
                const ac::State& state = *now->iclforge_state;
                if (state.settings_error && state.settings_error->revision == revision) {
                    return fail("refused its settings: " + state.settings_error->why);
                }
                if (state.settings_revision == revision) {
                    break;
                }
            }
            if (SteadyClock::now() >= deadline) {
                return fail("did not report its settings as applied");
            }
            std::this_thread::sleep_for(50ms);
        }
        run.settings_revision = revision;
        note(run.spec.label + ": took layout " + *run.spec.layout + " (revision " + std::to_string(revision) + ")");
    }
    return true;
}

void write_report(std::ostream& out, const std::string& server_name, const std::string& server_id,
                  const ProgrammeInfo& programme, std::uint64_t bursts_sent,
                  std::uint64_t pcm_frames_sent, std::optional<std::int64_t> start_server_us,
                  double played_seconds, const std::vector<PlayerRun>& runs,
                  const std::string& result) {
    std::string text;
    json::Writer w(text);
    w.begin_object();
    w.key("server").begin_object().member("name", server_name).member("id", server_id).end_object();
    w.key("programme").begin_object();
    w.member("file", programme.file);
    w.member("codec", programme.codec);
    w.member("channels", programme.channels);
    w.member("bursts", bursts_sent);
    w.member("pcm_frames", pcm_frames_sent);
    w.key("seconds").number(played_seconds, 1);
    w.key("start_server_us");
    start_server_us ? w.integer(*start_server_us) : w.null();
    w.end_object();

    w.key("players").begin_array();
    for (const PlayerRun& run : runs) {
        w.begin_object();
        w.member("label", run.spec.label);
        w.member("url", run.spec.url);
        w.member("client_id", run.client_id);
        w.member("name", run.name);
        w.member("dialect", run.dialect);
        w.member("paired_by", run.paired_by);
        w.key("layout");
        run.spec.layout ? w.string(*run.spec.layout) : w.null();
        w.member("settings_revision", run.settings_revision);
        w.member("role", run.spec.pcm ? "player@v1" : "_iclforge_player@v1");
        w.member("max_chunk_bytes", static_cast<std::uint64_t>(run.spec.max_chunk_bytes));
        w.member("codec", run.spec.pcm ? (run.spec.opus ? "opus" : "pcm") : "bursts");
        w.key("failure");
        run.failure ? w.string(*run.failure) : w.null();
        if (run.last_view && run.last_view->iclforge_state) {
            const ac::State& state = *run.last_view->iclforge_state;
            w.key("counters").begin_object();
            w.member("bursts_played", state.counters.bursts_played);
            w.member("underruns", state.counters.underruns);
            w.member("late_chunks", state.counters.late_chunks);
            w.member("dropped_chunks", state.counters.dropped_chunks);
            w.member("invalid_chunks", state.counters.invalid_chunks);
            w.end_object();
            w.key("settings_error");
            state.settings_error ? w.string(state.settings_error->why) : w.null();
        }
        w.key("decoder");
        if (run.decoder) {
            w.begin_object();
            w.member("acmod", run.decoder->acmod);
            w.member("lfe", run.decoder->lfe);
            w.member("substreams", run.decoder->substreams);
            w.member("objects", run.decoder->objects);
            w.member("objects_placed", run.decoder->objects_placed);
            w.key("dialnorm").number(run.decoder->dialnorm, 1);
            w.end_object();
        } else {
            w.null();
        }
        w.key("levels").begin_array();
        for (const ac::Level& level : run.levels) {
            w.begin_object();
            w.member("output", level.output);
            w.key("peak_db").number(level.peak_db, 1);
            w.key("rms_db").number(level.rms_db, 1);
            w.end_object();
        }
        w.end_array();
        if (run.spec.status_url) {
            w.key("status").begin_object();
            w.member("url", *run.spec.status_url);
            w.member("samples", static_cast<std::int64_t>(run.samples.size()));
            w.key("error");
            run.status_error ? w.string(*run.status_error) : w.null();
            // Where the board put the first frame, against where the group put it.
            std::optional<std::int64_t> low;
            std::optional<std::int64_t> high;
            std::int64_t underruns = 0;
            std::int64_t late = 0;
            std::int64_t dropped = 0;
            std::int64_t invalid = 0;
            std::int64_t resyncs = 0;
            std::int64_t worst = 0;
            std::optional<std::int64_t> decode_stack;
            std::optional<std::int64_t> server_stack;
            for (const StatusSample& sample : run.samples) {
                underruns = std::max(underruns, sample.underruns);
                late = std::max(late, sample.late);
                dropped = std::max(dropped, sample.dropped);
                invalid = std::max(invalid, sample.invalid);
                resyncs = std::max(resyncs, sample.resyncs);
                if (std::llabs(sample.worst_error_us) > std::llabs(worst)) {
                    worst = sample.worst_error_us;
                }
                if (sample.decode_stack_free > 0) {
                    decode_stack = std::min(decode_stack.value_or(sample.decode_stack_free), sample.decode_stack_free);
                }
                if (sample.server_stack_free > 0) {
                    server_stack = std::min(server_stack.value_or(sample.server_stack_free), sample.server_stack_free);
                }
                if (!sample.origin_server_us || !start_server_us) {
                    continue;
                }
                const std::int64_t off = *sample.origin_server_us - *start_server_us;
                low = std::min(low.value_or(off), off);
                high = std::max(high.value_or(off), off);
            }
            w.key("origin_error_us").begin_object();
            w.key("min");
            low ? w.integer(*low) : w.null();
            w.key("max");
            high ? w.integer(*high) : w.null();
            w.end_object();
            w.member("underruns", underruns);
            w.member("late", late);
            w.member("dropped", dropped);
            w.member("invalid", invalid);
            w.member("resyncs", resyncs);
            w.key("board_summary");
            run.board_summary ? w.string(*run.board_summary) : w.null();
            w.member("worst_error_us", worst);
            w.key("decode_stack_free");
            decode_stack ? w.integer(*decode_stack) : w.null();
            w.key("server_stack_free");
            server_stack ? w.integer(*server_stack) : w.null();
            w.end_object();
        }
        w.end_object();
    }
    w.end_array();

    // The boards against each other: at each second every board with a play reported, the
    // difference between the latest and earliest first-frame times.
    std::optional<std::int64_t> spread;
    std::int64_t compared = 0;
    const std::vector<const PlayerRun*> boards = [&] {
        std::vector<const PlayerRun*> found;
        for (const PlayerRun& run : runs) {
            if (run.spec.status_url) {
                found.push_back(&run);
            }
        }
        return found;
    }();
    if (boards.size() >= 2) {
        const std::size_t count = boards.front()->samples.size();
        for (std::size_t i = 0; i < count; ++i) {
            std::optional<std::int64_t> low;
            std::optional<std::int64_t> high;
            bool all = true;
            for (const PlayerRun* board : boards) {
                if (i >= board->samples.size() || !board->samples[i].origin_server_us ||
                    board->samples[i].playing == "idle") {
                    all = false;
                    break;
                }
                const std::int64_t origin = *board->samples[i].origin_server_us;
                low = std::min(low.value_or(origin), origin);
                high = std::max(high.value_or(origin), origin);
            }
            if (all && low && high) {
                spread = std::max(spread.value_or(0), *high - *low);
                ++compared;
            }
        }
    }
    w.key("boards").begin_object();
    w.member("compared_seconds", compared);
    w.key("spread_us");
    spread ? w.integer(*spread) : w.null();
    w.end_object();
    w.member("result", result);
    w.end_object();
    out << text << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<PlayerSpec> players;
    fs::path programme;
    // --lyrics: the document sent each player that takes one, as item 1.
    std::optional<fs::path> lyrics_path;
    // --title and the rest: what metadata@v1 says is playing, and the next song partway through.
    std::optional<std::string> meta_title;
    std::string meta_artist;
    std::string meta_album;
    std::optional<std::int32_t> meta_year;
    std::optional<long> meta_track_seconds;
    std::optional<long> then_after;
    std::string then_title;
    std::string then_artist;
    bool tone = false;
    std::int32_t tone_bits = 24;
    std::optional<long> seconds;
    std::optional<fs::path> sink_directory;
    std::string sink_layout = "2.0";
    fs::path state_directory = "hearth-testserver-state";
    std::string server_name = "Hearth test server";
    long timeout_seconds = 60;
    std::optional<long> stall_after;
    long stall_ms = 2500;
    std::optional<fs::path> report_path;

    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return EXIT_SUCCESS;
        }
        if (argument == "--hold") {
            if (players.empty() || players.back().in_process) {
                std::cerr << "--hold follows a --player\n";
                return kExitUsage;
            }
            players.back().hold = true;
            continue;
        }
        if (argument == "--opus") {
            if (players.empty() || players.back().in_process || !players.back().pcm) {
                std::cerr << "--opus follows a --player and its --pcm\n";
                return kExitUsage;
            }
            players.back().opus = true;
            continue;
        }
        if (argument == "--pcm") {
            if (players.empty() || players.back().in_process) {
                std::cerr << "--pcm follows a --player\n";
                return kExitUsage;
            }
            players.back().pcm = true;
            continue;
        }
        if (argument == "--tone") {
            tone = true;
            continue;
        }
        if (argument == "--lyrics") {
            if (i + 1 >= arguments.size()) {
                std::cerr << "--lyrics needs a file\n";
                return kExitUsage;
            }
            lyrics_path = fs::path(std::string(arguments[++i]));
            continue;
        }
        if (i + 1 >= arguments.size()) {
            std::cerr << argument << " needs a value\n";
            return kExitUsage;
        }
        const std::string value(arguments[++i]);
        const auto player = [&]() -> PlayerSpec* {
            if (players.empty() || players.back().in_process) {
                std::cerr << argument << " follows a --player\n";
                return nullptr;
            }
            return &players.back();
        };
        if (argument == "--player") {
            players.push_back({.url = value});
        } else if (argument == "--title") {
            meta_title = value;
        } else if (argument == "--artist") {
            meta_artist = value;
        } else if (argument == "--album") {
            meta_album = value;
        } else if (argument == "--then-title") {
            then_title = value;
        } else if (argument == "--then-artist") {
            then_artist = value;
        } else if (argument == "--year" || argument == "--track-seconds" || argument == "--then-after") {
            const std::optional<long> number = parse_number(value);
            if (!number || *number < 0) {
                std::cerr << argument << " takes a whole number\n";
                return kExitUsage;
            }
            if (argument == "--year") {
                meta_year = static_cast<std::int32_t>(*number);
            } else if (argument == "--track-seconds") {
                meta_track_seconds = *number;
            } else {
                then_after = *number;
            }
        } else if (argument == "--tone-bits") {
            const std::optional<long> bits = parse_number(value);
            if (!bits || (*bits != 16 && *bits != 24)) {
                std::cerr << "--tone-bits takes 16 or 24\n";
                return kExitUsage;
            }
            tone_bits = static_cast<std::int32_t>(*bits);
        } else if (argument == "--max-chunk-bytes") {
            PlayerSpec* spec = player();
            if (spec == nullptr) {
                return kExitUsage;
            }
            const std::optional<long> bytes = parse_number(value);
            if (!bytes || *bytes < 64) {
                std::cerr << "--max-chunk-bytes takes a whole number of bytes, at least 64\n";
                return kExitUsage;
            }
            spec->max_chunk_bytes = static_cast<std::size_t>(*bytes);
        } else if (argument == "--label" || argument == "--token" || argument == "--code-log" ||
                   argument == "--layout" || argument == "--trim-db" || argument == "--status") {
            PlayerSpec* spec = player();
            if (spec == nullptr) {
                return kExitUsage;
            }
            if (argument == "--label") {
                spec->label = value;
            } else if (argument == "--token") {
                spec->token = value;
            } else if (argument == "--code-log") {
                spec->code_log = fs::path(value);
            } else if (argument == "--layout") {
                spec->layout = value;
            } else if (argument == "--trim-db") {
                std::vector<double> trims;
                std::string_view list = value;
                while (!list.empty()) {
                    const std::size_t comma = list.find(',');
                    // A dB figure typed on a command line: a "+3" is fine, and a locale's comma is
                    // not a decimal point (iclforge::base::parse_double reads like std::from_chars).
                    const auto trim = iclforge::base::parse_double(
                        list.substr(0, comma), iclforge::base::kUserNumber);
                    if (!trim) {
                        std::cerr << "--trim-db takes numbers of dB separated by commas\n";
                        return kExitUsage;
                    }
                    trims.push_back(trim->value);
                    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                }
                spec->trim_db = std::move(trims);
            } else {
                spec->status_url = value;
            }
        } else if (argument == "--play") {
            programme = value;
        } else if (argument == "--seconds") {
            seconds = parse_number(value);
            if (!seconds || *seconds == 0) {
                std::cerr << "--seconds takes a whole number of seconds\n";
                return kExitUsage;
            }
        } else if (argument == "--stall-after") {
            stall_after = parse_number(value);
            if (!stall_after) {
                std::cerr << "--stall-after takes a whole number of seconds\n";
                return kExitUsage;
            }
        } else if (argument == "--stall-ms") {
            const std::optional<long> parsed = parse_number(value);
            if (!parsed || *parsed == 0) {
                std::cerr << "--stall-ms takes a whole number of milliseconds\n";
                return kExitUsage;
            }
            stall_ms = *parsed;
        } else if (argument == "--sink") {
            sink_directory = fs::path(value);
        } else if (argument == "--sink-layout") {
            sink_layout = value;
        } else if (argument == "--state") {
            state_directory = value;
        } else if (argument == "--name") {
            server_name = value;
        } else if (argument == "--timeout") {
            const std::optional<long> parsed = parse_number(value);
            if (!parsed || *parsed == 0) {
                std::cerr << "--timeout takes a whole number of seconds\n";
                return kExitUsage;
            }
            timeout_seconds = *parsed;
        } else if (argument == "--report") {
            report_path = fs::path(value);
        } else {
            std::cerr << "unknown option " << argument << "\n\n" << kUsage;
            return kExitUsage;
        }
    }
    if ((programme.empty() && !tone) || (!programme.empty() && tone) ||
        (players.empty() && !sink_directory)) {
        std::cerr << kUsage;
        return kExitUsage;
    }
    // A tone is PCM, which only player@v1 takes; and a player asked for PCM has no bursts to play.
    for (const PlayerSpec& spec : players) {
        if (spec.hold) {
            continue;
        }
        if (tone && !spec.pcm && !spec.in_process) {
            std::cerr << "--tone is PCM: " << spec.url << " needs --pcm (or --hold)\n";
            return kExitUsage;
        }
        if (!tone && spec.pcm) {
            std::cerr << "--pcm plays the --tone programme, not a file: " << spec.url << "\n";
            return kExitUsage;
        }
    }

    // The programme: a file's access units, or the tone.
    ProgrammeInfo info;
    // The file's bytes, for as long as `stream` lives: its access units are views into them.
    std::optional<std::vector<std::byte>> bytes;
    std::optional<iclforge::ac3::io::ScannedStream> stream;
    std::optional<Ac4Programme> ac4_programme;
    std::uint64_t pass_samples = 0;
    int passes = 1;
    if (tone) {
        if (!seconds) {
            seconds = 10;
        }
        info = {.file = "tone", .codec = "PCM", .channels = 2};
    } else {
        bytes = read_file(programme);
        if (!bytes) {
            std::cerr << "cannot read " << programme.string() << "\n";
            return kExitUsage;
        }
        if (starts_ac4(*bytes) || starts_truehd(*bytes)) {
            std::string why;
            const bool truehd = starts_truehd(*bytes);
            ac4_programme = truehd ? read_truehd_programme(*bytes, why) : read_ac4_programme(*bytes, why);
            if (!ac4_programme) {
                std::cerr << programme.string() << ": " << why << "\n";
                return kExitUsage;
            }
            if (ac4_programme->sample_rate != kSampleRate || ac4_programme->pass_samples == 0) {
                std::cerr << programme.string()
                          << " is not at 48 kHz, which is all a Hearth sink plays\n";
                return kExitUsage;
            }
            pass_samples = ac4_programme->pass_samples;
            info = {.file = programme.generic_string(), .codec = truehd ? "TrueHD" : "AC-4", .channels = 0};
        } else {
            std::expected<iclforge::ac3::io::ScannedStream, iclforge::ac3::io::ScanError> scanned =
                iclforge::ac3::io::scan(*bytes);
            if (!scanned || scanned->access_units.empty()) {
                std::cerr << programme.string() << " is not an AC-3, E-AC-3 or AC-4 stream\n";
                return kExitUsage;
            }
            if (scanned->sample_rate != iclforge::ac3::SampleRate::k48000) {
                std::cerr << programme.string()
                          << " is not at 48 kHz, which is all a Hearth sink plays\n";
                return kExitUsage;
            }
            stream = std::move(*scanned);
            pass_samples = iclforge::ac3::io::stream_duration_samples(*stream);
            info = {
                .file = programme.generic_string(),
                .codec = stream->kind == iclforge::ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3",
                .channels = stream->channels};
        }
        passes = seconds ? static_cast<int>(((static_cast<std::uint64_t>(*seconds) * kSampleRate) +
                                             pass_samples - 1) /
                                            pass_samples)
                         : 1;
    }

    auto store = iclforge::hearth::testserver::FileServerStore::open(state_directory);
    if (!store) {
        std::cerr << "hearth-testserver: " << store.error() << "\n";
        return kExitUsage;
    }
    Events events;
    auto host = ss::ServerHost::start({.identity = (*store)->identity(),
                                       .name = server_name,
                                       .languages = {"en"},
                                       .address = "127.0.0.1",
                                       .port = std::nullopt,
                                       .advertise = false,
                                       .browse = false,
                                       .mdns_interfaces = {}},
                                      **store, events);
    if (!host) {
        std::cerr << "hearth-testserver: " << host.error() << "\n";
        return kExitUsage;
    }
    note("server " + (*host)->server_id().substr(0, 8) + " (\"" + server_name + "\")");

    // A test sink in this process, as the reference the boards' levels are held to.
    SinkLog sink_log;
    std::unique_ptr<testsink::Sink> sink;
    if (sink_directory) {
        testsink::SinkOptions options;
        options.name = "Reference sink";
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = *sink_directory / "state";
        options.output_directory = *sink_directory / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = sink_layout;
        if (lyrics_path) {
            // The reference sink takes the document and the anchors too, so that the chain from them
            // to a sink's own clock is run here and reported at the end.
            options.lyrics_max_bytes = ac::kMaxLyricsBytes;
        }
        auto started = testsink::Sink::start(options, sink_log);
        if (!started) {
            std::cerr << "hearth-testserver: the test sink: " << started.error() << "\n";
            return kExitUsage;
        }
        sink = std::move(*started);
        players.push_back({.url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin",
                           .label = "reference sink",
                           .token = sink->pairing_token(),
                           .in_process = true,
                           .pcm = tone});
    }

    std::vector<PlayerRun> runs;
    for (const PlayerSpec& spec : players) {
        if (spec.token && !(*host)->enter_pairing_token(*spec.token)) {
            std::cerr << "not a pairing token: " << *spec.token << "\n";
            return kExitUsage;
        }
        runs.push_back({.spec = spec});
        if (spec.code_log && !spec.token) {
            // The connection's first activation is the pairing, which a board another server
            // holds (Music Assistant, attached and idle) admits beside it; asked after the first
            // activation of "nothing", the board refuses it with the rest.
            (void)(*host)->dial_to_pair(spec.url, m::PairMethod::kDynamicCode,
                                        m::CodeFormat::kDigits);
        } else {
            (*host)->dial(spec.url);
        }
    }

    std::string server_id = (*host)->server_id();
    std::uint64_t pcm_frames_sent = 0;
    const auto finish = [&](int status, const std::string& result, std::optional<std::int64_t> start, std::uint64_t sent,
                            double played) {
        for (PlayerRun& run : runs) {
            if (!run.client_id.empty()) {
                if (std::optional<ss::ClientView> view = (*host)->client(run.client_id)) {
                    run.last_view = std::move(view);
                }
            }
        }
        if (report_path) {
            std::ofstream out(*report_path, std::ios::binary | std::ios::trunc);
            write_report(out, server_name, server_id, info, sent, pcm_frames_sent, start, played,
                         runs, result);
        } else {
            write_report(std::cout, server_name, server_id, info, sent, pcm_frames_sent, start,
                         played, runs, result);
        }
        note(result);
        return status;
    };

    // Every player connected, paired and configured, together within the timeout.
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::seconds(timeout_seconds);
    for (PlayerRun& run : runs) {
        if (!bring_up(**host, events, run, deadline)) {
            return finish(kExitSetup, "failed: " + run.spec.label + " " + *run.failure, std::nullopt, 0, 0.0);
        }
    }

    // Held players only: nothing is played, and the connections are kept for the programme's
    // length, for another server to find the players taken.
    const bool any_played = std::any_of(runs.begin(), runs.end(), [](const PlayerRun& run) { return !run.spec.hold; });
    if (!any_played) {
        const auto hold_for = std::chrono::seconds(seconds.value_or(10));
        note("holding " + std::to_string(runs.size()) + " connection(s) for " +
             std::to_string(static_cast<long>(hold_for.count())) + " s");
        std::this_thread::sleep_for(hold_for);
        for (PlayerRun& run : runs) {
            run.failure = events.gone(run.client_id) ? std::optional<std::string>("displaced or closed") : std::nullopt;
        }
        return finish(EXIT_SUCCESS, "pass", std::nullopt, 0, 0.0);
    }

    std::shared_ptr<ss::Group> group = (*host)->make_group("Test group");
    for (const PlayerRun& run : runs) {
        if (!run.spec.hold) {
            group->add(run.client_id);
        }
    }
    // The tone is PCM at --tone-bits; a member is sent the first format it lists, rescaled.
    const std::int32_t kToneBitDepth = tone_bits;
    const bool from_file = stream.has_value() || ac4_programme.has_value();
    const ac::DataType data_type =
        ac4_programme.has_value()                                          ? ac4_programme->data_type
        : from_file && stream->kind == iclforge::ac3::io::StreamKind::kAc3 ? ac::DataType::kAc3
                                                                           : ac::DataType::kEac3;
    ss::Group::Programme group_programme{
        .pcm = std::nullopt, .bursts = std::nullopt, .buffered = true};
    if (tone) {
        group_programme.pcm = m::AudioFormat{.codec = m::Codec::kPcm,
                                             .channels = 2,
                                             .sample_rate = static_cast<std::int32_t>(kSampleRate),
                                             .bit_depth = kToneBitDepth};
    } else {
        group_programme.bursts = ac::StreamStart{
            .data_type = data_type, .sample_rate = static_cast<std::int32_t>(kSampleRate)};
    }
    if (!group->start(group_programme)) {
        return finish(kExitSetup, "failed: the group did not start", std::nullopt, 0, 0.0);
    }
    // The words of the programme, as item 1, to each player whose state lists the lyrics command; the
    // anchors that move the playhead follow with the bursts below.
    constexpr std::int64_t kLyricsItem = 1;
    bool lyrics_sent = false;
    if (lyrics_path) {
        std::optional<std::vector<std::byte>> document = read_file(*lyrics_path);
        if (!document) {
            return finish(kExitUsage, "cannot read " + lyrics_path->string(), std::nullopt, 0, 0.0);
        }
        ac::CommandMessage command;
        command.command = ac::Command::kLyrics;
        command.lyrics.item = kLyricsItem;
        command.lyrics.text.assign(reinterpret_cast<const char*>(document->data()), document->size());
        for (const PlayerRun& run : runs) {
            if (run.spec.hold) {
                continue;
            }
            if ((*host)->iclforge_command(run.client_id, command)) {
                lyrics_sent = true;
                note(run.spec.label + ": " + std::to_string(command.lyrics.text.size()) + " bytes of lyrics sent");
            } else {
                note(run.spec.label + ": does not take lyrics");
            }
        }
    }
    // An anchor says where in the document the sound is at a time on the server's clock: sent every 64
    // bursts (two seconds) and at each pass of the programme, as the engine sends them.
    constexpr std::int64_t kAnchorEveryBursts = 64;
    const auto send_anchor = [&](std::int64_t frame, std::int64_t frames) {
        const std::optional<std::int64_t> start = group->start_time();
        if (!lyrics_sent || !start || pass_samples == 0) {
            return;
        }
        const std::int64_t in_pass = frame % static_cast<std::int64_t>(pass_samples);
        const bool pass_start = in_pass < frames;
        if (!pass_start && frames > 0 && (frame / frames) % kAnchorEveryBursts != 0) {
            return;
        }
        ac::CommandMessage anchor;
        anchor.command = ac::Command::kLyricsPosition;
        anchor.lyrics_position = {.item = kLyricsItem,
                                  .position_ms = in_pass * 1000 / static_cast<std::int64_t>(kSampleRate),
                                  .at_us = *start + frame * 1'000'000 / static_cast<std::int64_t>(kSampleRate),
                                  .speed = 1.0};
        for (const PlayerRun& run : runs) {
            if (!run.spec.hold) {
                (void)(*host)->iclforge_command(run.client_id, anchor);
            }
        }
    };
    const double programme_seconds = tone
                                         ? static_cast<double>(*seconds)
                                         : static_cast<double>(pass_samples) * passes / kSampleRate;
    if (tone) {
        note("playing a tone for " + std::to_string(static_cast<long>(programme_seconds)) +
             " s, to " + std::to_string(runs.size()) + " player(s)");
    } else {
        note("playing " + programme.filename().string() + " " + std::to_string(passes) +
             " time(s) through, " + std::to_string(static_cast<long>(programme_seconds)) +
             " s, to " + std::to_string(runs.size()) + " player(s)");
    }

    // metadata@v1: what is playing, as the whole object the specification sends, from the start of the play
    // (the position is the play's own, on the server's clock); a player that does not list the role is
    // sent nothing. The next song, when asked for, goes from the loop below.
    const ss::SteadyClock metadata_clock;
    const auto say_song = [&](const std::string& title, const std::string& artist, const std::string& album,
                              std::optional<std::int32_t> year, long position_ms) {
        ss::metadata::State state;
        state.timestamp = metadata_clock.now_us();
        state.title = title;
        if (!artist.empty()) {
            state.artist = artist;
        }
        if (!album.empty()) {
            state.album = album;
        }
        state.year = year;
        const long length_seconds = meta_track_seconds.value_or(static_cast<long>(programme_seconds));
        state.progress = ss::metadata::Progress{.track_progress_ms = position_ms,
                                                .track_duration_ms = length_seconds * 1000,
                                                .playback_speed = 1000};
        group->set_metadata(std::move(state));
        note("metadata: \"" + title + "\"" + (artist.empty() ? "" : ", " + artist) +
             (album.empty() ? "" : ", " + album) + ", " + std::to_string(length_seconds) + " s");
    };
    if (meta_title) {
        say_song(*meta_title, meta_artist, meta_album, meta_year, 0);
    }
    bool then_said = false;

    // The boards' status, once a second, on a thread of its own.
    std::atomic<bool> playing{true};
    std::mutex samples_mutex;
    const SteadyClock::time_point play_started = SteadyClock::now();
    std::thread poller([&] {
        SteadyClock::time_point next = SteadyClock::now();
        while (playing.load()) {
            next += 1s;
            const double at = std::chrono::duration<double>(SteadyClock::now() - play_started).count();
            for (PlayerRun& run : runs) {
                if (!run.spec.status_url) {
                    continue;
                }
                std::string error;
                std::optional<StatusSample> sample = read_status(*run.spec.status_url, at, error);
                const std::lock_guard lock(samples_mutex);
                if (sample) {
                    run.samples.push_back(std::move(*sample));
                } else {
                    run.status_error = error;
                    run.samples.push_back({.seconds = at});
                }
            }
            std::this_thread::sleep_until(next);
        }
    });

    std::optional<BurstSource> source;
    if (stream) {
        source.emplace(*stream, passes);
    }
    std::optional<Ac4BurstSource> ac4_source;
    if (ac4_programme) {
        ac4_source.emplace(*ac4_programme, passes);
    }
    std::uint64_t sent = 0;
    // The reference sink's view of the lyrics, sampled with the players' checks: the last line it had
    // the sound in, where, and how many samples were on a line at all.
    std::string reference_line;
    std::int64_t reference_position_ms = 0;
    std::uint64_t reference_frames = 0;
    // The programme frame after the last burst sent.
    std::int64_t end_frame = 0;
    std::string failure;
    std::optional<ss::Group::Burst> burst;
    std::string error;
    // Each player still there, and what it last said of its decoder and levels.
    const auto check_players = [&] {
        for (PlayerRun& run : runs) {
            if (run.spec.hold) {
                continue;
            }
            const std::optional<ss::ClientView> view = (*host)->client(run.client_id);
            if (events.gone(run.client_id) || !view) {
                failure = run.spec.label + " went away during the play";
                continue;
            }
            if (view->iclforge_state) {
                if (view->iclforge_state->decoder) {
                    run.decoder = view->iclforge_state->decoder;
                }
                if (view->iclforge_state->levels && !view->iclforge_state->levels->empty()) {
                    run.levels = *view->iclforge_state->levels;
                }
            }
        }
    };
    const SteadyClock::time_point play_deadline =
        SteadyClock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(programme_seconds * 1500.0)) + 60s;
    SteadyClock::time_point next_check = SteadyClock::now();
    // The tone: 440 Hz left and 660 Hz right at -18 dBFS, in blocks of 20 ms, the first
    // sample of each block continuing the last of the one before.
    constexpr std::size_t kToneBlockFrames = kSampleRate / 50;
    const auto tone_frames = static_cast<std::uint64_t>(tone ? *seconds : 0) * kSampleRate;
    std::vector<std::int32_t> tone_block(kToneBlockFrames * 2);
    std::uint64_t tone_next = 0;
    bool tone_block_ready = false;
    // --stall-after: the pushes stop for --stall-ms from that time in the play.
    std::optional<SteadyClock::time_point> stall_until;
    bool stalled = false;
    while (failure.empty()) {
        if (meta_title && then_after && !then_said &&
            SteadyClock::now() >= play_started + std::chrono::seconds(*then_after)) {
            then_said = true;
            say_song(then_title.empty() ? std::string("Next song") : then_title, then_artist, std::string(),
                     std::nullopt, 0);
        }
        if (stall_after && !stalled &&
            SteadyClock::now() >= play_started + std::chrono::seconds(*stall_after)) {
            stalled = true;
            stall_until = SteadyClock::now() + std::chrono::milliseconds(stall_ms);
            note("stalling for " + std::to_string(stall_ms) + " ms");
        }
        if (tone) {
            if (tone_next >= tone_frames) {
                break;
            }
            if (!tone_block_ready) {
                constexpr double kPi = 3.14159265358979323846;
                const double kAmplitude =
                    0.125 * (static_cast<double>(std::int64_t{1} << (kToneBitDepth - 1)) -
                             1.0);  // -18 dBFS
                for (std::size_t k = 0; k < kToneBlockFrames; ++k) {
                    const double t = static_cast<double>(tone_next + k) / kSampleRate;
                    tone_block[(k * 2)] = static_cast<std::int32_t>(
                        std::lround(kAmplitude * std::sin(2.0 * kPi * 440.0 * t)));
                    tone_block[(k * 2) + 1] = static_cast<std::int32_t>(
                        std::lround(kAmplitude * std::sin(2.0 * kPi * 660.0 * t)));
                }
                tone_block_ready = true;
            }
            const std::size_t taken = group->push(tone_block);
            if (taken > 0) {
                tone_next += taken;
                pcm_frames_sent += taken;
                end_frame = static_cast<std::int64_t>(tone_next);
                tone_block_ready = false;
            } else {
                std::this_thread::sleep_for(5ms);
            }
        } else {
            if (!burst) {
                burst = ac4_source ? ac4_source->next(error) : source->next(error);
                if (!burst) {
                    break;
                }
            }
            if (stall_until && SteadyClock::now() < *stall_until) {
                std::this_thread::sleep_for(5ms);
            } else if (group->push_burst(*burst)) {
                ++sent;
                end_frame = burst->frame + burst->frames;
                send_anchor(burst->frame, burst->frames);
                burst.reset();
            } else {
                std::this_thread::sleep_for(5ms);
            }
        }
        if (SteadyClock::now() >= next_check) {
            next_check = SteadyClock::now() + 250ms;
            check_players();
            if (sink && lyrics_sent) {
                const auto frame = sink->lyric_frame();
                if (frame.mode == iclforge::lyrics::StageMode::kLine) {
                    reference_line = frame.current.text;
                    reference_position_ms = frame.position_ms;
                    ++reference_frames;
                }
            }
            if (SteadyClock::now() >= play_deadline) {
                failure = "the play took too long: the group stopped taking bursts";
            }
        }
    }
    if (failure.empty() && !error.empty()) {
        failure = error;
    }
    const std::optional<std::int64_t> start_server_us = group->start_time();
    // The group sends ahead of play, and stream/end has a player drop what it has not played yet
    // (planning/hearth-sendspin-extension.md, stream/end): the end waits for the last burst to have
    // played, and for the players' output queues after it.
    if (failure.empty() && start_server_us) {
        const std::int64_t played_out_us = *start_server_us + (end_frame * 1'000'000 / kSampleRate) + kEndMarginUs;
        const ss::SteadyClock server_clock;
        while (failure.empty() && server_clock.now_us() < played_out_us) {
            std::this_thread::sleep_for(250ms);
            check_players();
        }
    }
    group->stop();
    if (sink && lyrics_sent) {
        note("reference sink: " + std::to_string(reference_frames) + " lyric frame(s) on a line, the last at " +
             std::to_string(reference_position_ms) + " ms: \"" + reference_line + "\"");
    }

    // Every player has played every burst, or has stopped counting.
    if (failure.empty()) {
        const SteadyClock::time_point settle = SteadyClock::now() + 30s;
        std::map<std::string, std::pair<std::uint64_t, SteadyClock::time_point>> last;
        while (SteadyClock::now() < settle) {
            bool done = true;
            for (const PlayerRun& run : runs) {
                const std::optional<ss::ClientView> view =
                    run.spec.hold ? std::nullopt : (*host)->client(run.client_id);
                if (!view || !view->iclforge_state) {
                    continue;
                }
                const ac::Counters& counters = view->iclforge_state->counters;
                const std::uint64_t accounted = counters.bursts_played + counters.late_chunks + counters.dropped_chunks;
                auto& [count, since] = last[run.client_id];
                if (accounted != count) {
                    count = accounted;
                    since = SteadyClock::now();
                }
                if (accounted < sent && SteadyClock::now() - since < 3s) {
                    done = false;
                }
            }
            if (done) {
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
        std::this_thread::sleep_for(1500ms);
    }
    playing.store(false);
    poller.join();
    const double played = std::chrono::duration<double>(SteadyClock::now() - play_started).count();
    // A board's own totals for the stream, which it writes to its log as the stream ends.
    for (PlayerRun& run : runs) {
        if (run.spec.status_url && !run.spec.hold) {
            run.board_summary = read_board_summary(*run.spec.status_url);
        }
    }

    // What each player reported of the play.
    for (PlayerRun& run : runs) {
        if (run.spec.hold) {
            continue;
        }
        const std::optional<ss::ClientView> view = (*host)->client(run.client_id);
        if (run.spec.pcm) {
            // player@v1 reports no counters of its own; a board's GET /status does, when it was
            // given one (--status). Without it the play is judged only by the player staying.
            if (!view && failure.empty()) {
                failure = run.spec.label + " went away at the end";
            }
            std::int64_t underruns = 0;
            std::int64_t late = 0;
            std::int64_t dropped = 0;
            std::int64_t invalid = 0;
            for (const StatusSample& sample : run.samples) {
                underruns = std::max(underruns, sample.underruns);
                late = std::max(late, sample.late);
                dropped = std::max(dropped, sample.dropped);
                invalid = std::max(invalid, sample.invalid);
            }
            if (run.spec.status_url) {
                note(run.spec.label + ": " + std::to_string(pcm_frames_sent) + " frames sent, " +
                     std::to_string(underruns) + " underruns, " + std::to_string(late) + " late, " +
                     std::to_string(dropped) + " dropped, " + std::to_string(invalid) + " invalid" +
                     (run.board_summary ? "; the board: " + *run.board_summary : std::string{}));
                if (failure.empty() && (underruns > 0 || late > 0 || dropped > 0 || invalid > 0)) {
                    failure = run.spec.label +
                              " did not play every chunk: " + std::to_string(underruns) +
                              " underruns, " + std::to_string(late) + " late, " +
                              std::to_string(dropped) + " dropped, " + std::to_string(invalid) +
                              " invalid";
                }
            }
            continue;
        }
        if (!view || !view->iclforge_state) {
            if (failure.empty()) {
                failure = run.spec.label + " reported nothing at the end";
            }
            continue;
        }
        const ac::Counters& counters = view->iclforge_state->counters;
        note(run.spec.label + ": " + std::to_string(counters.bursts_played) + " bursts played of " +
            std::to_string(sent) + ", " + std::to_string(counters.underruns) + " underruns, " +
            std::to_string(counters.late_chunks) + " late, " + std::to_string(counters.dropped_chunks) + " dropped, " +
            std::to_string(counters.invalid_chunks) + " invalid");
        if (failure.empty() && counters.invalid_chunks > 0) {
            failure = run.spec.label + " found invalid chunks";
        }
        if (failure.empty() && counters.bursts_played + counters.late_chunks + counters.dropped_chunks < sent) {
            failure = run.spec.label + " did not account for every burst sent";
        }
        // The whole programme, played: a burst dropped for want of room, or
        // too late to play, is a gap in what was heard.
        if (failure.empty() && (counters.late_chunks > 0 || counters.dropped_chunks > 0)) {
            failure = run.spec.label + " did not play every burst: " + std::to_string(counters.late_chunks) +
                      " late, " + std::to_string(counters.dropped_chunks) + " dropped with no room for them";
        }
        if (failure.empty() && counters.underruns > 0) {
            failure = run.spec.label + " ran out of audio " + std::to_string(counters.underruns) + " time(s)";
        }
    }
    group.reset();
    if (!failure.empty()) {
        return finish(kExitPlay, "failed: " + failure, start_server_us, sent, played);
    }
    return finish(EXIT_SUCCESS, "pass", start_server_us, sent, played);
}
