#include "iclforge/lyrics/midi_melody.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

// The notes of a MIDI karaoke file, and which track is the melody (see midi_melody.hpp).
//
// A small Standard MIDI File reader of its own, for the events this needs: tempo, the words, and
// note on and off. parse_midi_karaoke() in lyrics.cpp reads the same container for the words; the
// two are kept apart so that the words' reader is not touched here.

namespace iclforge::lyrics {

namespace {

constexpr int kPercussionChannel = 9;
constexpr std::size_t kMinimumNotes = 8;
constexpr std::size_t kMinimumSyllables = 4;

struct Cursor {
    std::string_view bytes;
    std::size_t at = 0;
    bool bad = false;

    [[nodiscard]] bool done() const { return at >= bytes.size(); }

    std::uint8_t u8() {
        if (at >= bytes.size()) {
            bad = true;
            return 0;
        }
        return static_cast<std::uint8_t>(bytes[at++]);
    }

    std::uint32_t u16() {
        const std::uint32_t hi = u8();
        return (hi << 8) | u8();
    }

    std::uint32_t u32() {
        const std::uint32_t hi = u16();
        return (hi << 16) | u16();
    }

    // A variable-length quantity: at most four bytes.
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = u8();
            value = (value << 7) | (b & 0x7FU);
            if ((b & 0x80U) == 0) {
                return value;
            }
        }
        bad = true;
        return value;
    }

    std::string_view take(std::size_t n) {
        if (n > bytes.size() - at) {
            bad = true;
            at = bytes.size();
            return {};
        }
        const std::string_view out = bytes.substr(at, n);
        at += n;
        return out;
    }
};

struct TempoChange {
    std::uint64_t tick = 0;
    std::uint32_t microseconds_per_quarter = 500000;
};

struct RawNote {
    std::uint64_t start_tick = 0;
    std::uint64_t end_tick = 0;
    int pitch = 0;
};

struct Syllable {
    std::uint64_t tick = 0;
};

struct Candidate {
    std::vector<RawNote> notes;
};

bool is_marker_only(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return c == '/' || c == '\\' || c == ' ' || c == '\r' || c == '\n' || c == '\t';
    });
}

// Ticks to milliseconds through a tempo map sorted by tick.
std::int64_t to_ms(std::uint64_t tick, const std::vector<TempoChange>& tempos,
                   std::uint32_t ticks_per_quarter) {
    double micros = 0.0;
    std::uint64_t at = 0;
    std::uint32_t tempo = 500000;
    for (const TempoChange& change : tempos) {
        if (change.tick >= tick) {
            break;
        }
        micros += static_cast<double>(change.tick - at) * static_cast<double>(tempo) /
                  static_cast<double>(ticks_per_quarter);
        at = change.tick;
        tempo = change.microseconds_per_quarter;
    }
    micros += static_cast<double>(tick - at) * static_cast<double>(tempo) /
              static_cast<double>(ticks_per_quarter);
    return static_cast<std::int64_t>(micros / 1000.0 + 0.5);
}

}  // namespace

std::expected<Melody, std::string> read_midi_melody(std::string_view bytes,
                                                    std::int64_t tolerance_ms) {
    const std::string damaged = "The MIDI file is damaged.";
    if (bytes.size() < 14 || bytes.substr(0, 4) != "MThd") {
        return std::unexpected(std::string{"The file is not a MIDI file."});
    }
    Cursor header{bytes};
    header.at = 4;
    const std::uint32_t header_length = header.u32();
    header.u16();  // the format: 0 and 1 and 2 are all read the same way here
    const std::uint32_t track_count = header.u16();
    const std::uint32_t division = header.u16();
    if (header.bad || header_length < 6) {
        return std::unexpected(damaged);
    }
    if ((division & 0x8000U) != 0) {
        return std::unexpected(
            std::string{"The MIDI file counts time in SMPTE frames, which is not read."});
    }
    const std::uint32_t ticks_per_quarter = division;
    if (ticks_per_quarter == 0) {
        return std::unexpected(damaged);
    }

    Cursor cursor{bytes};
    cursor.at = 8 + header_length;

    std::vector<TempoChange> tempos;
    std::vector<Syllable> lyric_syllables;  // Lyric events (0x05)
    std::vector<Syllable> text_syllables;   // Text events (0x01) that are not "@" lines
    // (track, channel) -> its notes
    std::map<std::pair<std::size_t, int>, Candidate> candidates;

    for (std::uint32_t track = 0; track < track_count && !cursor.done(); ++track) {
        if (cursor.take(4) != "MTrk" || cursor.bad) {
            return std::unexpected(damaged);
        }
        const std::uint32_t length = cursor.u32();
        if (cursor.bad || length > bytes.size() - cursor.at) {
            return std::unexpected(damaged);
        }
        Cursor events{cursor.bytes.substr(cursor.at, length)};
        cursor.at += length;

        std::uint64_t tick = 0;
        int running = -1;
        // The note-on that has not yet been ended, by channel and pitch (index into the channel's
        // notes).
        std::map<std::pair<int, int>, std::size_t> open;
        while (!events.done()) {
            tick += events.vlq();
            const std::uint8_t status = events.u8();
            if (events.bad) {
                return std::unexpected(damaged);
            }
            if (status == 0xFF) {
                running = -1;
                const std::uint8_t type = events.u8();
                const std::uint32_t meta_length = events.vlq();
                const std::string_view data = events.take(meta_length);
                if (events.bad) {
                    return std::unexpected(damaged);
                }
                if (type == 0x51 && data.size() == 3) {
                    const std::uint32_t tempo =
                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0])) << 16) |
                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1])) << 8) |
                        static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2]));
                    if (tempo > 0) {
                        tempos.push_back({tick, tempo});
                    }
                } else if (type == 0x05 && !data.empty() && !is_marker_only(data)) {
                    lyric_syllables.push_back({tick});
                } else if (type == 0x01 && !data.empty() && data[0] != '@' &&
                           !is_marker_only(data)) {
                    text_syllables.push_back({tick});
                } else if (type == 0x2F) {
                    break;  // end of track
                }
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                running = -1;
                const std::uint32_t sysex_length = events.vlq();
                events.take(sysex_length);
                if (events.bad) {
                    return std::unexpected(damaged);
                }
                continue;
            }
            // A channel message. Under running status the byte just read is the first data byte
            // of a message with the previous status.
            int d1 = -1;
            if ((status & 0x80U) != 0) {
                running = status;
            } else {
                if (running < 0) {
                    return std::unexpected(damaged);
                }
                d1 = status;
            }
            const int kind = running & 0xF0;
            const int channel = running & 0x0F;
            if (d1 < 0) {
                d1 = events.u8();
            }
            // One data byte for a program change and a channel pressure, two for the rest.
            int d2 = 0;
            if (kind != 0xC0 && kind != 0xD0) {
                d2 = events.u8();
            }
            if (events.bad) {
                return std::unexpected(damaged);
            }
            if (kind == 0x90 && d2 > 0) {
                Candidate& c = candidates[{track, channel}];
                c.notes.push_back({tick, tick, d1});
                open[{channel, d1}] = c.notes.size() - 1U;
            } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                const auto it = open.find({channel, d1});
                if (it != open.end()) {
                    candidates[{track, channel}].notes[it->second].end_tick = tick;
                    open.erase(it);
                }
            }
        }
        // A note never ended runs to the end of its track.
        for (const auto& [key, index] : open) {
            RawNote& note = candidates[{track, key.first}].notes[index];
            if (note.end_tick <= note.start_tick) {
                note.end_tick = tick;
            }
        }
    }
    if (cursor.bad) {
        return std::unexpected(damaged);
    }

    std::sort(tempos.begin(), tempos.end(),
              [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });

    // The words: Lyric events where the file has them, else its Text events.
    std::vector<Syllable>& syllables = lyric_syllables.empty() ? text_syllables : lyric_syllables;
    if (syllables.size() < kMinimumSyllables) {
        return std::unexpected(
            std::string{"The MIDI file has no words to tell which track is the melody."});
    }
    std::vector<std::int64_t> syllable_ms;
    syllable_ms.reserve(syllables.size());
    for (const Syllable& s : syllables) {
        syllable_ms.push_back(to_ms(s.tick, tempos, ticks_per_quarter));
    }
    std::sort(syllable_ms.begin(), syllable_ms.end());

    struct Best {
        std::map<std::pair<std::size_t, int>, Candidate>::const_iterator at;
        double match = -1.0;
        std::size_t notes = 0;
    };
    Best best{candidates.end(), -1.0, 0};
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        const auto& [key, candidate] = *it;
        if (key.second == kPercussionChannel || candidate.notes.size() < kMinimumNotes) {
            continue;
        }
        std::vector<std::int64_t> starts;
        starts.reserve(candidate.notes.size());
        for (const RawNote& n : candidate.notes) {
            starts.push_back(to_ms(n.start_tick, tempos, ticks_per_quarter));
        }
        std::sort(starts.begin(), starts.end());
        std::size_t matched = 0;
        for (const std::int64_t s : syllable_ms) {
            const auto next = std::lower_bound(starts.begin(), starts.end(), s - tolerance_ms);
            if (next != starts.end() && *next <= s + tolerance_ms) {
                ++matched;
            }
        }
        const double match = static_cast<double>(matched) / static_cast<double>(syllable_ms.size());
        // A better match wins; at an equal match the track with fewer notes is the lead line, not a
        // chordal accompaniment that happens to land on the same beats.
        if (match > best.match + 1e-9 ||
            (std::fabs(match - best.match) <= 1e-9 && candidate.notes.size() < best.notes)) {
            best = {it, match, candidate.notes.size()};
        }
    }
    if (best.at == candidates.end()) {
        return std::unexpected(
            std::string{"The MIDI file has no track with enough notes to be a melody."});
    }
    if (best.match < 0.5) {
        return std::unexpected(
            std::string{"No track of the MIDI file has its notes where the words are, so there is "
                        "no melody to use."});
    }

    Melody melody;
    melody.track = best.at->first.first;
    melody.channel = best.at->first.second;
    melody.match = best.match;
    melody.syllables = syllable_ms.size();
    for (const RawNote& n : best.at->second.notes) {
        MelodyNote note;
        note.start_ms = to_ms(n.start_tick, tempos, ticks_per_quarter);
        note.end_ms = to_ms(std::max(n.end_tick, n.start_tick), tempos, ticks_per_quarter);
        note.pitch = n.pitch;
        melody.notes.push_back(note);
    }
    std::stable_sort(
        melody.notes.begin(), melody.notes.end(),
        [](const MelodyNote& a, const MelodyNote& b) { return a.start_ms < b.start_ms; });
    return melody;
}

}  // namespace iclforge::lyrics
