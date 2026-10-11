#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQmlIntegration>

#include "karaoke_controller.hpp"

// The picture of a CD+G disc (planning/karaoke-phase2.md, W13f and decision D11): the frame the
// KaraokeController holds for the position, drawn as large as fits and with the proportions of
// the disc's 300 by 216, in whole-pixel steps so that its small type stays crisp. The controller
// reads the file and follows the packets; this is only the glass in front of it, and is why it is
// a separate class: Qt Quick is not linked into the controller's own tests.
//
// Nothing here owns a clock. LyricView binds `positionMs` to the lyric clock, and the picture is a
// function of that position (and of the delay the singer has set, which the controller takes off).
namespace iclforge::hearth::ui {

class CdgPicture : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    // The KaraokeController to ask, held as an object so that QML assigns the singleton as it is.
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(qreal positionMs READ positionMs WRITE setPositionMs NOTIFY positionMsChanged)
    // Whether there is a picture to show.
    Q_PROPERTY(bool ready READ ready NOTIFY frameChanged)

public:
    explicit CdgPicture(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* controller() const;
    void setController(QObject* controller);
    [[nodiscard]] qreal positionMs() const { return position_ms_; }
    void setPositionMs(qreal position_ms);
    [[nodiscard]] bool ready() const { return !frame_.isNull(); }

    // The frame as last drawn, for a test that wants the pixels and not a screenshot.
    [[nodiscard]] QImage frame() const { return frame_; }

    void paint(QPainter* painter) override;

signals:
    void controllerChanged();
    void positionMsChanged();
    void frameChanged();

private:
    void refresh();

    QPointer<KaraokeController> controller_;
    qreal position_ms_ = 0;
    QImage frame_;
};

}  // namespace iclforge::hearth::ui
