#pragma once

#include <QObject>
#include <QtQmlIntegration>

// Whether the operating system has been asked to cut back animation, for
// Theme.qml's motion choice "system" (Settings > Appearance > Motion: System,
// Reduced, Full). Qt has no cross-platform API for this - QStyleHints says
// nothing about it as of 6.8 - so each platform answers in its own file
// (system_motion_query.hpp) and an unknown answers "not reduced".
//
// The answer is read once, when the first binding asks, and again whenever
// the window is activated: nothing tells a Qt program that Settings flipped
// the switch, and the person who just did so is about to come back to the
// window. Same shape as SystemTheme: a property QML binds to, with the
// choice and its default applied in Theme, not here.
class SystemMotion : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool reduced READ reduced NOTIFY reducedChanged)
    Q_PROPERTY(bool overridden READ overridden NOTIFY reducedChanged)

public:
    explicit SystemMotion(QObject* parent = nullptr);

    // What the system says, or the override standing in for it.
    [[nodiscard]] bool reduced() const;
    [[nodiscard]] bool overridden() const;

    // Ask the platform again.
    Q_INVOKABLE void refresh();
    // Stand in for the system's answer. The Qt Quick suites use this to be
    // "a machine that asked for reduced motion" without touching the
    // operating system's settings, and the picture tool does the same; the
    // windows never call it.
    Q_INVOKABLE void setOverride(bool reduced);
    Q_INVOKABLE void clearOverride();

signals:
    void reducedChanged();

private:
    bool system_ = false;
    bool has_override_ = false;
    bool override_ = false;
};
