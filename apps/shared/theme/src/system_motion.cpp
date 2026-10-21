#include "system_motion.hpp"

#include "system_motion_query.hpp"

#include <QCoreApplication>
#include <QGuiApplication>

SystemMotion::SystemMotion(QObject* parent)
    : QObject(parent), system_(iclforge::theme::query_system_reduced_motion()) {
    // qobject_cast, not qGuiApp: there is not always an application object, and the unit tests
    // run with a core one, which qGuiApp would hand back as a QGuiApplication all the same.
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()); app != nullptr) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            // Qt::ApplicationState is a set of bits (Active is 0x4), so MSVC wants the bit tested (C26813).
            if ((state & Qt::ApplicationActive) != 0) {
                refresh();
            }
        });
    }
}

bool SystemMotion::reduced() const {
    return has_override_ ? override_ : system_;
}

bool SystemMotion::overridden() const {
    return has_override_;
}

void SystemMotion::refresh() {
    const bool before = reduced();
    system_ = iclforge::theme::query_system_reduced_motion();
    if (reduced() != before) {
        emit reducedChanged();
    }
}

void SystemMotion::setOverride(bool reducedValue) {
    const bool before = reduced();
    const bool hadOverride = has_override_;
    has_override_ = true;
    override_ = reducedValue;
    if (reduced() != before || !hadOverride) {
        emit reducedChanged();
    }
}

void SystemMotion::clearOverride() {
    if (!has_override_) {
        return;
    }
    has_override_ = false;
    emit reducedChanged();
}
