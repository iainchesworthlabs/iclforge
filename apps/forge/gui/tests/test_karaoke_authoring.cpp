// What the Karaoke flow decides from the parts an author supplies
// (src/karaoke_authoring.hpp; planning/karaoke.md A2, A3 and A8): the coding
// mode Table C.2.1 gives for what was supplied, which file and channel each
// coded channel comes from, and the named problems and warnings. The table is
// transcribed here independently of the code under test.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "karaoke_authoring.hpp"

using forge_gui::KaraokeCheck;
using forge_gui::KaraokeCheckLevel;
using forge_gui::KaraokeFile;
using forge_gui::KaraokePart;
using forge_gui::KaraokePlan;
using forge_gui::KaraokeSpeakerFile;
using iclforge::ac3::Acmod;

namespace {

KaraokeFile part(KaraokePart kind, const char* name, std::size_t channels, std::uint32_t rate = 48000,
                 double seconds = 222.0) {
    KaraokeFile file;
    file.part = kind;
    file.name = name;
    file.channels = channels;
    file.sample_rate = rate;
    file.seconds = seconds;
    return file;
}

KaraokeFile backing() { return part(KaraokePart::kBacking, "backing.wav", 2); }
KaraokeFile melody() { return part(KaraokePart::kMelody, "melody.wav", 1); }
KaraokeFile lead() { return part(KaraokePart::kVocal1, "lead.wav", 1); }
KaraokeFile harmony() { return part(KaraokePart::kVocal2, "harmony.wav", 1); }
KaraokeFile effects() { return part(KaraokePart::kLfe, "effects.wav", 1); }

KaraokePlan plan_of(const std::vector<KaraokeFile>& files) {
    return forge_gui::plan_karaoke_parts(files);
}

const KaraokeCheck* find(const KaraokePlan& plan, const std::string& code) {
    for (const KaraokeCheck& check : plan.checks) {
        if (check.code == code) {
            return &check;
        }
    }
    return nullptr;
}

// The check with this code. The case ends where there is none, so a caller
// holds a reference, not a pointer a REQUIRE has only argued is not null.
const KaraokeCheck& check_of(const KaraokePlan& plan, const std::string& code) {
    for (const KaraokeCheck& check : plan.checks) {
        if (check.code == code) {
            return check;
        }
    }
    FAIL("no check named " << code);
    static const KaraokeCheck none;  // not reached: FAIL ends the case
    return none;
}

std::string positions(const KaraokePlan& plan) {
    std::string out;
    for (const auto& channel : plan.channels) {
        out += (out.empty() ? "" : " ") + channel.position;
    }
    return out;
}

std::string parts(const KaraokePlan& plan) {
    std::string out;
    for (const auto& channel : plan.channels) {
        out += (out.empty() ? "" : " ") + channel.part;
    }
    return out;
}

KaraokeSpeakerFile speakers_of(std::vector<std::string> names) {
    KaraokeSpeakerFile file;
    file.name = "song.wav";
    file.channels = names.size();
    file.sample_rate = 48000;
    file.seconds = 222.0;
    file.speakers = std::move(names);
    return file;
}

}  // namespace

TEST_CASE("karaoke authoring: the parts supplied choose Table C.2.1's mode",
          "[forge-gui][karaoke]") {
    struct Row {
        std::vector<KaraokeFile> files;
        Acmod acmod;
        const char* mode;
        const char* positions;
        const char* parts;
    };
    // L R is always the backing; the rest are where the mode puts them.
    const std::vector<Row> rows = {
        {{backing(), melody()}, Acmod::k3_0, "3/0", "L C R", "L M R"},
        {{backing(), lead()}, Acmod::k2_1, "2/1", "L R Cs", "L R V1"},
        {{backing(), melody(), lead()}, Acmod::k3_1, "3/1", "L C R Cs", "L M R V1"},
        {{backing(), lead(), harmony()}, Acmod::k2_2, "2/2", "L R Ls Rs", "L R V1 V2"},
        {{backing(), melody(), lead(), harmony()}, Acmod::k3_2, "3/2", "L C R Ls Rs", "L M R V1 V2"},
    };
    for (const Row& row : rows) {
        const KaraokePlan plan = forge_gui::plan_karaoke_parts(row.files);
        INFO(row.mode);
        CHECK(plan.buildable);
        CHECK(plan.acmod == row.acmod);
        CHECK(plan.mode == row.mode);
        CHECK(positions(plan) == row.positions);
        CHECK(parts(plan) == row.parts);
        CHECK(find(plan, "each-part-one-place") != nullptr);
    }
}

TEST_CASE("karaoke authoring: each coded channel names its file and channel",
          "[forge-gui][karaoke]") {
    // The order the files are handed over in is not the coded order: the
    // vocals first, the backing last.
    const std::vector<KaraokeFile> files = {harmony(), lead(), melody(), backing()};
    const KaraokePlan plan = forge_gui::plan_karaoke_parts(files);
    REQUIRE(plan.buildable);
    REQUIRE(plan.channels.size() == 5);
    // L C R Ls Rs = backing 0, melody, backing 1, lead, harmony.
    CHECK(plan.channels[0].file == 3);
    CHECK(plan.channels[0].channel == 0);
    CHECK(plan.channels[1].file == 2);
    CHECK(plan.channels[2].file == 3);
    CHECK(plan.channels[2].channel == 1);
    CHECK(plan.channels[3].file == 1);
    CHECK(plan.channels[4].file == 0);
    CHECK(plan.melody);
    CHECK(plan.vocals == 2);
}

TEST_CASE("karaoke authoring: what cannot be used is named, not fixed",
          "[forge-gui][karaoke]") {
    // No backing.
    KaraokePlan plan = plan_of({melody(), lead()});
    CHECK_FALSE(plan.buildable);
    REQUIRE(find(plan, "no-backing") != nullptr);
    CHECK(check_of(plan, "no-backing").level == KaraokeCheckLevel::kProblem);

    // A backing that is one channel is not L and R.
    plan = plan_of({part(KaraokePart::kBacking, "mono.wav", 1), melody()});
    CHECK_FALSE(plan.buildable);
    REQUIRE(find(plan, "backing-not-stereo") != nullptr);
    CHECK(check_of(plan, "backing-not-stereo").args == std::vector<std::string>{"mono.wav", "1"});

    // One that is already a 5.1 bed is not a backing either.
    plan = plan_of({part(KaraokePart::kBacking, "bed.wav", 6), lead()});
    CHECK_FALSE(plan.buildable);
    REQUIRE(find(plan, "backing-is-a-bed") != nullptr);
    CHECK(check_of(plan, "backing-is-a-bed").args == std::vector<std::string>{"bed.wav", "6"});

    // A melody or a vocal that is a pair of channels asks which one.
    plan = plan_of({backing(), part(KaraokePart::kVocal1, "duet.wav", 2)});
    CHECK_FALSE(plan.buildable);
    REQUIRE(find(plan, "part-not-mono") != nullptr);
    CHECK(check_of(plan, "part-not-mono").args == std::vector<std::string>{"duet.wav", "2", "vocal1"});

    // A second vocal with no first has no mode in Annex C.
    plan = plan_of({backing(), harmony()});
    CHECK_FALSE(plan.buildable);
    CHECK(find(plan, "vocal2-needs-vocal1") != nullptr);

    // A backing alone is plain stereo, with nothing to sing along to.
    plan = plan_of({backing()});
    CHECK_FALSE(plan.buildable);
    CHECK(find(plan, "nothing-to-sing") != nullptr);
    CHECK(plan.mode.empty());
    CHECK(plan.channels.empty());
}

TEST_CASE("karaoke authoring: a rate or a length that differs is a warning that says which",
          "[forge-gui][karaoke]") {
    // 44.1 kHz and ten seconds short: both are named, and the plan still builds.
    const KaraokePlan plan = plan_of({backing(), part(KaraokePart::kMelody, "melody.wav", 1, 44100, 222.0),
          part(KaraokePart::kVocal1, "lead.wav", 1, 48000, 212.0)});
    CHECK(plan.buildable);
    const KaraokeCheck& rate = check_of(plan, "rate-differs");
    CHECK(rate.level == KaraokeCheckLevel::kWarning);
    CHECK(rate.args == std::vector<std::string>{"melody.wav", "44.1", "48"});
    const KaraokeCheck& length = check_of(plan, "length-differs");
    CHECK(length.level == KaraokeCheckLevel::kWarning);
    CHECK(length.args == std::vector<std::string>{"lead.wav", "3:32", "3:42"});
    // And so the "all aligned" line is not claimed.
    CHECK(find(plan, "parts-aligned") == nullptr);

    // A tail that differs by less than an AC-3 frame's worth is the same length.
    const KaraokePlan same = plan_of({backing(), part(KaraokePart::kMelody, "melody.wav", 1, 48000, 222.03)});
    CHECK(find(same, "length-differs") == nullptr);
    const KaraokeCheck& aligned = check_of(same, "parts-aligned");
    CHECK(aligned.args == std::vector<std::string>{"3:42", "48"});
}

TEST_CASE("karaoke authoring: one file's own speakers name its parts",
          "[forge-gui][karaoke]") {
    struct Row {
        std::vector<std::string> names;
        Acmod acmod;
        const char* positions;
        const char* parts;
    };
    // WAVEFORMATEXTENSIBLE order: L R C, then Cs or Ls Rs.
    const std::vector<Row> rows = {
        {{"L", "R", "C"}, Acmod::k3_0, "L C R", "L M R"},
        {{"L", "R", "Cs"}, Acmod::k2_1, "L R Cs", "L R V1"},
        {{"L", "R", "C", "Cs"}, Acmod::k3_1, "L C R Cs", "L M R V1"},
        {{"L", "R", "Ls", "Rs"}, Acmod::k2_2, "L R Ls Rs", "L R V1 V2"},
        {{"L", "R", "C", "Ls", "Rs"}, Acmod::k3_2, "L C R Ls Rs", "L M R V1 V2"},
    };
    for (const Row& row : rows) {
        const KaraokePlan plan = forge_gui::plan_karaoke_speakers(speakers_of(row.names));
        INFO(row.positions);
        CHECK(plan.buildable);
        CHECK(plan.acmod == row.acmod);
        CHECK(positions(plan) == row.positions);
        CHECK(parts(plan) == row.parts);
    }
    // The channel of each is where the file keeps that speaker: C is the
    // third WAV channel, L the first.
    const KaraokePlan wide = forge_gui::plan_karaoke_speakers(speakers_of({"L", "R", "C", "Ls", "Rs"}));
    REQUIRE(wide.channels.size() == 5);
    CHECK(wide.channels[0].channel == 0);  // L
    CHECK(wide.channels[1].channel == 2);  // C
    CHECK(wide.channels[2].channel == 1);  // R
    CHECK(wide.channels[3].channel == 3);  // Ls
    CHECK(wide.channels[4].channel == 4);  // Rs
}

TEST_CASE("karaoke authoring: an LFE part is the stream's LFE and chooses no mode",
          "[forge-gui][karaoke]") {
    // After the full-bandwidth channels of every mode that can be made.
    struct Row {
        std::vector<KaraokeFile> files;
        Acmod acmod;
        const char* positions;
        const char* parts;
    };
    const std::vector<Row> rows = {
        {{backing(), melody(), effects()}, Acmod::k3_0, "L C R LFE", "L M R LFE"},
        {{backing(), lead(), effects()}, Acmod::k2_1, "L R Cs LFE", "L R V1 LFE"},
        {{backing(), melody(), lead(), effects()}, Acmod::k3_1, "L C R Cs LFE", "L M R V1 LFE"},
        {{backing(), lead(), harmony(), effects()}, Acmod::k2_2, "L R Ls Rs LFE", "L R V1 V2 LFE"},
        {{backing(), melody(), lead(), harmony(), effects()}, Acmod::k3_2, "L C R Ls Rs LFE",
         "L M R V1 V2 LFE"},
    };
    for (const Row& row : rows) {
        const KaraokePlan plan = plan_of(row.files);
        INFO(row.positions);
        CHECK(plan.buildable);
        CHECK(plan.acmod == row.acmod);
        CHECK(plan.lfe);
        CHECK(positions(plan) == row.positions);
        CHECK(parts(plan) == row.parts);
        // The LFE's file is the effects file, wherever it was listed.
        REQUIRE_FALSE(plan.channels.empty());
        CHECK(plan.channels.back().file == row.files.size() - 1);
        CHECK(plan.channels.back().channel == 0);
        CHECK(check_of(plan, "lfe-part").args == std::vector<std::string>{"effects.wav"});
    }

    // A stream without one has none, and says nothing of it.
    const KaraokePlan plain = plan_of({backing(), melody(), lead()});
    CHECK_FALSE(plain.lfe);
    CHECK(find(plain, "lfe-part") == nullptr);

    // It chooses no mode: a backing and an LFE are nothing to sing along to.
    const KaraokePlan alone = plan_of({backing(), effects()});
    CHECK_FALSE(alone.buildable);
    CHECK(alone.mode.empty());
    CHECK(find(alone, "nothing-to-sing") != nullptr);
}

TEST_CASE("karaoke authoring: an LFE part is checked like the other mono parts",
          "[forge-gui][karaoke]") {
    KaraokePlan plan = plan_of({backing(), melody(), part(KaraokePart::kLfe, "bass.wav", 2)});
    CHECK_FALSE(plan.buildable);
    CHECK(check_of(plan, "part-not-mono").args == std::vector<std::string>{"bass.wav", "2", "lfe"});

    plan = plan_of({backing(), melody(), part(KaraokePart::kLfe, "bass.wav", 1, 44100, 222.0)});
    CHECK(plan.buildable);
    CHECK(check_of(plan, "rate-differs").args == std::vector<std::string>{"bass.wav", "44.1", "48"});
    CHECK(find(plan, "parts-aligned") == nullptr);

    plan = plan_of({backing(), melody(), part(KaraokePart::kLfe, "bass.wav", 1, 48000, 200.0)});
    CHECK(plan.buildable);
    CHECK(check_of(plan, "length-differs").args == std::vector<std::string>{"bass.wav", "3:20", "3:42"});
}

TEST_CASE("karaoke authoring: a speaker file's own LFE is the stream's LFE",
          "[forge-gui][karaoke]") {
    // WAVEFORMATEXTENSIBLE order: L R C LFE Ls Rs.
    const KaraokePlan plan =
        forge_gui::plan_karaoke_speakers(speakers_of({"L", "R", "C", "LFE", "Ls", "Rs"}));
    CHECK(plan.buildable);
    CHECK(plan.mode == "3/2");
    CHECK(plan.lfe);
    CHECK(positions(plan) == "L C R Ls Rs LFE");
    REQUIRE(plan.channels.size() == 6);
    CHECK(plan.channels.back().channel == 3);  // where the file keeps it
    CHECK(check_of(plan, "lfe-part").args == std::vector<std::string>{"song.wav"});
    CHECK(find(plan, "lfe-present") == nullptr);

    // The same file without one has none.
    const KaraokePlan without = forge_gui::plan_karaoke_speakers(speakers_of({"L", "R", "C", "Ls", "Rs"}));
    CHECK_FALSE(without.lfe);
    CHECK(find(without, "lfe-part") == nullptr);
}

TEST_CASE("karaoke authoring: the room's LFE plays the stream's LFE under every listening",
          "[forge-gui][karaoke]") {
    using forge_gui::KaraokeListening;
    using forge_gui::KaraokeVocalsChoice;
    for (const KaraokeListening how :
         {KaraokeListening::kAsCoded, KaraokeListening::kAware, KaraokeListening::kCapable}) {
        const auto with = forge_gui::karaoke_outputs(true, 2, how, KaraokeVocalsChoice::kBoth, false, true);
        CHECK(with.lfe == std::vector<std::string>{"LFE"});
        const auto without = forge_gui::karaoke_outputs(true, 2, how, KaraokeVocalsChoice::kBoth, false, false);
        CHECK(without.lfe.empty());
        // A stereo fold leaves it out, which the page does not ask it not to.
        const auto folded = forge_gui::karaoke_outputs(true, 2, how, KaraokeVocalsChoice::kBoth, true, true);
        CHECK(folded.lfe.empty());
    }
}

TEST_CASE("karaoke authoring: a file that does not fit says why",
          "[forge-gui][karaoke]") {
    KaraokePlan plan;

    // Speakers that are not stated: three channels are 3/0 or 2/1.
    KaraokeSpeakerFile unnamed;
    unnamed.name = "three.wav";
    unnamed.channels = 3;
    plan = forge_gui::plan_karaoke_speakers(unnamed);
    CHECK_FALSE(plan.buildable);
    REQUIRE(find(plan, "speakers-unnamed") != nullptr);
    CHECK(check_of(plan, "speakers-unnamed").args == std::vector<std::string>{"three.wav", "3"});

    // A layout that is not one of the five.
    plan = forge_gui::plan_karaoke_speakers(speakers_of({"L", "R", "C", "Ls", "Rs", "Lc", "Rc"}));
    CHECK_FALSE(plan.buildable);
    CHECK(find(plan, "speakers-not-karaoke") != nullptr);

    // Stereo has nothing to sing along to.
    plan = forge_gui::plan_karaoke_speakers(speakers_of({"L", "R"}));
    CHECK_FALSE(plan.buildable);
    CHECK(find(plan, "nothing-to-sing") != nullptr);

    // No file.
    plan = forge_gui::plan_karaoke_speakers(KaraokeSpeakerFile{});
    CHECK_FALSE(plan.buildable);
    CHECK(find(plan, "no-file") != nullptr);
}

TEST_CASE("karaoke authoring: a duration is printed as the window prints one",
          "[forge-gui][karaoke]") {
    CHECK(forge_gui::karaoke_duration_text(222.4) == "3:42");
    CHECK(forge_gui::karaoke_duration_text(59.99) == "0:59");
    CHECK(forge_gui::karaoke_duration_text(0.0) == "0:00");
    CHECK(forge_gui::karaoke_duration_text(3600.0) == "60:00");
}

TEST_CASE("karaoke authoring: a mode starts at about 77 kbps a channel, on a rate Table 5.18 has",
          "[forge-gui][karaoke]") {
    // The design sheet's figure for the five channels of 3/2.
    CHECK(forge_gui::karaoke_default_bitrate_kbps("3/2") == 384);
    CHECK(forge_gui::karaoke_default_bitrate_kbps("3/1") == 320);
    CHECK(forge_gui::karaoke_default_bitrate_kbps("2/2") == 320);
    CHECK(forge_gui::karaoke_default_bitrate_kbps("3/0") == 256);
    CHECK(forge_gui::karaoke_default_bitrate_kbps("2/1") == 256);
    // No mode, or one that is not Annex C's: the encoder's own start.
    CHECK(forge_gui::karaoke_default_bitrate_kbps("") == 192);
    CHECK(forge_gui::karaoke_default_bitrate_kbps("2/0") == 192);

    // Each is a rate the AC-3 encoder takes, and more channels never take less.
    int previous = 0;
    for (const char* mode : {"3/0", "3/1", "3/2"}) {
        const int rate = forge_gui::karaoke_default_bitrate_kbps(mode);
        CHECK(std::ranges::find(iclforge::ac3::kBitratesKbps, static_cast<std::uint16_t>(rate)) !=
              iclforge::ac3::kBitratesKbps.end());
        CHECK(rate >= previous);
        previous = rate;
    }
}

namespace {

// A folder of its own under the temporary directory, removed with the test.
class ScratchFolder {
public:
    ScratchFolder() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("forge-gui-karaoke-lyrics-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    // Writes `text` to a file of this name here, and returns its path.
    [[nodiscard]] std::string file(const std::string& name, const std::string& text) const {
        const auto full = path_ / name;
        std::ofstream out(full, std::ios::binary);
        out << text;
        return full.string();
    }

private:
    std::filesystem::path path_;
};

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("karaoke authoring: the lyrics beside a file are the .lrc of its name, in any case",
          "[forge-gui][karaoke]") {
    const ScratchFolder folder;
    const std::string song = folder.file("song.wav", "x");
    // Nothing there yet; another name's lyrics are not this song's.
    CHECK(forge_gui::karaoke_lyrics_beside(song).empty());
    static_cast<void>(folder.file("other.lrc", "[00:01.00]no"));
    CHECK(forge_gui::karaoke_lyrics_beside(song).empty());

    const std::string lyrics = folder.file("song.lrc", "[00:01.00]hello");
    CHECK(forge_gui::karaoke_lyrics_beside(song) == lyrics);
    // The extension is matched without regard to case, as Hearth finds it.
    const ScratchFolder upper;
    const std::string loud = upper.file("Song.LRC", "[00:01.00]hello");
    CHECK(forge_gui::karaoke_lyrics_beside(upper.file("Song.wav", "x")) == loud);
}

TEST_CASE("karaoke authoring: lyrics are copied beside the stream under its name, and never over another",
          "[forge-gui][karaoke]") {
    using forge_gui::KaraokeLyricsCopy;
    const ScratchFolder source_folder;
    const ScratchFolder stream_folder;
    const std::string lyrics = source_folder.file("my words.lrc", "[00:01.00]hello");

    // The stream's name with .lrc, so that Hearth finds it as it finds any sidecar.
    const std::string stream = stream_folder.file("out.ac3", "x");
    CHECK(forge_gui::karaoke_copy_lyrics(lyrics, stream) == KaraokeLyricsCopy::kCopied);
    const std::string beside = forge_gui::karaoke_lyrics_beside(stream);
    REQUIRE_FALSE(beside.empty());
    CHECK(read_all(beside) == "[00:01.00]hello");

    // A lyrics file already there is left as it was, not replaced by this one.
    const std::string other = source_folder.file("other words.lrc", "[00:01.00]different");
    CHECK(forge_gui::karaoke_copy_lyrics(other, stream) == KaraokeLyricsCopy::kExists);
    CHECK(read_all(beside) == "[00:01.00]hello");

    // The file offered is the one already there: nothing to do, and nothing wrong.
    CHECK(forge_gui::karaoke_copy_lyrics(beside, stream) == KaraokeLyricsCopy::kAlreadyThere);

    // A file that is not there says so.
    CHECK(forge_gui::karaoke_copy_lyrics((std::filesystem::path(stream).parent_path() / "none.lrc").string(),
                                         stream_folder.file("second.ac3", "x")) ==
          KaraokeLyricsCopy::kUnreadable);
}
