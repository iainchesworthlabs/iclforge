#pragma once

#include <cstdint>

// The library's types by name only (libs/lyrics: lyrics.hpp and stage.hpp), which is all a pointer and
// two references need. sendspin.hpp includes this header in every build, and an image with no Sendspin
// player links no iclforge::lyrics and so has none of its headers to find (the component links it
// under CONFIG_ICLFORGE_SENDSPIN): including them here stopped every such image compiling. What reads
// a document or a stage includes the library's headers itself.
namespace iclforge::lyrics {
struct Lyrics;
struct Stage;
struct StageOptions;
}  // namespace iclforge::lyrics

// The lyrics the board holds, for a screen (planning/karaoke-p4x-panel.md, D5). In a header of its own,
// with nothing of ESP-IDF in it, so that the lyric screen builds on a computer as well (preview/):
// sendspin.hpp includes it, and the firmware's definition of sendspin_read_lyrics() is the Sendspin
// player's (sendspin/player/sendspin.cpp).

namespace player {

// A reader is called once by sendspin_read_lyrics(), UNDER the player's lock, so it copies what it
// needs and returns: the audio's session takes the same lock, and a reader that blocks holds the
// stream up.
class LyricsReader {
   public:
    virtual ~LyricsReader() = default;
    // `item` is the item the document is for, -1 before any has come. `lyrics` is the document held,
    // or null (none, or one the board could not read); `stage` is what a screen shows of it at the
    // time the sound is at on the board's own clock, less the delay asked for.
    virtual void on_lyrics(std::int64_t item, const iclforge::lyrics::Lyrics* lyrics,
                           const iclforge::lyrics::Stage& stage) = 0;
};

// Calls `reader` once with the lyrics held and their stage at `delay_ms` (positive shows the words
// later). False, with nothing called, in a build without a player.
bool sendspin_read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                          const iclforge::lyrics::StageOptions& options);

}  // namespace player
