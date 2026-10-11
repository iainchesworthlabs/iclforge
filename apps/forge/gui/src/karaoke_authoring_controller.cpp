#include "karaoke_authoring_controller.hpp"

#include <QStringList>
#include <QUrl>

#include <string>
#include <vector>

#include "karaoke_authoring.hpp"

namespace {

QString to_q(const std::string& text) {
    return QString::fromStdString(text);
}

QString level_name(forge_gui::KaraokeCheckLevel level) {
    switch (level) {
        case forge_gui::KaraokeCheckLevel::kOk: return QStringLiteral("ok");
        case forge_gui::KaraokeCheckLevel::kWarning: return QStringLiteral("warning");
        case forge_gui::KaraokeCheckLevel::kProblem: return QStringLiteral("problem");
    }
    return QStringLiteral("ok");
}

QVariantMap plan_to_map(const forge_gui::KaraokePlan& plan) {
    QVariantMap out;
    out.insert(QStringLiteral("buildable"), plan.buildable);
    out.insert(QStringLiteral("mode"), to_q(plan.mode));
    out.insert(QStringLiteral("melody"), plan.melody);
    out.insert(QStringLiteral("vocals"), plan.vocals);
    out.insert(QStringLiteral("hasProblem"), plan.has_problem());
    QVariantList channels;
    for (const auto& channel : plan.channels) {
        QVariantMap row;
        row.insert(QStringLiteral("position"), to_q(channel.position));
        row.insert(QStringLiteral("part"), to_q(channel.part));
        row.insert(QStringLiteral("file"), static_cast<int>(channel.file));
        row.insert(QStringLiteral("channel"), static_cast<int>(channel.channel));
        channels.append(row);
    }
    out.insert(QStringLiteral("channels"), channels);
    QVariantList checks;
    for (const auto& check : plan.checks) {
        QVariantMap row;
        row.insert(QStringLiteral("level"), level_name(check.level));
        row.insert(QStringLiteral("code"), to_q(check.code));
        QStringList args;
        for (const std::string& arg : check.args) {
            args.append(to_q(arg));
        }
        row.insert(QStringLiteral("args"), args);
        checks.append(row);
    }
    out.insert(QStringLiteral("checks"), checks);
    return out;
}

}  // namespace

KaraokeAuthoring::KaraokeAuthoring(QObject* parent) : QObject(parent) {}

QVariantMap KaraokeAuthoring::planParts(const QVariantList& files) const {
    std::vector<forge_gui::KaraokeFile> parts;
    parts.reserve(static_cast<std::size_t>(files.size()));
    for (const QVariant& value : files) {
        const QVariantMap map = value.toMap();
        forge_gui::KaraokeFile file;
        const QString kind = map.value(QStringLiteral("part")).toString();
        if (kind == QLatin1String("melody")) {
            file.part = forge_gui::KaraokePart::kMelody;
        } else if (kind == QLatin1String("vocal1")) {
            file.part = forge_gui::KaraokePart::kVocal1;
        } else if (kind == QLatin1String("vocal2")) {
            file.part = forge_gui::KaraokePart::kVocal2;
        } else {
            file.part = forge_gui::KaraokePart::kBacking;
        }
        file.name = map.value(QStringLiteral("name")).toString().toStdString();
        file.channels = static_cast<std::size_t>(map.value(QStringLiteral("channels")).toInt());
        file.sample_rate = static_cast<std::uint32_t>(map.value(QStringLiteral("rate")).toUInt());
        file.seconds = map.value(QStringLiteral("seconds")).toDouble();
        parts.push_back(std::move(file));
    }
    return plan_to_map(forge_gui::plan_karaoke_parts(parts));
}

QVariantMap KaraokeAuthoring::planSpeakers(const QVariantMap& file) const {
    forge_gui::KaraokeSpeakerFile in;
    in.name = file.value(QStringLiteral("name")).toString().toStdString();
    in.channels = static_cast<std::size_t>(file.value(QStringLiteral("channels")).toInt());
    in.sample_rate = static_cast<std::uint32_t>(file.value(QStringLiteral("rate")).toUInt());
    in.seconds = file.value(QStringLiteral("seconds")).toDouble();
    for (const QString& name : file.value(QStringLiteral("speakers")).toStringList()) {
        in.speakers.push_back(name.toStdString());
    }
    return plan_to_map(forge_gui::plan_karaoke_speakers(in));
}

QVariantMap KaraokeAuthoring::outputs(bool melody, int vocals, const QString& listening,
                                      const QString& choice, bool folded) const {
    forge_gui::KaraokeListening how = forge_gui::KaraokeListening::kAsCoded;
    if (listening == QLatin1String("aware")) {
        how = forge_gui::KaraokeListening::kAware;
    } else if (listening == QLatin1String("capable")) {
        how = forge_gui::KaraokeListening::kCapable;
    }
    forge_gui::KaraokeVocalsChoice vocal = forge_gui::KaraokeVocalsChoice::kBoth;
    if (choice == QLatin1String("none")) {
        vocal = forge_gui::KaraokeVocalsChoice::kNone;
    } else if (choice == QLatin1String("v1")) {
        vocal = forge_gui::KaraokeVocalsChoice::kV1;
    } else if (choice == QLatin1String("v2")) {
        vocal = forge_gui::KaraokeVocalsChoice::kV2;
    }
    const forge_gui::KaraokeOutputs out = forge_gui::karaoke_outputs(melody, vocals, how, vocal, folded);
    const auto names = [](const std::vector<std::string>& parts) {
        QStringList list;
        for (const std::string& part : parts) {
            list.append(to_q(part));
        }
        return list;
    };
    QVariantMap map;
    map.insert(QStringLiteral("l"), names(out.l));
    map.insert(QStringLiteral("c"), names(out.c));
    map.insert(QStringLiteral("r"), names(out.r));
    map.insert(QStringLiteral("ls"), names(out.ls));
    map.insert(QStringLiteral("rs"), names(out.rs));
    return map;
}

QString KaraokeAuthoring::durationText(double seconds) const {
    return to_q(forge_gui::karaoke_duration_text(seconds));
}

namespace {

// A file URL or a plain path, as the path a file system call takes.
std::string local_path(const QString& url_or_path) {
    const QUrl url(url_or_path);
    return (url.isLocalFile() ? url.toLocalFile() : url_or_path).toStdString();
}

}  // namespace

int KaraokeAuthoring::defaultBitrateKbps(const QString& mode) const {
    return forge_gui::karaoke_default_bitrate_kbps(mode.toStdString());
}

QString KaraokeAuthoring::lyricsBeside(const QString& url) const {
    const std::string found = forge_gui::karaoke_lyrics_beside(local_path(url));
    return found.empty() ? QString() : QUrl::fromLocalFile(to_q(found)).toString();
}

QString KaraokeAuthoring::copyLyrics(const QString& lyricsUrl, const QString& streamPath) const {
    switch (forge_gui::karaoke_copy_lyrics(local_path(lyricsUrl), local_path(streamPath))) {
        case forge_gui::KaraokeLyricsCopy::kCopied: return QStringLiteral("copied");
        case forge_gui::KaraokeLyricsCopy::kAlreadyThere: return QStringLiteral("already-there");
        case forge_gui::KaraokeLyricsCopy::kExists: return QStringLiteral("exists");
        case forge_gui::KaraokeLyricsCopy::kUnreadable: return QStringLiteral("unreadable");
        case forge_gui::KaraokeLyricsCopy::kFailed: break;
    }
    return QStringLiteral("failed");
}
