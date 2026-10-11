#include "karaoke_controller.hpp"

#include <QImage>
#include <QSettings>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QFileInfo>
#include <QUrl>

#include <fstream>
#include <system_error>
#include <vector>

#include "iclforge/lyrics/cdg.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/timing.hpp"

namespace iclforge::hearth::ui {

// The reader and the cursor are libs/lyrics' (planning/karaoke-phase2.md, W13a), which Hearth's
// window shares with the other programs that read lyrics.
using iclforge::lyrics::CdgFile;
using iclforge::lyrics::CdgScreen;
using iclforge::lyrics::find_cdg_file;
using iclforge::lyrics::find_lyrics_file;
using iclforge::lyrics::kCdgHeight;
using iclforge::lyrics::kCdgWidth;
using iclforge::lyrics::LyricLine;
using iclforge::lyrics::LyricPosition;
using iclforge::lyrics::Lyrics;
using iclforge::lyrics::LyricWord;
using iclforge::lyrics::lyric_position;
using iclforge::lyrics::read_cdg;
using iclforge::lyrics::read_lyrics;
using iclforge::lyrics::word_state;
using iclforge::lyrics::WordState;
using iclforge::lyrics::estimate_line;
using iclforge::lyrics::set_line_start;
using iclforge::lyrics::set_word_start;
using iclforge::lyrics::settle_ends;
using iclforge::lyrics::write_lrc;

namespace {

// The same organisation and application HearthController's QSettings use, so
// the lyric delay lives in the same file as the rest of the window's choices.
constexpr auto kDelayKey = "karaoke/lyricDelayMs";
constexpr auto kReactionKey = "karaoke/reactionMs";

[[nodiscard]] QString from_utf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// A word's text keeps the spacing written after it, so the words of a line
// joined are the line's text. A view lays words out one by one, and trailing
// spaces measure differently from one toolkit to the next, so the spacing is
// handed over as a flag and the text without it.
[[nodiscard]] QVariantMap word_to_map(const LyricWord& word) {
    std::string body = word.text;
    bool gap = false;
    while (!body.empty() && is_space(body.back())) {
        body.pop_back();
        gap = true;
    }
    QVariantMap map;
    map.insert(QStringLiteral("text"), from_utf8(body));
    map.insert(QStringLiteral("startMs"), static_cast<qlonglong>(word.start_ms));
    map.insert(QStringLiteral("endMs"), static_cast<qlonglong>(word.end_ms));
    map.insert(QStringLiteral("gap"), gap);
    return map;
}

[[nodiscard]] QVariantMap line_to_map(const LyricLine& line) {
    QVariantMap map;
    map.insert(QStringLiteral("text"), from_utf8(line.text));
    map.insert(QStringLiteral("startMs"), static_cast<qlonglong>(line.start_ms));
    map.insert(QStringLiteral("endMs"), static_cast<qlonglong>(line.end_ms));
    map.insert(QStringLiteral("wordTimed"), line.word_timed());
    QString singer;
    switch (line.singer) {
        case iclforge::lyrics::Singer::kFirst: singer = QStringLiteral("v1"); break;
        case iclforge::lyrics::Singer::kSecond: singer = QStringLiteral("v2"); break;
        case iclforge::lyrics::Singer::kBoth: singer = QStringLiteral("both"); break;
        case iclforge::lyrics::Singer::kUnmarked: break;
    }
    map.insert(QStringLiteral("singer"), singer);
    QVariantList words;
    words.reserve(static_cast<qsizetype>(line.words.size()));
    for (const LyricWord& word : line.words) {
        words.push_back(word_to_map(word));
    }
    map.insert(QStringLiteral("words"), words);
    return map;
}

// A singer as the model names it: "" for none, else "v1", "v2" or "both"; nothing for another word.
[[nodiscard]] std::optional<iclforge::lyrics::Singer> singer_from_key(const QString& key) {
    using iclforge::lyrics::Singer;
    if (key.isEmpty()) {
        return Singer::kUnmarked;
    }
    if (key == QLatin1String("v1")) {
        return Singer::kFirst;
    }
    if (key == QLatin1String("v2")) {
        return Singer::kSecond;
    }
    if (key == QLatin1String("both")) {
        return Singer::kBoth;
    }
    return std::nullopt;
}

// A name for an id tag: one line, with no bracket (a tag ends at the first `]`), and no run of
// spaces.
[[nodiscard]] std::string tag_value(const QString& name) {
    QString cleaned = name;
    for (QChar& c : cleaned) {
        if (c.unicode() < 0x20 || c == QLatin1Char('[') || c == QLatin1Char(']')) {
            c = QLatin1Char(' ');
        }
    }
    return cleaned.simplified().toStdString();
}

}  // namespace

struct KaraokeController::State {
    Lyrics lyrics;
    // The copy being timed, and which of its lines and words a person (or the
    // file) gave a time, as against an estimate.
    Lyrics editing;
    std::vector<char> stamped;
    std::vector<std::vector<char>> tapped;
    // The picture of a CD+G file and the screen that follows it. The screen holds a pointer to the
    // file, which is never moved once it is in here, and goes before it.
    std::optional<CdgFile> picture;
    std::unique_ptr<CdgScreen> screen;
};

KaraokeController::KaraokeController(QObject* parent) : QObject(parent), state_(std::make_unique<State>()) {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
                       QStringLiteral("Hearth"));
    delay_ms_ = std::clamp(settings.value(QLatin1StringView(kDelayKey), 0).toInt(), -kMaxDelayMs, kMaxDelayMs);
    reaction_ms_ = std::clamp(settings.value(QLatin1StringView(kReactionKey), kDefaultReactionMs).toInt(), 0,
                              kMaxReactionMs);
}

KaraokeController::~KaraokeController() = default;

void KaraokeController::setDelayMs(int delay_ms) {
    const int clamped = std::clamp(delay_ms, -kMaxDelayMs, kMaxDelayMs);
    if (clamped == delay_ms_) {
        return;
    }
    delay_ms_ = clamped;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
                       QStringLiteral("Hearth"));
    settings.setValue(QLatin1StringView(kDelayKey), delay_ms_);
    emit delayChanged();
}

void KaraokeController::setReactionMs(int reaction_ms) {
    const int clamped = std::clamp(reaction_ms, 0, kMaxReactionMs);
    if (clamped == reaction_ms_) {
        return;
    }
    reaction_ms_ = clamped;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
                       QStringLiteral("Hearth"));
    settings.setValue(QLatin1StringView(kReactionKey), reaction_ms_);
    emit reactionChanged();
}

void KaraokeController::clear_lyrics() {
    state_->lyrics = Lyrics{};
    state_->screen.reset();
    state_->picture.reset();
    frame_ = QImage{};
    frame_applied_ = 0;
    status_ = QStringLiteral("none");
    file_name_.clear();
    error_text_.clear();
    expected_name_.clear();
    title_.clear();
    artist_.clear();
    word_timed_ = false;
    has_singers_ = false;
    file_first_name_.clear();
    file_second_name_.clear();
    lines_.clear();
}

QString KaraokeController::firstSingerLabel() const {
    return !first_name_.isEmpty() ? first_name_
           : !file_first_name_.isEmpty() ? file_first_name_
                                         : QStringLiteral("V1");
}

QString KaraokeController::secondSingerLabel() const {
    return !second_name_.isEmpty() ? second_name_
           : !file_second_name_.isEmpty() ? file_second_name_
                                          : QStringLiteral("V2");
}

void KaraokeController::setFirstSingerName(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed == first_name_) {
        return;
    }
    first_name_ = trimmed;
    emit singerNamesChanged();
}

void KaraokeController::setSecondSingerName(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed == second_name_) {
        return;
    }
    second_name_ = trimmed;
    emit singerNamesChanged();
}

void KaraokeController::load(const QString& itemPath, qlonglong durationMs) {
    if (itemPath == item_path_ && durationMs == duration_ms_) {
        return;
    }
    item_path_ = itemPath;
    duration_ms_ = durationMs;
    clear_lyrics();
    if (!itemPath.isEmpty()) {
        const std::string path = itemPath.toStdString();
        const std::filesystem::path media{path};
        expected_name_ = from_utf8(media.stem().string() + ".lrc");
        if (const std::optional<std::filesystem::path> file = find_lyrics_file(path)) {
            file_name_ = from_utf8(file->filename().string());
            const std::optional<std::int64_t> duration =
                durationMs > 0 ? std::optional<std::int64_t>{durationMs} : std::nullopt;
            if (auto read = read_lyrics(*file, duration)) {
                state_->lyrics = std::move(*read);
                const Lyrics& lyrics = state_->lyrics;
                status_ = lyrics.timed ? QStringLiteral("timed") : QStringLiteral("plain");
                title_ = from_utf8(lyrics.title);
                artist_ = from_utf8(lyrics.artist);
                file_first_name_ = from_utf8(lyrics.first_singer_name);
                file_second_name_ = from_utf8(lyrics.second_singer_name);
                has_singers_ = iclforge::lyrics::has_singers(lyrics);
                lines_.reserve(static_cast<qsizetype>(lyrics.lines.size()));
                for (const LyricLine& line : lyrics.lines) {
                    word_timed_ = word_timed_ || line.word_timed();
                    lines_.push_back(line_to_map(line));
                }
            } else {
                status_ = QStringLiteral("error");
                error_text_ = from_utf8(read.error());
            }
        } else if (const std::optional<std::filesystem::path> disc = find_cdg_file(path)) {
            // No lyrics file, and the picture of a disc: its words are pixels, so what is shown
            // is the picture, against the position. A lyrics file beside the item wins, because
            // it is what a word-by-word fill and a nudge can follow.
            file_name_ = from_utf8(disc->filename().string());
            if (auto read = read_cdg(*disc)) {
                state_->picture.emplace(std::move(*read));
                state_->screen = std::make_unique<CdgScreen>(*state_->picture);
                status_ = QStringLiteral("picture");
            } else {
                status_ = QStringLiteral("error");
                error_text_ = from_utf8(read.error());
            }
        }
    }
    emit lyricsChanged();
    emit singerNamesChanged();
}

QImage KaraokeController::pictureFrame(qlonglong positionMs) {
    if (!state_->screen) {
        return {};
    }
    CdgScreen& screen = *state_->screen;
    screen.seek(positionMs - delay_ms_);
    if (!frame_.isNull() && screen.applied() == frame_applied_) {
        return frame_;
    }
    const std::vector<std::uint32_t> pixels = screen.pixels();
    QImage image(kCdgWidth, kCdgHeight, QImage::Format_ARGB32);
    for (int y = 0; y < kCdgHeight; ++y) {
        std::memcpy(image.scanLine(y), pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(kCdgWidth),
                    static_cast<std::size_t>(kCdgWidth) * sizeof(std::uint32_t));
    }
    frame_ = image;
    frame_applied_ = screen.applied();
    return frame_;
}

QVariantMap KaraokeController::cursor(qlonglong positionMs) const {
    QVariantMap map;
    const Lyrics& lyrics = state_->lyrics;
    const LyricPosition at = lyric_position(lyrics, positionMs, delay_ms_);
    map.insert(QStringLiteral("line"), at.line ? static_cast<int>(*at.line) : -1);
    map.insert(QStringLiteral("inLine"), at.in_line);
    map.insert(QStringLiteral("word"), at.word ? static_cast<int>(*at.word) : -1);
    map.insert(QStringLiteral("fraction"), at.fraction);
    map.insert(QStringLiteral("positionMs"), static_cast<qlonglong>(at.position_ms));

    QVariantList fills;
    if (lyrics.timed && at.line && *at.line < lyrics.lines.size() && at.in_line) {
        const LyricLine& line = lyrics.lines[*at.line];
        fills.reserve(static_cast<qsizetype>(line.words.size()));
        for (std::size_t i = 0; i < line.words.size(); ++i) {
            switch (word_state(line.words[i], at.position_ms)) {
                case WordState::kSung:
                    fills.push_back(1.0);
                    break;
                case WordState::kBeingSung: {
                    const LyricWord& word = line.words[i];
                    const double span = static_cast<double>(word.end_ms - word.start_ms);
                    const double through = span > 0.0 ? static_cast<double>(at.position_ms - word.start_ms) / span
                                                      : 0.0;
                    fills.push_back(std::clamp(through, 0.0, 1.0));
                    break;
                }
                case WordState::kNotYetSung:
                    fills.push_back(0.0);
                    break;
            }
        }
    }
    map.insert(QStringLiteral("fills"), fills);
    return map;
}

// ---- timing the lyrics -------------------------------------------------------------

qlonglong KaraokeController::adjusted(qlonglong position_ms) const {
    return std::max<qlonglong>(position_ms - reaction_ms_, 0);
}

void KaraokeController::refresh_timing() {
    const Lyrics& editing = state_->editing;
    timing_lines_.clear();
    timing_lines_.reserve(static_cast<qsizetype>(editing.lines.size()));
    next_line_ = -1;
    stamped_lines_ = 0;
    for (std::size_t i = 0; i < editing.lines.size(); ++i) {
        const LyricLine& line = editing.lines[i];
        QVariantMap map = line_to_map(line);
        const bool stamped = i < state_->stamped.size() && state_->stamped[i] != 0;
        map.insert(QStringLiteral("stamped"), stamped);
        QVariantList words = map.value(QStringLiteral("words")).toList();
        for (qsizetype w = 0; w < words.size(); ++w) {
            QVariantMap word = words[w].toMap();
            const bool tapped = i < state_->tapped.size() && static_cast<std::size_t>(w) < state_->tapped[i].size() &&
                                state_->tapped[i][static_cast<std::size_t>(w)] != 0;
            word.insert(QStringLiteral("tapped"), tapped);
            words[w] = word;
        }
        map.insert(QStringLiteral("words"), words);
        timing_lines_.push_back(map);
        if (stamped) {
            ++stamped_lines_;
        } else if (next_line_ < 0 && !line.text.empty()) {
            next_line_ = static_cast<int>(i);
        }
    }
}

bool KaraokeController::beginTiming(const QString& textFile) {
    timing_error_.clear();
    if (item_path_.isEmpty()) {
        timing_error_ = QStringLiteral("Nothing is playing to time the words to.");
        emit timingChanged();
        return false;
    }
    Lyrics source;
    if (!textFile.isEmpty()) {
        const QUrl url(textFile);
        const QString local = url.isLocalFile() ? url.toLocalFile() : textFile;
        const std::optional<std::int64_t> duration =
            duration_ms_ > 0 ? std::optional<std::int64_t>{duration_ms_} : std::nullopt;
        auto read = read_lyrics(std::filesystem::path(local.toStdString()), duration);
        if (!read) {
            timing_error_ = from_utf8(read.error());
            emit timingChanged();
            return false;
        }
        source = std::move(*read);
    } else if (status_ == QLatin1String("timed") || status_ == QLatin1String("plain")) {
        source = state_->lyrics;
    } else {
        timing_error_ = QStringLiteral("There are no words to time: choose a text file.");
        emit timingChanged();
        return false;
    }
    const bool any_text = std::any_of(source.lines.begin(), source.lines.end(),
                                      [](const LyricLine& line) { return !line.text.empty(); });
    if (!any_text) {
        timing_error_ = QStringLiteral("The file has no words to time.");
        emit timingChanged();
        return false;
    }
    State& state = *state_;
    state.editing = std::move(source);
    // A file that has times has them from the file: its lines are stamped and its words are
    // taken as given. A plain file has none of either.
    state.stamped.assign(state.editing.lines.size(), state.editing.timed ? 1 : 0);
    state.tapped.clear();
    for (const LyricLine& line : state.editing.lines) {
        state.tapped.emplace_back(line.words.size(), static_cast<char>(1));
    }
    timing_ = true;
    timing_dirty_ = false;
    refresh_timing();
    emit timingChanged();
    return true;
}

void KaraokeController::endTiming() {
    if (!timing_) {
        return;
    }
    timing_ = false;
    timing_dirty_ = false;
    timing_error_.clear();
    timing_lines_.clear();
    next_line_ = -1;
    stamped_lines_ = 0;
    state_->editing = Lyrics{};
    state_->stamped.clear();
    state_->tapped.clear();
    emit timingChanged();
}

bool KaraokeController::tapLine(qlonglong positionMs) {
    if (!timing_ || next_line_ < 0) {
        return false;
    }
    return restampLine(next_line_, positionMs);
}

bool KaraokeController::restampLine(int line, qlonglong positionMs) {
    if (!timing_ || line < 0 || !set_line_start(state_->editing, static_cast<std::size_t>(line), adjusted(positionMs))) {
        return false;
    }
    State& state = *state_;
    state.stamped[static_cast<std::size_t>(line)] = 1;
    // A line with words an estimate gave has its estimate moved with it; a tapped word moved
    // with its line is still the person's.
    settle_ends(state.editing, duration_ms_ > 0 ? std::optional<std::int64_t>{duration_ms_} : std::nullopt);
    timing_dirty_ = true;
    refresh_timing();
    emit timingChanged();
    return true;
}

int KaraokeController::estimateWords() {
    if (!timing_) {
        return 0;
    }
    State& state = *state_;
    settle_ends(state.editing, duration_ms_ > 0 ? std::optional<std::int64_t>{duration_ms_} : std::nullopt);
    int given = 0;
    for (std::size_t i = 0; i < state.editing.lines.size(); ++i) {
        if (state.stamped[i] != 0 && estimate_line(state.editing, i)) {
            state.tapped[i].assign(state.editing.lines[i].words.size(), static_cast<char>(0));
            ++given;
        }
    }
    if (given > 0) {
        timing_dirty_ = true;
        refresh_timing();
        emit timingChanged();
    }
    return given;
}

bool KaraokeController::tapWord(int line, int word, qlonglong positionMs) {
    if (!timing_ || line < 0 || word < 0 ||
        !set_word_start(state_->editing, static_cast<std::size_t>(line), static_cast<std::size_t>(word),
                        adjusted(positionMs))) {
        return false;
    }
    state_->tapped[static_cast<std::size_t>(line)][static_cast<std::size_t>(word)] = 1;
    timing_dirty_ = true;
    refresh_timing();
    emit timingChanged();
    return true;
}

bool KaraokeController::nudge(int line, int word, int deltaMs) {
    if (!timing_ || line < 0 || static_cast<std::size_t>(line) >= state_->editing.lines.size()) {
        return false;
    }
    const LyricLine& target = state_->editing.lines[static_cast<std::size_t>(line)];
    if (word < 0) {
        const bool done = set_line_start(state_->editing, static_cast<std::size_t>(line),
                                         target.start_ms + deltaMs);
        if (done) {
            settle_ends(state_->editing,
                        duration_ms_ > 0 ? std::optional<std::int64_t>{duration_ms_} : std::nullopt);
            timing_dirty_ = true;
            refresh_timing();
            emit timingChanged();
        }
        return done;
    }
    if (static_cast<std::size_t>(word) >= target.words.size()) {
        return false;
    }
    const std::int64_t start = target.words[static_cast<std::size_t>(word)].start_ms + deltaMs;
    // Not adjusted: a nudge is already in the file's own time.
    if (!set_word_start(state_->editing, static_cast<std::size_t>(line), static_cast<std::size_t>(word), start)) {
        return false;
    }
    state_->tapped[static_cast<std::size_t>(line)][static_cast<std::size_t>(word)] = 1;
    timing_dirty_ = true;
    refresh_timing();
    emit timingChanged();
    return true;
}

QString KaraokeController::timingFirstName() const {
    return timing_ ? from_utf8(state_->editing.first_singer_name) : QString();
}

QString KaraokeController::timingSecondName() const {
    return timing_ ? from_utf8(state_->editing.second_singer_name) : QString();
}

void KaraokeController::setTimingFirstName(const QString& name) {
    const std::string value = tag_value(name);
    if (!timing_ || value == state_->editing.first_singer_name) {
        return;
    }
    state_->editing.first_singer_name = value;
    timing_dirty_ = true;
    emit timingChanged();
}

void KaraokeController::setTimingSecondName(const QString& name) {
    const std::string value = tag_value(name);
    if (!timing_ || value == state_->editing.second_singer_name) {
        return;
    }
    state_->editing.second_singer_name = value;
    timing_dirty_ = true;
    emit timingChanged();
}

bool KaraokeController::setLineSinger(int line, const QString& singer) {
    const std::optional<iclforge::lyrics::Singer> value = singer_from_key(singer);
    if (!timing_ || !value || line < 0 ||
        static_cast<std::size_t>(line) >= state_->editing.lines.size()) {
        return false;
    }
    if (state_->editing.lines[static_cast<std::size_t>(line)].singer == *value) {
        return true;
    }
    if (!iclforge::lyrics::set_line_singer(state_->editing, static_cast<std::size_t>(line), *value)) {
        return false;
    }
    timing_dirty_ = true;
    refresh_timing();
    emit timingChanged();
    return true;
}

QString KaraokeController::saveTiming() {
    timing_error_.clear();
    if (!timing_) {
        timing_error_ = QStringLiteral("Nothing is being timed.");
        emit timingChanged();
        return timing_error_;
    }
    namespace fs = std::filesystem;
    const std::string item = item_path_.toStdString();
    const fs::path media{item};
    fs::path target = media.parent_path() / (media.stem().string() + ".lrc");
    if (const std::optional<fs::path> existing = find_lyrics_file(item)) {
        target = *existing;
    }
    settle_ends(state_->editing, duration_ms_ > 0 ? std::optional<std::int64_t>{duration_ms_} : std::nullopt);
    const std::string text = write_lrc(state_->editing);

    std::error_code error;
    if (fs::exists(target, error)) {
        // The file that was there is kept once, as it was: the first one is the one worth keeping.
        fs::path backup = target;
        backup += ".bak";
        if (!fs::exists(backup, error)) {
            fs::copy_file(target, backup, error);
            if (error) {
                timing_error_ = QStringLiteral("The lyrics file could not be backed up: %1")
                                    .arg(QString::fromStdString(error.message()));
                emit timingChanged();
                return timing_error_;
            }
        }
    }
    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.good()) {
            timing_error_ = QStringLiteral("The lyrics could not be written beside the item.");
            emit timingChanged();
            return timing_error_;
        }
    }
    fs::rename(temporary, target, error);
    if (error) {
        timing_error_ = QStringLiteral("The lyrics file could not be replaced: %1")
                            .arg(QString::fromStdString(error.message()));
        emit timingChanged();
        return timing_error_;
    }
    // What was written is what is loaded now: the view reads it as any file.
    const QString path = item_path_;
    const qlonglong duration = duration_ms_;
    item_path_.clear();
    load(path, duration);
    timing_dirty_ = false;
    emit timingChanged();
    return QString();
}

}  // namespace iclforge::hearth::ui
