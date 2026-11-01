#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/json.hpp"

// _iclforge_player@v1 (planning/hearth-sendspin-extension.md, The role _iclforge_player@v1): the
// objects the role adds to client/hello, client/state, stream/start and server/command, each a
// struct with a writer that appends it as the value of a key the caller has written, and a reader.
// Its burst chunk, ID 192 (message_id::kIclforgeBurst), is chunks.hpp's; the messages that carry
// these objects are messages.hpp's.
//
// Readers ignore unknown fields, as the specification says for every message (E14). A settings
// object is refused whole when a key it knows has a value out of range, and the reader then says
// which revision it refused and why, for the sink's settings_error.
//
// Nothing here depends on the codec library: the enumerations name what the page's text names, and
// a sink maps them onto iclforge::ac3 and iclforge::render.

namespace iclforge::sendspin::player {

inline constexpr std::string_view kRole = "_iclforge_player@v1";
inline constexpr std::string_view kSupportKey = "_iclforge_player@v1_support";
// The key of the role's object in client/state, server/command and stream/start, and its name in
// the roles of stream/clear and stream/end.
inline constexpr std::string_view kObjectKey = "_iclforge_player";

enum class DataType : std::uint8_t {
    kAc3,
    kEac3,
    // IEC 61937-14's four burst types, whichever the server's packer chose.
    kAc4,
    // TrueHD and MLP as access units, up to 24 to a chunk (a MAT frame's worth, 20 ms), not as the
    // padded MAT burst: Pc 22 and a Pd in bytes (planning/hearth-sendspin-extension.md, TrueHD).
    // Added to the data_types a sink lists, not a new version of the role.
    kTruehd,
};

// How many data types there are; the per-type limits below are indexed by DataType.
inline constexpr std::size_t kDataTypeCount = 4;

[[nodiscard]] std::string_view data_type_name(DataType type);
// The burst type a stream of `type` is sent as: for AC-4, its first, the one on
// a link at the content rate.
[[nodiscard]] BurstDataType burst_data_type(DataType type);
// Whether a burst of `burst` belongs in a stream of `type`: an AC-4 stream
// takes any of AC-4's four burst types, which differ only in the link a
// receiver would need and so in how long a frame they hold.
[[nodiscard]] bool carries(DataType type, BurstDataType burst);

// --- The support object in client/hello --------------------------------------------------------

struct Outputs {
    // At the current setting.
    std::int32_t count = 0;
    std::int32_t bit_depth = 0;
    // The widths the sink's own page can set.
    std::vector<std::int32_t> bit_depths;
};

struct Management {
    bool routing = false;
    std::array<double, 2> trim_db{0.0, 0.0};
    double max_delay_ms = 0.0;
    std::array<double, 2> crossover_hz{0.0, 0.0};
    bool identify = false;
};

// The decoder keys of a settings command, as a sink lists them in decoder_settings.
inline constexpr std::array<std::string_view, 16> kDecoderSettingNames{
    "mode",
    "drc_cut",
    "drc_boost",
    "heavy_compression",
    "dialnorm",
    "downmix",
    "ltrt_phase_shift",
    "mix_lfe",
    "programme",
    "objects",
    "concealment",
    "karaoke",
    "karaoke_vocals",
    "karaoke_melody_db",
    "karaoke_vocal1_db",
    "karaoke_vocal2_db",
};

// The span of karaoke_melody_db, karaoke_vocal1_db and karaoke_vocal2_db, in dB against Annex C's
// Table C.2.3: the bottom is off. These are ac3::kKaraokeMinLevelDb and kKaraokeMaxLevelDb
// (libs/ac3's output.hpp), which this library does not include; the host's sink form holds the
// two equal with a static_assert.
inline constexpr double kKaraokeLevelMinDb = -24.0;
inline constexpr double kKaraokeLevelMaxDb = 12.0;

// The longest lyric document, in bytes of text, the role carries in one message: a Noise transport
// message holds 65,518 bytes of plaintext (planning/hearth-sendspin-extension.md, Encryption), and
// a document's quotes, backslashes and line ends take a second byte each in JSON. A sink states
// its own, smaller limit in the support object.
inline constexpr std::uint32_t kMaxLyricsBytes = 60'000;

struct Support {
    std::vector<DataType> data_types;
    std::vector<std::int32_t> sample_rates;
    Outputs outputs;
    std::int32_t layout_grammar = 1;
    Management management;
    std::vector<std::string> decoder_settings;
    std::uint64_t buffer_capacity = 0;
    // The most bytes one chunk to this sink may be, from its type byte to its last byte of audio,
    // header included and the encryption's tag not: a burst chunk, and a player@v1 PCM chunk too,
    // which a sink that offers both roles takes at the same limit. 0 when the sink does not say,
    // as a sink from before the key reads, and a server then sends what the spec allows.
    std::uint32_t max_chunk_bytes = 0;
    // Per data type, the most channels (full-bandwidth and LFE) the sink decodes of a stream of
    // that type, indexed by DataType; 0 when it states none, which a server reads as "no limit
    // stated" and not as "none". A sink that refuses a wider syncframe states the figure it refuses
    // at, so a server can send that stream another way rather than find out one burst at a time.
    std::array<std::uint8_t, kDataTypeCount> max_coded_channels{};

    [[nodiscard]] std::uint8_t max_coded_channels_of(DataType type) const {
        return max_coded_channels[static_cast<std::size_t>(type)];
    }
    // Per data type, the most objects the sink reconstructs and places of a stream of that type in
    // real time, indexed by DataType. Unset when the sink states none for the type; 0 is a
    // statement, not an absence: the sink decodes the bed and does not reconstruct objects. A
    // stream with more objects than a sink states is decoded as its bed by the sink itself, so a
    // server that wants them placed sends it PCM rendered on its own side.
    std::array<std::optional<std::uint8_t>, kDataTypeCount> max_objects{};

    [[nodiscard]] std::optional<std::uint8_t> max_objects_of(DataType type) const {
        return max_objects[static_cast<std::size_t>(type)];
    }
    // Per data type, the most frames (access units, one a burst chunk) a second the sink decodes
    // of a stream of that type in real time, indexed by DataType; 0 when it states none, which a
    // server reads as "no limit stated". A stream whose frames come faster is sent another way, as
    // for max_coded_channels: an ESP32-S3 keeps up with AC-4 at 25 frames a second and falls behind
    // at 29.97, where a frame's decode, the frame-rate converter's, takes the same time. A frame
    // rate is compared as it is, so a limit of 30 takes 29.97 and 30 and one of 25 takes 24 and 25.
    std::array<std::uint16_t, kDataTypeCount> max_frame_rate{};

    [[nodiscard]] std::uint16_t max_frame_rate_of(DataType type) const {
        return max_frame_rate[static_cast<std::size_t>(type)];
    }
    // Per data type, the most coded data a sink takes of a stream of that type, in kbit/s, taken
    // over the longest burst chunk (so a peak of 24 TrueHD access units, 20 ms, is what counts, not
    // an average); 0 when it states none, which a server reads as "no limit stated". A stream whose
    // peak is higher is sent another way, as for max_coded_channels. Stated by a sink whose link or
    // ring cannot hold what the stream's declared peak rate allows.
    std::array<std::uint32_t, kDataTypeCount> max_data_rate{};

    [[nodiscard]] std::uint32_t max_data_rate_of(DataType type) const {
        return max_data_rate[static_cast<std::size_t>(type)];
    }
    // The most bytes of lyric document (an LRC text, UTF-8) the sink holds for the item playing,
    // 1 to kMaxLyricsBytes; 0 when it does not say, which is a sink that takes no lyrics. A sink
    // that states one lists the `lyrics` and `lyrics_position` commands too.
    std::uint32_t lyrics_max_bytes = 0;
    // The data types the sink reproduces karaoke for when it is told to (the `karaoke` decoder
    // setting), which a server reads before it sends such a stream as it is. Empty when it says
    // none: a sink from before the key reproduces AC-3's karaoke and nothing else, the codec its
    // path was written for, so empty reads as {kAc3} (planning/karaoke-eac3.md, D6). A sink that
    // reproduces an E-AC-3 stream's lists kEac3. A karaoke stream of a type that is not listed is
    // played as coded by the sink's own decoder, so a server that wants the reproduction sends it
    // PCM it has decoded itself, and a group hears one reproduction.
    std::vector<DataType> karaoke_codecs;
    // The decoder keys the sink may ask a server to change with a request (kRequestableKeys, a
    // subset of them), which are the singer's karaoke choices and are the server's to hold. Empty
    // when it asks for none, as a sink from before the key; a server then ignores any request from
    // it. A server only acts on a key the sink listed here (planning/hearth-android.md, D8).
    std::vector<std::string> requests;

    [[nodiscard]] bool reproduces_karaoke(DataType type) const {
        if (karaoke_codecs.empty()) {
            return type == DataType::kAc3;
        }
        for (const DataType listed : karaoke_codecs) {
            if (listed == type) {
                return true;
            }
        }
        return false;
    }
};

void write_support(json::Writer& w, const Support& support);
// Nothing when a required field is missing or of the wrong type, or data_types or sample_rates
// hold nothing this reader recognises: a server then does not activate the role.
[[nodiscard]] std::optional<Support> read_support(json::Value value);

// --- The state object in client/state ----------------------------------------------------------

enum class Command : std::uint8_t {
    kVolume,
    kMute,
    kSetOutputDelay,
    kSettings,
    kIdentify,
    // The item's lyric document, and where in the item the sound is (W7d, planning/karaoke-phase2.md).
    kLyrics,
    kLyricsPosition,
};

struct DecoderReport {
    DataType data_type = DataType::kEac3;
    // The coded audio coding mode, 0 to 7 (A/52, Table 5.8). For AC-4, the mode with the decoded
    // channels' front and surround speakers, and dialnorm 0 until the decoder reports one
    // (planning/hearth-sendspin-extension.md, State object).
    std::int32_t acmod = 0;
    bool lfe = false;
    std::int32_t substreams = 0;
    // Objects the stream carries, and whether the sink placed them.
    std::int32_t objects = 0;
    bool objects_placed = false;
    double dialnorm = 0.0;

    friend bool operator==(const DecoderReport&, const DecoderReport&) = default;
};

// Levels below this, silence included, are written as it: JSON has no minus infinity.
inline constexpr double kSilenceDb = -120.0;

struct Level {
    std::int32_t output = 0;
    double peak_db = 0.0;
    double rms_db = 0.0;
};

struct Counters {
    std::uint64_t bursts_played = 0;
    std::uint64_t underruns = 0;
    std::uint64_t late_chunks = 0;
    std::uint64_t dropped_chunks = 0;
    std::uint64_t invalid_chunks = 0;
};

struct SettingsError {
    std::int64_t revision = 0;
    std::string why;
};

// What the sink holds of an item's lyrics: the item's number as the server gave it, and the line the
// sink's own clock has the sound in (-1 before the first line, and for a file with no times).
struct LyricsReport {
    std::int64_t item = 0;
    std::int32_t line = -1;

    friend bool operator==(const LyricsReport&, const LyricsReport&) = default;
};

// How a stream leaves the sink (D6, planning/hearth-android.md): the coded bits go on to a receiver
// as they came ("passthrough", IEC 61937 over HDMI or S/PDIF), or the sink decoded them and plays
// the samples ("pcm"). A sink that can do both says which it is doing for the stream it has.
enum class Output : std::uint8_t {
    kPassthrough,
    kPcm,
};

[[nodiscard]] std::string_view output_name(Output output);

struct State {
    std::optional<std::int32_t> volume;
    std::optional<bool> muted;
    std::int32_t output_delay_ms = 0;
    std::int32_t required_lead_time_ms = 0;
    std::int32_t min_buffer_ms = 0;
    std::vector<Command> supported_commands;
    std::int64_t settings_revision = 0;
    std::optional<SettingsError> settings_error;
    std::optional<DecoderReport> decoder;
    std::optional<std::vector<Level>> levels;
    Counters counters;
    std::optional<std::string> why;
    // The layout in force, in the layout grammar (support.layout_grammar), as the sink's own page
    // or the last settings command made it; absent when the sink does not say. A server that
    // renders for a sink reads it here instead of keeping what it last sent.
    std::optional<std::string> layout;
    // The lyrics the sink holds, absent while it holds none. Reported when the item's document is
    // taken and whenever the line changes, which is a few times a minute and not each word.
    std::optional<LyricsReport> lyrics;
    // How the stream the sink has leaves it, while it has one. Absent with no stream, and for a sink
    // that does not say (every sink from before the key), which a server reads as "not known" and
    // not as either value. A value a reader does not know reads as absent.
    std::optional<Output> output;
};

void write_state(json::Writer& w, const State& state);
[[nodiscard]] std::optional<State> read_state(json::Value value);

// --- The object in stream/start ----------------------------------------------------------------

struct StreamStart {
    DataType data_type = DataType::kEac3;
    std::int32_t sample_rate = 48000;
};

void write_stream_start(json::Writer& w, const StreamStart& start);
[[nodiscard]] std::optional<StreamStart> read_stream_start(json::Value value);

// --- The object in server/command --------------------------------------------------------------

enum class DecoderMode : std::uint8_t { kLine, kRf, kCustom };
enum class Downmix : std::uint8_t { kLoRo, kLtRt };
enum class ObjectsPolicy : std::uint8_t { kAuto, kAlways, kNever };
enum class Concealment : std::uint8_t { kNone, kRepeatFade, kMute };
// Annex C of ETSI TS 102 366, for an AC-3 stream flagged bsmod 7 (a karaoke stream): "off" plays
// the coded channels, "aware" reproduces L, C and R with the vocals at the stream's own level, and
// "capable" lets `karaoke_vocals` choose which of the stream's vocals are heard. A sink that does
// not list the keys plays a karaoke stream as coded, which is why a server that wants the
// reproduction sends such a sink PCM it has decoded itself.
enum class Karaoke : std::uint8_t { kOff, kAware, kCapable };
// Which vocals the capable reproduction plays; ignored by the other two.
enum class KaraokeVocals : std::uint8_t { kNone, kV1, kV2, kBoth };

// The decoder keys of a settings command; each absent key leaves the library's default.
struct DecoderSettings {
    std::optional<DecoderMode> mode;
    std::optional<double> drc_cut;
    std::optional<double> drc_boost;
    std::optional<bool> heavy_compression;
    std::optional<bool> dialnorm;
    std::optional<Downmix> downmix;
    std::optional<bool> ltrt_phase_shift;
    std::optional<bool> mix_lfe;
    // Absent, null (the stream's own choice), or a programme number.
    bool has_programme = false;
    std::optional<std::int32_t> programme;
    std::optional<ObjectsPolicy> objects;
    std::optional<Concealment> concealment;
    std::optional<Karaoke> karaoke;
    std::optional<KaraokeVocals> karaoke_vocals;
    // The capable reproduction's level of the melody and of each vocal, in dB against the table
    // (0 or absent is the table; kKaraokeLevelMinDb is off). Read by "capable" alone.
    std::optional<double> karaoke_melody_db;
    std::optional<double> karaoke_vocal1_db;
    std::optional<double> karaoke_vocal2_db;
};

// A settings command's object, which replaces the sink's settings whole.
struct Settings {
    std::int64_t revision = 0;
    std::optional<std::string> layout;
    std::optional<std::string> routing;
    std::optional<std::vector<double>> trim_db;
    std::optional<std::vector<double>> delay_ms;
    std::optional<double> crossover_hz;
    DecoderSettings decoder;
};

struct Identify {
    std::int32_t output = 0;
    double level_db = -30.0;
};

// kLyrics: the document of item `item`, an LRC text in UTF-8 (the library's write_lrc), which
// replaces the sink's. An empty text takes the item's lyrics away.
struct LyricsDocument {
    std::int64_t item = 0;
    std::string text;
};

// kLyricsPosition: where in the item the sound is, as of a time on the server's clock. The sink
// plays `position_ms` at `at_us` (the server clock, which its time filter maps to its own), and from
// there the position moves at `speed` times real time: 1 while the item plays, 0 held (paused, or
// ended). Applied when the server clock reaches `at_us`, so that a hold sent for the end of what is
// buffered does not stop the words before then.
struct LyricsAnchor {
    std::int64_t item = 0;
    std::int64_t position_ms = 0;
    std::int64_t at_us = 0;
    double speed = 1.0;
};

// --- A sink's request, in client/command --------------------------------------------------------

// The decoder keys a sink may ask for: the singer's karaoke choices, the reproduction, which vocals
// the capable one plays, and the three levels. The names are the settings command's (DecoderSettings).
inline constexpr std::array<std::string_view, 5> kRequestableKeys{
    "karaoke", "karaoke_vocals", "karaoke_melody_db", "karaoke_vocal1_db", "karaoke_vocal2_db",
};

// What a sink asks of the server's karaoke choice: any of the keys above, and only those. The
// server holds the choice (the singer's, on its Karaoke page) and applies a request as it would
// the singer's change, so that every member and every screen move together; a request is advice
// and not a command, and the sink learns what the server did from the settings it is sent.
struct Request {
    DecoderSettings decoder;
};

// `client/command`'s object `_iclforge_player`: {"command": "decoder", "decoder": {...}}.
void write_request(json::Writer& w, const Request& request);
// Nothing when `command` is not "decoder", no requestable key is present, or one holds a value out
// of range. A key that is not requestable is ignored, as every unknown key is (E14).
[[nodiscard]] std::optional<Request> read_request(json::Value value);
// The request cut to the keys `support` lists: what a server acts on. Nothing when none is left.
[[nodiscard]] std::optional<Request> accept_request(const Request& request, const Support& support);

struct CommandMessage {
    Command command = Command::kVolume;
    std::int32_t volume = 0;
    bool mute = false;
    std::int32_t output_delay_ms = 0;
    Settings settings;
    // kIdentify: the tone to play, or nothing to stop it.
    std::optional<Identify> identify;
    LyricsDocument lyrics;
    LyricsAnchor lyrics_position;
};

void write_command(json::Writer& w, const CommandMessage& command);

enum class CommandError : std::uint8_t {
    // The command, or a value it needs, is missing or of the wrong type.
    kMalformed,
    // A settings object with a known key out of range: settings_error names the revision.
    kSettingsRefused,
};

struct CommandFailure {
    CommandError error = CommandError::kMalformed;
    SettingsError settings;
};

[[nodiscard]] std::expected<CommandMessage, CommandFailure> read_command(json::Value value);

// What read_command cannot check without the sink's own support object: one trim and one delay
// per output, each inside management's range; a crossover inside its range; routing only when
// management offers it; and decoder keys only from decoder_settings. The reason for
// settings_error, or nothing when the settings fit. The layout and routing text are the sink's
// to parse, with iclforge::render.
[[nodiscard]] std::optional<std::string> check_settings(const Settings& settings,
                                                        const Support& support);

}  // namespace iclforge::sendspin::player
