// No screen: a build for a board without one. See ../panel.hpp.

#include "../panel.hpp"

namespace player {

bool panel_built() { return false; }

bool panel_start() { return false; }

PanelStats panel_stats() { return {}; }

}  // namespace player
