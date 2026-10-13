#include "system_motion_query.hpp"

#include <windows.h>

namespace iclforge::theme {

bool query_system_reduced_motion() {
    // Settings > Accessibility > Visual effects > Animation effects. The
    // call reports whether client-area animation is ON, so reduced motion is
    // its absence; a failed call is "not reduced".
    BOOL animations = TRUE;
    if (::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) == 0) {
        return false;
    }
    return animations == FALSE;
}

}  // namespace iclforge::theme
