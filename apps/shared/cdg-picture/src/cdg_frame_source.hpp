#pragma once

#include <QImage>
#include <QObject>

// Whatever a window gets the picture of a CD+G disc from (planning/karaoke-phase2.md, W13f): the
// controller of the program that holds the disc and the position it is shown at. CdgPicture, the
// item that draws it, knows only this, so Hearth's KaraokeController, Crucible's CrucibleLyrics
// and the Forge GUI's player each give it frames from their own state, and the item is none of
// theirs.
//
// A source holds a CdgFrames (cdg_frames.hpp) and answers from it; it owns the delay and any other
// offset between the clock a view shows and the disc's, and takes it off before it asks.
namespace iclforge::apps {

class CdgFrameSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // The picture of disc `key` at `positionMs`. `key` is 0 for a source with one disc (Hearth,
    // the Forge GUI) and says which of several for one with many (an application id in Crucible).
    // A null image when there is no picture.
    [[nodiscard]] virtual QImage pictureFrame(int key, qlonglong positionMs) = 0;

signals:
    // Disc `key` is a different picture at the same position: another file, or another delay.
    void pictureChanged(int key);
};

}  // namespace iclforge::apps
