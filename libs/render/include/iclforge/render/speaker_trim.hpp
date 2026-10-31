#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/layout.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/routing.hpp"

// A listener's own level and distance trim for each speaker of the room, kept by the speaker's ROLE and turned
// into the per-output trims and delays iclforge::render::TrimDelay applies (trim_delay.hpp).
//
// Why a role and not an output. The output a speaker is wired to is the board's business (the layout's slot
// order, a server's patch); where the speaker stands and how loud it is against the others is the room's. A trim
// kept against "Ls" is still the left surround's after the layout changes from 5.1 to 7.1.4, or a server patches
// it to another output, and a speaker the layout does not have costs nothing and is remembered for the day it
// comes back. A slot placed by an angle alone has no role of the standard's, so it is kept as "S" and its slot
// number.
//
// Level is in 0.5 dB steps within +-12 dB. Distance is the speaker's distance from the listener, 0.10 m to 10 m,
// in whole centimetres, and 0 is "not set": a speaker with no distance sits where the farthest speaker that has
// one does, so setting one distance alone moves nothing and each further one moves only itself. The delay a
// speaker gets is the extra distance the farthest speaker has over it, at the speed of sound: the FARTHEST speaker
// is never delayed, so the board's play-time stamp (the time the server asked for) is the farthest speaker's, and
// a group of boards stays together however its rooms are measured. A delay the board has no room for (the build's
// delay lines, TrimDelay) is cut to what it holds and said so.
//
// Standard library only and tested on the host (libs/render/tests/test_speaker_trim.cpp). The table is plain
// data of 150 bytes, for a board's settings to hold by value. NOTHING here allocates except the text, report and
// JSON helpers, which the control surface calls and a player never does.

namespace iclforge::render {

// A speaker's role as a number that is stored: 0 to 21 are the Table E2.5 locations in the order below, 22 to 37
// are the slots 1 to 16 of a layout that placed a speaker by angle. The numbers are in the settings of boards on
// shelves and are NEVER renumbered; a new role takes the next number (test_speaker_trim.cpp pins them).
namespace speaker_trim {

using RoleCode = std::uint8_t;

inline constexpr std::size_t kLocationRoles = 22;
inline constexpr std::size_t kRoles = kLocationRoles + OutputLayout::kMaxSlots;
inline constexpr RoleCode kNoRole = 255;

// The outputs a trim resolves to: a player's, Playout::kMaxOutputs.
inline constexpr std::size_t kOutputs = 16;

inline constexpr double kLevelMinDb = -12.0;
inline constexpr double kLevelMaxDb = 12.0;
inline constexpr double kLevelStepDb = 0.5;
inline constexpr int kDistanceMinCm = 10;
inline constexpr int kDistanceMaxCm = 1000;
// Metres a second, the figure an AVR's distance setting is made with.
inline constexpr double kSpeedOfSoundMps = 343.0;

inline constexpr std::array<std::string_view, kRoles> kRoleNames = {
    "L",   "C",   "R",   "Ls",  "Rs", "Lc", "Rc", "Lrs", "Rrs", "Cs", "Ts", "Lsd", "Rsd", "Lw", "Rw", "Vhl", "Vhr", "Vhc", "Lts", "Rts", "LFE2", "LFE",
    "S1",  "S2",  "S3",  "S4",  "S5", "S6", "S7", "S8",  "S9",  "S10", "S11", "S12", "S13", "S14", "S15", "S16"};

// What a person reads: "Front left" for "L".
inline constexpr std::array<std::string_view, kRoles> kRoleLabels = {
    "Front left",
    "Centre",
    "Front right",
    "Left surround",
    "Right surround",
    "Left centre",
    "Right centre",
    "Left rear surround",
    "Right rear surround",
    "Rear centre",
    "Top",
    "Left side",
    "Right side",
    "Left wide",
    "Right wide",
    "Left front height",
    "Right front height",
    "Centre height",
    "Left top",
    "Right top",
    "LFE 2",
    "Subwoofer",
    "Speaker 1",
    "Speaker 2",
    "Speaker 3",
    "Speaker 4",
    "Speaker 5",
    "Speaker 6",
    "Speaker 7",
    "Speaker 8",
    "Speaker 9",
    "Speaker 10",
    "Speaker 11",
    "Speaker 12",
    "Speaker 13",
    "Speaker 14",
    "Speaker 15",
    "Speaker 16"};

[[nodiscard]] constexpr std::string_view role_name(RoleCode role) {
    return role < kRoles ? kRoleNames[role] : std::string_view{};
}

[[nodiscard]] constexpr std::string_view role_label(RoleCode role) {
    return role < kRoles ? kRoleLabels[role] : std::string_view{};
}

[[nodiscard]] constexpr RoleCode role_of_location(iclforge::base::Location location) {
    using L = iclforge::base::Location;
    switch (location) {
        case L::kLeft: return 0;
        case L::kCentre: return 1;
        case L::kRight: return 2;
        case L::kLeftSurround: return 3;
        case L::kRightSurround: return 4;
        case L::kLc: return 5;
        case L::kRc: return 6;
        case L::kLrs: return 7;
        case L::kRrs: return 8;
        case L::kCs: return 9;
        case L::kTs: return 10;
        case L::kLsd: return 11;
        case L::kRsd: return 12;
        case L::kLw: return 13;
        case L::kRw: return 14;
        case L::kVhl: return 15;
        case L::kVhr: return 16;
        case L::kVhc: return 17;
        case L::kLts: return 18;
        case L::kRts: return 19;
        case L::kLfe2: return 20;
        case L::kLfe: return 21;
    }
    return kNoRole;
}

// The role of slot `slot` of `layout`: its location's, or the slot's own number for one placed by an angle. None
// for a slot nothing is connected to.
[[nodiscard]] inline RoleCode role_of_slot(const OutputLayout& layout, std::size_t slot) {
    if (slot >= layout.slots()) {
        return kNoRole;
    }
    const Speaker& speaker = layout.slot(slot);
    if (speaker.kind == Speaker::Kind::kEmpty) {
        return kNoRole;
    }
    if (speaker.location) {
        return role_of_location(*speaker.location);
    }
    return static_cast<RoleCode>(kLocationRoles + slot);
}

namespace detail {

[[nodiscard]] inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

[[nodiscard]] inline bool same_word(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// A decimal number: an optional sign, digits, an optional fraction. No exponent and no thousands.
[[nodiscard]] inline std::optional<double> number(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    double value = 0.0;
    std::size_t digits = 0;
    std::size_t at = 0;
    for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at, ++digits) {
        value = (value * 10.0) + static_cast<double>(text[at] - '0');
    }
    if (at < text.size() && text[at] == '.') {
        ++at;
        double scale = 0.1;
        for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at, ++digits) {
            value += scale * static_cast<double>(text[at] - '0');
            scale /= 10.0;
        }
    }
    if (digits == 0 || at != text.size()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

}  // namespace detail

// The role a word names, in any case: the standard's names ("Ls"), the slot names ("S3"), and the plain ones a
// person types ("FL", "FR", "SL", "SR", "BL", "BR", "SUB").
[[nodiscard]] inline std::optional<RoleCode> parse_role(std::string_view word) {
    word = detail::trimmed(word);
    for (std::size_t i = 0; i < kRoles; ++i) {
        if (detail::same_word(word, kRoleNames[i])) {
            return static_cast<RoleCode>(i);
        }
    }
    struct Alias {
        std::string_view word;
        RoleCode role;
    };
    constexpr std::array<Alias, 7> kAliases = {Alias{"FL", 0}, Alias{"FR", 2},  Alias{"SL", 3}, Alias{"SR", 4},
                                               Alias{"BL", 7}, Alias{"BR", 8}, Alias{"SUB", 21}};
    for (const Alias& alias : kAliases) {
        if (detail::same_word(word, alias.word)) {
            return alias.role;
        }
    }
    return std::nullopt;
}

// One role's trim. The defaults are the unset trim: 0 dB and no distance.
struct SpeakerTrim {
    // Level in half decibels, -24 to 24.
    std::int8_t half_db = 0;
    // Distance in centimetres: 0 (not set), or 10 to 1000.
    std::uint16_t cm = 0;

    [[nodiscard]] double level_db() const { return static_cast<double>(half_db) * kLevelStepDb; }
    [[nodiscard]] double distance_m() const { return static_cast<double>(cm) / 100.0; }
    [[nodiscard]] bool neutral() const { return half_db == 0 && cm == 0; }
    friend bool operator==(const SpeakerTrim&, const SpeakerTrim&) = default;
};

// The words a PUT /trim body is made of, and what is wrong with one that is not. Every message is a literal.
struct ParseResult {
    bool ok = true;
    const char* error = "";
};

class SpeakerTrimTable {
   public:
    // What to_blob() writes at most: a version, a count, and four bytes for each role that is not the default.
    static constexpr std::size_t kBlobBytes = 2 + (kRoles * 4);
    static constexpr std::uint8_t kBlobVersion = 1;

    [[nodiscard]] SpeakerTrim get(RoleCode role) const { return role < kRoles ? trims_[role] : SpeakerTrim{}; }

    // False, changing nothing, for a role that does not exist, a level outside [kLevelMinDb, kLevelMaxDb] or not a
    // multiple of kLevelStepDb (to a thousandth), NaN included.
    bool set_level_db(RoleCode role, double db) {
        if (role >= kRoles || !(db >= kLevelMinDb && db <= kLevelMaxDb)) {
            return false;
        }
        const double steps = db / kLevelStepDb;
        const double rounded = std::round(steps);
        if (std::fabs(steps - rounded) > 0.002) {
            return false;
        }
        trims_[role].half_db = static_cast<std::int8_t>(rounded);
        return true;
    }

    // The speaker's distance from the listener, to the centimetre. False, changing nothing, outside [0.10, 10] m.
    bool set_distance_m(RoleCode role, double metres) {
        if (role >= kRoles || !(metres >= kDistanceMinCm / 100.0 - 1e-9 && metres <= kDistanceMaxCm / 100.0 + 1e-9)) {
            return false;
        }
        trims_[role].cm = static_cast<std::uint16_t>(std::lround(metres * 100.0));
        return true;
    }

    void clear_distance(RoleCode role) {
        if (role < kRoles) {
            trims_[role].cm = 0;
        }
    }
    void clear(RoleCode role) {
        if (role < kRoles) {
            trims_[role] = SpeakerTrim{};
        }
    }
    void clear_all() { trims_.fill(SpeakerTrim{}); }

    // How far apart the nearest and the farthest speaker that has a distance are, in centimetres; 0 when fewer
    // than two have. Over every role, whether or not the layout in force has it: a layout that comes back finds
    // the span it was left with.
    [[nodiscard]] std::uint16_t span_cm() const {
        std::uint16_t nearest = 0xFFFF;
        std::uint16_t farthest = 0;
        for (const SpeakerTrim& t : trims_) {
            if (t.cm != 0) {
                nearest = std::min(nearest, t.cm);
                farthest = std::max(farthest, t.cm);
            }
        }
        return farthest > nearest ? static_cast<std::uint16_t>(farthest - nearest) : std::uint16_t{0};
    }

    // Every role at the default.
    [[nodiscard]] bool neutral() const {
        return std::all_of(trims_.begin(), trims_.end(), [](const SpeakerTrim& t) { return t.neutral(); });
    }

    // How many roles are not at the default: the number the Preferences row says ("3 speakers set").
    [[nodiscard]] std::size_t set_count() const {
        return static_cast<std::size_t>(
            std::count_if(trims_.begin(), trims_.end(), [](const SpeakerTrim& t) { return !t.neutral(); }));
    }

    // The stored form: only the roles that are not the default. Returns the bytes written to `out`, which must
    // hold kBlobBytes.
    [[nodiscard]] std::size_t to_blob(std::span<std::uint8_t> out) const {
        if (out.size() < kBlobBytes) {
            return 0;
        }
        std::size_t at = 2;
        std::uint8_t count = 0;
        for (std::size_t role = 0; role < kRoles; ++role) {
            const SpeakerTrim& t = trims_[role];
            if (t.neutral()) {
                continue;
            }
            out[at++] = static_cast<std::uint8_t>(role);
            out[at++] = static_cast<std::uint8_t>(t.half_db);
            out[at++] = static_cast<std::uint8_t>(t.cm & 0xFFU);
            out[at++] = static_cast<std::uint8_t>(t.cm >> 8);
            ++count;
        }
        out[0] = kBlobVersion;
        out[1] = count;
        return at;
    }

    // The table a stored form holds. False, with this table as it was, for a version or a role this reader does
    // not know, a level or distance outside the ranges, or a length that is not the count's.
    bool from_blob(std::span<const std::uint8_t> blob) {
        if (blob.size() < 2 || blob[0] != kBlobVersion || blob[1] > kRoles || blob.size() != 2 + (static_cast<std::size_t>(blob[1]) * 4)) {
            return false;
        }
        SpeakerTrimTable read;
        for (std::size_t i = 0; i < blob[1]; ++i) {
            const std::uint8_t* const entry = blob.data() + 2 + (i * 4);
            const RoleCode role = entry[0];
            const auto half_db = static_cast<std::int8_t>(entry[1]);
            const auto cm = static_cast<std::uint16_t>(entry[2] | (entry[3] << 8));
            if (role >= kRoles || half_db < -24 || half_db > 24 || (cm != 0 && (cm < kDistanceMinCm || cm > kDistanceMaxCm))) {
                return false;
            }
            read.trims_[role] = SpeakerTrim{.half_db = half_db, .cm = cm};
        }
        *this = read;
        return true;
    }

    // Changes the table by a body of statements, separated by line ends or semicolons:
    //   reset                          every speaker back to the default
    //   <role> reset                   that speaker back to the default
    //   <role> level=-1.5              its level, in dB, in 0.5 steps
    //   <role> distance=2.4            its distance, in metres (none takes it away)
    //   <role> level=-1.5 distance=2.4 both
    // Commas may stand for spaces. All or nothing: a body with anything wrong in it changes nothing.
    [[nodiscard]] ParseResult apply_text(std::string_view body) {
        SpeakerTrimTable next = *this;
        std::size_t statements = 0;
        while (!body.empty()) {
            const std::size_t end = body.find_first_of("\n;");
            const std::string_view statement = detail::trimmed(body.substr(0, end));
            body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
            if (statement.empty()) {
                continue;
            }
            ++statements;
            if (const ParseResult result = next.apply_statement(statement); !result.ok) {
                return result;
            }
        }
        if (statements == 0) {
            return {.ok = false, .error = "an empty body: say reset, or a speaker and a level or distance"};
        }
        *this = next;
        return {};
    }

    friend bool operator==(const SpeakerTrimTable&, const SpeakerTrimTable&) = default;

   private:
    [[nodiscard]] ParseResult apply_statement(std::string_view statement) {
        // Words on spaces and commas.
        std::array<std::string_view, 6> words{};
        std::size_t count = 0;
        while (!statement.empty()) {
            const std::size_t end = statement.find_first_of(" ,\t");
            const std::string_view word = statement.substr(0, end);
            statement = end == std::string_view::npos ? std::string_view{} : statement.substr(end + 1);
            if (word.empty()) {
                continue;
            }
            if (count == words.size()) {
                return {.ok = false, .error = "too many words in one statement"};
            }
            words[count++] = word;
        }
        if (count == 1 && detail::same_word(words[0], "reset")) {
            clear_all();
            return {};
        }
        const std::optional<RoleCode> role = parse_role(words[0]);
        if (!role) {
            return {.ok = false, .error = "not a speaker: use L, C, R, Ls, Rs, Lrs, Rrs, Vhl, Vhr, Lts, Rts, LFE and so on, or S1 to S16"};
        }
        if (count == 1) {
            return {.ok = false, .error = "a speaker wants reset, level=<dB> or distance=<m>"};
        }
        // Checked on a copy, so that a statement with a good word and a bad one changes nothing.
        SpeakerTrim trim = trims_[*role];
        for (std::size_t i = 1; i < count; ++i) {
            const std::string_view word = words[i];
            if (detail::same_word(word, "reset")) {
                trim = SpeakerTrim{};
                continue;
            }
            const std::size_t equals = word.find('=');
            if (equals == std::string_view::npos) {
                return {.ok = false, .error = "a speaker wants reset, level=<dB> or distance=<m>"};
            }
            const std::string_view key = word.substr(0, equals);
            std::string_view value = word.substr(equals + 1);
            if (detail::same_word(key, "level")) {
                if (value.size() >= 2 && detail::same_word(value.substr(value.size() - 2), "db")) {
                    value.remove_suffix(2);
                }
                const std::optional<double> db = detail::number(value);
                SpeakerTrimTable probe;
                if (!db || !probe.set_level_db(0, *db)) {
                    return {.ok = false, .error = "a level is -12 to 12 dB, in steps of 0.5"};
                }
                trim.half_db = probe.trims_[0].half_db;
            } else if (detail::same_word(key, "distance")) {
                if (detail::same_word(value, "none")) {
                    trim.cm = 0;
                    continue;
                }
                if (!value.empty() && (value.back() == 'm' || value.back() == 'M')) {
                    value.remove_suffix(1);
                }
                const std::optional<double> metres = detail::number(value);
                SpeakerTrimTable probe;
                if (!metres || !probe.set_distance_m(0, *metres)) {
                    return {.ok = false, .error = "a distance is 0.1 to 10 metres, or none"};
                }
                trim.cm = probe.trims_[0].cm;
            } else {
                return {.ok = false, .error = "a speaker wants reset, level=<dB> or distance=<m>"};
            }
        }
        trims_[*role] = trim;
        return {};
    }

    std::array<SpeakerTrim, kRoles> trims_{};
};

// --- what a screen's editor does with a trim ------------------------------------------------------------
// The stepping and the words of the Speakers sheet (panel/lvgl/sound_sheet.cpp), here so that the host tests hold
// them: a finger moves a level or a distance along the same grid a PUT /trim is held to, and nothing in the sheet
// can ask the table for a value it would refuse.

// The level `direction` (+1 more, -1 less) from `half_db`, in half decibels; nothing at the end of the range.
[[nodiscard]] constexpr std::optional<std::int8_t> step_half_db(std::int8_t half_db, int direction) {
    constexpr int kMostHalfDb = static_cast<int>(kLevelMaxDb / kLevelStepDb);
    const int next = static_cast<int>(half_db) + (direction > 0 ? 1 : direction < 0 ? -1 : 0);
    if (direction == 0 || next < -kMostHalfDb || next > kMostHalfDb) {
        return std::nullopt;
    }
    return static_cast<std::int8_t>(next);
}

// The distance `direction` from `cm`: exactly `step_cm` more or less, held to the range the table takes. A speaker with
// no distance (0) steps up to `seed_cm`, the distance the editor starts one at, whatever the step; stepping down from the
// nearest distance takes it away again (0), and a step that would pass the nearest stops at it first. Nothing past the
// far end.
[[nodiscard]] constexpr std::optional<std::uint16_t> step_distance_cm(std::uint16_t cm, int direction,
                                                                      std::uint16_t step_cm, std::uint16_t seed_cm) {
    if (direction == 0 || step_cm == 0) {
        return std::nullopt;
    }
    if (cm == 0) {
        if (direction < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::clamp<int>(seed_cm, kDistanceMinCm, kDistanceMaxCm));
    }
    if (direction > 0) {
        if (cm >= kDistanceMaxCm) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::min<int>(static_cast<int>(cm) + step_cm, kDistanceMaxCm));
    }
    if (cm <= kDistanceMinCm) {
        return std::uint16_t{0};
    }
    return static_cast<std::uint16_t>(std::max<int>(static_cast<int>(cm) - step_cm, kDistanceMinCm));
}

// "+1.5 dB", "-12 dB", "0 dB": the sign only on a level that is not zero, and a whole number without a point.
[[nodiscard]] inline std::string level_text(std::int8_t half_db) {
    std::array<char, 24> text{};
    const int magnitude = half_db < 0 ? -half_db : half_db;
    const char* const sign = half_db < 0 ? "-" : (half_db > 0 ? "+" : "");
    if ((magnitude % 2) == 0) {
        std::snprintf(text.data(), text.size(), "%s%d dB", sign, magnitude / 2);
    } else {
        std::snprintf(text.data(), text.size(), "%s%d.5 dB", sign, magnitude / 2);
    }
    return text.data();
}

// "2.4 m", "2.45 m", "3 m", or "not set" for 0.
[[nodiscard]] inline std::string distance_text(std::uint16_t cm) {
    if (cm == 0) {
        return "not set";
    }
    std::array<char, 24> text{};
    if ((cm % 100) == 0) {
        std::snprintf(text.data(), text.size(), "%u m", static_cast<unsigned>(cm / 100));
    } else if ((cm % 10) == 0) {
        std::snprintf(text.data(), text.size(), "%u.%u m", static_cast<unsigned>(cm / 100), static_cast<unsigned>((cm % 100) / 10));
    } else {
        std::snprintf(text.data(), text.size(), "%u.%02u m", static_cast<unsigned>(cm / 100), static_cast<unsigned>(cm % 100));
    }
    return text.data();
}

// The statements a screen hands to PUT /trim's parser (SpeakerTrimTable::apply_text): "Ls level=-1.5",
// "Ls distance=2.4" (or "Ls distance=none" for 0) and "Ls reset".
[[nodiscard]] inline std::string level_statement(RoleCode role, std::int8_t half_db) {
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%.*s level=%.1f", static_cast<int>(role_name(role).size()), role_name(role).data(),
                  static_cast<double>(half_db) * kLevelStepDb);
    return text.data();
}

[[nodiscard]] inline std::string distance_statement(RoleCode role, std::uint16_t cm) {
    std::array<char, 40> text{};
    if (cm == 0) {
        std::snprintf(text.data(), text.size(), "%.*s distance=none", static_cast<int>(role_name(role).size()), role_name(role).data());
    } else {
        std::snprintf(text.data(), text.size(), "%.*s distance=%u.%02u", static_cast<int>(role_name(role).size()),
                      role_name(role).data(), static_cast<unsigned>(cm / 100), static_cast<unsigned>(cm % 100));
    }
    return text.data();
}

[[nodiscard]] inline std::string reset_statement(RoleCode role) { return std::string(role_name(role)) + " reset"; }

// What the Preferences row says about the table: "none set", "1 speaker set", "3 speakers set".
[[nodiscard]] inline std::string summary_text(const SpeakerTrimTable& table) {
    const std::size_t count = table.set_count();
    if (count == 0) {
        return "none set";
    }
    return std::to_string(count) + (count == 1 ? " speaker set" : " speakers set");
}

// What a build can do with a trim. Level needs nothing; a delay needs a delay line for each output
// (TrimDelay::storage_floats()), which a board holds in PSRAM or does not hold.
struct TrimCaps {
    bool level = true;
    bool delay = false;
    // Samples of delay a build holds for each output, and the extra distance that is at the sample rate: how far
    // apart the nearest and the farthest speaker can be.
    std::size_t max_delay_samples = 0;
    double max_span_m = 0.0;
    std::uint16_t max_span_cm = 0;
    // Why there is no delay, when there is none.
    const char* delay_reason = "this board has no memory for delay lines";
};

[[nodiscard]] inline TrimCaps trim_caps(std::size_t max_delay_samples, std::uint32_t sample_rate_hz) {
    TrimCaps caps;
    caps.max_delay_samples = max_delay_samples;
    caps.delay = max_delay_samples > 0;
    caps.max_span_m = caps.delay ? static_cast<double>(max_delay_samples) * kSpeedOfSoundMps / static_cast<double>(std::max<std::uint32_t>(sample_rate_hz, 1)) : 0.0;
    if (caps.delay) {
        caps.delay_reason = "";
        caps.max_span_cm = static_cast<std::uint16_t>(std::min<std::uint64_t>(
            (static_cast<std::uint64_t>(max_delay_samples) * 34300U) / std::max<std::uint32_t>(sample_rate_hz, 1), 0xFFFFU));
    }
    return caps;
}

// Samples for `extra_cm` centimetres of extra distance at `sample_rate_hz`, to the nearest.
[[nodiscard]] constexpr std::uint32_t delay_samples_for(std::uint32_t extra_cm, std::uint32_t sample_rate_hz) {
    const std::uint64_t numerator = static_cast<std::uint64_t>(extra_cm) * sample_rate_hz * 2U;
    constexpr std::uint64_t kCmPerSecondTimesTwo = 2U * 34300U;
    return static_cast<std::uint32_t>((numerator + (kCmPerSecondTimesTwo / 2U)) / kCmPerSecondTimesTwo);
}

// What the table comes to for a layout, patched to a player's outputs.
struct TrimResolved {
    // The role at each output, or none for an output no speaker is patched to.
    std::array<RoleCode, kOutputs> role{};
    std::array<double, kOutputs> level_db{};
    std::array<std::uint32_t, kOutputs> delay{};
    // The outputs whose delay was cut to what the build holds.
    std::uint32_t clamped = 0;
    // The distance the delays are counted from, in centimetres; 0 when no speaker of the layout has one.
    std::uint16_t farthest_cm = 0;

    TrimResolved() { role.fill(kNoRole); }

    [[nodiscard]] bool neutral() const {
        for (std::size_t o = 0; o < kOutputs; ++o) {
            if (level_db[o] != 0.0 || delay[o] != 0) {
                return false;
            }
        }
        return true;
    }
};

// The role at each output `routing` patches a slot of `layout` to. Slots that are not connected, channels the
// patch leaves out and outputs past kOutputs have none.
[[nodiscard]] inline std::array<RoleCode, kOutputs> roles_of_outputs(const OutputLayout& layout, const Routing& routing) {
    std::array<RoleCode, kOutputs> roles{};
    roles.fill(kNoRole);
    const std::size_t channels = std::min(layout.slots(), routing.channels());
    for (std::size_t c = 0; c < channels; ++c) {
        const int output = routing.output_of(c);
        const RoleCode role = role_of_slot(layout, c);
        if (output >= 0 && static_cast<std::size_t>(output) < kOutputs && role != kNoRole) {
            roles[static_cast<std::size_t>(output)] = role;
        }
    }
    return roles;
}

// The trims and delays of the table for the speakers at `roles`; an output with no role has the neutral trim.
// `max_delay_samples` is what the build's delay lines hold (0 for none: no delay at all).
[[nodiscard]] inline TrimResolved resolve_trim(const std::array<RoleCode, kOutputs>& roles, const SpeakerTrimTable& table,
                                               std::uint32_t sample_rate_hz, std::size_t max_delay_samples) {
    TrimResolved out;
    out.role = roles;
    std::array<int, kOutputs> cm{};
    std::uint16_t farthest = 0;
    for (std::size_t o = 0; o < kOutputs; ++o) {
        if (roles[o] == kNoRole) {
            continue;
        }
        const SpeakerTrim trim = table.get(roles[o]);
        out.level_db[o] = trim.level_db();
        cm[o] = trim.cm;
        farthest = std::max(farthest, trim.cm);
    }
    out.farthest_cm = farthest;
    for (std::size_t o = 0; o < kOutputs; ++o) {
        if (roles[o] == kNoRole || cm[o] == 0) {
            continue;
        }
        const std::uint32_t wanted = delay_samples_for(static_cast<std::uint32_t>(farthest - cm[o]), sample_rate_hz);
        const auto held = static_cast<std::uint32_t>(std::min<std::size_t>(max_delay_samples, 0xFFFFFFFFU));
        if (wanted > held) {
            out.delay[o] = held;
            out.clamped |= std::uint32_t{1} << o;
        } else {
            out.delay[o] = wanted;
        }
    }
    return out;
}

// resolve_trim() for the patch of `routing` from `layout`.
[[nodiscard]] inline TrimResolved resolve_trim(const OutputLayout& layout, const Routing& routing, const SpeakerTrimTable& table,
                                               std::uint32_t sample_rate_hz, std::size_t max_delay_samples) {
    return resolve_trim(roles_of_outputs(layout, routing), table, sample_rate_hz, max_delay_samples);
}

// --- GET /trim -----------------------------------------------------------------------------------------

struct TrimRow {
    RoleCode role = kNoRole;
    int output = -1;
    // What the table says.
    double level_db = 0.0;
    double distance_m = 0.0;
    // What is in force at this output (a server's settings can differ from the table).
    double applied_level_db = 0.0;
    std::uint32_t applied_delay_samples = 0;
    bool delay_clamped = false;
};

struct TrimReport {
    std::string_view layout;
    std::uint32_t sample_rate_hz = 48000;
    TrimCaps caps;
    // The player's server has set the levels, or the delays, in force: the table is not what plays.
    bool server_level = false;
    bool server_delay = false;
    double farthest_m = 0.0;
    std::vector<TrimRow> rows;
};

namespace detail {

inline void append_json_text(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    out += '"';
}

inline void append_fixed(std::string& out, double value, int decimals) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.*f", decimals, value);
    out += text.data();
}

inline void append_unsigned(std::string& out, unsigned long long value) {
    std::array<char, 24> text{};
    std::snprintf(text.data(), text.size(), "%llu", value);
    out += text.data();
}

}  // namespace detail

// The report as JSON, for GET /trim:
//   {"layout":"5.1","level":{"available":true,"min_db":-12,"max_db":12,"step_db":0.5},
//    "delay":{"available":true,"min_m":0.1,"max_m":10,"max_span_m":10.29,"samples":1440,"reason":""},
//    "server_level":false,"server_delay":false,"farthest_m":2.4,
//    "speakers":[{"role":"L","label":"Front left","output":0,"level_db":-1.5,"distance_m":2.4,
//                 "applied_level_db":-1.5,"delay_samples":0,"delay_ms":0.0,"clamped":false}, ...]}
[[nodiscard]] inline std::string trim_json(const TrimReport& report) {
    std::string out = "{\"layout\":";
    detail::append_json_text(out, report.layout);
    out += ",\"level\":{\"available\":";
    out += report.caps.level ? "true" : "false";
    out += ",\"min_db\":";
    detail::append_fixed(out, kLevelMinDb, 1);
    out += ",\"max_db\":";
    detail::append_fixed(out, kLevelMaxDb, 1);
    out += ",\"step_db\":";
    detail::append_fixed(out, kLevelStepDb, 1);
    out += "},\"delay\":{\"available\":";
    out += report.caps.delay ? "true" : "false";
    out += ",\"min_m\":";
    detail::append_fixed(out, kDistanceMinCm / 100.0, 2);
    out += ",\"max_m\":";
    detail::append_fixed(out, kDistanceMaxCm / 100.0, 2);
    out += ",\"max_span_m\":";
    detail::append_fixed(out, report.caps.max_span_m, 2);
    out += ",\"samples\":";
    detail::append_unsigned(out, report.caps.max_delay_samples);
    out += ",\"reason\":";
    detail::append_json_text(out, report.caps.delay ? "" : report.caps.delay_reason);
    out += "},\"server_level\":";
    out += report.server_level ? "true" : "false";
    out += ",\"server_delay\":";
    out += report.server_delay ? "true" : "false";
    out += ",\"farthest_m\":";
    detail::append_fixed(out, report.farthest_m, 2);
    out += ",\"speakers\":[";
    bool first = true;
    for (const TrimRow& row : report.rows) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"role\":";
        detail::append_json_text(out, role_name(row.role));
        out += ",\"label\":";
        detail::append_json_text(out, role_label(row.role));
        out += ",\"output\":";
        detail::append_unsigned(out, static_cast<unsigned long long>(std::max(row.output, 0)));
        out += ",\"level_db\":";
        detail::append_fixed(out, row.level_db, 1);
        out += ",\"distance_m\":";
        detail::append_fixed(out, row.distance_m, 2);
        out += ",\"applied_level_db\":";
        detail::append_fixed(out, row.applied_level_db, 1);
        out += ",\"delay_samples\":";
        detail::append_unsigned(out, row.applied_delay_samples);
        out += ",\"delay_ms\":";
        detail::append_fixed(out, static_cast<double>(row.applied_delay_samples) * 1000.0 / static_cast<double>(std::max<std::uint32_t>(report.sample_rate_hz, 1)), 2);
        out += ",\"clamped\":";
        out += row.delay_clamped ? "true" : "false";
        out += '}';
    }
    out += "]}";
    return out;
}

}  // namespace speaker_trim

using speaker_trim::SpeakerTrimTable;

}  // namespace iclforge::render
