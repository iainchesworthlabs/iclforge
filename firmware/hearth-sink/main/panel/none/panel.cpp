// No screen: a build for a board without one. See ../panel.hpp.

#include "../panel.hpp"

namespace player {

bool panel_built() { return false; }

bool panel_has_view(iclforge::nowview::View /*view*/) { return false; }

void panel_set_view(iclforge::nowview::View /*view*/) {}

void panel_set_dim_after(int /*minutes*/) {}

bool panel_start() { return false; }

PanelStats panel_stats() { return {}; }

iclforge::DisplayCaps panel_display_caps() { return {}; }

iclforge::devview::Facts panel_board_facts() { return {}; }

}  // namespace player
