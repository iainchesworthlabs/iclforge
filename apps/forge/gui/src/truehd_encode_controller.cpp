#include "truehd_encode_controller.hpp"

#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "truehd_encode_core.hpp"
#include "truehd_wrap.hpp"

namespace {

namespace th = iclforge::truehd;
namespace core = iclforge::apps::truehd_encode;
namespace wrap = iclforge::apps::truehd_wrap;

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// A path as a shell takes it on any of the three platforms: in double quotes.
QString quoted(const QString& path) {
    return QStringLiteral("\"%1\"").arg(path);
}

QString container_label(wrap::Container container) {
    switch (container) {
        case wrap::Container::kMatroska:
            return QStringLiteral("Matroska");
        case wrap::Container::kMp4:
            return QStringLiteral("MP4");
        case wrap::Container::kTransport:
            return QStringLiteral("a Blu-ray transport stream");
    }
    return {};
}

// The options the window takes besides verify and the downmix box, in the order the command line
// is written: the ones that shape the stream, then the channel meaning (the names the command line
// takes, core::meaning_option_names).
QStringList make_option_names() {
    QStringList names{
        QStringLiteral("restart"),     QStringLiteral("peak-rate"),   QStringLiteral("rate-check"),
        QStringLiteral("bitrate"),     QStringLiteral("major-sync"),  QStringLiteral("fir-order"),
        QStringLiteral("matrix"),      QStringLiteral("stereo-mode"), QStringLiteral("downmix-gains")};
    for (const std::string_view name : core::meaning_option_names()) {
        names.append(to_qstring(name));
    }
    return names;
}

const QStringList kOptionNames = make_option_names();

// Everything one run needs, copied out so the job holds nothing of the controller's.
struct Job {
    QString source;
    QString output;
    core::Settings settings;
};

struct Result {
    QString error;
    QString summary;
};

Result run(const Job& job, const std::atomic<bool>& stop,
           const std::function<void(double)>& report_progress) {
    Result result;
    auto wav = iclforge::ac3::io::read_wav(job.source.toStdString());
    if (!wav) {
        result.error = QStringLiteral("%1: %2").arg(
            job.source, to_qstring(iclforge::ac3::io::describe(wav.error())));
        return result;
    }
    if (wav->frame_count() == 0) {
        result.error = QStringLiteral("%1 has no samples to encode.").arg(job.source);
        return result;
    }
    const auto layout = core::layout_of(*wav);
    if (!layout) {
        result.error = QStringLiteral("%1: %2").arg(job.source, QString::fromStdString(layout.error()));
        return result;
    }
    th::EncoderConfig config;
    if (const auto applied = core::apply(job.settings, config); !applied) {
        result.error = to_qstring(applied.error());
        return result;
    }

    auto last_report = std::chrono::steady_clock::now();
    const auto outcome = core::encode_wav(
        *wav, *layout, config, [&](std::uint64_t done, std::uint64_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report > std::chrono::milliseconds(50) || done == total) {
                last_report = now;
                report_progress(static_cast<double>(done) / static_cast<double>(std::max<std::uint64_t>(total, 1)));
            }
            return !stop.load(std::memory_order_relaxed);
        });
    if (!outcome) {
        if (outcome.error().cancelled) {
            result.error = QStringLiteral("Stopped. Nothing was written.");
        } else {
            result.error = QStringLiteral("The encode failed: %1%2")
                               .arg(to_qstring(th::to_string(outcome.error().error.kind)),
                                    outcome.error().error.detail.empty()
                                        ? QString()
                                        : QStringLiteral(": ") + to_qstring(outcome.error().error.detail));
        }
        return result;
    }
    // The bare stream, or the same access units in the container the output's name asks for.
    std::vector<std::byte> wrapped;
    QString container_name;
    if (const auto container = wrap::container_of(job.output.toStdString())) {
        const auto scan = wrap::scan_stream(outcome->stream, job.source.toStdString());
        if (!scan) {
            result.error = QStringLiteral("The stream could not be wrapped: %1")
                               .arg(QString::fromStdString(scan.error()));
            return result;
        }
        auto file = wrap::mux(*container, *scan);
        if (!file) {
            result.error = QStringLiteral("The stream could not be wrapped: %1")
                               .arg(QString::fromStdString(file.error()));
            return result;
        }
        wrapped = std::move(*file);
        container_name = container_label(*container);
    }
    const std::vector<std::byte>& bytes = container_name.isEmpty() ? outcome->stream : wrapped;
    std::ofstream out{job.output.toStdString(), std::ios::binary | std::ios::trunc};
    if (out) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
    }
    if (!out) {
        result.error = QStringLiteral("Could not write %1.").arg(job.output);
        return result;
    }
    const double pcm_bytes = static_cast<double>(outcome->frames) * outcome->channels * 3.0;
    result.summary =
        QStringLiteral("Wrote %1: %2 channel(s) at %3 Hz, %4 s, %5 access units, %6 restart "
                       "intervals, %7 bytes (%8% of the 24-bit PCM)%9%10%11")
            .arg(job.output)
            .arg(outcome->channels)
            .arg(outcome->sample_rate)
            .arg(static_cast<double>(outcome->frames) / outcome->sample_rate, 0, 'f', 2)
            .arg(outcome->access_units)
            .arg(outcome->stats.restart_intervals)
            .arg(bytes.size())
            .arg(100.0 * static_cast<double>(outcome->stream.size()) / pcm_bytes, 0, 'f', 1)
            .arg(container_name.isEmpty() ? QString() : QStringLiteral(" · in %1").arg(container_name))
            .arg(job.settings.verify
                     ? QStringLiteral(" · verified: every access unit decodes to the input")
                     : QString())
            .arg(job.settings.meaning.empty()
                     ? QString()
                     : QStringLiteral(" · channel meaning written (provisional, no oracle until the "
                                      "DEE set)"));
    return result;
}

}  // namespace

TruehdEncodeController::TruehdEncodeController(QObject* parent) : QObject(parent) {}

TruehdEncodeController::~TruehdEncodeController() {
    stop_.store(true, std::memory_order_relaxed);
    jobs_.wait();
}

QString TruehdEncodeController::cliLine() const {
    QString line = QStringLiteral("forge truehd-encode %1 %2")
                       .arg(source_.isEmpty() ? QStringLiteral("<in.wav>") : quoted(source_),
                            output_.isEmpty() ? QStringLiteral("<out.thd>") : quoted(output_));
    if (downmix_) {
        line += QStringLiteral(" truehd-downmix=standard");
    }
    for (const QString& name : kOptionNames) {
        const auto found = option_text_.constFind(name);
        if (found != option_text_.cend()) {
            line += QStringLiteral(" truehd-%1=%2").arg(name, *found);
        }
    }
    if (verify_) {
        line += QStringLiteral(" verify");
    }
    return line;
}

QVariantMap TruehdEncodeController::options() const {
    QVariantMap map;
    for (const QString& name : kOptionNames) {
        map.insert(name, option_text_.value(name));
    }
    return map;
}

QStringList TruehdEncodeController::optionNames() {
    return kOptionNames;
}

void TruehdEncodeController::setOption(const QString& name, const QString& text) {
    if (busy_ || !kOptionNames.contains(name)) {
        return;
    }
    const QString trimmed = text.trimmed();
    QString sentence;
    if (!trimmed.isEmpty()) {
        core::Settings probe;
        const auto set =
            core::set_option(probe, name.toStdString(), trimmed.toStdString());
        if (!set) {
            sentence = QStringLiteral("%1 (got '%2')").arg(to_qstring(set.error()), trimmed);
        }
    }
    const QString before = option_text_.value(name);
    const bool had_error = option_errors_.contains(name);
    if (trimmed.isEmpty()) {
        option_text_.remove(name);
    } else {
        option_text_.insert(name, trimmed);
    }
    if (sentence.isEmpty()) {
        option_errors_.remove(name);
    } else {
        option_errors_.insert(name, sentence);
    }
    if (before == trimmed && had_error == !sentence.isEmpty()) {
        return;
    }
    emit optionsChanged();
    emit stateChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::resetOptions() {
    if (busy_ || (option_text_.isEmpty() && option_errors_.isEmpty())) {
        return;
    }
    option_text_.clear();
    option_errors_.clear();
    emit optionsChanged();
    emit stateChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::setVerify(bool on) {
    if (on == verify_) {
        return;
    }
    verify_ = on;
    emit optionsChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::setStandardDownmix(bool on) {
    if (on == downmix_) {
        return;
    }
    downmix_ = on;
    emit optionsChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::chooseSource(const QUrl& url) {
    if (busy_) {
        return;
    }
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    source_ = path;
    summary_.clear();
    error_.clear();
    emit sourceChanged();
    emit resultChanged();
    if (output_.isEmpty()) {
        const QFileInfo info{path};
        output_ = info.dir().filePath(info.completeBaseName() + QStringLiteral(".thd"));
        emit outputChanged();
    }
    emit stateChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::chooseOutput(const QUrl& url) {
    if (busy_) {
        return;
    }
    QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    if (!wrap::container_of(path.toStdString()) &&
        QFileInfo{path}.suffix().compare(QStringLiteral("thd"), Qt::CaseInsensitive) != 0) {
        path += QStringLiteral(".thd");  // anything but a container's name is the bare stream
    }
    output_ = path;
    emit outputChanged();
    emit stateChanged();
    emit cliLineChanged();
}

void TruehdEncodeController::encode() {
    if (!canEncode()) {
        return;
    }
    if (QFileInfo{source_}.canonicalFilePath() == QFileInfo{output_}.canonicalFilePath() &&
        QFileInfo::exists(output_)) {
        error_ = QStringLiteral("The output is the source.");
        emit resultChanged();
        return;
    }
    busy_ = true;
    stop_.store(false, std::memory_order_relaxed);
    progress_ = 0.0;
    summary_.clear();
    error_.clear();
    emit stateChanged();
    emit progressChanged();
    emit resultChanged();
    Job job{.source = source_, .output = output_, .settings = {}};
    job.settings.standard_downmix = downmix_;
    job.settings.verify = verify_;
    for (auto it = option_text_.cbegin(); it != option_text_.cend(); ++it) {
        // Each text was accepted by setOption, or canEncode would be false.
        (void)core::set_option(job.settings, it.key().toStdString(), it.value().toStdString());
    }
    jobs_.run([this, job] {
        const Result result = run(job, stop_, [this](double fraction) {
            QMetaObject::invokeMethod(this, [this, fraction] {
                progress_ = fraction;
                emit progressChanged();
            });
        });
        QMetaObject::invokeMethod(this, [this, result] {
            busy_ = false;
            summary_ = result.summary;
            error_ = result.error;
            if (result.error.isEmpty()) {
                progress_ = 1.0;
            }
            emit stateChanged();
            emit progressChanged();
            emit resultChanged();
        });
    });
}

void TruehdEncodeController::cancel() {
    stop_.store(true, std::memory_order_relaxed);
}

void TruehdEncodeController::clear() {
    if (busy_) {
        return;
    }
    source_.clear();
    output_.clear();
    summary_.clear();
    error_.clear();
    progress_ = 0.0;
    emit sourceChanged();
    emit outputChanged();
    emit resultChanged();
    emit progressChanged();
    emit stateChanged();
    emit cliLineChanged();
}
