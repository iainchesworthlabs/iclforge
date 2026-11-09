#include "truehd_encode_controller.hpp"

#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
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
    QStringList extra_sources;
    QString map;                    // empty: the first WAV alone, its channel mask naming the speakers
    std::vector<double> offsets_s;  // by source, the first WAV being 0
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
    std::vector<iclforge::ac3::io::WavData> sources;
    for (const QString& path : QStringList{job.source} + job.extra_sources) {
        auto more = iclforge::ac3::io::read_wav(path.toStdString());
        if (!more) {
            result.error = QStringLiteral("%1: %2").arg(
                path, to_qstring(iclforge::ac3::io::describe(more.error())));
            return result;
        }
        sources.push_back(std::move(*more));
    }
    // A start offset is seconds at the first WAV's sample rate, as the command line takes it.
    std::vector<std::size_t> offset_samples(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const double seconds = i < job.offsets_s.size() ? job.offsets_s[i] : 0.0;
        offset_samples[i] = static_cast<std::size_t>(
            std::llround(seconds * static_cast<double>(sources.front().sample_rate)));
    }
    iclforge::ac3::io::WavData wav;
    core::Layout layout;
    QString placed;  // map: where each channel of the stream came from
    if (!job.map.isEmpty()) {
        const auto placements = core::parse_map(job.map.toStdString());
        if (!placements) {
            result.error = QString::fromStdString(placements.error());
            return result;
        }
        auto joined = core::assemble(sources, offset_samples, *placements);
        if (!joined) {
            result.error = QString::fromStdString(joined.error());
            return result;
        }
        for (const core::Placement& p : *placements) {
            placed += QStringLiteral("%1%2 <- %3.%4")
                          .arg(placed.isEmpty() ? QString() : QStringLiteral(", "),
                               to_qstring(th::channel_name(p.place)))
                          .arg(p.source)
                          .arg(p.channel);
        }
        wav = std::move(joined->wav);
        layout = std::move(joined->layout);
    } else {
        wav = std::move(sources.front());
        if (offset_samples.front() != 0) {
            core::pad_leading(wav, offset_samples.front());
        }
        auto named = core::layout_of(wav);
        if (!named) {
            result.error = QStringLiteral("%1: %2").arg(job.source, QString::fromStdString(named.error()));
            return result;
        }
        layout = std::move(*named);
    }
    if (wav.frame_count() == 0) {
        result.error = QStringLiteral("%1 has no samples to encode.").arg(job.source);
        return result;
    }
    th::EncoderConfig config;
    if (const auto applied = core::apply(job.settings, config); !applied) {
        result.error = to_qstring(applied.error());
        return result;
    }

    auto last_report = std::chrono::steady_clock::now();
    const auto outcome = core::encode_wav(
        wav, layout, config, [&](std::uint64_t done, std::uint64_t total) {
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
                       "intervals, %7 bytes (%8% of the 24-bit PCM)%9%10%11%12")
            .arg(job.output)
            .arg(outcome->channels)
            .arg(outcome->sample_rate)
            .arg(static_cast<double>(outcome->frames) / outcome->sample_rate, 0, 'f', 2)
            .arg(outcome->access_units)
            .arg(outcome->stats.restart_intervals)
            .arg(bytes.size())
            .arg(100.0 * static_cast<double>(outcome->stream.size()) / pcm_bytes, 0, 'f', 1)
            .arg(container_name.isEmpty() ? QString() : QStringLiteral(" · in %1").arg(container_name))
            .arg(placed.isEmpty() ? QString() : QStringLiteral(" · placed: %1").arg(placed))
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
    for (const QString& extra : extra_sources_) {
        line += QStringLiteral(" src=%1").arg(quoted(extra));
    }
    if (!map_text_.isEmpty()) {
        line += QStringLiteral(" map=%1").arg(map_text_.contains(' ') ? quoted(map_text_) : map_text_);
    }
    for (qsizetype i = 0; i < offset_text_.size(); ++i) {
        if (!offset_text_[i].isEmpty() && offset_error_[i].isEmpty()) {
            line += QStringLiteral(" offset=%1:%2").arg(i).arg(offset_text_[i]);
        }
    }
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

bool TruehdEncodeController::has_offset_error() const {
    return std::any_of(offset_error_.cbegin(), offset_error_.cend(),
                       [](const QString& sentence) { return !sentence.isEmpty(); });
}

void TruehdEncodeController::sources_changed() {
    check_map();
    summary_.clear();
    error_.clear();
    emit sourcesChanged();
    emit resultChanged();
    emit stateChanged();
    emit cliLineChanged();
}

// A map the command line would refuse, or sources with no map to say where their channels go.
void TruehdEncodeController::check_map() {
    map_error_.clear();
    if (!map_text_.isEmpty()) {
        const auto placements = core::parse_map(map_text_.toStdString());
        if (!placements) {
            map_error_ = QString::fromStdString(placements.error());
        }
    } else if (!extra_sources_.isEmpty()) {
        map_error_ = QStringLiteral(
            "More than one source needs a map saying where each channel of each source goes, as in "
            "0.0:L,0.1:R,1.0:C,2.0:LFE,2.1:Ls,2.2:Rs");
    }
}

void TruehdEncodeController::addSource(const QUrl& url) {
    const QString path = url.toLocalFile();
    if (busy_ || path.isEmpty()) {
        return;
    }
    while (offset_text_.size() < extra_sources_.size() + 1) {  // one per source so far
        offset_text_.append(QString());
        offset_error_.append(QString());
    }
    extra_sources_.append(path);
    offset_text_.append(QString());
    offset_error_.append(QString());
    sources_changed();
}

void TruehdEncodeController::removeSource(int extraIndex) {
    if (busy_ || extraIndex < 0 || extraIndex >= extra_sources_.size()) {
        return;
    }
    extra_sources_.removeAt(extraIndex);
    // Offsets are by source, the first WAV being 0: the removed source's goes with it.
    if (extraIndex + 1 < offset_text_.size()) {
        offset_text_.removeAt(extraIndex + 1);
        offset_error_.removeAt(extraIndex + 1);
    }
    sources_changed();
}

void TruehdEncodeController::setMap(const QString& text) {
    const QString trimmed = text.trimmed();
    if (busy_ || trimmed == map_text_) {
        return;
    }
    map_text_ = trimmed;
    sources_changed();
}

void TruehdEncodeController::setOffset(int sourceIndex, const QString& text) {
    if (busy_ || sourceIndex < 0 || sourceIndex > extra_sources_.size()) {
        return;
    }
    while (offset_text_.size() <= sourceIndex) {
        offset_text_.append(QString());
        offset_error_.append(QString());
    }
    const QString trimmed = text.trimmed();
    QString sentence;
    if (!trimmed.isEmpty()) {
        bool ok = false;
        const double seconds = trimmed.toDouble(&ok);
        if (!ok || !std::isfinite(seconds) || seconds < 0.0) {
            sentence = QStringLiteral("A start offset is seconds, 0 or more (got '%1')").arg(trimmed);
        }
    }
    if (offset_text_[sourceIndex] == trimmed && offset_error_[sourceIndex] == sentence) {
        return;
    }
    offset_text_[sourceIndex] = trimmed;
    offset_error_[sourceIndex] = sentence;
    sources_changed();
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
    Job job{.source = source_,
            .extra_sources = extra_sources_,
            .map = map_text_,
            .offsets_s = {},
            .output = output_,
            .settings = {}};
    for (const QString& text : offset_text_) {
        job.offsets_s.push_back(text.isEmpty() ? 0.0 : text.toDouble());  // checked by setOffset
    }
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
    extra_sources_.clear();
    map_text_.clear();
    map_error_.clear();
    offset_text_.clear();
    offset_error_.clear();
    summary_.clear();
    error_.clear();
    progress_ = 0.0;
    emit sourceChanged();
    emit sourcesChanged();
    emit outputChanged();
    emit resultChanged();
    emit progressChanged();
    emit stateChanged();
    emit cliLineChanged();
}
