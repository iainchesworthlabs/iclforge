#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "media_session.hpp"

// apps/crucible/ui/src/media_session.cpp: what a media session implies and whose it is
// (planning/karaoke-phase2.md, D9). The platform readers are held where they run (a real session,
// by hand, and the QML suite with a scripted snapshot); this holds the arithmetic and the matching.

namespace {

using iclforge::crucible::ui::best_session;
using iclforge::crucible::ui::executable_of_app_user_model_id;
using iclforge::crucible::ui::executable_of_mpris_bus_name;
using iclforge::crucible::ui::match_score;
using iclforge::crucible::ui::MediaApplication;
using iclforge::crucible::ui::MediaPlayback;
using iclforge::crucible::ui::MediaSession;
using iclforge::crucible::ui::position_at;
using iclforge::crucible::ui::track_key;

MediaSession playing(std::int64_t position_ms, std::int64_t read_at_ms) {
    MediaSession session;
    session.id = "player";
    session.has_position = true;
    session.position_ms = position_ms;
    session.read_at_ms = read_at_ms;
    session.playback = MediaPlayback::kPlaying;
    return session;
}

}  // namespace

TEST_CASE("media session: a playing session's position is carried forward at its rate",
          "[crucible][media-session]") {
    MediaSession session = playing(10000, 5000);
    CHECK(position_at(session, 5000).value() == 10000);
    CHECK(position_at(session, 7250).value() == 12250);
    // Half speed.
    session.rate = 0.5;
    CHECK(position_at(session, 7250).value() == 11125);
    // A rate that is zero, negative or not a number is real time: a session that says "playing"
    // is not held still by a bad rate.
    session.rate = 0.0;
    CHECK(position_at(session, 7000).value() == 12000);
    session.rate = -1.0;
    CHECK(position_at(session, 7000).value() == 12000);
    session.rate = std::numeric_limits<double>::quiet_NaN();
    CHECK(position_at(session, 7000).value() == 12000);
}

TEST_CASE("media session: a paused or stopped session holds its position, and a clock behind the report is the report",
          "[crucible][media-session]") {
    MediaSession session = playing(10000, 5000);
    session.playback = MediaPlayback::kPaused;
    CHECK(position_at(session, 9000).value() == 10000);
    session.playback = MediaPlayback::kStopped;
    CHECK(position_at(session, 9000).value() == 10000);
    // Playing, with a now that is before the report: no position earlier than the one reported.
    session = playing(10000, 5000);
    CHECK(position_at(session, 4000).value() == 10000);
}

TEST_CASE("media session: the position is held to the track's length and never negative",
          "[crucible][media-session]") {
    MediaSession session = playing(179000, 0);
    session.duration_ms = 180000;
    CHECK(position_at(session, 500).value() == 179500);
    CHECK(position_at(session, 60000).value() == 180000);
    // No length published: not clamped above.
    session.duration_ms = 0;
    CHECK(position_at(session, 60000).value() == 239000);
    session = playing(-50, 0);
    CHECK(position_at(session, 0).value() == 0);
}

TEST_CASE("media session: a session with no timeline has no position", "[crucible][media-session]") {
    MediaSession session = playing(1000, 0);
    session.has_position = false;
    CHECK_FALSE(position_at(session, 5000).has_value());
}

TEST_CASE("media session: a program's name is read out of a session's id", "[crucible][media-session]") {
    CHECK(executable_of_app_user_model_id("Spotify.exe") == "spotify");
    CHECK(executable_of_app_user_model_id("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") == "spotify");
    CHECK(executable_of_app_user_model_id("C:\\Program Files\\VideoLAN\\VLC\\vlc.exe") == "vlc");
    CHECK(executable_of_app_user_model_id("Chrome") == "chrome");
    CHECK(executable_of_app_user_model_id("").empty());

    CHECK(executable_of_mpris_bus_name("org.mpris.MediaPlayer2.vlc") == "vlc");
    CHECK(executable_of_mpris_bus_name("org.mpris.MediaPlayer2.vlc.instance7389") == "vlc");
    CHECK(executable_of_mpris_bus_name("org.mpris.MediaPlayer2.Spotify") == "spotify");
    // A name that is not in the standard form is kept whole.
    CHECK(executable_of_mpris_bus_name("com.example.Player") == "com.example.player");
}

TEST_CASE("media session: a track is told by its title, artist and length", "[crucible][media-session]") {
    MediaSession a;
    a.title = "Auld Lang Syne";
    a.artist = "Anon";
    a.duration_ms = 180000;
    MediaSession b = a;
    CHECK(track_key(a) == track_key(b));
    b.title = "Another";
    CHECK(track_key(a) != track_key(b));
    b = a;
    b.duration_ms = 181000;
    CHECK(track_key(a) != track_key(b));
    // A session that publishes none of the three has no key, so nothing is told apart by it.
    CHECK(track_key(MediaSession{}).empty());
}

TEST_CASE("media session: an application is matched by its identifiers before its names",
          "[crucible][media-session]") {
    MediaSession spotify;
    spotify.id = "Spotify.exe";
    spotify.name = "Spotify";
    spotify.executable = "spotify";

    // The executable's path against the session's id: case and ".exe" do not matter.
    MediaApplication app{"Spotify", "C:\\Users\\a\\AppData\\Roaming\\Spotify\\Spotify.exe", ""};
    CHECK(match_score(app, spotify) >= 90);
    app.image_path = "c:/programs/SPOTIFY.EXE";
    CHECK(match_score(app, spotify) >= 90);

    // A packaged application: its own id.
    MediaSession packaged;
    packaged.id = "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify";
    MediaApplication store{"Spotify", "C:\\Program Files\\WindowsApps\\SpotifyAB\\Spotify.exe",
                           "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"};
    CHECK(match_score(store, packaged) == 100);
    // Without the id, the part after the bang names the program.
    store.app_id.clear();
    CHECK(match_score(store, packaged) >= 90);

    // Linux: /usr/bin/vlc against org.mpris.MediaPlayer2.vlc, and a desktop entry's trailing part.
    MediaSession vlc;
    vlc.id = "org.mpris.MediaPlayer2.vlc";
    vlc.executable = "vlc";
    vlc.desktop_id = "org.videolan.VLC";
    MediaApplication player{"VLC media player", "/usr/bin/vlc", ""};
    CHECK(match_score(player, vlc) >= 90);
    vlc.executable.clear();
    CHECK(match_score(player, vlc) >= 80);
    // A sandboxed application's id is the desktop entry.
    player.app_id = "org.videolan.VLC";
    CHECK(match_score(player, vlc) == 100);

    // Names: what the room shows against what the player calls itself.
    MediaSession named;
    named.id = "x";
    named.name = "Amberol";
    CHECK(match_score(MediaApplication{"amberol", "", ""}, named) >= 60);
}

TEST_CASE("media session: unrelated applications do not match, and a short name is not an accident's match",
          "[crucible][media-session]") {
    MediaSession spotify;
    spotify.id = "Spotify.exe";
    spotify.name = "Spotify";
    spotify.executable = "spotify";

    CHECK(match_score(MediaApplication{"Firefox", "C:\\Program Files\\Mozilla Firefox\\firefox.exe", ""}, spotify) == 0);
    CHECK(match_score(MediaApplication{}, spotify) == 0);
    // A three-letter name that is only contained in another is not a match.
    MediaSession longer;
    longer.id = "z";
    longer.name = "Amazon Music";
    CHECK(match_score(MediaApplication{"maz", "", ""}, longer) == 0);
}

TEST_CASE("media session: the best session is the highest score, a playing one before a paused one",
          "[crucible][media-session]") {
    MediaSession paused;
    paused.id = "Spotify.exe";
    paused.executable = "spotify";
    paused.playback = MediaPlayback::kPaused;
    MediaSession live = paused;
    live.playback = MediaPlayback::kPlaying;
    MediaSession other;
    other.id = "chrome.exe";
    other.executable = "chrome";

    const MediaApplication app{"Spotify", "C:\\x\\Spotify.exe", ""};
    // Nothing matches.
    CHECK_FALSE(best_session(app, {other}).has_value());
    CHECK_FALSE(best_session(app, {}).has_value());
    // Equal scores: the playing one wins whichever comes first.
    CHECK(best_session(app, {other, paused, live}).value() == 2);
    CHECK(best_session(app, {live, paused}).value() == 0);
    // Equal and equally paused: the first.
    CHECK(best_session(app, {paused, paused}).value() == 0);
    // A better score beats a playing one: an identifier match over a name match.
    MediaSession by_name;
    by_name.id = "other-id";
    by_name.name = "Spotify";
    by_name.playback = MediaPlayback::kPlaying;
    CHECK(best_session(app, {by_name, paused}).value() == 1);
}
