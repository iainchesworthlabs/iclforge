#include "karaoke_authoring.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace forge_gui {

namespace {

using iclforge::ac3::Acmod;

// Two parts whose lengths differ by less than this are the same length: a
// recording's tail is rarely cut to the frame, and the encoder's own padding
// is the AC-3 frame (1536 samples, 32 ms at 48 kHz).
constexpr double kLengthToleranceSeconds = 0.05;

std::string number(std::size_t value) {
    return std::to_string(value);
}

// "48" and "44.1": the way the window prints a rate in kHz.
std::string khz(std::uint32_t hz) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", static_cast<double>(hz) / 1000.0);
    std::string out = text;
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0) {
        out.resize(out.size() - 2);
    }
    return out;
}

std::string mode_name(Acmod acmod) {
    switch (acmod) {
        case Acmod::k3_0: return "3/0";
        case Acmod::k2_1: return "2/1";
        case Acmod::k3_1: return "3/1";
        case Acmod::k2_2: return "2/2";
        case Acmod::k3_2: return "3/2";
        default: return {};
    }
}

const KaraokeFile* find_part(std::span<const KaraokeFile> files, KaraokePart part, std::size_t* index) {
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].part == part) {
            if (index != nullptr) {
                *index = i;
            }
            return &files[i];
        }
    }
    return nullptr;
}

void add(KaraokePlan& plan, KaraokeCheckLevel level, std::string code, std::vector<std::string> args = {}) {
    plan.checks.push_back(KaraokeCheck{level, std::move(code), std::move(args)});
}

// Table C.2.1: which mode a set of parts is, or nothing where Annex C has none
// (a backing alone is plain stereo, with nothing to sing along to).
bool mode_for(bool melody, int vocals, Acmod* acmod) {
    if (vocals == 0) {
        if (!melody) {
            return false;
        }
        *acmod = Acmod::k3_0;
    } else if (vocals == 1) {
        *acmod = melody ? Acmod::k3_1 : Acmod::k2_1;
    } else {
        *acmod = melody ? Acmod::k3_2 : Acmod::k2_2;
    }
    return true;
}

// The five modes' speakers in coded order, with the part each carries.
struct ModeRow {
    Acmod acmod;
    std::array<std::string_view, 5> positions;
    std::array<std::string_view, 5> parts;
    std::size_t count;
};

constexpr std::array<ModeRow, 5> kModes{{
    {Acmod::k3_0, {"L", "C", "R", "", ""}, {"L", "M", "R", "", ""}, 3},
    {Acmod::k2_1, {"L", "R", "Cs", "", ""}, {"L", "R", "V1", "", ""}, 3},
    {Acmod::k3_1, {"L", "C", "R", "Cs", ""}, {"L", "M", "R", "V1", ""}, 4},
    {Acmod::k2_2, {"L", "R", "Ls", "Rs", ""}, {"L", "R", "V1", "V2", ""}, 4},
    {Acmod::k3_2, {"L", "C", "R", "Ls", "Rs"}, {"L", "M", "R", "V1", "V2"}, 5},
}};

const ModeRow& row_for(Acmod acmod) {
    for (const ModeRow& row : kModes) {
        if (row.acmod == acmod) {
            return row;
        }
    }
    return kModes.front();
}

std::string join(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& word : words) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

}  // namespace

bool KaraokePlan::has_problem() const {
    return std::any_of(checks.begin(), checks.end(),
                       [](const KaraokeCheck& c) { return c.level == KaraokeCheckLevel::kProblem; });
}

int karaoke_default_bitrate_kbps(const std::string& mode) {
    // About 77 kbps a channel: 3/2's five is the sheet's 384.
    if (mode == "3/2") return 384;
    if (mode == "3/1" || mode == "2/2") return 320;
    if (mode == "3/0" || mode == "2/1") return 256;
    return 192;
}

namespace {

bool is_lrc_extension(const std::filesystem::path& extension) {
    const std::string text = extension.string();
    return text.size() == 4 && text[0] == '.' &&
           std::tolower(static_cast<unsigned char>(text[1])) == 'l' &&
           std::tolower(static_cast<unsigned char>(text[2])) == 'r' &&
           std::tolower(static_cast<unsigned char>(text[3])) == 'c';
}

}  // namespace

std::string karaoke_lyrics_beside(const std::string& media_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path media(media_path);
    const fs::path folder = media.has_parent_path() ? media.parent_path() : fs::path(".");
    const std::string stem = media.stem().string();
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& candidate = it->path();
        if (is_lrc_extension(candidate.extension()) && candidate.stem().string() == stem &&
            it->is_regular_file(ec)) {
            return candidate.string();
        }
    }
    return {};
}

KaraokeLyricsCopy karaoke_copy_lyrics(const std::string& lyrics_path, const std::string& stream_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path source(lyrics_path);
    if (!fs::is_regular_file(source, ec)) {
        return KaraokeLyricsCopy::kUnreadable;
    }
    fs::path target(stream_path);
    target.replace_extension(".lrc");
    if (fs::exists(target, ec)) {
        return fs::equivalent(source, target, ec) ? KaraokeLyricsCopy::kAlreadyThere
                                                  : KaraokeLyricsCopy::kExists;
    }
    return fs::copy_file(source, target, fs::copy_options::none, ec) && !ec
               ? KaraokeLyricsCopy::kCopied
               : KaraokeLyricsCopy::kFailed;
}

std::string karaoke_duration_text(double seconds) {
    const auto whole = static_cast<long long>(std::max(0.0, seconds));
    char text[32];
    std::snprintf(text, sizeof text, "%lld:%02lld", whole / 60, whole % 60);
    return text;
}

KaraokePlan plan_karaoke_parts(std::span<const KaraokeFile> files) {
    KaraokePlan plan;
    std::size_t backing_at = 0;
    std::size_t melody_at = 0;
    std::size_t vocal1_at = 0;
    std::size_t vocal2_at = 0;
    const KaraokeFile* backing = find_part(files, KaraokePart::kBacking, &backing_at);
    const KaraokeFile* melody = find_part(files, KaraokePart::kMelody, &melody_at);
    const KaraokeFile* vocal1 = find_part(files, KaraokePart::kVocal1, &vocal1_at);
    const KaraokeFile* vocal2 = find_part(files, KaraokePart::kVocal2, &vocal2_at);

    // What each place asks of its file.
    bool backing_usable = false;
    if (backing == nullptr) {
        add(plan, KaraokeCheckLevel::kProblem, "no-backing");
    } else if (backing->channels == 2) {
        backing_usable = true;
    } else if (backing->channels > 2) {
        // Already a bed: its channels are speakers, not a backing, and it
        // belongs on the one-multichannel-file path.
        add(plan, KaraokeCheckLevel::kProblem, "backing-is-a-bed", {backing->name, number(backing->channels)});
    } else {
        add(plan, KaraokeCheckLevel::kProblem, "backing-not-stereo", {backing->name, number(backing->channels)});
    }
    const std::array<std::pair<const KaraokeFile*, const char*>, 3> monos{{
        {melody, "melody"}, {vocal1, "vocal1"}, {vocal2, "vocal2"}}};
    for (const auto& [file, place] : monos) {
        if (file != nullptr && file->channels != 1) {
            add(plan, KaraokeCheckLevel::kProblem, "part-not-mono", {file->name, number(file->channels), place});
        }
    }
    if (vocal2 != nullptr && vocal1 == nullptr) {
        // Annex C has no mode that carries V2 without V1.
        add(plan, KaraokeCheckLevel::kProblem, "vocal2-needs-vocal1", {vocal2->name});
    }

    plan.melody = melody != nullptr;
    plan.vocals = (vocal1 != nullptr ? 1 : 0) + (vocal2 != nullptr ? 1 : 0);
    Acmod acmod = Acmod::k2_0;
    const bool have_mode = mode_for(plan.melody, plan.vocals, &acmod);
    if (!have_mode && backing != nullptr) {
        add(plan, KaraokeCheckLevel::kProblem, "nothing-to-sing");
    }
    if (have_mode) {
        plan.acmod = acmod;
        plan.mode = mode_name(acmod);
    }

    // The coded channels, where the backing can give the two it owns.
    if (have_mode && backing_usable) {
        const ModeRow& row = row_for(acmod);
        for (std::size_t i = 0; i < row.count; ++i) {
            const std::string_view part = row.parts[i];
            KaraokeChannel channel;
            channel.position = std::string{row.positions[i]};
            channel.part = std::string{part};
            if (part == "L" || part == "R") {
                channel.file = backing_at;
                channel.channel = part == "L" ? 0 : 1;
            } else if (part == "M") {
                channel.file = melody_at;
            } else if (part == "V1") {
                // A vocal2 alone has been named; V1's slot is the one file.
                channel.file = vocal1 != nullptr ? vocal1_at : vocal2_at;
            } else {
                channel.file = vocal2_at;
            }
            plan.channels.push_back(std::move(channel));
        }
    }

    // What the encoder will absorb, named rather than done quietly.
    bool aligned = backing != nullptr;
    if (backing != nullptr) {
        for (const KaraokeFile* file : {melody, vocal1, vocal2}) {
            if (file == nullptr) {
                continue;
            }
            if (file->sample_rate != backing->sample_rate) {
                aligned = false;
                add(plan, KaraokeCheckLevel::kWarning, "rate-differs",
                    {file->name, khz(file->sample_rate), khz(backing->sample_rate)});
            }
            if (std::fabs(file->seconds - backing->seconds) > kLengthToleranceSeconds) {
                aligned = false;
                add(plan, KaraokeCheckLevel::kWarning, "length-differs",
                    {file->name, karaoke_duration_text(file->seconds),
                     karaoke_duration_text(backing->seconds)});
            }
        }
    }
    plan.buildable = have_mode && backing_usable && !plan.has_problem();
    if (plan.buildable) {
        if (aligned) {
            add(plan, KaraokeCheckLevel::kOk, "parts-aligned",
                {karaoke_duration_text(backing->seconds), khz(backing->sample_rate)});
        }
        add(plan, KaraokeCheckLevel::kOk, "each-part-one-place");
    }
    return plan;
}

KaraokePlan plan_karaoke_speakers(const KaraokeSpeakerFile& file) {
    KaraokePlan plan;
    if (file.channels == 0) {
        add(plan, KaraokeCheckLevel::kProblem, "no-file");
        return plan;
    }
    if (file.speakers.empty() || file.speakers.size() != file.channels) {
        // A file that does not name its speakers is only a count: three
        // channels are 3/0 or 2/1, and four are 3/1 or 2/2.
        add(plan, KaraokeCheckLevel::kProblem, "speakers-unnamed", {file.name, number(file.channels)});
        return plan;
    }
    bool lfe = false;
    std::vector<std::string> fullband;
    for (const std::string& name : file.speakers) {
        if (name == "LFE") {
            lfe = true;
        } else {
            fullband.push_back(name);
        }
    }
    const auto has = [&fullband](std::string_view name) {
        return std::find(fullband.begin(), fullband.end(), name) != fullband.end();
    };
    const ModeRow* match = nullptr;
    for (const ModeRow& row : kModes) {
        if (row.count != fullband.size()) {
            continue;
        }
        const bool same = std::all_of(row.positions.begin(), row.positions.begin() + static_cast<long>(row.count),
                                      [&has](std::string_view p) { return has(p); });
        if (same) {
            match = &row;
            break;
        }
    }
    if (match == nullptr) {
        if (fullband.size() == 2 && has("L") && has("R")) {
            add(plan, KaraokeCheckLevel::kProblem, "nothing-to-sing");
        } else {
            add(plan, KaraokeCheckLevel::kProblem, "speakers-not-karaoke", {join(file.speakers)});
        }
        return plan;
    }
    plan.acmod = match->acmod;
    plan.mode = mode_name(match->acmod);
    plan.melody = has("C");
    plan.vocals = has("Rs") ? 2 : (has("Cs") || has("Ls") ? 1 : 0);
    for (std::size_t i = 0; i < match->count; ++i) {
        // Where the file keeps the speaker this place is.
        const auto at = std::find(file.speakers.begin(), file.speakers.end(), std::string{match->positions[i]});
        KaraokeChannel channel;
        channel.position = std::string{match->positions[i]};
        channel.part = std::string{match->parts[i]};
        channel.file = 0;
        channel.channel = static_cast<std::size_t>(at - file.speakers.begin());
        plan.channels.push_back(std::move(channel));
    }
    if (lfe) {
        // Karaoke streams made here carry none, and the file's is not dropped
        // in silence.
        add(plan, KaraokeCheckLevel::kProblem, "lfe-present", {file.name});
    }
    plan.buildable = !plan.has_problem();
    if (plan.buildable) {
        add(plan, KaraokeCheckLevel::kOk, "each-part-one-place");
    }
    return plan;
}

KaraokeOutputs karaoke_outputs(bool melody, int vocals, KaraokeListening listening,
                               KaraokeVocalsChoice choice, bool folded) {
    KaraokeOutputs out;
    out.l = {"L"};
    out.r = {"R"};
    const bool v1 = vocals >= 1;
    const bool v2 = vocals >= 2;
    if (folded) {
        // 2/0: the melody in both, a single vocal in both (at 0.7), a pair on
        // their own sides. Any Lo/Ro fold is the aware column (Annex C), so a
        // stream asked for as coded is that.
        if (melody) {
            out.l.push_back("M");
            out.r.push_back("M");
        }
        if (listening != KaraokeListening::kCapable) {
            if (vocals == 1) {
                out.l.push_back("V1");
                out.r.push_back("V1");
            } else if (vocals == 2) {
                out.l.push_back("V1");
                out.r.push_back("V2");
            }
        } else if (choice == KaraokeVocalsChoice::kBoth) {
            if (v1) {
                out.l.push_back("V1");
            }
            if (v2) {
                out.r.push_back("V2");
            }
        } else if (choice == KaraokeVocalsChoice::kV1 && v1) {
            out.l.push_back("V1");
            out.r.push_back("V1");
        } else if (choice == KaraokeVocalsChoice::kV2 && v2) {
            out.l.push_back("V2");
            out.r.push_back("V2");
        }
        return out;
    }
    if (listening == KaraokeListening::kAsCoded) {
        // Each part where its coded position puts it: the melody in the
        // centre, a single vocal in the one surround (which a 5.1 room sounds
        // from both), a pair in the two.
        if (melody) {
            out.c.push_back("M");
        }
        if (v2) {
            out.ls.push_back("V1");
            out.rs.push_back("V2");
        } else if (v1) {
            out.ls.push_back("V1");
            out.rs.push_back("V1");
        }
        return out;
    }
    if (melody) {
        out.c.push_back("M");
    }
    if (listening == KaraokeListening::kAware) {
        // Table C.2.2: the vocals at the level the stream gives them. A single
        // vocal joins the melody; a pair goes to the sides.
        if (vocals == 1) {
            out.c.push_back("V1");
        } else if (vocals == 2) {
            out.l.push_back("V1");
            out.r.push_back("V2");
        }
        return out;
    }
    // Table C.2.3: the listener's choice, of what the stream has. A single
    // vocal goes to the centre with the melody; "both" is the V1+V2 column,
    // V1 to the left and V2 to the right - and a stream with only V1 puts it
    // on the left alone, which a decoder test found (the first reading had
    // it in the centre).
    if (choice == KaraokeVocalsChoice::kBoth) {
        if (v1) {
            out.l.push_back("V1");
        }
        if (v2) {
            out.r.push_back("V2");
        }
    } else if (choice == KaraokeVocalsChoice::kV1 && v1) {
        out.c.push_back("V1");
    } else if (choice == KaraokeVocalsChoice::kV2 && v2) {
        out.c.push_back("V2");
    }
    return out;
}

}  // namespace forge_gui
