// The media sessions of a Linux build whose Qt has no D-Bus module (apps/crucible/ui/src/
// media_session.hpp): none can be read, and the reader says so, so that the lyric strip falls back to
// Start and the nudge and explains why. The other half of "exactly one of two readers is
// compiled" with media_sessions.cpp, chosen by whether Qt6::DBus was found.

#include "media_session.hpp"

#include <memory>
#include <vector>

namespace iclforge::crucible::ui {

namespace {

class NoMediaSessions final : public MediaSessions {
public:
    [[nodiscard]] std::vector<MediaSession> read() override { return {}; }

    [[nodiscard]] MediaSessionSupport support() const override {
        return {false, "This build of Crucible was made without Qt's D-Bus module, so it cannot read what "
                       "other players are playing."};
    }
};

}  // namespace

std::shared_ptr<MediaSessions> platform_media_sessions() {
    return std::make_shared<NoMediaSessions>();
}

}  // namespace iclforge::crucible::ui
