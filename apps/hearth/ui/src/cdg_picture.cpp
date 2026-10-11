#include "cdg_picture.hpp"

#include <QPainter>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace iclforge::hearth::ui {

CdgPicture::CdgPicture(QQuickItem* parent) : QQuickPaintedItem(parent) {
    // What is drawn is the frame, a whole image: nothing to blend under it.
    setOpaquePainting(true);
}

QObject* CdgPicture::controller() const {
    return controller_.data();
}

void CdgPicture::setController(QObject* controller) {
    auto* typed = qobject_cast<KaraokeController*>(controller);
    if (typed == controller_.data()) {
        return;
    }
    if (controller_ != nullptr) {
        disconnect(controller_.data(), nullptr, this, nullptr);
    }
    controller_ = typed;
    if (typed != nullptr) {
        // A new item or a nudge of the delay is a different picture at the same position.
        connect(typed, &KaraokeController::lyricsChanged, this, &CdgPicture::refresh);
        connect(typed, &KaraokeController::delayChanged, this, &CdgPicture::refresh);
    }
    emit controllerChanged();
    refresh();
}

void CdgPicture::setPositionMs(qreal position_ms) {
    if (position_ms == position_ms_) {
        return;
    }
    position_ms_ = position_ms;
    emit positionMsChanged();
    refresh();
}

void CdgPicture::refresh() {
    const bool was_ready = ready();
    frame_ = controller_ != nullptr ? controller_->pictureFrame(static_cast<qlonglong>(std::llround(position_ms_)))
                                    : QImage{};
    update();
    if (was_ready != ready()) {
        emit frameChanged();
    }
}

void CdgPicture::paint(QPainter* painter) {
    painter->fillRect(QRectF(0, 0, width(), height()), Qt::black);
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return;
    }
    // The largest whole multiple that fits, or the largest scale that fits when the picture is
    // smaller than the disc's own size: whole steps keep the pixels square and their edges clean.
    const qreal fit = std::min(width() / frame_.width(), height() / frame_.height());
    const qreal scale = fit >= 1.0 ? std::floor(fit) : fit;
    const qreal w = frame_.width() * scale;
    const qreal h = frame_.height() * scale;
    const QRectF target((width() - w) / 2.0, (height() - h) / 2.0, w, h);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, scale < 1.0);
    painter->drawImage(target, frame_);
}

}  // namespace iclforge::hearth::ui
