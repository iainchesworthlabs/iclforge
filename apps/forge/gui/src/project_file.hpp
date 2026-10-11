#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

// The Forge GUI's project file: what a person has set up in the window, kept in a file that can
// be opened again, moved to another folder and handed to someone else (docs/forge/gui/projects.md
// is the format's own page; the example under examples/karaoke/auld-lang-syne/ ships one).
//
// A project is a JSON document:
//
//   { "format": "iclforge-project", "version": 1, "title": "...", "flow": "karaoke",
//     "sources": [ { "path": "parts/backing.wav", "part": "backing" }, ... ],
//     "encode": { "codec": "ac3", "bitrateKbps": 384, "bsmod": 7, ... },
//     "karaoke": { "listening": "aware", "vocals": "both" } }
//
// and it is built to be extended:
//
//   - `version` is the format's own revision. A reader opens any version up to the one it
//     knows and refuses a newer one by saying so, rather than reading half of it.
//   - A key this reader does not know is not an error: it is kept in the map read() returns and
//     written back by write(), so opening and saving a project made by a newer Forge loses
//     nothing it did not understand.
//   - Every saved setting is one row of a table in project_file.cpp, with a stable key and a
//     stable value (a code from the standard, or an id), never the position of an entry in a
//     list the window may reorder. A new setting is a new row, not a new format.
//   - A source's `path` is relative to the project file's folder when it can be, so a folder of
//     parts and its project move together.
//
// This class is the file half: it reads, checks and writes the document, and maps the encode
// settings onto the controller's properties. What a flow does with the rest (Main.qml, and
// KaraokeFlow.qml for its parts) stays in QML beside the state it owns.
class ProjectFile : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString format READ format CONSTANT)
    Q_PROPERTY(int version READ version CONSTANT)
    Q_PROPERTY(QString suffix READ suffix CONSTANT)

   public:
    explicit ProjectFile(QObject* parent = nullptr);

    static constexpr int kVersion = 1;

    [[nodiscard]] QString format() const;
    [[nodiscard]] int version() const { return kVersion; }
    // "forgeproj", without the dot.
    [[nodiscard]] QString suffix() const;

    // Every message below is a map of `code` and `args`, not a sentence: the window words it,
    // and so translates it. The codes: cannot-open (name), not-json (name), not-a-project
    // (name), no-version (name), newer-version (name, the file's version, the one read here),
    // cannot-write (name), source-no-path (the source's number), setting-unknown-choice (key,
    // value), setting-out-of-range (key, value), bitrate-not-offered (kbps), setting-failed
    // (key).

    // Reads `file`. The map holds `ok`, and when it is false `error`, a message. When true,
    // `project` is the whole document as the file wrote it, unknown keys included, with each
    // source given a `url` (its path made absolute from the project's folder) and `exists`; and
    // `warnings` lists the messages for what was set aside (a source with no path).
    Q_INVOKABLE QVariantMap read(const QUrl& file) const;

    // Writes `project` to `file`, with format and version stamped on it. Each source's `path` may
    // be a local path or a file URL; it is written relative to the file's folder when it can be.
    // The map holds `ok` and, when false, `error`. The file is replaced only once whole.
    Q_INVOKABLE QVariantMap write(const QUrl& file, const QVariantMap& project) const;

    // `text` (a path or a file URL) as a clean local path with forward slashes: what two
    // spellings of one file have in common, for telling whether they are the same file.
    Q_INVOKABLE QString localPath(const QString& text) const;

    // The encode settings of `controller` (an EncoderController) as the `encode` object holds
    // them: only the keys in this version's table.
    Q_INVOKABLE QVariantMap captureEncode(QObject* controller) const;

    // Applies the settings of `encode` that belong to `phase` (0: the codec and container, which
    // the rest depend on and so go before the sources; 1: the rest, which go after the draft's
    // shape is settled) and returns a message for each value it could not use. A key not in
    // the table is left alone, silently: it is another version's.
    Q_INVOKABLE QVariantList applyEncode(QObject* controller, const QVariantMap& encode,
                                         int phase) const;
};
