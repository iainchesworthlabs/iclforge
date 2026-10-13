#include "window_placement.hpp"

#include <QMetaType>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

namespace iclforge::window_memory {

namespace {

// How much of a window's top has to be on a screen for a remembered position to be worth keeping:
// enough to see the window and to take hold of it. The strip is the height a title bar has; the
// client area starts below the frame, so a window whose client top is just inside the bottom of a
// screen has its title bar off it, and does not pass.
constexpr int kGrabStripHeight = 40;
constexpr int kGrabWidth = 100;
constexpr int kGrabHeight = 24;

// A rectangle no window has: a number a hand-edited store or a broken write could hold. Well
// beyond any desktop (an 8K monitor is 7680 pixels wide), small enough that the sums below
// cannot overflow an int.
constexpr int kMostPixels = 65536;

bool in_range(const QRect& r) {
    return r.isValid() && r.width() <= kMostPixels && r.height() <= kMostPixels &&
           std::abs(r.x()) <= kMostPixels && std::abs(r.y()) <= kMostPixels;
}

bool strip_reachable(const QPoint& position, int width, const QList<QRect>& screens) {
    const QRect strip(position, QSize(width, kGrabStripHeight));
    return std::any_of(screens.begin(), screens.end(), [&strip](const QRect& screen) {
        const QRect shown = strip.intersected(screen);
        return shown.width() >= kGrabWidth && shown.height() >= kGrabHeight;
    });
}

// The screen a rectangle mostly lies on; the first (the primary) when it lies on none.
QRect screen_for(const QRect& rect, const QList<QRect>& screens) {
    QRect best = screens.first();
    qint64 best_area = -1;
    for (const QRect& screen : screens) {
        const QRect overlap = rect.intersected(screen);
        const qint64 area = static_cast<qint64>(std::max(0, overlap.width())) *
                            static_cast<qint64>(std::max(0, overlap.height()));
        if (area > best_area) {
            best = screen;
            best_area = area;
        }
    }
    return best;
}

}  // namespace

QString geometry_text(const QRect& geometry) {
    return QStringLiteral("%1,%2,%3,%4")
        .arg(geometry.x())
        .arg(geometry.y())
        .arg(geometry.width())
        .arg(geometry.height());
}

std::optional<QRect> parse_geometry(const QVariant& value) {
    QRect rect;
    const int type = value.metaType().id();
    if (type == QMetaType::QRect) {
        rect = value.toRect();
    } else if (type == QMetaType::QString || type == QMetaType::QByteArray ||
               type == QMetaType::QStringList || type == QMetaType::QVariantList) {
        // The registry hands back text. An INI file does too when the value is quoted, which is how
        // QSettings writes it, and a list when it is not: a hand-edited
        // `window/geometry=100,100,1100,700` is read as four strings. A comma separates the four
        // numbers that geometry_text() writes, and a space is accepted as well.
        const QString text = (type == QMetaType::QStringList || type == QMetaType::QVariantList)
                                 ? value.toStringList().join(QLatin1Char(','))
                                 : value.toString();
        const QStringList parts =
            text.trimmed().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
        if (parts.size() != 4) {
            return std::nullopt;
        }
        int numbers[4] = {};
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            numbers[i] = parts[i].toInt(&ok);
            if (!ok) {
                return std::nullopt;
            }
        }
        rect = QRect(numbers[0], numbers[1], numbers[2], numbers[3]);
    } else {
        return std::nullopt;
    }
    if (!in_range(rect)) {
        return std::nullopt;
    }
    return rect;
}

std::optional<State> load(QSettings& settings) {
    State state;
    if (settings.contains(QLatin1String(kGeometryKey))) {
        state.geometry = parse_geometry(settings.value(QLatin1String(kGeometryKey)));
    }
    const bool has_maximised = settings.contains(QLatin1String(kMaximisedKey));
    if (has_maximised) {
        state.maximised = settings.value(QLatin1String(kMaximisedKey)).toBool();
    }
    if (!state.geometry.has_value() && !has_maximised) {
        return std::nullopt;
    }
    return state;
}

void save(QSettings& settings, const State& state) {
    if (state.geometry.has_value()) {
        settings.setValue(QLatin1String(kGeometryKey), geometry_text(*state.geometry));
    }
    settings.setValue(QLatin1String(kMaximisedKey), state.maximised);
}

Placement choose_placement(const State& remembered, const QList<QRect>& screens,
                           QSize fallback_size, QSize minimum) {
    Placement placement;
    placement.maximised = remembered.maximised;

    QSize size = remembered.geometry ? remembered.geometry->size() : fallback_size;
    size = size.expandedTo(minimum);

    if (screens.isEmpty()) {
        // Nothing to check a position against (a desktop always has a screen, so this is a
        // headless run): a position that cannot be checked is not used.
        placement.size = size;
        return placement;
    }

    if (remembered.geometry &&
        strip_reachable(remembered.geometry->topLeft(), size.width(), screens)) {
        placement.position = remembered.geometry->topLeft();
        const QRect where = screen_for(QRect(*placement.position, size), screens);
        size = size.boundedTo(where.size()).expandedTo(minimum);
    } else {
        size = size.boundedTo(screens.first().size()).expandedTo(minimum);
    }
    placement.size = size;
    return placement;
}

}  // namespace iclforge::window_memory
