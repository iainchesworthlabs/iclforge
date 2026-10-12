#include "lyrics_controller.hpp"

#include <QFile>
#include <QImage>
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

#include "cdg_frames.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "media_monitor.hpp"

namespace {

using iclforge::crucible::ui::MediaApplication;
using iclforge::crucible::ui::MediaPlayback;
using iclforge::crucible::ui::MediaSession;

using iclforge::lyrics::LyricLine;
using iclforge::lyrics::LyricPosition;
using iclforge::lyrics::Lyrics;
using iclforge::lyrics::LyricWord;
using iclforge::lyrics::WordState;

// No lyric file is anywhere near this; the library refuses a file past it too, and this refuses
// before reading it.
constexpr qint64 kMaxFileBytes = 4LL * 1024 * 1024;
// A CD's subcode is at most 80 minutes of 24-byte packets, 300 a second: under 35 MiB. The library
// refuses a file past this too, and this refuses before reading it.
constexpr qint64 kMaxDiscBytes = 64LL * 1024 * 1024;

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
    // A CD+G disc, when the file was one (status "picture").
    iclforge::apps::CdgFrames disc;
    QString status = QStringLiteral("none");
    QString file_name;
    QString error_text;
    QVariantList lines;
    // By hand (Start): `running` and `started_ms` are the position. Not by hand, the position is
    // the media session's.
    bool by_hand = false;
    bool running = false;
    qlonglong started_ms = 0;
    int nudge_ms = 0;
    // A media session had the position and went away: the words carry on by hand from where they
    // were, and say so.
    bool lost = false;
    // The session the person picked (held until it goes), or empty for the best match.
    std::string session_id;
    // The matched session as last read, the track it was when the lyrics were bound to it, and
    // whether it has since become another.
    std::optional<MediaSession> session;
    std::string bound_track;
    bool track_changed = false;
};

[[nodiscard]] bool is_followable(const Entry& entry) {
    return entry.status == QStringLiteral("timed") || entry.status == QStringLiteral("picture");
}

// The media session has the position: one is matched, it publishes a timeline, and it is still the
// track the lyrics were bound to.
[[nodiscard]] bool session_has_position(const Entry& entry) {
    return !entry.by_hand && entry.session.has_value() && entry.session->has_position && !entry.track_changed;
}

[[nodiscard]] bool is_following(const Entry& entry) {
    return is_followable(entry) && (entry.by_hand ? entry.running : session_has_position(entry));
}

// What changed for a person, as one string to compare before and after a read of the sessions.
[[nodiscard]] std::string stamp(const Entry& entry) {
    std::string out = entry.by_hand ? "h" : "s";
    out += entry.running ? "r" : "-";
    out += entry.lost ? "l" : "-";
    out += entry.track_changed ? "c" : "-";
    if (entry.session) {
        out += entry.session->id;
        out += entry.session->has_position ? "+" : "-";
        out += entry.session->playback == MediaPlayback::kPlaying ? "p" : "n";
        out += entry.session->title;
    }
    return out;
}

// What a list of sessions is, for knowing whether a picker has to reread.
[[nodiscard]] std::string signature(const std::vector<MediaSession>& sessions) {
    std::string out;
    for (const MediaSession& session : sessions) {
        out += session.id + "|" + session.title + "|" + session.artist + "|"
               + std::to_string(static_cast<int>(session.playback)) + "|" + (session.has_position ? "1" : "0") + "\n";
    }
    return out;
}

// An application's lyrics from the bytes of a file, as the reader takes them: an LRC file (UTF-8, or
// UTF-16 with a byte-order mark), a SubRip file, an Advanced SubStation Alpha script or a MIDI karaoke
// file, told apart by what is in them and not by their name (parse_timed_text). An entry with the
// lines, or one in the "error" state with the reader's reason.
[[nodiscard]] Entry make_entry(const QString& name, std::string_view bytes, qlonglong duration_ms) {
    Entry entry;
    entry.file_name = name;
    const std::optional<std::int64_t> duration =
        duration_ms > 0 ? std::optional<std::int64_t>{duration_ms} : std::nullopt;
    auto parsed = iclforge::lyrics::parse_timed_text(bytes, duration);
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

// An application's CD+G disc from the bytes of a `.cdg` file: an entry in the "picture" state, or in
// the "error" one with the reader's reason.
[[nodiscard]] Entry make_disc_entry(const QString& name, std::string_view bytes) {
    Entry entry;
    entry.file_name = name;
    if (auto loaded = entry.disc.load_bytes(bytes)) {
        entry.status = QStringLiteral("picture");
    } else {
        entry.status = QStringLiteral("error");
        entry.error_text = from_utf8(loaded.error());
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
    // What each application is, for matching its session: kept apart from the entries because the
    // strip says it before there is a file, and a new file does not forget it.
    std::unordered_map<int, MediaApplication> applications;
    // The sessions as last read, and whether any read (or a test's script) has been applied: a
    // reader that is switched off is "unavailable" until something is.
    std::vector<MediaSession> sessions;
    bool fed = false;
    // The thread that reads them; null when the environment turns it off (and in the QML suite).
    MediaMonitor* monitor = nullptr;

    [[nodiscard]] const Entry* find(int app) const {
        const auto it = entries.find(app);
        return it == entries.end() ? nullptr : &it->second;
    }
};

CrucibleLyrics::CrucibleLyrics(QObject* parent)
    : iclforge::apps::CdgFrameSource(parent), state_(std::make_unique<State>()) {
    // A new file, a clear or a nudge is a different picture at the same position.
    connect(this, &CrucibleLyrics::changed, this, &CrucibleLyrics::pictureChanged);
    // The sessions are read by a thread of the monitor's, only while an application with lyrics
    // wants one. ICLFORGE_CRUCIBLE_MEDIA_SESSIONS=off turns it off (and is what the QML suite sets, so
    // that a developer's own music does not move a test).
    if (qgetenv("ICLFORGE_CRUCIBLE_MEDIA_SESSIONS") != "off") {
        state_->monitor = new MediaMonitor(nullptr, MediaMonitor::kPollIntervalMs, this);
        connect(state_->monitor, &MediaMonitor::sessionsRead, this,
                [this](const std::vector<MediaSession>& read) { applySessions(read, nowMs()); });
    }
}

CrucibleLyrics::~CrucibleLyrics() {
    // The reading thread is joined while this object and its state are whole: a read that finished
    // after them would be delivered to a lambda that holds `this`.
    if (state_->monitor != nullptr) {
        state_->monitor->setWanted(false);
    }
}

qlonglong CrucibleLyrics::nowMs() const {
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<qlonglong>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

namespace {

// Whether any application with lyrics is waiting on a session.
[[nodiscard]] bool any_wants_sessions(const std::unordered_map<int, Entry>& entries) {
    for (const auto& item : entries) {
        if (is_followable(item.second) && !item.second.by_hand) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool CrucibleLyrics::importText(int app, const QString& name, const QString& text, qlonglong durationMs) {
    const QByteArray bytes = text.toUtf8();
    Entry entry = make_entry(name, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())},
                             durationMs);
    const bool ok = entry.status != QStringLiteral("error");
    state_->entries[app] = std::move(entry);
    refresh_monitor();
    emit changed(app);
    return ok;
}

bool CrucibleLyrics::importFile(int app, const QString& fileUrl, qlonglong durationMs) {
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    const qsizetype slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    const QString name = path.mid(slash + 1);
    QFile file(path);
    // A CD+G disc is told by its extension, in any case: its bytes are packets and say nothing
    // else, and Hearth finds a disc by the same extension (find_cdg_file).
    const bool disc = path.endsWith(QStringLiteral(".cdg"), Qt::CaseInsensitive);
    Entry entry;
    if (!file.open(QIODevice::ReadOnly)) {
        entry = failed_entry(name, disc ? QStringLiteral("The CD+G file could not be opened.")
                                        : QStringLiteral("The lyrics file could not be opened."));
    } else if (file.size() > (disc ? kMaxDiscBytes : kMaxFileBytes)) {
        entry = failed_entry(name, disc ? QStringLiteral("The CD+G file is too large to be a disc.")
                                        : QStringLiteral("The lyrics file is too large to be lyrics."));
    } else if (disc) {
        const QByteArray bytes = file.readAll();
        entry = make_disc_entry(name, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())});
    } else {
        // The bytes go to the reader as they are: it decides UTF-8, UTF-16 or neither, and says why.
        const QByteArray bytes = file.readAll();
        entry = make_entry(name, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())},
                           durationMs);
    }
    const bool ok = entry.status != QStringLiteral("error");
    state_->entries[app] = std::move(entry);
    refresh_monitor();
    emit changed(app);
    return ok;
}

void CrucibleLyrics::clear(int app) {
    if (state_->entries.erase(app) > 0) {
        refresh_monitor();
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
        state_->applications.erase(app);
        emit changed(app);
    }
    refresh_monitor();
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
    // A plain file has no times to follow and an error has no lyrics: only a timed file or a disc
    // starts.
    if (it == state_->entries.end()
        || (it->second.status != QStringLiteral("timed") && it->second.status != QStringLiteral("picture"))) {
        return;
    }
    // Start is by hand: it takes the position from the person, from this moment, and lets go of
    // whichever session had it.
    it->second.by_hand = true;
    it->second.running = true;
    it->second.started_ms = now_ms;
    it->second.lost = false;
    refresh_monitor();
    emit changed(app);
}

void CrucibleLyrics::stop(int app) {
    const auto it = state_->entries.find(app);
    if (it == state_->entries.end() || !is_following(it->second)) {
        return;
    }
    // Stopping a session's following is by hand too: it stays stopped until the person says
    // (Start, or the media session again), and does not start again with the next read.
    it->second.by_hand = true;
    it->second.running = false;
    it->second.lost = false;
    refresh_monitor();
    emit changed(app);
}

bool CrucibleLyrics::running(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr && is_following(*entry);
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
    if (entry == nullptr || !is_following(*entry)) {
        return -1;
    }
    if (entry->by_hand) {
        return std::max<qlonglong>(0, now_ms - entry->started_ms);
    }
    // The session's own position, carried forward from when it was read.
    const std::optional<std::int64_t> position = iclforge::crucible::ui::position_at(*entry->session, now_ms);
    return position ? static_cast<qlonglong>(*position) : -1;
}

void CrucibleLyrics::refresh_monitor() {
    if (state_->monitor != nullptr) {
        state_->monitor->setWanted(any_wants_sessions(state_->entries));
    }
}

void CrucibleLyrics::setApplication(int app, const QString& name, const QString& imagePath, const QString& appId) {
    MediaApplication facts;
    facts.name = name.toStdString();
    facts.image_path = imagePath.toStdString();
    facts.app_id = appId.toStdString();
    const auto it = state_->applications.find(app);
    if (it != state_->applications.end() && it->second.name == facts.name && it->second.image_path == facts.image_path
        && it->second.app_id == facts.app_id) {
        return;
    }
    state_->applications[app] = std::move(facts);
    // Another application, or another name for it, is another match: ask again of what was read.
    if (state_->entries.contains(app)) {
        applySessions(state_->sessions, nowMs());
    }
}

QString CrucibleLyrics::positionSource(int app) const {
    const Entry* entry = state_->find(app);
    return entry != nullptr && entry->by_hand ? QStringLiteral("hand") : QStringLiteral("session");
}

bool CrucibleLyrics::sessionsAvailable() const {
    if (state_->monitor == nullptr) {
        // Switched off: only what a script has fed is there to follow.
        return state_->fed;
    }
    return state_->monitor->support().available;
}

QString CrucibleLyrics::sessionsUnavailableReason() const {
    if (state_->monitor == nullptr) {
        return state_->fed ? QString{}
                           : QStringLiteral("Reading what other players are playing is turned off.");
    }
    const iclforge::crucible::ui::MediaSessionSupport support = state_->monitor->support();
    return support.available ? QString{} : from_utf8(support.reason);
}

QString CrucibleLyrics::sessionState(int app) const {
    const Entry* entry = state_->find(app);
    if (entry == nullptr || !is_followable(*entry)) {
        return QStringLiteral("none");
    }
    if (entry->by_hand) {
        return entry->lost ? QStringLiteral("lost") : QStringLiteral("hand");
    }
    if (!sessionsAvailable()) {
        return QStringLiteral("unavailable");
    }
    if (entry->session && entry->track_changed) {
        return QStringLiteral("changed");
    }
    if (entry->session) {
        return entry->session->has_position ? QStringLiteral("following") : QStringLiteral("no-timeline");
    }
    return QStringLiteral("waiting");
}

QString CrucibleLyrics::sessionLabel(int app) const {
    const Entry* entry = state_->find(app);
    if (entry == nullptr || !entry->session) {
        return {};
    }
    const MediaSession& session = *entry->session;
    return from_utf8(session.name.empty() ? session.id : session.name);
}

QString CrucibleLyrics::sessionTrack(int app) const {
    const Entry* entry = state_->find(app);
    if (entry == nullptr || !entry->session) {
        return {};
    }
    const MediaSession& session = *entry->session;
    if (session.title.empty()) {
        return {};
    }
    return from_utf8(session.artist.empty() ? session.title : session.title + " - " + session.artist);
}

void CrucibleLyrics::useSession(int app, const QString& sessionId) {
    const auto it = state_->entries.find(app);
    if (it == state_->entries.end() || !is_followable(it->second)) {
        return;
    }
    Entry& entry = it->second;
    entry.by_hand = false;
    entry.running = false;
    entry.lost = false;
    entry.track_changed = false;
    entry.session_id = sessionId.toStdString();
    entry.session.reset();
    entry.bound_track.clear();
    refresh_monitor();
    // Match at once against what was last read, so that the strip has it before the next poll.
    applySessions(state_->sessions, nowMs());
    emit changed(app);
}

QVariantList CrucibleLyrics::sessions(int app) const {
    QVariantList out;
    const Entry* entry = state_->find(app);
    for (const MediaSession& session : state_->sessions) {
        QVariantMap map;
        map.insert(QStringLiteral("id"), from_utf8(session.id));
        map.insert(QStringLiteral("name"), from_utf8(session.name.empty() ? session.id : session.name));
        map.insert(QStringLiteral("title"), from_utf8(session.title));
        map.insert(QStringLiteral("artist"), from_utf8(session.artist));
        map.insert(QStringLiteral("state"), session.playback == MediaPlayback::kPlaying ? QStringLiteral("playing")
                                            : session.playback == MediaPlayback::kPaused ? QStringLiteral("paused")
                                                                                         : QStringLiteral("stopped"));
        map.insert(QStringLiteral("hasPosition"), session.has_position);
        map.insert(QStringLiteral("matched"),
                   entry != nullptr && entry->session.has_value() && entry->session->id == session.id);
        out.push_back(map);
    }
    return out;
}

void CrucibleLyrics::applySessions(const std::vector<MediaSession>& read, qlonglong nowMs) {
    const bool list_changed = !state_->fed || signature(read) != signature(state_->sessions);
    state_->fed = true;
    state_->sessions = read;

    std::vector<int> touched;
    for (auto& item : state_->entries) {
        Entry& entry = item.second;
        if (!is_followable(entry) || entry.by_hand) {
            continue;
        }
        const std::string before = stamp(entry);

        std::optional<std::size_t> pick;
        if (!entry.session_id.empty()) {
            for (std::size_t i = 0; i < read.size(); ++i) {
                if (read[i].id == entry.session_id) {
                    pick = i;
                    break;
                }
            }
        } else if (const auto facts = state_->applications.find(item.first); facts != state_->applications.end()) {
            pick = iclforge::crucible::ui::best_session(facts->second, read);
        }

        if (pick) {
            entry.session = read[*pick];
            const std::string key = iclforge::crucible::ui::track_key(*entry.session);
            if (entry.bound_track.empty()) {
                entry.bound_track = key;
            } else if (!key.empty() && key != entry.bound_track) {
                entry.track_changed = true;
            }
        } else if (entry.session) {
            // It had a session and the session went: the words carry on from where they were, by
            // hand, and say so, rather than jumping or stopping. A session that was not playing
            // has no time to carry on with: it is held where it was, stopped.
            const std::optional<std::int64_t> position = iclforge::crucible::ui::position_at(*entry.session, nowMs);
            const bool was_playing = entry.session->playback == MediaPlayback::kPlaying && !entry.track_changed;
            entry.by_hand = true;
            entry.running = position.has_value() && was_playing;
            entry.started_ms = nowMs - position.value_or(0);
            entry.lost = true;
            entry.session.reset();
        }
        if (stamp(entry) != before) {
            touched.push_back(item.first);
        }
    }
    refresh_monitor();
    for (const int app : touched) {
        emit changed(app);
    }
    if (list_changed) {
        emit sessionsChanged();
    }
}

void CrucibleLyrics::applySessionList(const QVariantList& read, qlonglong nowMs) {
    std::vector<MediaSession> sessions;
    sessions.reserve(static_cast<std::size_t>(read.size()));
    for (const QVariant& value : read) {
        const QVariantMap map = value.toMap();
        MediaSession session;
        session.id = map.value(QStringLiteral("id")).toString().toStdString();
        session.name = map.value(QStringLiteral("name")).toString().toStdString();
        session.executable = map.value(QStringLiteral("executable")).toString().toStdString();
        session.desktop_id = map.value(QStringLiteral("desktopId")).toString().toStdString();
        session.title = map.value(QStringLiteral("title")).toString().toStdString();
        session.artist = map.value(QStringLiteral("artist")).toString().toStdString();
        session.duration_ms = map.value(QStringLiteral("durationMs")).toLongLong();
        session.has_position = map.value(QStringLiteral("hasPosition"), true).toBool();
        session.position_ms = map.value(QStringLiteral("positionMs")).toLongLong();
        session.read_at_ms = nowMs;
        session.rate = map.value(QStringLiteral("rate"), 1.0).toDouble();
        const QString playback = map.value(QStringLiteral("playback"), QStringLiteral("playing")).toString();
        session.playback = playback == QLatin1String("playing")  ? MediaPlayback::kPlaying
                           : playback == QLatin1String("paused") ? MediaPlayback::kPaused
                                                                 : MediaPlayback::kStopped;
        sessions.push_back(std::move(session));
    }
    applySessions(sessions, nowMs);
}

QImage CrucibleLyrics::pictureFrame(int key, qlonglong positionMs) {
    const auto it = state_->entries.find(key);
    if (it == state_->entries.end() || it->second.status != QStringLiteral("picture")) {
        return {};
    }
    Entry& entry = it->second;
    // The nudge shows the words later by moving the song's position back, and the picture the same.
    return entry.disc.frame(std::max<qlonglong>(0, positionMs) - entry.nudge_ms);
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
    if (entry == nullptr || !is_following(*entry) || !entry->lyrics.timed) {
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
