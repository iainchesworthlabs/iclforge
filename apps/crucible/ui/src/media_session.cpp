#include "media_session.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace iclforge::crucible::ui {

namespace {

// ASCII lower case: an application name and an identifier are compared, not collated.
[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The file name of a path in either separator, with a trailing ".exe" taken off and lower-cased: the
// program's own name as a session id or an image path spells it.
[[nodiscard]] std::string program_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = lower(slash == std::string_view::npos ? path : path.substr(slash + 1));
    constexpr std::string_view kExe = ".exe";
    if (name.size() > kExe.size() && name.compare(name.size() - kExe.size(), kExe.size(), kExe) == 0) {
        name.resize(name.size() - kExe.size());
    }
    return name;
}

// A packaged application's user model id is "<package family>!<application>": the application is
// what names it.
[[nodiscard]] std::string id_name(std::string_view id) {
    const std::size_t bang = id.find_last_of('!');
    return program_name(bang == std::string_view::npos ? id : id.substr(bang + 1));
}

// A name too short to be told from an accident when it is only contained in another.
constexpr std::size_t kMinContainedName = 4;

[[nodiscard]] bool contains_name(const std::string& haystack, const std::string& needle) {
    return needle.size() >= kMinContainedName && haystack.find(needle) != std::string::npos;
}

}  // namespace

std::string executable_of_app_user_model_id(std::string_view id) {
    return id_name(id);
}

std::string executable_of_mpris_bus_name(std::string_view bus_name) {
    constexpr std::string_view kPrefix = "org.mpris.MediaPlayer2.";
    if (bus_name.substr(0, kPrefix.size()) == kPrefix) {
        bus_name.remove_prefix(kPrefix.size());
    }
    // A second instance of a player is "<name>.instance<id>".
    const std::size_t instance = bus_name.rfind(".instance");
    if (instance != std::string_view::npos && instance > 0) {
        bus_name = bus_name.substr(0, instance);
    }
    return lower(bus_name);
}

std::optional<std::int64_t> position_at(const MediaSession& session, std::int64_t now_ms) {
    if (!session.has_position) {
        return std::nullopt;
    }
    double position = static_cast<double>(session.position_ms);
    if (session.playback == MediaPlayback::kPlaying) {
        const std::int64_t elapsed = std::max<std::int64_t>(0, now_ms - session.read_at_ms);
        const double rate = std::isfinite(session.rate) && session.rate > 0.0 ? session.rate : 1.0;
        position += static_cast<double>(elapsed) * rate;
    }
    std::int64_t out = static_cast<std::int64_t>(std::llround(position));
    if (session.duration_ms > 0) {
        out = std::min(out, session.duration_ms);
    }
    return std::max<std::int64_t>(0, out);
}

std::string track_key(const MediaSession& session) {
    if (session.title.empty() && session.artist.empty() && session.duration_ms <= 0) {
        return {};
    }
    return session.title + "\n" + session.artist + "\n" + std::to_string(session.duration_ms);
}

int match_score(const MediaApplication& application, const MediaSession& session) {
    const std::string app_id = lower(application.app_id);
    const std::string image = program_name(application.image_path);
    const std::string name = lower(application.name);
    const std::string session_id = lower(session.id);
    const std::string session_desktop = lower(session.desktop_id);
    const std::string session_exe = lower(session.executable);
    const std::string session_name = lower(session.name);

    int best = 0;
    // The application's own id, which is what a packaged Windows application and a sandboxed Linux
    // one are known by.
    if (!app_id.empty() && (app_id == session_id || app_id == session_desktop)) {
        best = std::max(best, 100);
    }
    // The executable's name: "Spotify.exe" against a session id of "Spotify.exe", /usr/bin/vlc
    // against "org.mpris.MediaPlayer2.vlc" (the platform puts "vlc" in `executable`).
    if (!image.empty()) {
        if (image == session_exe || image == program_name(session.id) || image == id_name(session.id)) {
            best = std::max(best, 90);
        } else if (!session_desktop.empty() && contains_name(session_desktop, image)) {
            best = std::max(best, 50);
        }
    }
    // A desktop entry against the application's id by a trailing part: "org.videolan.VLC" is "vlc".
    if (!session_desktop.empty() && !image.empty() && session_desktop.size() > image.size()
        && session_desktop.compare(session_desktop.size() - image.size(), image.size(), image) == 0
        && session_desktop[session_desktop.size() - image.size() - 1] == '.') {
        best = std::max(best, 80);
    }
    // What the room shows against what the player calls itself.
    if (!name.empty() && !session_name.empty() && name == session_name) {
        best = std::max(best, 60);
    }
    if (!name.empty() && !session_name.empty()
        && (contains_name(session_name, name) || contains_name(name, session_name))) {
        best = std::max(best, 30);
    }
    // The room's name against the session's identifier or executable.
    if (!name.empty() && (name == session_exe || name == id_name(session.id))) {
        best = std::max(best, 40);
    }
    return best;
}

std::optional<std::size_t> best_session(const MediaApplication& application,
                                        const std::vector<MediaSession>& sessions) {
    std::optional<std::size_t> pick;
    int best_score = 0;
    bool best_playing = false;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        const int score = match_score(application, sessions[i]);
        if (score <= 0) {
            continue;
        }
        const bool playing = sessions[i].playback == MediaPlayback::kPlaying;
        if (!pick || score > best_score || (score == best_score && playing && !best_playing)) {
            pick = i;
            best_score = score;
            best_playing = playing;
        }
    }
    return pick;
}

}  // namespace iclforge::crucible::ui
