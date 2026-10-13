// SystemMotion: the judgement parts of "has the operating system been asked to cut back
// animation?". What each platform's own call returns is not tested here - that is the operating
// system's - but which printed value counts as a yes, what an unreadable answer means, and how the
// override that stands in for the system behaves.
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QObject>

#include "system_motion.hpp"
#include "system_motion_parse.hpp"
#include "system_motion_query.hpp"

using iclforge::theme::reduced_from_gsettings_enable_animations;
using iclforge::theme::reduced_from_kde_animation_duration_factor;

namespace {

// The Linux read starts gsettings through QProcess, whose notifiers want an application object;
// none of these cases needs an event loop, so a core application is made once and left. It is
// not a QGuiApplication, which SystemMotion has to tell from one (it listens only to a GUI
// application's activation).
void ensure_application() {
    static int argc = 1;
    static char name[] = "iclforge-theme-tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
    (void)app;
}

}  // namespace

// The same checks at compile time: the parsers are constexpr so a regression in what counts as
// "reduced" is a build error on every platform, not only a test failure.
static_assert(reduced_from_gsettings_enable_animations("false\n"));
static_assert(!reduced_from_gsettings_enable_animations("true\n"));
static_assert(reduced_from_kde_animation_duration_factor("0"));
static_assert(!reduced_from_kde_animation_duration_factor("1"));

TEST_CASE("gsettings: only a plain false is a request to cut back", "[system-motion]") {
    CHECK(reduced_from_gsettings_enable_animations("false\n"));
    CHECK(reduced_from_gsettings_enable_animations("false"));
    CHECK(reduced_from_gsettings_enable_animations("  false \r\n"));

    CHECK_FALSE(reduced_from_gsettings_enable_animations("true\n"));
    // No gsettings, no schema, a process that failed: empty, and "not reduced".
    CHECK_FALSE(reduced_from_gsettings_enable_animations(""));
    CHECK_FALSE(reduced_from_gsettings_enable_animations("\n"));
    // Anything it does not recognise is not a request either - a missing tool must not take
    // animation away from a person who never asked.
    CHECK_FALSE(reduced_from_gsettings_enable_animations("No such schema 'org.gnome.desktop.interface'"));
    CHECK_FALSE(reduced_from_gsettings_enable_animations("0"));
    CHECK_FALSE(reduced_from_gsettings_enable_animations("False"));
}

TEST_CASE("KDE: only a duration factor of zero is a request to cut back", "[system-motion]") {
    CHECK(reduced_from_kde_animation_duration_factor("0"));
    CHECK(reduced_from_kde_animation_duration_factor("0.0"));
    CHECK(reduced_from_kde_animation_duration_factor("0.000"));
    CHECK(reduced_from_kde_animation_duration_factor(" 0 \n"));

    // 1 is the default speed, below it is faster, above it slower: none of them is "off".
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("1"));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("1.0"));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("0.5"));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("0.25"));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("2"));
    // Absent or unreadable: "not reduced".
    CHECK_FALSE(reduced_from_kde_animation_duration_factor(""));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("."));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("0.0.0"));
    CHECK_FALSE(reduced_from_kde_animation_duration_factor("off"));
}

TEST_CASE("SystemMotion: the override stands in for the system, and says when it changes", "[system-motion]") {
    ensure_application();
    SystemMotion motion;
    const bool system = iclforge::theme::query_system_reduced_motion();
    int changes = 0;
    QObject::connect(&motion, &SystemMotion::reducedChanged, [&changes] { ++changes; });

    // Not overridden: what the platform said when it was made.
    CHECK_FALSE(motion.overridden());
    CHECK(motion.reduced() == system);

    // An override is an answer, and a change even when it equals the system's: `overridden` is
    // part of what a window can bind to.
    motion.setOverride(true);
    CHECK(motion.overridden());
    CHECK(motion.reduced());
    CHECK(changes == 1);

    // The same override again says nothing new.
    motion.setOverride(true);
    CHECK(changes == 1);

    motion.setOverride(false);
    CHECK_FALSE(motion.reduced());
    CHECK(changes == 2);

    // Back to the system's answer.
    motion.clearOverride();
    CHECK_FALSE(motion.overridden());
    CHECK(motion.reduced() == system);
    CHECK(changes == 3);

    // Nothing to clear is not a change.
    motion.clearOverride();
    CHECK(changes == 3);
}

TEST_CASE("SystemMotion: refresh re-asks the platform and does not disturb an override", "[system-motion]") {
    ensure_application();
    SystemMotion motion;
    int changes = 0;
    QObject::connect(&motion, &SystemMotion::reducedChanged, [&changes] { ++changes; });

    // The platform has not changed between two asks, so a refresh is silent.
    motion.refresh();
    CHECK(changes == 0);

    motion.setOverride(true);
    REQUIRE(changes == 1);
    motion.refresh();
    CHECK(motion.reduced());
    CHECK(changes == 1);
}
