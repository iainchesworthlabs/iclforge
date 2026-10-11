#include "project_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QVariant>
#include <QVariantList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {

constexpr const char* kFormatId = "iclforge-project";
constexpr const char* kSuffix = "forgeproj";

// ---- the settings the `encode` object holds -------------------------------------------------
// One row for each. `key` is what the file calls it, `property` the EncoderController property it
// is read from and written to. A setting whose value is one of a list of choices is kept by the
// id of the choice (ids[i] is the value of the property at i), not by the position or the label,
// which the window is free to change; a number is kept as the number. `phase` orders the
// application: 0 before the sources are loaded (the rest of the draft depends on it), 1 after.
//
// A row is added here when a setting is worth keeping; nothing else changes. A key a file has
// that is not in this table is carried through untouched by read() and write().
struct Setting {
    const char* key;
    const char* property;
    int phase;
    // The ids of a choice, in the property's order; empty for a number.
    std::array<const char*, 6> ids;
    int id_count;
    // A number's range.
    int min;
    int max;
};

// Codecs and containers by the order EncoderController lists them: codecNames() and
// containerNames(). tst_projects.qml holds the order, so a reordering shows there.
constexpr std::array<Setting, 6> kSettings{{
    {"codec", "codecIndex", 0, {"ac3", "eac3", "ac4"}, 3, 0, 0},
    {"container",
     "containerIndex",
     0,
     {"elementary", "matroska", "spdif", "mp4", "fmp4", "mpegts"},
     6,
     0,
     0},
    // The rungs the codec offers are checked as well: see applyEncode().
    {"bitrateKbps", "bitrateKbps", 1, {}, 0, 32, 6144},
    // A/52 Table 5.7's code: 7 is the karaoke main service at a mode above 1/0.
    {"bsmod", "bsmodIndex", 1, {}, 0, 0, 7},
    // Table 5.9's centre and 5.10's surround mix level codes.
    {"cmixlev", "cmixIndex", 1, {}, 0, 0, 2},
    {"surmixlev", "surmixIndex", 1, {}, 0, 0, 2},
}};

QString local_path(const QString& text) {
    if (text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        return QUrl(text).toLocalFile();
    }
    return text;
}

// The document as written: format and version first so that a person (or a file(1)) sees what it
// is, then the keys this version knows in reading order, then the rest alphabetically.
const QStringList& key_order() {
    static const QStringList order{
        QStringLiteral("format"),      QStringLiteral("version"), QStringLiteral("title"),
        QStringLiteral("flow"),        QStringLiteral("sources"), QStringLiteral("bed"),
        QStringLiteral("lfe"),         QStringLiteral("extras"),  QStringLiteral("atmos"),
        QStringLiteral("assignments"), QStringLiteral("encode"),  QStringLiteral("karaoke"),
    };
    return order;
}

QString quoted(const QString& text) {
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    // ["..."] without the brackets.
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

void print(const QJsonValue& value, int depth, QString& out) {
    const QString pad(2 * (depth + 1), QLatin1Char(' '));
    const QString close_pad(2 * depth, QLatin1Char(' '));
    switch (value.type()) {
        case QJsonValue::Null:
        case QJsonValue::Undefined:
            out += QStringLiteral("null");
            return;
        case QJsonValue::Bool:
            out += value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
            return;
        case QJsonValue::Double: {
            const double number = value.toDouble();
            // A whole number is written as one, as the person who typed it would.
            if (std::floor(number) == number && std::fabs(number) < 9007199254740992.0) {
                out += QString::number(static_cast<qlonglong>(number));
            } else {
                out += QString::number(number, 'g', 17);
            }
            return;
        }
        case QJsonValue::String:
            out += quoted(value.toString());
            return;
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            if (array.isEmpty()) {
                out += QStringLiteral("[]");
                return;
            }
            out += QStringLiteral("[\n");
            for (int i = 0; i < array.size(); ++i) {
                out += pad;
                print(array.at(i), depth + 1, out);
                out += i + 1 < array.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
            }
            out += close_pad + QLatin1Char(']');
            return;
        }
        case QJsonValue::Object: {
            const QJsonObject object = value.toObject();
            if (object.isEmpty()) {
                out += QStringLiteral("{}");
                return;
            }
            out += QStringLiteral("{\n");
            const QStringList keys = object.keys();
            for (int i = 0; i < keys.size(); ++i) {
                out += pad + quoted(keys[i]) + QStringLiteral(": ");
                print(object.value(keys[i]), depth + 1, out);
                out += i + 1 < keys.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
            }
            out += close_pad + QLatin1Char('}');
            return;
        }
    }
}

QString render(const QJsonObject& document) {
    QStringList keys;
    for (const QString& key : key_order()) {
        if (document.contains(key)) {
            keys.append(key);
        }
    }
    for (const QString& key : document.keys()) {
        if (!keys.contains(key)) {
            keys.append(key);
        }
    }
    QString out = QStringLiteral("{\n");
    for (int i = 0; i < keys.size(); ++i) {
        out += QStringLiteral("  ") + quoted(keys[i]) + QStringLiteral(": ");
        print(document.value(keys[i]), 1, out);
        out += i + 1 < keys.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
    }
    out += QStringLiteral("}\n");
    return out;
}

// A message is a code and its arguments, not a sentence: the window words it, and so translates
// it, and a sentence built here could not be (karaoke_authoring.hpp says the same).
QVariantMap message(const QString& code, const QStringList& args = {}) {
    QVariantMap out;
    out.insert(QStringLiteral("code"), code);
    out.insert(QStringLiteral("args"), args);
    return out;
}

QVariantMap failure(const QString& code, const QStringList& args = {}) {
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), message(code, args));
    return out;
}

}  // namespace

ProjectFile::ProjectFile(QObject* parent) : QObject(parent) {}

QString ProjectFile::format() const {
    return QString::fromLatin1(kFormatId);
}

QString ProjectFile::suffix() const {
    return QString::fromLatin1(kSuffix);
}

QVariantMap ProjectFile::read(const QUrl& file) const {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const QString name = QFileInfo(path).fileName();
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("cannot-open"), {name});
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(QStringLiteral("not-json"), {name});
    }
    QJsonObject project = document.object();
    if (project.value(QStringLiteral("format")).toString() != QLatin1String(kFormatId)) {
        return failure(QStringLiteral("not-a-project"), {name});
    }
    const QJsonValue version = project.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() < 1.0 ||
        std::floor(version.toDouble()) != version.toDouble()) {
        return failure(QStringLiteral("no-version"), {name});
    }
    if (version.toInt() > kVersion) {
        return failure(QStringLiteral("newer-version"),
                       {name, QString::number(version.toInt()), QString::number(kVersion)});
    }

    QVariantList warnings;
    const QDir folder = QFileInfo(path).absoluteDir();
    const QJsonArray given = project.value(QStringLiteral("sources")).toArray();
    QJsonArray sources;
    for (int i = 0; i < given.size(); ++i) {
        QJsonObject source = given.at(i).toObject();
        const QString text = local_path(source.value(QStringLiteral("path")).toString());
        if (text.isEmpty()) {
            warnings.append(message(QStringLiteral("source-no-path"), {QString::number(i + 1)}));
            continue;
        }
        const QString absolute =
            QDir::cleanPath(QDir::isAbsolutePath(text) ? text : folder.absoluteFilePath(text));
        source.insert(QStringLiteral("url"), QUrl::fromLocalFile(absolute).toString());
        source.insert(QStringLiteral("exists"), QFileInfo::exists(absolute));
        sources.append(source);
    }
    if (project.contains(QStringLiteral("sources"))) {
        project.insert(QStringLiteral("sources"), sources);
    }

    QVariantMap out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("project"), project.toVariantMap());
    out.insert(QStringLiteral("warnings"), warnings);
    return out;
}

QVariantMap ProjectFile::write(const QUrl& file, const QVariantMap& project) const {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const QDir folder = QFileInfo(path).absoluteDir();
    QJsonObject document = QJsonObject::fromVariantMap(project);
    document.insert(QStringLiteral("format"), QString::fromLatin1(kFormatId));
    document.insert(QStringLiteral("version"), kVersion);

    if (document.contains(QStringLiteral("sources"))) {
        QJsonArray sources;
        const QJsonArray given = document.value(QStringLiteral("sources")).toArray();
        for (const QJsonValue& entry : given) {
            QJsonObject source = entry.toObject();
            // What read() adds is the file's location made absolute, not part of the document.
            source.remove(QStringLiteral("url"));
            source.remove(QStringLiteral("exists"));
            const QString text = local_path(source.value(QStringLiteral("path")).toString());
            if (!text.isEmpty()) {
                const QString absolute = QDir::cleanPath(
                    QDir::isAbsolutePath(text) ? text : folder.absoluteFilePath(text));
                // Another drive has no relative path: relativeFilePath answers with the
                // absolute one, which is then what is kept.
                source.insert(QStringLiteral("path"), folder.relativeFilePath(absolute));
            }
            sources.append(source);
        }
        document.insert(QStringLiteral("sources"), sources);
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        return failure(QStringLiteral("cannot-write"), {QFileInfo(path).fileName()});
    }
    out.write(render(document).toUtf8());
    if (!out.commit()) {
        return failure(QStringLiteral("cannot-write"), {QFileInfo(path).fileName()});
    }
    QVariantMap ok;
    ok.insert(QStringLiteral("ok"), true);
    return ok;
}

QString ProjectFile::localPath(const QString& text) const {
    const QString path = local_path(text);
    return path.isEmpty() ? path : QDir::cleanPath(path);
}

QVariantMap ProjectFile::captureEncode(QObject* controller) const {
    QVariantMap out;
    if (controller == nullptr) {
        return out;
    }
    for (const Setting& setting : kSettings) {
        const QVariant value = controller->property(setting.property);
        if (!value.isValid()) {
            continue;
        }
        const int number = value.toInt();
        if (setting.id_count > 0) {
            if (number >= 0 && number < setting.id_count) {
                out.insert(QString::fromLatin1(setting.key),
                           QString::fromLatin1(setting.ids[static_cast<std::size_t>(number)]));
            }
        } else {
            out.insert(QString::fromLatin1(setting.key), number);
        }
    }
    return out;
}

QVariantList ProjectFile::applyEncode(QObject* controller, const QVariantMap& encode,
                                      int phase) const {
    QVariantList problems;
    if (controller == nullptr) {
        return problems;
    }
    for (const Setting& setting : kSettings) {
        if (setting.phase != phase) {
            continue;
        }
        const QString key = QString::fromLatin1(setting.key);
        if (!encode.contains(key)) {
            continue;
        }
        const QVariant given = encode.value(key);
        int number = 0;
        if (setting.id_count > 0) {
            const QString id = given.toString();
            number = -1;
            for (int i = 0; i < setting.id_count; ++i) {
                if (id == QLatin1String(setting.ids[static_cast<std::size_t>(i)])) {
                    number = i;
                }
            }
            if (number < 0) {
                problems.append(message(QStringLiteral("setting-unknown-choice"), {key, id}));
                continue;
            }
        } else {
            bool whole = false;
            number = given.toInt(&whole);
            if (!whole || number < setting.min || number > setting.max) {
                problems.append(
                    message(QStringLiteral("setting-out-of-range"), {key, given.toString()}));
                continue;
            }
            if (QLatin1String(setting.key) == QLatin1String("bitrateKbps")) {
                const QVariantList offered = controller->property("bitrates").toList();
                const bool listed =
                    std::any_of(offered.begin(), offered.end(),
                                [number](const QVariant& rung) { return rung.toInt() == number; });
                if (!listed) {
                    problems.append(
                        message(QStringLiteral("bitrate-not-offered"), {QString::number(number)}));
                    continue;
                }
            }
        }
        if (!controller->setProperty(setting.property, number)) {
            problems.append(message(QStringLiteral("setting-failed"), {key}));
        }
    }
    return problems;
}
