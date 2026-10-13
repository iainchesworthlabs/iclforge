#include "system_motion_query.hpp"

#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

namespace iclforge::theme {

// System Settings > Accessibility > Display > Reduce motion, which AppKit
// answers as -[NSWorkspace accessibilityDisplayShouldReduceMotion].
//
// Asked through the Objective-C runtime from a plain C++ file, not from a
// .mm: apps/crucible enables the OBJCXX language for its own .mm files, and
// asking forge-gui and Hearth to do the same for one boolean would put a
// language setup (the standard, the assert-macro define) into two more
// directories that nobody here can build without a Mac. NSWorkspace is in
// AppKit, which every Qt GUI program on macOS has loaded; if the class were
// ever missing the answer is "not reduced".
//
// **Not built on the machine this was written on.** The runtime calls are
// the documented ones and small, but the macOS leg is the first compile.
bool query_system_reduced_motion() {
    using Message = id (*)(id, SEL);
    using MessageBool = BOOL (*)(id, SEL);

    const id workspace_class = reinterpret_cast<id>(objc_getClass("NSWorkspace"));
    if (workspace_class == nullptr) {
        return false;
    }
    const id workspace =
        reinterpret_cast<Message>(objc_msgSend)(workspace_class, sel_registerName("sharedWorkspace"));
    if (workspace == nullptr) {
        return false;
    }
    return reinterpret_cast<MessageBool>(objc_msgSend)(
               workspace, sel_registerName("accessibilityDisplayShouldReduceMotion")) != 0;
}

}  // namespace iclforge::theme
