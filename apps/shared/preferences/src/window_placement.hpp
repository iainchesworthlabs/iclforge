#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QVariant>
#include <optional>

// What forge-gui, Hearth and Crucible remember about their window between starts, and where a
// remembered window may open. This file is the part that needs no display and no window: the two
// keys, how they are read and written, and the rule that turns what was remembered and the screens
// that are connected now into a size and a position. window_memory.hpp is the part that holds a
// QWindow and applies the answer.
//
// The two keys are in each program's own settings store (organisation `iclforge`; application
// `forge-gui`, `Hearth` or `Crucible`), in the `window` group, like the rest of the family's
// `group/name` keys:
//
//   window/geometry   "x,y,width,height": the window's NORMAL rectangle, the client area in the
//                     window system's logical pixels, as QWindow::geometry() reports it. It is
//                     the rectangle the window returns to when it is not maximised, so it is
//                     kept while the window is maximised and is not the maximised rectangle.
//   window/maximised  true when the window was maximised when it was last closed. Not set for a
//                     minimised, full-screen or hidden window: they say nothing about where the
//                     person left it.
//
// Text and not a QRect variant, so that a person reading the registry or the INI file, and a
// capture's `--setting window/geometry=100,100,1100,700`, see the same thing; a QRect variant
// written by anything else is read as well.
//
// Nothing here changes a minimum size. A program's minimum (forge-gui 1280 by 900, Hearth and
// Crucible 960 by 620) is its Main.qml's and is passed in: a remembered size below it is raised
// to it, and the minimum wins over a screen that is smaller than it, as it does when there is no
// memory at all.

namespace iclforge::window_memory {

inline constexpr char kGeometryKey[] = "window/geometry";
inline constexpr char kMaximisedKey[] = "window/maximised";

// What is held in the store. `geometry` is empty when nothing usable is remembered of the
// rectangle (no value, or one that is not four whole numbers with a positive width and height).
struct State {
    std::optional<QRect> geometry;
    bool maximised = false;
};

// Where to open, and how.
struct Placement {
    QSize size;
    std::optional<QPoint> position;  // empty: leave the position to the window system
    bool maximised = false;
};

// The window's remembered state, or nothing when the store has neither key in a usable form.
// A value that cannot be read is ignored as if it were not there (a hand-edited store never
// stops a program from starting).
std::optional<State> load(QSettings& settings);

// Writes both keys. A State with no geometry leaves the geometry key as it is.
void save(QSettings& settings, const State& state);

// The text form of a rectangle, and the rectangle a text (or a QRect variant) says: the pair
// load() and save() use, exposed for the tests that hold the format.
QString geometry_text(const QRect& geometry);
std::optional<QRect> parse_geometry(const QVariant& value);

// Where to open, given what was remembered and the usable area (QScreen::availableGeometry(), so
// not under a task bar) of every connected screen, the primary first.
//
//  - The size is the remembered one, or `fallback_size` (the window's own size before anything
//    is applied) when only the maximised flag was remembered; it is raised to `minimum`, and
//    brought down to the screen it lands on, but never below `minimum`.
//  - The position is kept only when a strip of the window's top, the part a person grabs, shows
//    on some connected screen. A window left on a monitor that is no longer connected, or on a
//    resolution that no longer reaches it, would otherwise open where nobody can see it or drag
//    it back; its position is dropped (the window system places it) and its size is kept,
//    fitted to the primary screen.
Placement choose_placement(const State& remembered, const QList<QRect>& screens,
                           QSize fallback_size, QSize minimum);

}  // namespace iclforge::window_memory
