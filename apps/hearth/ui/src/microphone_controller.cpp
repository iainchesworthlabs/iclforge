#include "microphone_controller.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariant>

// Qt's headers define `slots` as a macro, and iclforge::render::OutputLayout has a method of that
// name (hearth-ui-qt-slots-macro-collides-with-render-layout): undone before the engine's headers,
// which reach render.hpp through the item loader.
#undef slots

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "item_loader.hpp"
#include "microphone_input.hpp"
#include "microphone_session.hpp"
#include "reference_melody.hpp"
#include "take_recorder.hpp"

namespace iclforge::hearth::ui {

namespace {

constexpr int kTickMs = 20;
// What the page is told every this many ticks: 100 ms, which is as often as a meter is worth
// repainting and keeps the bindings quiet.
constexpr int kPublishEvery = 5;

[[nodiscard]] std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The same organisation and application HearthController's own settings use.
[[nodiscard]] QSettings kept() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
                     QStringLiteral("Hearth"));
}

[[nodiscard]] double level_from_db(double db) {
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

[[nodiscard]] std::optional<ReferencePart> part_from_text(const QString& text) {
    if (text == QLatin1String("m")) {
        return ReferencePart::kMelodyGuide;
    }
    if (text == QLatin1String("v1")) {
        return ReferencePart::kVocal1;
    }
    if (text == QLatin1String("v2")) {
        return ReferencePart::kVocal2;
    }
    return std::nullopt;
}

}  // namespace

struct MicrophoneController::State {
    State(std::unique_ptr<iclforge::hearth::MicrophoneInput> input,
          std::function<std::int64_t()> clock_fn)
        : session(std::move(input)), clock(std::move(clock_fn)) {}

    iclforge::hearth::MicrophoneSession session;
    std::function<std::int64_t()> clock;
    // The reference job in flight: stopped by raising its flag, and joined by the destructor.
    std::shared_ptr<std::atomic<bool>> cancel;
    std::jthread worker;
    int generation = 0;
};

MicrophoneController::MicrophoneController(QObject* parent)
    : MicrophoneController(iclforge::hearth::make_device_input(), parent) {}

MicrophoneController::MicrophoneController(std::unique_ptr<iclforge::hearth::MicrophoneInput> input,
                                           QObject* parent)
    : MicrophoneController(std::move(input), steady_now_ns, parent) {}

MicrophoneController::MicrophoneController(std::unique_ptr<iclforge::hearth::MicrophoneInput> input,
                                           std::function<std::int64_t()> clock, QObject* parent)
    : QObject(parent), state_data_(std::make_unique<State>(std::move(input), std::move(clock))) {
    // The preferences: which input, what the person said of their equipment, how they aligned it.
    // Never "on": a microphone is not opened by anything that was kept.
    QSettings settings = kept();
    device_id_ = settings.value(QStringLiteral("microphone/deviceId")).toString();
    headphones_ = settings.value(QStringLiteral("microphone/headphones"), false).toBool();
    takes_folder_ = settings.value(QStringLiteral("microphone/takesFolder")).toString();
    if (takes_folder_.isEmpty()) {
        // Offered, not made: the person's own documents, under a folder of Hearth's.
        const QString documents =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        takes_folder_ = QDir(documents.isEmpty() ? QDir::homePath() : documents)
                            .filePath(QStringLiteral("Hearth takes"));
    }
    input_delay_ms_ = std::clamp(
        settings.value(QStringLiteral("microphone/inputDelayMs"), 0).toInt(), 0, kMaxAlignmentMs);
    offset_ms_ = std::clamp(settings.value(QStringLiteral("microphone/offsetMs"), 0).toInt(),
                            -kMaxAlignmentMs, kMaxAlignmentMs);
    state_data_->session.set_headphones(headphones_);
    state_data_->session.set_input_delay_ms(input_delay_ms_);
    state_data_->session.set_offset_ms(offset_ms_);

    timer_.setInterval(kTickMs);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    publish(true);
}

MicrophoneController::~MicrophoneController() {
    if (state_data_->cancel) {
        state_data_->cancel->store(true);
    }
    // Closing the device is part of going away: nothing outlives the window listening.
    state_data_->session.stop();
}

void MicrophoneController::setDeviceId(const QString& id) {
    if (id == device_id_) {
        return;
    }
    device_id_ = id;
    kept().setValue(QStringLiteral("microphone/deviceId"), id);
    emit deviceChanged();
}

void MicrophoneController::setHeadphones(bool headphones) {
    if (headphones == headphones_) {
        return;
    }
    headphones_ = headphones;
    kept().setValue(QStringLiteral("microphone/headphones"), headphones);
    state_data_->session.set_headphones(headphones);
    emit headphonesChanged();
    publish(true);
}

void MicrophoneController::setInputDelayMs(int ms) {
    ms = std::clamp(ms, 0, kMaxAlignmentMs);
    if (ms == input_delay_ms_) {
        return;
    }
    input_delay_ms_ = ms;
    kept().setValue(QStringLiteral("microphone/inputDelayMs"), ms);
    state_data_->session.set_input_delay_ms(ms);
    emit alignmentChanged();
}

void MicrophoneController::setOffsetMs(int ms) {
    ms = std::clamp(ms, -kMaxAlignmentMs, kMaxAlignmentMs);
    if (ms == offset_ms_) {
        return;
    }
    offset_ms_ = ms;
    kept().setValue(QStringLiteral("microphone/offsetMs"), ms);
    state_data_->session.set_offset_ms(ms);
    emit alignmentChanged();
}

void MicrophoneController::refreshDevices() {
    QVariantList list;
    for (const iclforge::hearth::InputDevice& d : state_data_->session.devices()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), QString::fromStdString(d.id));
        row.insert(QStringLiteral("name"), QString::fromStdString(d.name));
        row.insert(QStringLiteral("isDefault"), d.is_default);
        list.push_back(row);
    }
    devices_ = list;
    emit devicesChanged();
}

bool MicrophoneController::start() {
    if (on()) {
        return true;
    }
    const auto started = state_data_->session.start(device_id_.toStdString());
    if (!started) {
        fail(QString::fromStdString(started.error()));
        return false;
    }
    state_ = QStringLiteral("on");
    error_text_.clear();
    ticks_ = 0;
    timer_.start();
    emit stateChanged();
    publish(true);
    return true;
}

void MicrophoneController::stop() {
    // A take running when the microphone is turned off is a take: kept, and reported.
    if (recording_) {
        stopRecording();
    }
    timer_.stop();
    state_data_->session.stop();
    const bool changed = state_ != QLatin1String("off") || !error_text_.isEmpty();
    state_ = QStringLiteral("off");
    error_text_.clear();
    if (changed) {
        emit stateChanged();
    }
    publish(true);
}

bool MicrophoneController::startRecording() {
    if (recording_) {
        return true;
    }
    take_error_.clear();
    if (!on()) {
        take_error_ = tr("Turn the microphone on first.");
        emit recordingChanged();
        return false;
    }
    const QString title = QFileInfo(item_path_).completeBaseName();
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    const std::string stem =
        iclforge::hearth::take_file_stem(title.toStdString(), stamp.toStdString());
    // A name is to the second, so a take begun at once after another would write over it: a number
    // is added to the name when the file, or its description, is there already.
    const QString base = QDir(takes_folder_).filePath(QString::fromStdString(stem));
    const auto taken = [](const QString& name) {
        return QFileInfo::exists(name + QStringLiteral(".flac")) ||
               QFileInfo::exists(name + QStringLiteral(".take.json"));
    };
    QString chosen = base;
    for (int n = 2; taken(chosen); ++n) {
        chosen = base + QStringLiteral(" (%1)").arg(n);
    }
    const QString file = chosen + QStringLiteral(".flac");

    iclforge::hearth::TakeMetadata metadata;
    metadata.item_path = item_path_.toStdString();
    metadata.item_title = title.toStdString();
    metadata.reference =
        reference_status_ == QLatin1String("ready") ? reference_text_.toStdString() : std::string{};
    const auto begun =
        state_data_->session.begin_take(std::filesystem::path(file.toStdU16String()), metadata);
    if (!begun) {
        take_error_ = QString::fromStdString(begun.error());
        emit recordingChanged();
        return false;
    }
    recording_ = true;
    take_seconds_ = 0.0;
    emit recordingChanged();
    return true;
}

void MicrophoneController::stopRecording() {
    if (!recording_) {
        return;
    }
    recording_ = false;
    const auto done = state_data_->session.end_take();
    if (done) {
        remember_take(*done);
    } else {
        take_error_ = QString::fromStdString(done.error());
    }
    take_seconds_ = 0.0;
    emit recordingChanged();
}

void MicrophoneController::remember_take(const iclforge::hearth::TakeInfo& info) {
    QVariantMap take;
    const QString path = QString::fromStdU16String(info.audio.u16string());
    take.insert(QStringLiteral("path"), path);
    take.insert(QStringLiteral("name"), QFileInfo(path).fileName());
    take.insert(QStringLiteral("folder"), QFileInfo(path).absolutePath());
    take.insert(QStringLiteral("seconds"), info.seconds);
    last_take_ = take;
}

bool MicrophoneController::deleteLastTake() {
    const QString path = last_take_.value(QStringLiteral("path")).toString();
    if (path.isEmpty() || recording_) {
        return false;
    }
    bool ok = QFile::remove(path);
    QString sidecar = path;
    sidecar.chop(5);  // ".flac"
    sidecar += QStringLiteral(".take.json");
    ok = QFile::remove(sidecar) && ok;
    last_take_.clear();
    emit recordingChanged();
    return ok;
}

void MicrophoneController::showTakes() {
    const QString folder = last_take_.contains(QStringLiteral("folder"))
                               ? last_take_.value(QStringLiteral("folder")).toString()
                               : takes_folder_;
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void MicrophoneController::setTakesFolder(const QString& urlOrPath) {
    const QUrl url(urlOrPath);
    const QString path = url.isLocalFile() ? url.toLocalFile() : urlOrPath;
    if (path.isEmpty() || path == takes_folder_) {
        return;
    }
    takes_folder_ = QDir::toNativeSeparators(path);
    kept().setValue(QStringLiteral("microphone/takesFolder"), takes_folder_);
    emit takesFolderChanged();
}

void MicrophoneController::restartScore() {
    state_data_->session.clear_singer();
    publish(true);
}

void MicrophoneController::fail(const QString& text) {
    if (recording_) {
        stopRecording();
    }
    timer_.stop();
    state_data_->session.stop();
    state_ = QStringLiteral("error");
    error_text_ = text;
    emit stateChanged();
    publish(true);
}

QString MicrophoneController::preferredPart(const QString& reproduction, const QString& vocals) {
    if (reproduction == QLatin1String("capable")) {
        if (vocals == QLatin1String("none")) {
            return QStringLiteral("v1");
        }
        if (vocals == QLatin1String("v1")) {
            return QStringLiteral("v2");
        }
        if (vocals == QLatin1String("v2")) {
            return QStringLiteral("v1");
        }
    }
    return QStringLiteral("m");
}

void MicrophoneController::load(const QString& itemPath, const QString& prefer) {
    if (itemPath == item_path_ && prefer == prefer_) {
        return;
    }
    const bool item_changed = itemPath != item_path_;
    item_path_ = itemPath;
    prefer_ = prefer;
    // The score is of an item: another one starts it again, and a microphone left on with nothing
    // to sing along to is closed.
    if (item_changed) {
        state_data_->session.clear_singer();
        if (itemPath.isEmpty() && on()) {
            stop();
        }
    }

    if (state_data_->cancel) {
        state_data_->cancel->store(true);
    }
    state_data_->session.set_reference(std::nullopt);
    if (itemPath.isEmpty()) {
        reference_status_ = QStringLiteral("none");
        reference_text_ = tr("Nothing is playing.");
        emit referenceChanged();
        publish(true);
        return;
    }

    reference_status_ = QStringLiteral("loading");
    reference_text_ = tr("Reading the melody…");
    emit referenceChanged();

    // The reading is a pass over the whole stream, so it is not done here. A job that was
    // superseded (its flag raised) is thrown away when it reports.
    const int generation = ++state_data_->generation;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    state_data_->cancel = cancel;
    const std::string path = itemPath.toStdString();
    const std::optional<ReferencePart> part = part_from_text(prefer);
    QPointer<MicrophoneController> self(this);
    // A previous worker has been told to stop; joining it here is brief (it looks every 256
    // frames) and keeps one at a time.
    if (state_data_->worker.joinable()) {
        state_data_->worker.request_stop();
        state_data_->worker.join();
    }
    state_data_->worker = std::jthread([self, cancel, path, part, generation] {
        struct Outcome {
            bool ok = false;
            QString text;
            std::optional<iclforge::pitch::Contour> contour;
        };
        auto outcome = std::make_shared<Outcome>();
        const auto loaded = make_file_item_loader()(path);
        if (!loaded) {
            outcome->text = QString::fromStdString(loaded.error());
        } else {
            auto result = reference_from_karaoke(loaded->bytes, part, cancel.get());
            if (result) {
                outcome->ok = true;
                outcome->text = QString::fromStdString(result->description);
                outcome->contour = std::move(result->contour);
            } else {
                outcome->text = QString::fromStdString(result.error());
            }
        }
        if (cancel->load()) {
            return;
        }
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [self, outcome, generation] {
                if (!self || self->state_data_->generation != generation) {
                    return;
                }
                if (outcome->ok) {
                    self->reference_status_ = QStringLiteral("ready");
                    self->reference_text_ = outcome->text;
                    self->state_data_->session.set_reference(std::move(outcome->contour));
                } else {
                    self->reference_status_ = QStringLiteral("none");
                    self->reference_text_ = outcome->text;
                    self->state_data_->session.set_reference(std::nullopt);
                }
                emit self->referenceChanged();
                self->publish(true);
            },
            Qt::QueuedConnection);
    });
}

void MicrophoneController::anchor(qlonglong positionMs, bool playing) {
    state_data_->session.anchor(positionMs, playing, state_data_->clock());
}

void MicrophoneController::tick() {
    using Health = iclforge::hearth::MicrophoneHealth;
    const Health health = state_data_->session.pump(state_data_->clock());
    if (health == Health::kGone) {
        fail(tr("The microphone went away. Plug it in again and turn it back on."));
        return;
    }
    if (health == Health::kStalled) {
        fail(tr("The microphone stopped delivering audio. It may have been disconnected."));
        return;
    }
    if (++ticks_ >= kPublishEvery) {
        ticks_ = 0;
        publish(false);
    }
}

void MicrophoneController::publish(bool force) {
    const iclforge::hearth::MicrophoneSnapshot s = state_data_->session.snapshot();

    const double db = static_cast<double>(s.level_db);
    const double level = level_from_db(db);
    const bool meter_changed = force || std::fabs(level - level_) > 0.004 ||
                               s.clipping != clipping_ ||
                               std::fabs(s.silent_seconds - silent_seconds_) > 0.5;
    level_db_ = db;
    level_ = level;
    clipping_ = s.clipping;
    silent_seconds_ = s.silent_seconds;
    if (meter_changed) {
        emit meterChanged();
    }

    if (recording_) {
        if (!s.taking) {
            // The session ended it by itself: the disk filled. Say so, and keep what is there.
            recording_ = false;
            take_error_ = QString::fromStdString(s.take_error);
            if (const auto kept = state_data_->session.take_ended_by_itself()) {
                remember_take(*kept);
            }
            take_seconds_ = 0.0;
            emit recordingChanged();
        } else if (std::fabs(s.take_seconds - take_seconds_) >= 0.1) {
            take_seconds_ = s.take_seconds;
            emit recordingChanged();
        }
    }

    QVariantMap live;
    if (s.live) {
        live.insert(QStringLiteral("voiced"), s.live->voiced);
        live.insert(QStringLiteral("hz"), static_cast<double>(s.live->hz));
        live.insert(QStringLiteral("referenceHz"), static_cast<double>(s.live->reference_hz));
        live.insert(QStringLiteral("itemMs"), static_cast<qlonglong>(s.live->item_ms));
        if (s.live->cents_from_reference) {
            live.insert(QStringLiteral("centsFromReference"), *s.live->cents_from_reference);
        }
    }
    if (live != live_) {
        live_ = live;
        emit liveChanged();
    }

    QVariantMap score;
    score.insert(QStringLiteral("withheld"), s.score_withheld);
    score.insert(QStringLiteral("percent"),
                 s.total.counted > 0 && s.total.sung > 0
                     ? static_cast<int>(std::lround(s.total.fraction() * 100.0))
                     : -1);
    score.insert(QStringLiteral("coveragePercent"),
                 static_cast<int>(std::lround(s.total.coverage() * 100.0)));
    score.insert(QStringLiteral("meanCents"), s.total.mean_cents);
    score.insert(QStringLiteral("counted"), static_cast<int>(s.total.counted));
    score.insert(QStringLiteral("hit"), static_cast<int>(s.total.hit));
    score.insert(QStringLiteral("unplaced"), static_cast<qulonglong>(s.unplaced_frames));
    score.insert(QStringLiteral("timingEstimated"), s.timing_estimated);
    if (s.lag_ms) {
        score.insert(QStringLiteral("lagMs"), static_cast<int>(*s.lag_ms));
    }
    if (s.key_offset_cents) {
        score.insert(QStringLiteral("keyOffsetCents"), *s.key_offset_cents);
    }
    if (score != score_) {
        score_ = score;
        emit scoreChanged();
    }
}

}  // namespace iclforge::hearth::ui
