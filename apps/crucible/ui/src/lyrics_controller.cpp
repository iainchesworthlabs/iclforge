#include "lyrics_controller.hpp"

#include <QFile>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"

namespace {

using iclforge::lyrics::LyricLine;
using iclforge::lyrics::LyricPosition;
using iclforge::lyrics::Lyrics;
using iclforge::lyrics::LyricWord;
using iclforge::lyrics::WordState;

// No lyric file is anywhere near this; the library refuses a file past it too, and this refuses
// before reading it.
constexpr qint64 kMaxFileBytes = 4LL * 1024 * 1024;

[[nodiscard]] QString from_utf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// A word's text keeps the spacing written after it; a view lays words out one by one, so the
// spacing is handed over as a flag and the text without it (the Hearth window's own shape).
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
    QVariantList words;
    words.reserve(static_cast<qsizetype>(line.words.size()));
    for (const LyricWord& word : line.words) {
        words.push_back(word_to_map(word));
    }
    map.insert(QStringLiteral("words"), words);
    return map;
}

// What one application has: the file, and the clock that follows it.
struct Entry {
    Lyrics lyrics;
    QString status = QStringLiteral("none");
    QString file_name;
    QString error_text;
    QVariantList lines;
    bool running = false;
    qlonglong started_ms = 0;
    int nudge_ms = 0;
};

// An application's lyrics from the bytes of a file, as the reader takes them (UTF-8, or UTF-16 with
// a byte-order mark): an entry with the lines, or one in the "error" state with the reader's reason.
[[nodiscard]] Entry make_entry(const QString& name, std::string_view bytes, qlonglong duration_ms) {
    Entry entry;
    entry.file_name = name;
    const std::optional<std::int64_t> duration =
        duration_ms > 0 ? std::optional<std::int64_t>{duration_ms} : std::nullopt;
    auto parsed = iclforge::lyrics::parse_lyrics(bytes, duration);
    if (!parsed) {
        entry.status = QStringLiteral("error");
        entry.error_text = from_utf8(parsed.error());
        return entry;
    }
    entry.lyrics = std::move(*parsed);
    entry.status = entry.lyrics.timed ? QStringLiteral("timed") : QStringLiteral("plain");
    entry.lines.reserve(static_cast<qsizetype>(entry.lyrics.lines.size()));
    for (const LyricLine& line : entry.lyrics.lines) {
        entry.lines.push_back(line_to_map(line));
    }
    return entry;
}

[[nodiscard]] Entry failed_entry(const QString& name, const QString& why) {
    Entry entry;
    entry.file_name = name;
    entry.status = QStringLiteral("error");
    entry.error_text = why;
    return entry;
}

}  // namespace

struct CrucibleLyrics::State {
    std::unordered_map<int, Entry> entries;

    [[nodiscard]] const Entry* find(int app) const {
        const auto it = entries.find(app);
        return it == entries.end() ? nullptr : &it->second;
    }
};

CrucibleLyrics::CrucibleLyrics(QObject* parent) : QObject(parent), state_(std::make_unique<State>()) {}

CrucibleLyrics::~CrucibleLyrics() = default;

qlonglong CrucibleLyrics::nowMs() const {
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<qlonglong>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

bool CrucibleLyrics::importText(int app, const QString& name, const QString& text, qlonglong durationMs) {
    const QByteArray bytes = text.toUtf8();
    Entry entry = make_entry(name, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())},
                             durationMs);
    const bool ok = entry.status != QStringLiteral("error");
    state_->entries[app] = std::move(entry);
    emit changed(app);
    return ok;
}

bool CrucibleLyrics::importFile(int app, const QString& fileUrl, qlonglong durationMs) {
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    const qsizetype slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    const QString name = path.mid(slash + 1);
    QFile file(path);
    Entry entry;
    if (!file.open(QIODevice::ReadOnly)) {
        entry = failed_entry(name, QStringLiteral("The lyrics file could not be opened."));
    } else if (file.size() > kMaxFileBytes) {
        entry = failed_entry(name, QStringLiteral("The lyrics file is too large to be lyrics."));
    } else {
        // The bytes go to the reader as they are: it decides UTF-8, UTF-16 or neither, and says why.
        const QByteArray bytes = file.readAll();
        entry = make_entry(name, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())},
                           durationMs);
    }
    const bool ok = entry.status != QStringLiteral("error");
    state_->entries[app] = std::move(entry);
    emit changed(app);
    return ok;
}

void CrucibleLyrics::clear(int app) {
    if (state_->entries.erase(app) > 0) {
        emit changed(app);
    }
}

void CrucibleLyrics::prune(const QVariantList& liveApps) {
    QSet<int> live;
    for (const QVariant& value : liveApps) {
        live.insert(value.toInt());
    }
    std::vector<int> gone;
    for (const auto& item : state_->entries) {
        if (!live.contains(item.first)) {
            gone.push_back(item.first);
        }
    }
    for (const int app : gone) {
        state_->entries.erase(app);
        emit changed(app);
    }
}

QString CrucibleLyrics::status(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr ? entry->status : QStringLiteral("none");
}

QString CrucibleLyrics::fileName(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr ? entry->file_name : QString{};
}

QString CrucibleLyrics::errorText(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr ? entry->error_text : QString{};
}

QVariantList CrucibleLyrics::lines(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr ? entry->lines : QVariantList{};
}

void CrucibleLyrics::start(int app) {
    startAt(app, nowMs());
}

void CrucibleLyrics::startAt(int app, qlonglong now_ms) {
    const auto it = state_->entries.find(app);
    // A plain file has no times to follow and an error has no lyrics: only a timed file starts.
    if (it == state_->entries.end() || it->second.status != QStringLiteral("timed")) {
        return;
    }
    it->second.running = true;
    it->second.started_ms = now_ms;
    emit changed(app);
}

void CrucibleLyrics::stop(int app) {
    const auto it = state_->entries.find(app);
    if (it == state_->entries.end() || !it->second.running) {
        return;
    }
    it->second.running = false;
    emit changed(app);
}

bool CrucibleLyrics::running(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr && entry->running;
}

int CrucibleLyrics::nudgeMs(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr ? entry->nudge_ms : 0;
}

void CrucibleLyrics::setNudgeMs(int app, int ms) {
    const auto it = state_->entries.find(app);
    if (it == state_->entries.end()) {
        return;
    }
    const int held = std::clamp(ms, -kMaxNudgeMs, kMaxNudgeMs);
    if (held == it->second.nudge_ms) {
        return;
    }
    it->second.nudge_ms = held;
    emit changed(app);
}

void CrucibleLyrics::nudge(int app, int deltaMs) {
    setNudgeMs(app, nudgeMs(app) + deltaMs);
}

qlonglong CrucibleLyrics::positionMs(int app, qlonglong now_ms) const {
    const Entry* entry = state_->find(app);
    if (entry == nullptr || !entry->running) {
        return -1;
    }
    return std::max<qlonglong>(0, now_ms - entry->started_ms);
}

QVariantMap CrucibleLyrics::cursor(int app, qlonglong now_ms) const {
    QVariantMap map;
    map.insert(QStringLiteral("line"), -1);
    map.insert(QStringLiteral("inLine"), false);
    map.insert(QStringLiteral("word"), -1);
    map.insert(QStringLiteral("fraction"), 0.0);
    map.insert(QStringLiteral("positionMs"), static_cast<qlonglong>(-1));
    map.insert(QStringLiteral("fills"), QVariantList{});
    const Entry* entry = state_->find(app);
    if (entry == nullptr || !entry->running || !entry->lyrics.timed) {
        return map;
    }
    const qlonglong position = positionMs(app, now_ms);
    const LyricPosition at = iclforge::lyrics::lyric_position(entry->lyrics, position, entry->nudge_ms);
    map.insert(QStringLiteral("line"), at.line ? static_cast<int>(*at.line) : -1);
    map.insert(QStringLiteral("inLine"), at.in_line);
    map.insert(QStringLiteral("word"), at.word ? static_cast<int>(*at.word) : -1);
    map.insert(QStringLiteral("fraction"), at.fraction);
    map.insert(QStringLiteral("positionMs"), static_cast<qlonglong>(at.position_ms));

    QVariantList fills;
    if (at.line && *at.line < entry->lyrics.lines.size() && at.in_line) {
        const LyricLine& line = entry->lyrics.lines[*at.line];
        fills.reserve(static_cast<qsizetype>(line.words.size()));
        for (const LyricWord& word : line.words) {
            switch (iclforge::lyrics::word_state(word, at.position_ms)) {
                case WordState::kSung:
                    fills.push_back(1.0);
                    break;
                case WordState::kBeingSung: {
                    const double span = static_cast<double>(word.end_ms - word.start_ms);
                    const double through =
                        span > 0.0 ? static_cast<double>(at.position_ms - word.start_ms) / span : 0.0;
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
