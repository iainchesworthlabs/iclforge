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
    // "vocal1", "vocal2"), name, channels, rate (Hz) and seconds. The plan:
    // buildable, mode ("3/2", an EncoderController.bedChoices id, or ""),
    // melody, vocals, channels (position, part, file, channel - `file` an
    // index into `files`) and checks (level "ok", "warning" or "problem",
    // code and args; the page turns a code into a sentence).
    Q_INVOKABLE QVariantMap planParts(const QVariantList& files) const;

    // The same for one multichannel file whose speakers name its channels:
    // name, channels, rate, seconds and speakers (Table E2.5 short names in
    // channel order, empty when the file does not say).
    Q_INVOKABLE QVariantMap planSpeakers(const QVariantMap& file) const;

    // Which parts reach which speaker (karaoke_outputs()): l, c, r, ls and rs,
    // each a list of Table C.2.1 names. `listening` is "asCoded", "aware" or
    // "capable" and `choice` the vocals "capable" plays: "none", "v1", "v2" or
    // "both".
    Q_INVOKABLE QVariantMap outputs(bool melody, int vocals, const QString& listening,
                                    const QString& choice, bool folded = false) const;

    // "3:42".
    Q_INVOKABLE QString durationText(double seconds) const;
};
