#pragma once

namespace iclforge::theme {

// Whether the operating system has been asked to cut back animation. One
// definition per platform, in platform/<os>/system_motion_query.cpp, chosen
// by cmake/SystemMotion.cmake: Windows asks SPI_GETCLIENTAREAANIMATION, macOS
// NSWorkspace's accessibilityDisplayShouldReduceMotion, Linux the desktop's
// own animations switch (GNOME's enable-animations, KDE's
// AnimationDurationFactor). A platform with no such setting, or one that
// cannot be read, answers false - "not reduced" - never a guess.
//
// Cheap enough to call when the window is activated; not for a per-frame
// loop (the Linux answer starts gsettings).
[[nodiscard]] bool query_system_reduced_motion();

}  // namespace iclforge::theme
