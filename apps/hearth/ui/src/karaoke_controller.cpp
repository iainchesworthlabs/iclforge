#include "karaoke_controller.hpp"

#include <QSettings>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "iclforge/lyrics/lyrics.hpp"

namespace iclforge::hearth::ui {

// The reader and the cursor are libs/lyrics' (planning/karaoke-phase2.md, W13a), which Hearth's
// window shares with the other programs that read lyrics.
using iclforge::lyrics::find_lyrics_file;
using iclforge::lyrics::LyricLine;
using iclforge::lyrics::LyricPosition;
using iclforge::lyrics::Lyrics;
using iclforge::lyrics::LyricWord;
using iclforge::lyrics::lyric_position;
using iclforge::lyrics::read_lyrics;
using iclforge::lyrics::word_state;
using iclforge::lyrics::WordState;

namespace {

// The same organisation and application HearthController's QSettings use, so
// the lyric delay lives in the same file as the rest of the window's choices.
constexpr auto kDelayKey = "karaoke/lyricDelayMs";

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

}  // namespace

struct KaraokeController::State {
    Lyrics lyrics;
};

KaraokeController::KaraokeController(QObject* parent) : QObject(parent), state_(std::make_unique<State>()) {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
                       QStringLiteral("Hearth"));
    delay_ms_ = std::clamp(settings.value(QLatin1StringView(kDelayKey), 0).toInt(), -kMaxDelayMs, kMaxDelayMs);
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

void KaraokeController::clear_lyrics() {
    state_->lyrics = Lyrics{};
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
        }
    }
    emit lyricsChanged();
    emit singerNamesChanged();
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

}  // namespace iclforge::hearth::ui
