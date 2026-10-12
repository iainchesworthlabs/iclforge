#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The operating system's media sessions (planning/karaoke-phase2.md, D9, and
// planning/karaoke-phase2-findings.md, "The media-session back ends"): where a song a player is
// playing is, as the player has told the system, so that Crucible's lyric strip can follow an
// application without a person pressing Start. Windows publishes it through the System Media
// Transport Controls, Linux through MPRIS on the D-Bus session bus, macOS through the now-playing
// information; each is one platform file under ui/src/platform/<os>/media_sessions.*, selected by
// CMake, and this header is what they share.
//
// Everything here is Qt-free, so that it is held by plain Catch2 cases (test_media_session.cpp):
// the session as read, the position it implies at a later time, and which session an application's
// is. Reading the sessions is an inter-process call and happens on a thread of its own
// (media_monitor.hpp); nothing here blocks except MediaSessions::read().

namespace iclforge::crucible::ui {

enum class MediaPlayback : std::uint8_t {
    kStopped,
    kPaused,
    kPlaying,
};

// One application's session, as read at one instant.
struct MediaSession {
    // The system's own name for the session's source: a Windows application user model id
    // ("Spotify.exe", or "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify" for a packaged one), an
    // MPRIS bus name ("org.mpris.MediaPlayer2.vlc"), a macOS bundle identifier. What a person
    // picks from a list and what a chosen session is held by.
    std::string id;
    // What a person calls it ("Spotify"), when the platform says; empty otherwise.
    std::string name;
    // The program's own name, lower case, with no directory and no extension, as far as the
    // platform lets it be told: "spotify", "vlc". For matching an application.
    std::string executable;
    // The desktop entry's basename where there is one (MPRIS DesktopEntry: "org.videolan.VLC").
    std::string desktop_id;
    // The process id, where the platform says (MPRIS has it from the bus; the Windows transport
    // controls have none). 0: not said.
    std::uint32_t pid = 0;

    std::string title;
    std::string artist;
    // The track's length; 0 when the player does not publish one.
    std::int64_t duration_ms = 0;

    // Whether the session publishes a timeline at all: a live stream or a player that does not
    // report one has a state and no position, and cannot be followed.
    bool has_position = false;
    // Where the track was at `read_at_ms`.
    std::int64_t position_ms = 0;
    // This process's steady clock, in milliseconds, at the moment `position_ms` was true: the
    // platform has already taken the player's own report time into account (the Windows timeline
    // says when it was last updated), so a later position is position_ms plus the time since.
    std::int64_t read_at_ms = 0;
    // The playback rate: 1 is real time, 0.5 half speed.
    double rate = 1.0;
    MediaPlayback playback = MediaPlayback::kStopped;
};

// Where the track is at `now_ms` (the same steady clock as read_at_ms): the reported position,
// carried forward at the rate while it is playing and held while it is paused or stopped, never
// before 0 and never past the track's length where it is known. nullopt for a session with no
// timeline. A `now_ms` earlier than the report is the report, not a position before it.
[[nodiscard]] std::optional<std::int64_t> position_at(const MediaSession& session, std::int64_t now_ms);

// The program's own name, lower case, from a Windows application user model id: the part after
// the last '!' of a packaged application's ("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify" is
// "spotify"), the file name without ".exe" otherwise ("Spotify.exe" is "spotify"). What a
// platform puts in MediaSession::executable, so that an application is matched by it.
[[nodiscard]] std::string executable_of_app_user_model_id(std::string_view id);

// The same from an MPRIS bus name: the part after "org.mpris.MediaPlayer2." without a trailing
// ".instance<pid>" ("org.mpris.MediaPlayer2.vlc.instance7389" is "vlc"), lower case.
[[nodiscard]] std::string executable_of_mpris_bus_name(std::string_view bus_name);

// What makes a track this track and not another: title, artist and length. A new track changes
// it, so lyrics imported for one are not followed against the next. Empty for a session that
// publishes none of the three.
[[nodiscard]] std::string track_key(const MediaSession& session);

// An application of the room, as the facts a session is matched by.
struct MediaApplication {
    // The name the room shows ("Spotify").
    std::string name;
    // The executable's path as the engine read it (a Windows path, a Linux /proc/<pid>/exe).
    std::string image_path;
    // The platform's application id where it has one (a packaged Windows application's user
    // model id, a Linux desktop-entry id), or empty.
    std::string app_id;
};

// How well `session` is `application`'s, 0 for not at all. Strong where an identifier is the same
// (the application id against the session's id or desktop entry, the executable's name against
// the session's), weaker for the displayed names; a name that merely contains another counts only
// when it is long enough not to be an accident. Case and a trailing ".exe" do not matter.
[[nodiscard]] int match_score(const MediaApplication& application, const MediaSession& session);

// The session that is `application`'s: the best score, a playing one before a paused one at the
// same score, then the first. nullopt when nothing scores. An index into `sessions`.
[[nodiscard]] std::optional<std::size_t> best_session(const MediaApplication& application,
                                                       const std::vector<MediaSession>& sessions);

// What a platform says about its own ability to read sessions, as Foreground::support() does for
// the full-screen rule: one line the window can show, rather than silence that reads as "nothing
// is playing".
struct MediaSessionSupport {
    bool available = false;
    // Empty when available.
    std::string reason;
};

class MediaSessions {
public:
    virtual ~MediaSessions() = default;

    // Every session the system has now. An inter-process call that may take tens of milliseconds:
    // never from the window's thread. Empty when there are none, and when the platform cannot
    // say (support() tells which).
    [[nodiscard]] virtual std::vector<MediaSession> read() = 0;
    [[nodiscard]] virtual MediaSessionSupport support() const = 0;
};

// The platform's reader: exactly one platform/<os>/media_sessions.* defines it, chosen by CMake.
// Never null; a platform that cannot read sessions returns an object that says so.
[[nodiscard]] std::shared_ptr<MediaSessions> platform_media_sessions();

}  // namespace iclforge::crucible::ui
