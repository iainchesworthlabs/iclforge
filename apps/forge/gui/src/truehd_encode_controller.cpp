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

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "truehd_encode_core.hpp"

namespace {

namespace th = iclforge::truehd;
namespace core = iclforge::apps::truehd_encode;

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// A path as a shell takes it on any of the three platforms: in double quotes.
QString quoted(const QString& path) {
    return QStringLiteral("\"%1\"").arg(path);
}

// Everything one run needs, copied out so the job holds nothing of the controller's.
struct Job {
    QString source;
    QString output;
    bool verify = false;
    bool downmix = false;
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
    config.downmix = job.downmix ? th::DownmixMode::kStandard : th::DownmixMode::kNone;
    config.verify = job.verify;

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
    std::ofstream out{job.output.toStdString(), std::ios::binary | std::ios::trunc};
    if (out) {
        out.write(reinterpret_cast<const char*>(outcome->stream.data()),
                  static_cast<std::streamsize>(outcome->stream.size()));
        out.flush();
    }
    if (!out) {
        result.error = QStringLiteral("Could not write %1.").arg(job.output);
        return result;
    }
    const double pcm_bytes = static_cast<double>(outcome->frames) * outcome->channels * 3.0;
    result.summary =
        QStringLiteral("Wrote %1: %2 channel(s) at %3 Hz, %4 s, %5 access units, %6 restart "
                       "intervals, %7 bytes (%8% of the 24-bit PCM)%9")
            .arg(job.output)
            .arg(outcome->channels)
            .arg(outcome->sample_rate)
            .arg(static_cast<double>(outcome->frames) / outcome->sample_rate, 0, 'f', 2)
            .arg(outcome->access_units)
            .arg(outcome->stats.restart_intervals)
            .arg(outcome->stream.size())
            .arg(100.0 * static_cast<double>(outcome->stream.size()) / pcm_bytes, 0, 'f', 1)
            .arg(job.verify ? QStringLiteral(" · verified: every access unit decodes to the input")
                            : QString());
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
    if (verify_) {
        line += QStringLiteral(" verify");
    }
    return line;
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
    if (QFileInfo{path}.suffix().compare(QStringLiteral("thd"), Qt::CaseInsensitive) != 0) {
        path += QStringLiteral(".thd");  // this window writes the bare stream
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
    const Job job{.source = source_, .output = output_, .verify = verify_, .downmix = downmix_};
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
