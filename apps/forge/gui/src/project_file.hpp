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
// The document itself - reading it, checking its version, keeping what this version does not
// know, paths relative to the file - is apps/shared/project, which `forge` reads the same file
// with. This is the window's half: it hands QML the document as JSON text (QML has JSON.parse and
// JSON.stringify, and a JS object keeps its keys in order, which a QVariantMap would not), and it
// maps the settings a project holds onto the EncoderController's properties.
//
// A project is built to be extended, and the rules that allow it are the document's own
// (project_document.hpp): `version`, keys kept, paths relative. What this file adds is that
// every saved setting is one row of a table in project_file.cpp, keyed by what the command
// line calls it ("dialnorm", "drc", "bsmod") and holding what the command line takes ("auto",
// "-4.5", "voiceover"), never a position in a list the window may reorder. A new setting is a new
// row, not a new format.
//
// Messages are a code and its arguments, not a sentence: the window words them, and so
// translates them.
class ProjectFile : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString format READ format CONSTANT)
    Q_PROPERTY(int version READ version CONSTANT)
    Q_PROPERTY(QString suffix READ suffix CONSTANT)

public:
    explicit ProjectFile(QObject* parent = nullptr);

    [[nodiscard]] QString format() const;
    [[nodiscard]] int version() const;
    // "forgeproj", without the dot.
    [[nodiscard]] QString suffix() const;

    // The codes of the messages below, with their arguments: cannot-open (name), not-json (name),
    // not-a-project (name), no-version (name), newer-version (name, the file's version, the one
    // read here), cannot-write (name), source-no-path (the source's number),
    // setting-unknown-choice (key, value), setting-out-of-range (key, value),
    // bitrate-not-offered (kbps), setting-failed (key), setting-not-held (key: the project has
    // it and `forge` uses it, and this window has no control for it), objects-more (the
    // project's count, the window's), objects-failed.

    // Reads `file`. The map holds `ok`, and when it is false `error`, a message. When true,
    // `text` is the whole document as JSON, unknown keys included and in the order they were
    // written, with each source given a `url` (its path made absolute from the project's folder)
    // and `exists`; and `warnings` lists the messages for what was set aside.
    Q_INVOKABLE QVariantMap read(const QUrl& file) const;

    // Writes `text`, a project as JSON, to `file`, with format and version stamped on it. Each
    // source's `path` may be a local path or a file URL; it is written relative to the file's
    // folder where there is a relative form. The map holds `ok` and, when false, `error`. The
    // file is replaced only once whole.
    Q_INVOKABLE QVariantMap write(const QUrl& file, const QString& text) const;

    // `text` (a path or a file URL) as a clean local path with forward slashes: what two
    // spellings of one file have in common, for telling whether they are the same file.
    Q_INVOKABLE QString localPath(const QString& text) const;

    // The encode settings of `controller` (an EncoderController) as the `encode` object holds
    // them, as JSON text in the order the table has them: codec, container, bitrateKbps, tools,
    // vbr and `options`, which has only what differs from the command line's defaults.
    Q_INVOKABLE QString captureEncode(QObject* controller) const;

    // The keys of `options` this window holds, which a capture writes whole: any other key a
    // project has there is another version's, or the command line's, and is carried through.
    Q_INVOKABLE QStringList optionKeys() const;

    // The same for an AC-4 project: `forge ac4-encode`'s own keys, which the AC-4 tab holds
    // (frame-rate, rate-mode, codec-mode, dialnorm, loudness, drc, cmixlev, surmixlev, dmixmod,
    // dialogue-*, iframe-interval, crc), and `coding`, which AC-4 objects add.
    Q_INVOKABLE QStringList ac4OptionKeys() const;

    // Applies `encode` to `controller` in two parts, so that the rest of the draft can be set
    // up between them. Phase 0 puts every setting to the command line's default
    // (EncoderController::resetEncodeSettings) and sets the codec and the container, which the
    // rest of the draft depends on. Phase 1, after the sources and the channel plan, sets the
    // bit rate, the coding tools, variable bit rate and `options`. Returns a message for each
    // value it could not use, and for each option the project has that this window has no
    // control for.
    Q_INVOKABLE QVariantList applyEncode(QObject* controller, const QVariantMap& encode,
                                         int phase) const;

    // The objects of an object (Atmos) session as `objects` holds them, as JSON text: how many
    // there are and their placements and motion, as the scene file `forge atmos-encode` reads
    // (iclforge::objects::oba::to_json). Empty text when `controller` is not in object mode.
    Q_INVOKABLE QString captureObjects(QObject* controller) const;

    // Puts the motion `objects` (JSON text, as captureObjects wrote it) holds onto the objects
    // `controller` has. Returns the messages for what did not fit.
    Q_INVOKABLE QVariantList applyObjects(QObject* controller, const QString& objects) const;
};
