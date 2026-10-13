#include "system_motion_query.hpp"

namespace iclforge::theme {

// A platform this program has no way to ask - the BSDs, Haiku - answers "not
// reduced". The person's own Settings > Appearance > Motion choice still
// applies there.
bool query_system_reduced_motion() {
    return false;
}

}  // namespace iclforge::theme
