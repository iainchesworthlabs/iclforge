// The media sessions of macOS (apps/crucible/ui/src/media_session.hpp): the system's now-playing
// information, which is MediaRemote.framework's. That framework is PRIVATE: it is resolved here at
// run time with dlopen, so that nothing links against it and a system without it, or one that has
// withdrawn it, is a reader that says so and not a program that does not start.
//
// Two things about it are the owner's to weigh (planning/karaoke-phase2-findings.md):
//   - it is not a documented interface, and developers report that macOS 15.4 and later returns
//     nothing for a program Apple has not entitled, which is what an unsigned or ordinary
//     developer-signed Crucible is. Then read() is empty and support() says so: the strip keeps
//     Start and the nudge.
//   - the one now-playing client is the whole of it: macOS has a single "now playing" application,
//     not a list, so at most one session is returned.
// NO MAC HAS EXECUTED THIS FILE (the same status as apps/crucible/engine/src/platform/macos/):
// CI builds it; the key names and the callback's shape are from general knowledge of the framework
// and are to be checked on a Mac before anyone relies on them.
//
// The callbacks run on a global queue and hand back what they read as plain C++ values, so that no
// Objective-C object outlives its block (this target is compiled without ARC).

#include "media_session.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace iclforge::crucible::ui {

namespace {

constexpr const char* kFrameworkPath = "/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote";

using GetNowPlayingInfo = void (*)(dispatch_queue_t, void (^)(CFDictionaryRef));
using GetNowPlayingPid = void (*)(dispatch_queue_t, void (^)(int));

[[nodiscard]] std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string to_std(NSString* text) {
    return text != nil ? std::string([text UTF8String]) : std::string{};
}

// A callback's answer arriving: waited for with a timeout, and shared with the block so that it
// outlives a read that gave up (no dispatch object is created or released here).
struct Arrival {
    std::mutex mutex;
    std::condition_variable arrived;
    bool done = false;

    void signal() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        arrived.notify_all();
    }

    [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return arrived.wait_for(lock, timeout, [this] { return done; });
    }
};

// What one read of the now-playing dictionary held, as values.
struct Raw {
    bool any = false;
    std::string title;
    std::string artist;
    bool has_duration = false;
    double duration_s = 0.0;
    bool has_elapsed = false;
    double elapsed_s = 0.0;
    bool has_rate = false;
    double rate = 0.0;
    // Seconds since the elapsed time was true, when the dictionary said.
    double age_s = 0.0;
};

class NowPlaying final : public MediaSessions {
public:
    NowPlaying() {
        handle_ = dlopen(kFrameworkPath, RTLD_LAZY);
        if (handle_ != nullptr) {
            info_ = reinterpret_cast<GetNowPlayingInfo>(dlsym(handle_, "MRMediaRemoteGetNowPlayingInfo"));
            pid_ = reinterpret_cast<GetNowPlayingPid>(dlsym(handle_, "MRMediaRemoteGetNowPlayingApplicationPID"));
        }
    }

    ~NowPlaying() override {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    [[nodiscard]] std::vector<MediaSession> read() override {
        std::vector<MediaSession> out;
        if (info_ == nullptr) {
            return out;
        }
        @autoreleasepool {
            const auto raw = std::make_shared<Raw>();
            const auto arrival = std::make_shared<Arrival>();
            info_(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(CFDictionaryRef dictionary) {
                if (dictionary != nullptr && CFDictionaryGetCount(dictionary) > 0) {
                    NSDictionary* info = (__bridge NSDictionary*)dictionary;
                    raw->any = true;
                    raw->title = to_std(info[@"kMRMediaRemoteNowPlayingInfoTitle"]);
                    raw->artist = to_std(info[@"kMRMediaRemoteNowPlayingInfoArtist"]);
                    NSNumber* duration = info[@"kMRMediaRemoteNowPlayingInfoDuration"];
                    NSNumber* elapsed = info[@"kMRMediaRemoteNowPlayingInfoElapsedTime"];
                    NSNumber* rate = info[@"kMRMediaRemoteNowPlayingInfoPlaybackRate"];
                    NSDate* stamp = info[@"kMRMediaRemoteNowPlayingInfoTimestamp"];
                    if (duration != nil) {
                        raw->has_duration = true;
                        raw->duration_s = [duration doubleValue];
                    }
                    if (elapsed != nil) {
                        raw->has_elapsed = true;
                        raw->elapsed_s = [elapsed doubleValue];
                    }
                    if (rate != nil) {
                        raw->has_rate = true;
                        raw->rate = [rate doubleValue];
                    }
                    if (stamp != nil) {
                        raw->age_s = -[stamp timeIntervalSinceNow];
                    }
                }
                arrival->signal();
            });
            // The framework answers at once or not at all: a second is the longest a poll waits.
            const bool answered = arrival->wait_for(std::chrono::seconds(1));
            if (!answered || !raw->any) {
                empty_reads_.fetch_add(1);
                return out;
            }
            empty_reads_.store(0);

            MediaSession session;
            session.title = raw->title;
            session.artist = raw->artist;
            if (raw->has_duration) {
                session.duration_ms = static_cast<std::int64_t>(raw->duration_s * 1000.0);
            }
            const bool playing = raw->has_rate && raw->rate > 0.0;
            session.rate = playing ? raw->rate : 1.0;
            session.playback = playing ? MediaPlayback::kPlaying : MediaPlayback::kPaused;
            if (raw->has_elapsed) {
                session.has_position = true;
                session.position_ms = static_cast<std::int64_t>(raw->elapsed_s * 1000.0);
                // The elapsed time is true at the time stamp, which may be earlier than now.
                const double age = raw->age_s > 0.0 ? raw->age_s : 0.0;
                session.read_at_ms = steady_now_ms() - static_cast<std::int64_t>(age * 1000.0);
            }

            // Which application: the pid of the now-playing client, then its bundle and name.
            if (pid_ != nullptr) {
                const auto pid = std::make_shared<int>(0);
                const auto got = std::make_shared<Arrival>();
                pid_(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(int value) {
                    *pid = value;
                    got->signal();
                });
                const bool have_pid = got->wait_for(std::chrono::seconds(1));
                if (have_pid && *pid > 0) {
                    session.pid = static_cast<std::uint32_t>(*pid);
                    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:*pid];
                    if (app != nil) {
                        session.id = to_std([app bundleIdentifier]);
                        session.name = to_std([app localizedName]);
                        session.desktop_id = session.id;
                        session.executable = to_std([[[app executableURL] lastPathComponent] lowercaseString]);
                    }
                }
            }
            if (session.id.empty()) {
                session.id = "now-playing";
            }
            out.push_back(std::move(session));
        }
        return out;
    }

    [[nodiscard]] MediaSessionSupport support() const override {
        if (info_ == nullptr) {
            return {false, "This version of macOS has no now-playing information that a program can read."};
        }
        // Several empty answers in a row are what a withheld framework looks like, and also what an
        // idle machine looks like: say which it may be, not that it is.
        if (empty_reads_.load() >= 8) {
            return {false,
                    "macOS gave no now-playing information. Nothing may be playing; or this version of macOS "
                    "(15.4 and later) withholds it from programs Apple has not entitled."};
        }
        return {true, {}};
    }

private:
    void* handle_ = nullptr;
    GetNowPlayingInfo info_ = nullptr;
    GetNowPlayingPid pid_ = nullptr;
    mutable std::atomic<int> empty_reads_{0};
};

}  // namespace

std::shared_ptr<MediaSessions> platform_media_sessions() {
    return std::make_shared<NowPlaying>();
}

}  // namespace iclforge::crucible::ui
