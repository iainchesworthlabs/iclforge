#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <optional>

#include "window_placement.hpp"

// What forge-gui, Hearth and Crucible remember about their window and where a remembered window may
// open (apps/shared/preferences/src/window_placement.hpp). The stores are INI files in a temporary
// directory (QSettings::setPath), the way the Qt Quick suites isolate theirs, so no test reads or
// writes the registry or a real person's settings. The placement rule is arithmetic over rectangles
// and takes the screens as a list, so every arrangement of monitors is a test here with no display.

using iclforge::window_memory::choose_placement;
using iclforge::window_memory::geometry_text;
using iclforge::window_memory::kGeometryKey;
using iclforge::window_memory::kMaximisedKey;
using iclforge::window_memory::load;
using iclforge::window_memory::parse_geometry;
using iclforge::window_memory::Placement;
using iclforge::window_memory::save;
using iclforge::window_memory::State;

namespace {

// The minimums of the programs' Main.qml files: forge-gui 1280 by 900, Hearth and Crucible 960 by
// 620. The rule is given the minimum and never looks one up.
constexpr QSize kHearthMinimum{960, 620};
constexpr QSize kForgeMinimum{1280, 900};

// What a window's own size is before anything is applied (Hearth's Main.qml: 1280 by 800).
constexpr QSize kOwnSize{1280, 800};

// One 1920 by 1080 monitor with a 40 pixel task bar under it, which is what
// QScreen::availableGeometry() reports for it.
const QRect kPrimary{0, 0, 1920, 1040};
// A second monitor to the right, and one to the left (a negative x).
const QRect kRight{1920, 0, 1920, 1080};
const QRect kLeft{-1920, 0, 1920, 1080};

// The settings format and directory the test runs under.
class Scratch {
   public:
    Scratch() {
        REQUIRE(dir_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    static QSettings store() {
        return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("iclforge-tests"),
                         QStringLiteral("window-memory"));
    }

   private:
    QTemporaryDir dir_;
};

State remembered(const QRect& geometry, bool maximised = false) {
    return State{geometry, maximised};
}

}  // namespace

TEST_CASE("the window is remembered under window/geometry and window/maximised",
          "[window-memory][store]") {
    Scratch scratch;

    // The names are the contract with the family's other apps and with a person reading the
    // registry: held here as the literal strings, not through the constants.
    CHECK(QString::fromLatin1(kGeometryKey) == QStringLiteral("window/geometry"));
    CHECK(QString::fromLatin1(kMaximisedKey) == QStringLiteral("window/maximised"));

    {
        QSettings settings = Scratch::store();
        save(settings, remembered(QRect(120, 60, 1100, 700), true));
        settings.sync();
    }
    QSettings raw = Scratch::store();
    // Text, so that the registry, the INI file and a capture's --setting all show the same thing.
    CHECK(raw.value(QStringLiteral("window/geometry")).toStringList().join(QLatin1Char(',')) ==
          QStringLiteral("120,60,1100,700"));
    CHECK(raw.value(QStringLiteral("window/maximised")).toBool());
}

TEST_CASE("a store with nothing remembered loads as nothing", "[window-memory][store]") {
    Scratch scratch;
    QSettings settings = Scratch::store();
    CHECK_FALSE(load(settings).has_value());
}

TEST_CASE("what was saved is what loads", "[window-memory][store]") {
    Scratch scratch;
    QSettings settings = Scratch::store();

    SECTION("a normal window") {
        save(settings, remembered(QRect(-1700, 30, 1280, 900)));
        const std::optional<State> state = load(settings);
        REQUIRE(state.has_value());
        CHECK(state->geometry == QRect(-1700, 30, 1280, 900));
        CHECK_FALSE(state->maximised);
    }
    SECTION("a maximised window keeps the rectangle it returns to") {
        save(settings, remembered(QRect(40, 40, 1000, 640), true));
        const std::optional<State> state = load(settings);
        REQUIRE(state.has_value());
        CHECK(state->geometry == QRect(40, 40, 1000, 640));
        CHECK(state->maximised);
    }
    SECTION("saving with no rectangle leaves the rectangle that is there") {
        save(settings, remembered(QRect(40, 40, 1000, 640)));
        save(settings, State{std::nullopt, true});
        const std::optional<State> state = load(settings);
        REQUIRE(state.has_value());
        CHECK(state->geometry == QRect(40, 40, 1000, 640));
        CHECK(state->maximised);
    }
}

TEST_CASE("a value that is not a rectangle is ignored, and never stops the program", "[window-memory][store]") {
    Scratch scratch;
    QSettings settings = Scratch::store();

    const QString bad = GENERATE(QStringLiteral(""), QStringLiteral("abc"), QStringLiteral("1,2,3"),
                                 QStringLiteral("1,2,3,4,5"), QStringLiteral("1,2,x,4"),
                                 QStringLiteral("10,10,0,500"), QStringLiteral("10,10,500,0"),
                                 QStringLiteral("10,10,-500,500"), QStringLiteral("10,10,99999999,500"),
                                 QStringLiteral("99999999,10,500,500"));
    CAPTURE(bad);
    settings.setValue(QStringLiteral("window/geometry"), bad);
    // With no maximised key either, there is nothing usable, as for an empty store.
    CHECK_FALSE(load(settings).has_value());

    // And with one, the flag is still honoured and the rectangle is absent.
    settings.setValue(QStringLiteral("window/maximised"), true);
    const std::optional<State> state = load(settings);
    REQUIRE(state.has_value());
    CHECK_FALSE(state->geometry.has_value());
    CHECK(state->maximised);
}

TEST_CASE("a rectangle is read from the forms a store hands back", "[window-memory][store]") {
    CHECK(parse_geometry(QVariant(QStringLiteral("100,200,1100,700"))) == QRect(100, 200, 1100, 700));
    CHECK(parse_geometry(QVariant(QStringLiteral(" 100, 200, 1100, 700 "))) == QRect(100, 200, 1100, 700));
    // A hand-edited store with spaces, and a QRect variant written by something else.
    CHECK(parse_geometry(QVariant(QStringLiteral("100 200 1100 700"))) == QRect(100, 200, 1100, 700));
    CHECK(parse_geometry(QVariant(QRect(5, 6, 700, 500))) == QRect(5, 6, 700, 500));
    // An INI file reads an unquoted comma-separated value as a list of strings.
    CHECK(parse_geometry(QVariant(QStringList{QStringLiteral("100"), QStringLiteral("200"),
                                              QStringLiteral("1100"), QStringLiteral("700")})) ==
          QRect(100, 200, 1100, 700));
    CHECK_FALSE(parse_geometry(QVariant(QStringList{QStringLiteral("100"), QStringLiteral("200")}))
                    .has_value());
    // Not a rectangle at all.
    CHECK_FALSE(parse_geometry(QVariant(7)).has_value());
    CHECK_FALSE(parse_geometry(QVariant()).has_value());
    CHECK(geometry_text(QRect(-5, 6, 700, 500)) == QStringLiteral("-5,6,700,500"));
}

TEST_CASE("the maximised flag is read as a bool from the text a capture sets", "[window-memory][store]") {
    Scratch scratch;
    QSettings settings = Scratch::store();
    settings.setValue(QStringLiteral("window/maximised"), QStringLiteral("true"));
    REQUIRE(load(settings).has_value());
    CHECK(load(settings)->maximised);
    settings.setValue(QStringLiteral("window/maximised"), QStringLiteral("false"));
    REQUIRE(load(settings).has_value());
    CHECK_FALSE(load(settings)->maximised);
}

TEST_CASE("a window that is on a screen opens exactly where it was left", "[window-memory][placement]") {
    const Placement p =
        choose_placement(remembered(QRect(300, 120, 1100, 700)), {kPrimary}, kOwnSize, kHearthMinimum);
    REQUIRE(p.position.has_value());
    CHECK(*p.position == QPoint(300, 120));
    CHECK(p.size == QSize(1100, 700));
    CHECK_FALSE(p.maximised);
}

TEST_CASE("a position that is off every screen is dropped and the size is kept", "[window-memory][placement]") {
    const QRect off_screen = GENERATE(QRect(5000, 100, 1100, 700),    // right of everything
                                      QRect(-4000, 100, 1100, 700),   // left of everything
                                      QRect(100, 3000, 1100, 700),    // below
                                      QRect(100, -2000, 1100, 700));  // above
    CAPTURE(off_screen);
    const Placement p = choose_placement(remembered(off_screen), {kPrimary}, kOwnSize, kHearthMinimum);
    // The window system places it; it does not get a position that nobody can see.
    CHECK_FALSE(p.position.has_value());
    CHECK(p.size == QSize(1100, 700));
}

TEST_CASE("a window left on a monitor that is gone opens on the one that is there", "[window-memory][placement]") {
    const State on_second = remembered(QRect(2200, 100, 1280, 800));

    SECTION("both monitors connected: it opens on the second") {
        const Placement p = choose_placement(on_second, {kPrimary, kRight}, kOwnSize, kHearthMinimum);
        REQUIRE(p.position.has_value());
        CHECK(*p.position == QPoint(2200, 100));
        CHECK(p.size == QSize(1280, 800));
    }
    SECTION("the second monitor unplugged: the position goes, the size stays") {
        const Placement p = choose_placement(on_second, {kPrimary}, kOwnSize, kHearthMinimum);
        CHECK_FALSE(p.position.has_value());
        CHECK(p.size == QSize(1280, 800));
    }
    SECTION("a monitor to the left, at a negative x, is a screen like another") {
        const Placement p = choose_placement(remembered(QRect(-1700, 40, 1280, 800)), {kPrimary, kLeft},
                                             kOwnSize, kHearthMinimum);
        REQUIRE(p.position.has_value());
        CHECK(*p.position == QPoint(-1700, 40));
        const Placement gone = choose_placement(remembered(QRect(-1700, 40, 1280, 800)), {kPrimary},
                                                kOwnSize, kHearthMinimum);
        CHECK_FALSE(gone.position.has_value());
    }
}

TEST_CASE("a window is kept where it was when its title is still there to take hold of",
          "[window-memory][placement]") {
    SECTION("most of it off the right edge, its left end on the screen") {
        const Placement p = choose_placement(remembered(QRect(1700, 100, 1100, 700)), {kPrimary}, kOwnSize,
                                             kHearthMinimum);
        REQUIRE(p.position.has_value());
        CHECK(*p.position == QPoint(1700, 100));
    }
    SECTION("only a few pixels of its left end on the screen: nobody can take hold of it") {
        const Placement p = choose_placement(remembered(QRect(1890, 100, 1100, 700)), {kPrimary}, kOwnSize,
                                             kHearthMinimum);
        CHECK_FALSE(p.position.has_value());
    }
    SECTION("its top under the task bar: the title is off the usable area") {
        // The client area starts at y 1030 on a screen whose usable area ends at 1040.
        const Placement p = choose_placement(remembered(QRect(100, 1030, 1100, 700)), {kPrimary}, kOwnSize,
                                             kHearthMinimum);
        CHECK_FALSE(p.position.has_value());
    }
    SECTION("its top above the screen, its title off it") {
        const Placement p = choose_placement(remembered(QRect(100, -60, 1100, 700)), {kPrimary}, kOwnSize,
                                             kHearthMinimum);
        CHECK_FALSE(p.position.has_value());
    }
}

TEST_CASE("a size below the program's minimum is raised to it", "[window-memory][placement]") {
    const Placement hearth =
        choose_placement(remembered(QRect(100, 100, 700, 400)), {kPrimary}, kOwnSize, kHearthMinimum);
    CHECK(hearth.size == QSize(960, 620));
    REQUIRE(hearth.position.has_value());

    const Placement forge =
        choose_placement(remembered(QRect(100, 100, 1100, 880)), {kPrimary}, QSize(1280, 900), kForgeMinimum);
    CHECK(forge.size == QSize(1280, 900));

    // Only one side below it is raised on that side alone.
    const Placement narrow =
        choose_placement(remembered(QRect(100, 100, 900, 700)), {kPrimary}, kOwnSize, kHearthMinimum);
    CHECK(narrow.size == QSize(960, 700));
}

TEST_CASE("a size larger than the screen is brought down to it, and never below the minimum",
          "[window-memory][placement]") {
    SECTION("larger than the monitor it was on") {
        const Placement p = choose_placement(remembered(QRect(0, 0, 3000, 2000)), {kPrimary}, kOwnSize,
                                             kHearthMinimum);
        REQUIRE(p.position.has_value());
        CHECK(p.size == QSize(1920, 1040));
    }
    SECTION("a smaller monitor than the one it was on") {
        const QRect small{0, 0, 1366, 728};
        const Placement p =
            choose_placement(remembered(QRect(100, 20, 1600, 900)), {small}, kOwnSize, kHearthMinimum);
        REQUIRE(p.position.has_value());
        CHECK(p.size == QSize(1366, 728));
    }
    SECTION("a monitor smaller than the minimum: the minimum is the program's, and wins") {
        const QRect tiny{0, 0, 1024, 728};
        const Placement p =
            choose_placement(remembered(QRect(0, 0, 1280, 900)), {tiny}, QSize(1280, 900), kForgeMinimum);
        CHECK(p.size == QSize(1280, 900));
    }
    SECTION("a position dropped for being off every screen is fitted to the primary one") {
        const Placement p = choose_placement(remembered(QRect(9000, 0, 3000, 2000)), {kPrimary, kRight},
                                             kOwnSize, kHearthMinimum);
        CHECK_FALSE(p.position.has_value());
        CHECK(p.size == QSize(1920, 1040));
    }
}

TEST_CASE("a window that was maximised opens maximised", "[window-memory][placement]") {
    SECTION("with the rectangle it goes back to") {
        const Placement p = choose_placement(remembered(QRect(200, 100, 1100, 700), true), {kPrimary},
                                             kOwnSize, kHearthMinimum);
        CHECK(p.maximised);
        REQUIRE(p.position.has_value());
        CHECK(*p.position == QPoint(200, 100));
        CHECK(p.size == QSize(1100, 700));
    }
    SECTION("with no rectangle: the window's own size, and no position") {
        const Placement p =
            choose_placement(State{std::nullopt, true}, {kPrimary}, kOwnSize, kHearthMinimum);
        CHECK(p.maximised);
        CHECK_FALSE(p.position.has_value());
        CHECK(p.size == kOwnSize);
    }
    SECTION("whose monitor is gone: still maximised, on the monitor there is") {
        const Placement p = choose_placement(remembered(QRect(2200, 100, 1100, 700), true), {kPrimary},
                                             kOwnSize, kHearthMinimum);
        CHECK(p.maximised);
        CHECK_FALSE(p.position.has_value());
    }
}

TEST_CASE("a position that cannot be checked against a screen is not used", "[window-memory][placement]") {
    const Placement p =
        choose_placement(remembered(QRect(100, 100, 1100, 700)), {}, kOwnSize, kHearthMinimum);
    CHECK_FALSE(p.position.has_value());
    CHECK(p.size == QSize(1100, 700));
}
