// No Sendspin player: a build without a network, or without the component's
// player (CONFIG_ICLFORGE_SENDSPIN). See ../../sendspin.hpp.

#include "sendspin.hpp"

#include <optional>
#include <string_view>

namespace player {

bool sendspin_built() { return false; }

void sendspin_start(const iclforge::render::OutputLayout& /*layout*/) {}

bool sendspin_running() { return false; }

bool sendspin_playing() { return false; }

void sendspin_set_external(bool /*external*/) {}

bool sendspin_set_layout(const iclforge::render::OutputLayout& /*layout*/) { return true; }

void sendspin_set_dynamic_range(iclforge::ac3::DynamicRange /*range*/) {}

void sendspin_set_block_tap(iclforge::BlockTap* /*tap*/) {}

void sendspin_board_changed() {}

void sendspin_name_changed() {}

void sendspin_leave() {}

std::optional<iclforge::ControlSendspin> sendspin_status() { return std::nullopt; }

SoundCaps sendspin_sound_caps() { return {}; }

void sendspin_set_speaker_trim(const iclforge::render::speaker_trim::SpeakerTrimTable& /*table*/) {}

std::optional<SpeakerTrimStatus> sendspin_speaker_trim_status() { return std::nullopt; }

iclforge::render::IdentifyOutcome sendspin_identify(const iclforge::render::IdentifyRequest& /*request*/) {
    return iclforge::render::IdentifyOutcome::kNoPlayer;
}

IdentifyStatus sendspin_identify_status() { return {}; }

SendspinPlaying sendspin_now_playing() { return {}; }

SendspinLevels sendspin_now_levels() { return {}; }

bool sendspin_pairing(std::string_view /*action*/) { return false; }

std::optional<iclforge::ControlPairings> sendspin_pairings() { return std::nullopt; }

std::optional<bool> sendspin_forget_server(std::string_view /*server_id*/) { return std::nullopt; }

bool sendspin_console(std::string_view /*line*/) { return false; }

bool sendspin_read_lyrics(LyricsReader& /*reader*/, std::int64_t /*delay_ms*/,
                          const iclforge::lyrics::StageOptions& /*options*/) {
    return false;
}

bool sendspin_read_title(TitleReader& /*reader*/) { return false; }

void sendspin_poll() {}

}  // namespace player
