#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <optional>

#include "window_placement.hpp"

// Opens a program's top-level window where it was left and keeps what it does, in the program's own
// settings store: window_placement.hpp has the keys and the rules. One per program, made in main()
// after the engine has loaded Main.qml.
//
//   engine.setInitialProperties({{QStringLiteral("visible"), false}});   // before it is shown
//   engine.loadFromModule(...);
//   static_cast<void>(window_memory.attach(root_window));                // places it, shows it
//
// The window is asked to load hidden so that it is placed before it is first shown, and never
// opens at one place and jumps to another. attach() shows it, so a program that asks for that must
// call it on every path. When the window turns out to be visible already (a Main.qml that does not
// go through the engine's initial properties), attach() moves and resizes it where it is; the
// result is the same, with the one jump.
//
// It follows the window until the application quits and writes what it learns: the normal
// geometry whenever the window is windowed, the maximised flag whenever it is windowed or
// maximised. Minimised, full-screen and hidden windows are not recorded: Hearth's lyric screen
// takes the whole screen and hands the window back as it was, and Crucible hides to its tray; a
// remembered window is the one a person last placed.
//
// Not applied to a test: the Qt Quick suites and the capture modes run on an empty scratch store
// (QSettings::setPath on a temporary directory), so there is nothing remembered for them to open
// at, and a window under test opens at its own size.

namespace iclforge::window_memory {

class WindowMemory : public QObject {
    Q_OBJECT

public:
    explicit WindowMemory(QObject* parent = nullptr);

    // Reads the store, places `window` (its size, and its position where the position is on a
    // connected screen), shows it, and starts keeping its state. Returns whether anything was
    // remembered. The minimum size it is held to is the window's own (QWindow::minimumSize()).
    // Call it once, with the program's main window.
    bool attach(QWindow* window);

    // The screens' usable areas, primary first: what choose_placement() is given.
    static QList<QRect> connected_screens();

private:
    void changed();
    void capture();
    void persist();

    QPointer<QWindow> window_;
    QTimer settle_;
    State state_;
    bool have_state_ = false;
    std::optional<State> written_;  // the last state persist() wrote, so that an unchanged one is not
};

}  // namespace iclforge::window_memory
