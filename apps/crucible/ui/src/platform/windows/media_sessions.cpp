// The media sessions of Windows (apps/crucible/ui/src/media_session.hpp): the System Media Transport
// Controls' global session manager, which every player that shows its title in the volume flyout
// publishes to. Windows 10, version 1809 and later (Windows.Media.Control).
//
// Read with C++/WinRT, on the thread read() is called from: the calls are inter-process and
// synchronous here, and the window never makes them. The session names its source by application
// user model id and gives no process id, so an application is matched to it by name
// (media_session.cpp). The timeline's position is "current as of LastUpdatedTime" (UTC); that age
// is taken off the steady clock's reading, so that position_at() can carry it forward.

#include "media_session.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace iclforge::crucible::ui {

namespace {

namespace control = winrt::Windows::Media::Control;

[[nodiscard]] std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::int64_t to_ms(winrt::Windows::Foundation::TimeSpan span) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
}

// This thread's apartment, once. A thread that is already in one (a different mode) is left in it:
// the calls below work in either.
void ensure_apartment() {
    thread_local bool done = false;
    if (done) {
        return;
    }
    done = true;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
    }
}

class SystemTransportControls final : public MediaSessions {
public:
    [[nodiscard]] std::vector<MediaSession> read() override {
        std::vector<MediaSession> out;
        try {
            ensure_apartment();
            if (!manager_) {
                manager_ = control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            }
            const std::int64_t now = steady_now_ms();
            const auto utc_now = winrt::clock::now();
            for (const control::GlobalSystemMediaTransportControlsSession& session : manager_.GetSessions()) {
                try {
                    out.push_back(read_one(session, now, utc_now));
                } catch (const winrt::hresult_error&) {
                    // A session that closed between listing and reading is not one.
                }
            }
        } catch (const winrt::hresult_error&) {
            // The manager could not be had (the service is not running, or the API is absent):
            // nothing to read; ask again next time.
            manager_ = nullptr;
        } catch (const std::exception&) {
            manager_ = nullptr;
        }
        return out;
    }

    [[nodiscard]] MediaSessionSupport support() const override {
        MediaSessionSupport out;
        try {
            out.available = winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
                L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager");
        } catch (const winrt::hresult_error&) {
            out.available = false;
        }
        if (!out.available) {
            out.reason = "This version of Windows has no global media session manager (it needs Windows 10, "
                         "version 1809 or later).";
        }
        return out;
    }

private:
    [[nodiscard]] static MediaSession read_one(const control::GlobalSystemMediaTransportControlsSession& session,
                                               std::int64_t now_ms,
                                               winrt::clock::time_point utc_now) {
        MediaSession out;
        out.id = winrt::to_string(session.SourceAppUserModelId());
        out.executable = executable_of_app_user_model_id(out.id);

        const auto playback = session.GetPlaybackInfo();
        using Status = control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
        switch (playback.PlaybackStatus()) {
            case Status::Playing:
                out.playback = MediaPlayback::kPlaying;
                break;
            case Status::Paused:
                out.playback = MediaPlayback::kPaused;
                break;
            default:
                out.playback = MediaPlayback::kStopped;
                break;
        }
        if (const auto rate = playback.PlaybackRate()) {
            out.rate = rate.Value();
        }

        const auto timeline = session.GetTimelineProperties();
        const std::int64_t start = to_ms(timeline.StartTime());
        const std::int64_t end = to_ms(timeline.EndTime());
        out.duration_ms = end > start ? end - start : 0;
        // A session that never reported a timeline has an end time of 0 and a last update at the
        // epoch; a position is then not something it said.
        const auto updated = timeline.LastUpdatedTime();
        out.has_position = updated.time_since_epoch().count() != 0 && (end > start || to_ms(timeline.Position()) > 0);
        out.position_ms = std::max<std::int64_t>(0, to_ms(timeline.Position()) - start);
        // The age of the report, taken off the steady clock's reading; a clock that has the update
        // in the future (a player's own clock) is no age.
        std::int64_t age_ms = 0;
        if (out.has_position) {
            age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(utc_now - updated).count();
            age_ms = std::clamp<std::int64_t>(age_ms, 0, 24LL * 60 * 60 * 1000);
        }
        out.read_at_ms = now_ms - age_ms;

        if (const auto props = session.TryGetMediaPropertiesAsync().get()) {
            out.title = winrt::to_string(props.Title());
            out.artist = winrt::to_string(props.Artist());
        }
        return out;
    }

    control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
};

}  // namespace

std::shared_ptr<MediaSessions> platform_media_sessions() {
    return std::make_shared<SystemTransportControls>();
}

}  // namespace iclforge::crucible::ui
