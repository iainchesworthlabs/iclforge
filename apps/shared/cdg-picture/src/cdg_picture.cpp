#include "cdg_picture.hpp"

#include <QPainter>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace iclforge::apps {

CdgPicture::CdgPicture(QQuickItem* parent) : QQuickPaintedItem(parent) {
    // What is drawn is a black backdrop and the frame over it: nothing to blend under it.
    setOpaquePainting(true);
}

QObject* CdgPicture::source() const {
    return source_.data();
}

void CdgPicture::setSource(QObject* source) {
    auto* typed = qobject_cast<CdgFrameSource*>(source);
    if (typed == source_.data()) {
        return;
    }
    if (source_ != nullptr) {
        disconnect(source_.data(), nullptr, this, nullptr);
    }
    source_ = typed;
    if (typed != nullptr) {
        // Another file or another delay is a different picture at the same position.
        connect(typed, &CdgFrameSource::pictureChanged, this, [this](int key) {
            if (key == key_) {
                refresh();
            }
        });
    }
    emit sourceChanged();
    refresh();
}

void CdgPicture::setKey(int key) {
    if (key == key_) {
        return;
    }
    key_ = key;
    emit keyChanged();
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
    frame_ = source_ != nullptr ? source_->pictureFrame(key_, static_cast<qlonglong>(std::llround(position_ms_)))
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

}  // namespace iclforge::apps
