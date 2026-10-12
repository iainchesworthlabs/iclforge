#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

// The Karaoke flow's rules, for QML (planning/karaoke-design.md, K6): a
// stateless singleton over karaoke_authoring.hpp, so what the page shows of a
// set of parts - the coding mode, where each coded channel comes from, the
// checks - is what the tested helper decided, not something QML works out
// again. The flow's own state (which file is which part) lives in the page,
// and EncoderController keeps the draft; this holds neither.
class KaraokeAuthoring : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit KaraokeAuthoring(QObject* parent = nullptr);

    // `files`: one map per part supplied - part ("backing", "melody",
    // "vocal1", "vocal2", "lfe"), name, channels, rate (Hz) and seconds. The plan:
    // buildable, mode ("3/2", an EncoderController.bedChoices id, or ""),
    // melody, vocals, lfe, channels (position, part, file, channel - `file` an
    // index into `files`) and checks (level "ok", "warning" or "problem",
    // code and args; the page turns a code into a sentence).
    Q_INVOKABLE QVariantMap planParts(const QVariantList& files) const;

    // The same for one multichannel file whose speakers name its channels:
    // name, channels, rate, seconds and speakers (Table E2.5 short names in
    // channel order, empty when the file does not say).
    Q_INVOKABLE QVariantMap planSpeakers(const QVariantMap& file) const;

    // Which parts reach which speaker (karaoke_outputs()): l, c, r, ls, rs and
    // lfe, each a list of Table C.2.1 names ("LFE" for the low-frequency
    // channel). `listening` is "asCoded", "aware" or "capable" and `choice`
    // the vocals "capable" plays: "none", "v1", "v2" or "both".
    Q_INVOKABLE QVariantMap outputs(bool melody, int vocals, const QString& listening,
                                    const QString& choice, bool folded = false,
                                    bool lfe = false) const;

    // "3:42".
    Q_INVOKABLE QString durationText(double seconds) const;

    // The kbps a stream of `mode` ("3/2") starts at (karaoke_default_bitrate_kbps()).
    Q_INVOKABLE int defaultBitrateKbps(const QString& mode) const;

    // The lyrics file of the same name beside a file (`.lrc`, else `.srt`, `.ass` or `.kar`), as a
    // file URL, or "" (karaoke_lyrics_beside()). `url` is a file URL or a path.
    Q_INVOKABLE QString lyricsBeside(const QString& url) const;

    // Puts a lyrics file beside a saved stream as the stream's own UTF-8 `.lrc`
    // (karaoke_copy_lyrics()): "copied", "already-there", "exists" (one was there and is left
    // alone), "unreadable" or "failed".
    Q_INVOKABLE QString copyLyrics(const QString& lyricsUrl, const QString& streamPath) const;

    // Why a lyrics file cannot be used as lyrics, in the reader's words, or "" when it can
    // (karaoke_lyrics_problem()): a file in a code page is said so when it is chosen, and the page
    // asks again for the reason when a copy came back "unreadable". `url` is a file URL or a
    // path.
    Q_INVOKABLE QString lyricsProblem(const QString& url) const;
};
