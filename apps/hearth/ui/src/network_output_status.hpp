#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "network_group_sink.hpp"

namespace iclforge::sendspin {
class Group;
}  // namespace iclforge::sendspin

// What NetworkController knows about each group, shared with HearthController
// without either holding a reference to the other's QObject
// (network_controller.hpp's own comment explains why they do not: two
// independent QML singletons, on the same poll rhythm for the same reason,
// with no natural owner to hand a reference between them). Both are
// QML_SINGLETON, instantiated by the QML engine itself with no constructor
// arguments to inject one through, so this is the third, shared piece of
// state the coupling needs instead - a Meyers singleton, not a QObject,
// deliberately outside either controller's own ownership.
//
// NetworkController's poll() replaces the whole table each tick
// (set_groups()), so a group that disappears (deleted, or the sink lost)
// drops out within one tick rather than being read stale. HearthController's
// own poll() reads it to keep OutputPreferences::group_ready current for
// whichever group is pinned as the output (hearth_controller.cpp's own
// selectOutputGroup()/poll()), and the resolver Player's own NetworkGroupSink
// is built with (hearth_controller.cpp's EngineOutputs) reads group() at each
// open() - always whatever NetworkController last published, never a value
// captured once.
//
// Locked: the two controllers run on the GUI thread, but that resolver runs on
// the engine's own thread, whenever the player opens its output - while the
// GUI thread may be replacing the table under it.

namespace iclforge::hearth::ui {

class NetworkOutputStatus {
public:
    static NetworkOutputStatus& instance() {
        static NetworkOutputStatus status;
        return status;
    }

    struct Entry {
        // At least one member exists and is connected right now
        // (network_view.hpp's own GroupMemberFacts::connected) - see this
        // class's own header comment for why this does not require every
        // member: a group with one of two sinks briefly disconnected can
        // still usefully play to the other, and Group::push()/push_burst()
        // already route to whichever members are actually there.
        bool ready = false;
        std::shared_ptr<sendspin::Group> group{};
    };

    // Called by NetworkController's poll() with its own status().groups,
    // keyed by GroupFacts::id (the id a caller resolves by - group_id(),
    // never a display name: NetworkSinks::group()'s own comment says why).
    void set_groups(std::map<std::string, Entry> groups) {
        std::map<std::string, Entry> old;
        {
            const std::lock_guard lock(mutex_);
            old = std::exchange(groups_, std::move(groups));
        }
        // The old table's Group references go here, outside the lock: the
        // last one to a deleted group runs ~Group(), which reaches the host.
    }

    // False, and null, for an id nothing has published - deleted, or never
    // created, or NetworkController has not started yet at all.
    [[nodiscard]] bool ready(const std::string& group_id) const {
        const std::lock_guard lock(mutex_);
        const auto found = groups_.find(group_id);
        return found != groups_.end() && found->second.ready;
    }
    [[nodiscard]] std::shared_ptr<sendspin::Group> group(const std::string& group_id) const {
        const std::lock_guard lock(mutex_);
        const auto found = groups_.find(group_id);
        return found != groups_.end() ? found->second.group : nullptr;
    }

    // What decides each member's form when a group opens (NetworkSinks::plan_group()), handed
    // across the same way the groups are: NetworkController sets it once its NetworkSinks
    // exists and clears it before that goes, and the engine's group sink calls plan() as the
    // group opens, on the engine's own thread. With none set, plan() answers nothing and the
    // group plays as it did before there was one.
    //
    // plan() runs the planner with the lock held, so clearing it waits for a plan in flight
    // and the NetworkSinks it reaches is never used after set_planner({}) returns.
    void set_planner(iclforge::hearth::MemberPlanner planner) {
        const std::lock_guard lock(mutex_);
        planner_ = std::move(planner);
    }
    [[nodiscard]] std::vector<iclforge::render::OutputLayout> plan(
        const iclforge::hearth::GroupPlanRequest& request) const {
        const std::lock_guard lock(mutex_);
        return planner_ ? planner_(request) : std::vector<iclforge::render::OutputLayout>{};
    }

private:
    NetworkOutputStatus() = default;

    mutable std::mutex mutex_;
    std::map<std::string, Entry> groups_;
    iclforge::hearth::MemberPlanner planner_;
};

}  // namespace iclforge::hearth::ui
