// No screen: a build for a board without one. See ../panel.hpp.

#include "../panel.hpp"

namespace player {

bool panel_built() { return false; }

bool panel_has_view(iclforge::nowview::View /*view*/) { return false; }

void panel_set_view(iclforge::nowview::View /*view*/) {}

void panel_set_dim_after(int /*minutes*/) {}

void panel_set_speakers_style(iclforge::nowview::SpeakersStyle /*style*/) {}
void panel_set_brightness(int /*percent*/) {}

void panel_set_clock_format(iclforge::wallclock::ClockFormat /*format*/) {}

void panel_set_night(const iclforge::nowview::NightSchedule& /*night*/) {}

bool panel_start() { return false; }

PanelStats panel_stats() { return {}; }

std::optional<std::string> panel_stats_text() { return std::nullopt; }

iclforge::DisplayCaps panel_display_caps() { return {}; }

iclforge::devview::Facts panel_board_facts() { return {}; }

}  // namespace player
