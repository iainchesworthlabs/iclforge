#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQmlIntegration>

#include "cdg_frame_source.hpp"

// The picture of a CD+G disc (planning/karaoke-phase2.md, W13f and decision D11): the frame a
// CdgFrameSource holds for the position, drawn as large as fits and with the proportions of the
// disc's 300 by 216, in whole-pixel steps so that its small type stays crisp. The source reads the
// file and follows the packets; this is only the glass in front of it, and is why it is a separate
// class: Qt Quick is not linked into a controller's own tests.
//
// Nothing here owns a clock. The view binds `positionMs` to its lyric clock, and the picture is a
// function of that position (and of whatever offset the source takes off).
namespace iclforge::apps {

class CdgPicture : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    // The CdgFrameSource to ask, held as an object so that QML assigns a singleton as it is.
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
    // Which disc of the source: 0 where it has one, an application id where it has many.
    Q_PROPERTY(int key READ key WRITE setKey NOTIFY keyChanged)
    Q_PROPERTY(qreal positionMs READ positionMs WRITE setPositionMs NOTIFY positionMsChanged)
    // Whether there is a picture to show.
    Q_PROPERTY(bool ready READ ready NOTIFY frameChanged)

public:
    explicit CdgPicture(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* source() const;
    void setSource(QObject* source);
    [[nodiscard]] int key() const { return key_; }
    void setKey(int key);
    [[nodiscard]] qreal positionMs() const { return position_ms_; }
    void setPositionMs(qreal position_ms);
    [[nodiscard]] bool ready() const { return !frame_.isNull(); }

    // The frame as last drawn, for a test that wants the pixels and not a screenshot.
    [[nodiscard]] QImage frame() const { return frame_; }

    void paint(QPainter* painter) override;

signals:
    void sourceChanged();
    void keyChanged();
    void positionMsChanged();
    void frameChanged();

private:
    void refresh();

    QPointer<CdgFrameSource> source_;
    int key_ = 0;
    qreal position_ms_ = 0;
    QImage frame_;
};

}  // namespace iclforge::apps
