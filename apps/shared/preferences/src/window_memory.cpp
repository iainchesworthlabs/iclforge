#include "window_memory.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>

namespace iclforge::window_memory {

namespace {

// A quiet this long in the window's geometry and visibility signals means it has stopped being
// moved or sized, and that a maximise has landed: those signals do not arrive in a fixed order
// (a maximise changes the size before it changes the visibility), so the state is read once, after.
constexpr int kSettleMs = 500;

bool same(const State& a, const State& b) {
    return a.maximised == b.maximised && a.geometry == b.geometry;
}

}  // namespace

WindowMemory::WindowMemory(QObject* parent) : QObject(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(kSettleMs);
    connect(&settle_, &QTimer::timeout, this, [this] {
        capture();
        persist();
    });
}

QList<QRect> WindowMemory::connected_screens() {
    QList<QRect> screens;
    const QList<QScreen*> all = QGuiApplication::screens();  // the primary first
    screens.reserve(all.size());
    for (const QScreen* screen : all) {
        screens.append(screen->availableGeometry());
    }
    return screens;
}

bool WindowMemory::attach(QWindow* window) {
    if (window == nullptr || !window_.isNull()) {
        return false;
    }
    window_ = window;

    bool remembered = false;
    {
        QSettings settings;
        if (const std::optional<State> loaded = load(settings)) {
            remembered = true;
            state_ = *loaded;
            have_state_ = true;
            written_ = *loaded;

            const Placement placement = choose_placement(
                *loaded, connected_screens(), window->size(), window->minimumSize());
            if (placement.position) {
                window->setGeometry(QRect(*placement.position, placement.size));
            } else {
                window->resize(placement.size);
            }
            if (placement.maximised) {
                // The normal rectangle above is what the window returns to; Qt keeps it while
                // the window is maximised. showMaximized() also shows a window that is hidden.
                window->showMaximized();
            }
        }
    }
    if (!window->isVisible()) {
        window->show();
    }
    // The window is placed and shown: from here its changes are the person's.
    capture();

    connect(window, &QWindow::xChanged, this, &WindowMemory::changed);
    connect(window, &QWindow::yChanged, this, &WindowMemory::changed);
    connect(window, &QWindow::widthChanged, this, &WindowMemory::changed);
    connect(window, &QWindow::heightChanged, this, &WindowMemory::changed);
    connect(window, &QWindow::visibilityChanged, this, &WindowMemory::changed);
    connect(qGuiApp, &QGuiApplication::aboutToQuit, this, [this] {
        capture();
        persist();
    });
    return remembered;
}

void WindowMemory::changed() {
    settle_.start();
}

void WindowMemory::capture() {
    if (window_.isNull()) {
        return;
    }
    switch (window_->visibility()) {
        case QWindow::Windowed:
            state_.geometry = window_->geometry();
            state_.maximised = false;
            have_state_ = true;
            break;
        case QWindow::Maximized:
            // Keep the last normal rectangle: it is where an un-maximise goes back to.
            state_.maximised = true;
            have_state_ = true;
            break;
        default:
            // Minimised, full screen, hidden: nothing is learned.
            break;
    }
}

void WindowMemory::persist() {
    if (!have_state_ || (written_ && same(*written_, state_))) {
        return;
    }
    QSettings settings;
    save(settings, state_);
    settings.sync();
    written_ = state_;
}

}  // namespace iclforge::window_memory
