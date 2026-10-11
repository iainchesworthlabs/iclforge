#include "project_file.hpp"

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QMetaType>
#include <QTemporaryFile>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/objects/scene.hpp"
#include "project_document.hpp"

namespace project = iclforge::apps::project;
using Json = project::Json;
namespace fs = std::filesystem;

namespace {

// ---- strings and paths ---------------------------------------------------------------------------

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

fs::path to_path(const QString& local) {
    return fs::path{local.toStdU16String()};
}

QString from_path(const fs::path& path) {
    return QString::fromStdU16String(path.generic_u16string());
}

// A path or a file URL as a local path.
QString local_of(const QString& text) {
    if (text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        return QUrl(text).toLocalFile();
    }
    return text;
}

QString dump(const Json& value) {
    // Replace what is not UTF-8 rather than throw: the text goes to QML, which has no use for an
    // exception.
    return QString::fromStdString(
        value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

// ---- messages ------------------------------------------------------------------------------------
// A message is a code and its arguments, not a sentence: the window words it, and so translates
// it, and a sentence built here could not be (karaoke_authoring.hpp says the same).

QVariantMap message(const QString& code, const QStringList& args = {}) {
    QVariantMap out;
    out.insert(QStringLiteral("code"), code);
    out.insert(QStringLiteral("args"), args);
    return out;
}

QVariantMap message_of(const project::Message& m) {
    QStringList args;
    for (const auto& arg : m.args) {
        args.append(q(arg));
    }
    return message(q(m.code), args);
}

QVariantMap failure(const project::Message& m) {
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), message_of(m));
    return out;
}

// ---- the draft -----------------------------------------------------------------------------------
// The EncoderController as a bag of named properties, so that what is here does not include its
// header (a 2,000-line interface that the window changes weekly) and reads the same through QML.

struct Draft {
    QObject* controller;

    [[nodiscard]] QVariant get(const char* name) const { return controller->property(name); }
    [[nodiscard]] int get_int(const char* name) const { return get(name).toInt(); }
    [[nodiscard]] double get_double(const char* name) const { return get(name).toDouble(); }
    [[nodiscard]] bool get_bool(const char* name) const { return get(name).toBool(); }
    [[nodiscard]] QStringList names(const char* name) const { return get(name).toStringList(); }
    bool set(const char* name, const QVariant& value) const {
        return controller->setProperty(name, value);
    }
};

// What a project value is, read leniently: a number may be written as a string (the command line
// takes it that way), and a flag as true or "on".
std::optional<double> as_number(const QVariant& value) {
    if (!value.isValid() || value.typeId() == QMetaType::Bool) {
        return std::nullopt;
    }
    bool ok = false;
    const double number = value.typeId() == QMetaType::QString
                              ? value.toString().trimmed().toDouble(&ok)
                              : value.toDouble(&ok);
    return ok ? std::optional<double>{number} : std::nullopt;
}

std::optional<int> as_whole(const QVariant& value) {
    const auto number = as_number(value);
    if (!number || std::floor(*number) != *number || std::fabs(*number) > 1e9) {
        return std::nullopt;
    }
    return static_cast<int>(*number);
}

std::optional<bool> as_flag(const QVariant& value) {
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool();
    }
    if (value.typeId() == QMetaType::QString) {
        const QString text = value.toString().toLower();
        if (text == QLatin1String("on") || text == QLatin1String("true")) {
            return true;
        }
        if (text == QLatin1String("off") || text == QLatin1String("false")) {
            return false;
        }
    }
    return std::nullopt;
}

// The position of `value` among `tokens`, or -1.
int token_index(const QVariant& value, std::initializer_list<const char*> tokens) {
    if (value.typeId() != QMetaType::QString) {
        return -1;
    }
    const QString text = value.toString().trimmed().toLower();
    int at = 0;
    for (const char* token : tokens) {
        if (text == QLatin1String(token)) {
            return at;
        }
        ++at;
    }
    return -1;
}

// ---- codec, container ----------------------------------------------------------------------------
// In the order EncoderController lists them: codecNames() and containerNames(). tst_projects.qml
// holds that order, so that a reordering shows there and not in a person's project.

constexpr std::array<const char*, 3> kCodecIds{"ac3", "eac3", "ac4"};
constexpr std::array<const char*, 6> kContainerIds{"elementary", "matroska", "spdif",
                                                   "mp4",        "fmp4",     "mpegts"};

// ---- the options ---------------------------------------------------------------------------------
// The command line's own keys and values, for the metadata, compression and loudness a plan
// carries: print_meta_usage in forge's help is the grammar, and EncoderController::metaTokens is
// what the window writes of its draft, which capture_options mirrors member for member - only what
// differs from the default, so that a project of a plain encode has none.
//
// kOptionOrder is the order they are applied in, which is not the order a file has them: a level
// is set before the switch that turns the compression on, and a service before what it implies.

constexpr double kCeilingDefault = -0.5;
constexpr double kDialogueDefault = -20.0;
constexpr int kDialnormDefault = 31;
constexpr int kCmixDefault = 1;     // -4.5 dB
constexpr int kSurmixDefault = 1;   // -6 dB
constexpr int kLfeMixDefault = 0;   // meta::kLfeMixLevelIdeal
constexpr int kDmixDefault = 2;     // Lo/Ro
constexpr int kMixLevelFloor = 80;  // meta::kMixLevelBaseDbSpl
constexpr int kMixLevelCeiling = kMixLevelFloor + 31;
constexpr std::array<double, 3> kCmixDb{-3.0, -4.5, -6.0};
constexpr std::array<double, 2> kSurmixDb{-3.0, -6.0};

Json capture_options(const Draft& d) {
    Json out = Json::object();
    const QStringList drcs = d.names("drcNames");
    const bool atmos = d.get_bool("atmosEnabled");
    const int codec = d.get_int("codecIndex");

    if (const int i = d.get_int("drcIndex"); i > 0 && i < drcs.size()) {
        out["drc"] = drcs[i].toStdString();
    }
    if (d.get_bool("heavy")) {
        out["heavy"] = true;
        if (d.get_double("ceilingDb") != kCeilingDefault) {
            out["ceiling"] = d.get_double("ceilingDb");
        }
        if (d.get_double("dialogueDb") != kDialogueDefault) {
            out["dialogue"] = d.get_double("dialogueDb");
        }
    }
    if (d.get_bool("measureDialnorm")) {
        out["dialnorm"] = "auto";
    } else if (d.get_int("dialnorm") != kDialnormDefault) {
        out["dialnorm"] = d.get_int("dialnorm");
    }
    if (d.get_bool("dualMono") && !atmos) {
        if (const int i = d.get_int("drc2Index"); i > 0 && i < drcs.size()) {
            out["drc2"] = drcs[i].toStdString();
        }
        if (d.get_bool("heavy2")) {
            out["heavy2"] = true;
            if (d.get_double("ceiling2Db") != kCeilingDefault) {
                out["ceiling2"] = d.get_double("ceiling2Db");
            }
            if (d.get_double("dialogue2Db") != kDialogueDefault) {
                out["dialogue2"] = d.get_double("dialogue2Db");
            }
        }
        if (d.get_bool("measureDialnorm2")) {
            out["dialnorm2"] = "auto";
        } else if (d.get_int("dialnorm2") != kDialnormDefault) {
            out["dialnorm2"] = d.get_int("dialnorm2");
        }
    }
    if (const int i = d.get_int("cmixIndex"); i != kCmixDefault && i >= 0 && i < 3) {
        out["cmixlev"] = kCmixDb[static_cast<std::size_t>(i)];
    }
    if (const int i = d.get_int("surmixIndex"); i != kSurmixDefault && i >= 0 && i < 3) {
        if (i == 2) {
            out["surmixlev"] = "off";
        } else {
            out["surmixlev"] = kSurmixDb[static_cast<std::size_t>(i)];
        }
    }
    if (codec == 1 && d.get_bool("mixmeta")) {
        out["mixmeta"] = true;
    }
    if (const int v = d.get_int("lfeMix"); v != kLfeMixDefault) {
        if (v < 0) {
            out["lfemix"] = "off";
        } else {
            out["lfemix"] = v;
        }
    }
    if (const int i = d.get_int("dmixIndex"); i != kDmixDefault && i >= 0 && i < 3) {
        static constexpr std::array<const char*, 3> kTokens{"none", "ltrt", "loro"};
        out["dmixmod"] = kTokens[static_cast<std::size_t>(i)];
    }
    if (const int i = d.get_int("bsmodIndex"); i > 0 && i < 8) {
        static constexpr std::array<const char*, 8> kTokens{
            "cm", "me", "vi", "hi", "dialogue", "commentary", "emergency", "voiceover"};
        out["bsmod"] = kTokens[static_cast<std::size_t>(i)];
    }
    static constexpr std::array<const char*, 3> kMode{"none", "off", "on"};
    if (d.get_bool("surroundModeAvailable")) {
        if (const int i = d.get_int("dsurmodIndex"); i > 0 && i < 3) {
            out["dsurmod"] = kMode[static_cast<std::size_t>(i)];
        }
        if (const int i = d.get_int("dheadphonIndex"); i > 0 && i < 3) {
            out["dheadphonmod"] = kMode[static_cast<std::size_t>(i)];
        }
    }
    if (d.get_bool("surroundExAvailable")) {
        static constexpr std::array<const char*, 4> kEx{"none", "off", "ex", "pliiz"};
        if (const int i = d.get_int("dsurexIndex"); i > 0 && i < 4) {
            out["dsurexmod"] = kEx[static_cast<std::size_t>(i)];
        }
    }
    if (const int level = d.get_int("mixLevelDbSpl"); level >= kMixLevelFloor) {
        out["mixlevel"] = level;
        if (const int i = d.get_int("roomTypeIndex"); i > 0 && i < 3) {
            static constexpr std::array<const char*, 3> kRoom{"none", "large", "small"};
            out["roomtyp"] = kRoom[static_cast<std::size_t>(i)];
        }
    }
    if (d.get_int("adConvIndex") != 0) {
        out["adconvtyp"] = "hdcd";
    }
    if (d.get_bool("copyrightBit")) {
        out["copyright"] = true;
    }
    if (!d.get_bool("originalBitstream")) {
        out["origbs"] = "off";
    }
    // Only worth spelling when nothing above already implies it: the three xbsi2 tokens and
    // dmixmod= each turn Annex D on by themselves.
    if (codec == 0 && d.get_bool("annexD") && !out.contains("adconvtyp") &&
        d.get_int("dsurexIndex") == 0 && d.get_int("dheadphonIndex") == 0 &&
        d.get_int("dmixIndex") == kDmixDefault) {
        out["annexd"] = true;
    }
    return out;
}

// What one option does to the draft: 0 for it done, 1 for a value it cannot take.
using Apply = int (*)(const Draft&, const QVariant&);

int apply_dialnorm(const Draft& d, const QVariant& v, const char* value_property,
                   const char* measure_property) {
    if (v.typeId() == QMetaType::QString && v.toString().toLower() == QLatin1String("auto")) {
        d.set(measure_property, true);
        return 0;
    }
    const auto n = as_whole(v);
    if (!n || *n < 1 || *n > 31) {
        return 1;
    }
    d.set(value_property, *n);
    d.set(measure_property, false);
    return 0;
}

int apply_drc(const Draft& d, const QVariant& v, const char* property) {
    if (v.typeId() != QMetaType::QString) {
        return 1;
    }
    const int at = static_cast<int>(d.names("drcNames").indexOf(v.toString()));
    if (at < 0) {
        return 1;
    }
    d.set(property, at);
    return 0;
}

int apply_flag(const Draft& d, const QVariant& v, const char* property) {
    const auto on = as_flag(v);
    if (!on) {
        return 1;
    }
    d.set(property, *on);
    return 0;
}

int apply_number(const Draft& d, const QVariant& v, const char* property) {
    const auto number = as_number(v);
    if (!number) {
        return 1;
    }
    d.set(property, *number);
    return 0;
}

int apply_token(const Draft& d, const QVariant& v, const char* property,
                std::initializer_list<const char*> tokens) {
    const int at = token_index(v, tokens);
    if (at < 0) {
        return 1;
    }
    d.set(property, at);
    return 0;
}

struct OptionRow {
    const char* key;
    Apply apply;
};

constexpr std::array<OptionRow, 25> kOptionOrder{{
    {"dialnorm",
     [](const Draft& d, const QVariant& v) {
         return apply_dialnorm(d, v, "dialnorm", "measureDialnorm");
     }},
    {"drc", [](const Draft& d, const QVariant& v) { return apply_drc(d, v, "drcIndex"); }},
    {"ceiling", [](const Draft& d, const QVariant& v) { return apply_number(d, v, "ceilingDb"); }},
    {"dialogue",
     [](const Draft& d, const QVariant& v) { return apply_number(d, v, "dialogueDb"); }},
    {"heavy", [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "heavy"); }},
    {"dialnorm2",
     [](const Draft& d, const QVariant& v) {
         return apply_dialnorm(d, v, "dialnorm2", "measureDialnorm2");
     }},
    {"drc2", [](const Draft& d, const QVariant& v) { return apply_drc(d, v, "drc2Index"); }},
    {"ceiling2",
     [](const Draft& d, const QVariant& v) { return apply_number(d, v, "ceiling2Db"); }},
    {"dialogue2",
     [](const Draft& d, const QVariant& v) { return apply_number(d, v, "dialogue2Db"); }},
    {"heavy2", [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "heavy2"); }},
    {"cmixlev",
     [](const Draft& d, const QVariant& v) {
         const auto db = as_number(v);
         for (std::size_t i = 0; db && i < kCmixDb.size(); ++i) {
             if (*db == kCmixDb[i]) {
                 d.set("cmixIndex", static_cast<int>(i));
                 return 0;
             }
         }
         return 1;
     }},
    {"surmixlev",
     [](const Draft& d, const QVariant& v) {
         if (token_index(v, {"off"}) == 0) {
             d.set("surmixIndex", 2);
             return 0;
         }
         const auto db = as_number(v);
         for (std::size_t i = 0; db && i < kSurmixDb.size(); ++i) {
             if (*db == kSurmixDb[i]) {
                 d.set("surmixIndex", static_cast<int>(i));
                 return 0;
             }
         }
         return 1;
     }},
    {"mixmeta", [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "mixmeta"); }},
    {"lfemix",
     [](const Draft& d, const QVariant& v) {
         if (token_index(v, {"off"}) == 0) {
             d.set("lfeMix", -1);
             return 0;
         }
         const auto n = as_whole(v);
         if (!n || *n < 0 || *n > 31) {
             return 1;
         }
         d.set("lfeMix", *n);
         return 0;
     }},
    {"dmixmod",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "dmixIndex", {"none", "ltrt", "loro"});
     }},
    {"bsmod",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "bsmodIndex",
                            {"cm", "me", "vi", "hi", "dialogue", "commentary", "emergency",
                             "voiceover"});
     }},
    {"dsurmod",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "dsurmodIndex", {"none", "off", "on"});
     }},
    {"dheadphonmod",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "dheadphonIndex", {"none", "off", "on"});
     }},
    {"dsurexmod",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "dsurexIndex", {"none", "off", "ex", "pliiz"});
     }},
    {"mixlevel",
     [](const Draft& d, const QVariant& v) {
         const auto n = as_whole(v);
         if (!n || *n < kMixLevelFloor || *n > kMixLevelCeiling) {
             return 1;
         }
         d.set("mixLevelDbSpl", *n);
         return 0;
     }},
    {"roomtyp",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "roomTypeIndex", {"none", "large", "small"});
     }},
    {"adconvtyp",
     [](const Draft& d, const QVariant& v) {
         return apply_token(d, v, "adConvIndex", {"standard", "hdcd"});
     }},
    {"copyright",
     [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "copyrightBit"); }},
    {"origbs",
     [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "originalBitstream"); }},
    {"annexd", [](const Draft& d, const QVariant& v) { return apply_flag(d, v, "annexD"); }},
}};

}  // namespace

ProjectFile::ProjectFile(QObject* parent) : QObject(parent) {}

QString ProjectFile::format() const {
    return q(std::string{project::kFormat});
}

int ProjectFile::version() const {
    return project::kVersion;
}

QString ProjectFile::suffix() const {
    return q(std::string{project::kSuffix});
}

QVariantMap ProjectFile::read(const QUrl& file) const {
    const fs::path path = to_path(file.isLocalFile() ? file.toLocalFile() : file.toString());
    auto opened = project::read_project(path);
    if (!opened) {
        return failure(opened.error());
    }
    QVariantList warnings;
    for (const auto& warning : opened->warnings) {
        warnings.append(message_of(warning));
    }
    Json& doc = opened->project;
    // Where each source is, made absolute from the project's folder, for the window to open: not
    // part of the document, and taken off again by write().
    const fs::path folder = path.parent_path();
    if (auto sources = doc.find("sources"); sources != doc.end() && sources->is_array()) {
        for (auto& source : *sources) {
            const std::string stored = project::string_of(source, "path");
            if (!source.is_object() || stored.empty()) {
                continue;
            }
            const fs::path absolute = project::resolved_path(folder, stored);
            std::error_code ignored;
            source["url"] = QUrl::fromLocalFile(from_path(absolute)).toString().toStdString();
            source["exists"] = fs::exists(absolute, ignored);
        }
    }
    QVariantMap out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("text"), dump(doc));
    out.insert(QStringLiteral("warnings"), warnings);
    return out;
}

QVariantMap ProjectFile::write(const QUrl& file, const QString& text) const {
    const fs::path path = to_path(file.isLocalFile() ? file.toLocalFile() : file.toString());
    Json doc = Json::parse(text.toStdString(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return failure({"not-json", {path.filename().generic_string()}});
    }
    const fs::path folder = path.parent_path();
    if (auto sources = doc.find("sources"); sources != doc.end() && sources->is_array()) {
        for (auto& source : *sources) {
            if (!source.is_object()) {
                continue;
            }
            // What read() adds is where the file is made absolute, not part of the document.
            source.erase("url");
            source.erase("exists");
            const std::string given = project::string_of(source, "path");
            if (given.empty()) {
                continue;
            }
            const QString local = local_of(q(given));
            const fs::path absolute = project::resolved_path(folder, local.toStdString());
            // Another drive has no relative form: stored_path answers with the absolute one.
            source["path"] = project::stored_path(folder, absolute);
        }
    }
    auto written = project::write_project(path, std::move(doc));
    if (!written) {
        return failure(written.error());
    }
    QVariantMap ok;
    ok.insert(QStringLiteral("ok"), true);
    return ok;
}

QString ProjectFile::localPath(const QString& text) const {
    const QString path = local_of(text);
    return path.isEmpty() ? path : QDir::cleanPath(path);
}

QString ProjectFile::captureEncode(QObject* controller) const {
    if (controller == nullptr) {
        return QStringLiteral("{}");
    }
    const Draft d{controller};
    Json encode = Json::object();
    if (const int i = d.get_int("codecIndex"); i >= 0 && i < 3) {
        encode["codec"] = kCodecIds[static_cast<std::size_t>(i)];
    }
    if (const int i = d.get_int("containerIndex"); i >= 0 && i < 6) {
        encode["container"] = kContainerIds[static_cast<std::size_t>(i)];
    }
    encode["bitrateKbps"] = d.get_int("bitrateKbps");
    // The coding tools, as the `+`-joined token the command line takes: "none" is what a plan
    // that says nothing has, and is not written.
    if (const QString tools = d.get("toolsToken").toString();
        !tools.isEmpty() && tools != QLatin1String("none")) {
        encode["tools"] = tools.toStdString();
    }
    if (d.get_bool("vbrAvailable") && d.get_bool("vbrEnabled")) {
        encode["vbr"] = d.get("vbrToken").toString().toStdString();
    }
    encode["options"] = capture_options(d);
    return dump(encode);
}

QStringList ProjectFile::optionKeys() const {
    QStringList keys;
    for (const auto& row : kOptionOrder) {
        keys.append(QString::fromLatin1(row.key));
    }
    return keys;
}

QVariantList ProjectFile::applyEncode(QObject* controller, const QVariantMap& encode,
                                      int phase) const {
    QVariantList problems;
    if (controller == nullptr) {
        return problems;
    }
    const Draft d{controller};

    if (phase == 0) {
        QMetaObject::invokeMethod(controller, "resetEncodeSettings");
        const auto choose = [&](const char* key, const char* property, const auto& ids) {
            if (!encode.contains(QLatin1String(key))) {
                return;
            }
            const QVariant given = encode.value(QLatin1String(key));
            int at = -1;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (given.toString() == QLatin1String(ids[i])) {
                    at = static_cast<int>(i);
                }
            }
            if (at < 0) {
                problems.append(message(QStringLiteral("setting-unknown-choice"),
                                        {QString::fromLatin1(key), given.toString()}));
            } else {
                d.set(property, at);
            }
        };
        choose("codec", "codecIndex", kCodecIds);
        choose("container", "containerIndex", kContainerIds);
        return problems;
    }

    // Phase 1. The rate first: a band edge the tools take is a fraction of it.
    if (encode.contains(QStringLiteral("bitrateKbps"))) {
        const QVariant given = encode.value(QStringLiteral("bitrateKbps"));
        const auto number = as_whole(given);
        if (!number || *number < 32 || *number > 6144) {
            problems.append(message(QStringLiteral("setting-out-of-range"),
                                    {QStringLiteral("bitrateKbps"), given.toString()}));
        } else {
            const QVariantList offered = d.get("bitrates").toList();
            const bool listed =
                std::any_of(offered.begin(), offered.end(),
                            [n = *number](const QVariant& rung) { return rung.toInt() == n; });
            if (!listed) {
                problems.append(
                    message(QStringLiteral("bitrate-not-offered"), {QString::number(*number)}));
            } else {
                d.set("bitrateKbps", *number);
            }
        }
    }
    if (encode.contains(QStringLiteral("tools"))) {
        const QString tools = encode.value(QStringLiteral("tools")).toString();
        bool ok = false;
        QMetaObject::invokeMethod(controller, "setToolsFromToken", Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, tools));
        if (!ok) {
            problems.append(message(QStringLiteral("setting-unknown-choice"),
                                    {QStringLiteral("tools"), tools}));
        }
    }
    if (encode.contains(QStringLiteral("vbr"))) {
        const QString vbr = encode.value(QStringLiteral("vbr")).toString();
        bool ok = false;
        QMetaObject::invokeMethod(controller, "setVbrFromToken", Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, vbr));
        if (!ok) {
            problems.append(message(QStringLiteral("setting-unknown-choice"),
                                    {QStringLiteral("vbr"), vbr}));
        }
    }
    const QVariantMap options = encode.value(QStringLiteral("options")).toMap();
    QStringList handled;
    for (const auto& row : kOptionOrder) {
        const QString key = QString::fromLatin1(row.key);
        if (!options.contains(key)) {
            continue;
        }
        handled.append(key);
        const QVariant value = options.value(key);
        if (row.apply(d, value) != 0) {
            problems.append(
                message(QStringLiteral("setting-out-of-range"), {key, value.toString()}));
        }
    }
    // A key of some other version, or one the command line takes that this window has no control
    // for: kept in the file and used by `forge`, and said here so that this window's own encode is
    // not taken for the project's.
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        if (!handled.contains(it.key())) {
            problems.append(message(QStringLiteral("setting-not-held"), {it.key()}));
        }
    }
    return problems;
}

QString ProjectFile::captureObjects(QObject* controller) const {
    if (controller == nullptr || !controller->property("atmosEnabled").toBool() ||
        controller->property("objectCount").toInt() <= 0) {
        return {};
    }
    // The scene the window would export for `atmos-encode`: the objects as they are placed and
    // moved, through the same function, so the project's copy is the file the command bar's line
    // names.
    QTemporaryFile scratch;
    if (!scratch.open()) {
        return {};
    }
    const QString path = scratch.fileName();
    scratch.close();
    bool ok = false;
    QMetaObject::invokeMethod(controller, "exportObjectScene", Q_RETURN_ARG(bool, ok),
                              Q_ARG(QUrl, QUrl::fromLocalFile(path)));
    QFile exported(path);
    if (!ok || !exported.open(QIODevice::ReadOnly)) {
        return {};
    }
    Json scene = Json::parse(exported.readAll().toStdString(), nullptr, /*allow_exceptions=*/false);
    if (scene.is_discarded()) {
        return {};
    }
    Json out = Json::object();
    out["count"] = controller->property("objectCount").toInt();
    out["scene"] = std::move(scene);
    return dump(out);
}

QVariantList ProjectFile::applyObjects(QObject* controller, const QString& objects) const {
    QVariantList problems;
    if (controller == nullptr) {
        return problems;
    }
    const Json doc = Json::parse(objects.toStdString(), nullptr, /*allow_exceptions=*/false);
    const Json* scene_json = doc.is_object() ? project::member(doc, "scene") : nullptr;
    if (scene_json == nullptr) {
        problems.append(message(QStringLiteral("objects-failed")));
        return problems;
    }
    const auto scene = iclforge::objects::oba::scene_from_json(
        scene_json->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    if (!scene) {
        problems.append(message(QStringLiteral("objects-failed")));
        return problems;
    }
    const int have = controller->property("objectCount").toInt();
    const auto list = scene->objects();
    if (static_cast<int>(list.size()) > have) {
        problems.append(message(QStringLiteral("objects-more"),
                                {QString::number(list.size()), QString::number(have)}));
    }
    for (int i = 0; i < have && static_cast<std::size_t>(i) < list.size(); ++i) {
        QVariantList keyframes;
        for (const auto& point : list[static_cast<std::size_t>(i)].automation) {
            QVariantMap key;
            key.insert(QStringLiteral("time"), point.time_s);
            key.insert(QStringLiteral("x"), point.position.x);
            key.insert(QStringLiteral("y"), point.position.y);
            key.insert(QStringLiteral("z"), point.position.z);
            key.insert(QStringLiteral("gain"), point.gain);
            key.insert(QStringLiteral("lfeSend"), point.lfe_send);
            keyframes.append(key);
        }
        if (keyframes.isEmpty()) {
            continue;
        }
        QMetaObject::invokeMethod(controller, "setObjectPathKeyframes", Q_ARG(int, i),
                                  Q_ARG(QVariantList, keyframes), Q_ARG(QString, QString()));
    }
    return problems;
}
