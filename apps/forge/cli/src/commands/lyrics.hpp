#pragma once

#include <string_view>

#include "../support.hpp"

// `lyrics=` on the encoders that write a stream (encode, eac3-encode, ac4-encode): a lyrics file
// put beside the stream as its own, so that what Forge makes is a stream and its words, and
// Hearth, which finds `<stream>.lrc` by name, plays both on every sink.
//
// The file may be in any format libs/lyrics reads (`.lrc`, `.srt`, `.ass`, `.kar`, in UTF-8 or
// UTF-16); what is written is the one format every player has, UTF-8 LRC, with a duet's singers
// and names, word times where the file has them, and the gaps between cues marked. A code page is
// refused by name, as everywhere else, unless `lyrics-encoding=<page>` says which it is (Windows-1252,
// Shift-JIS, GBK ...: apps/shared/text-encoding); a page is never guessed at, and a UTF-8 or UTF-16
// file is read as it is whatever is named. An `.lrc` of that name already beside the stream is left
// as it was.
//
// Two steps around the encode, so that a bad file fails before any work is done and a failed
// encode leaves no words beside nothing.
namespace forge_cli::commands {

// Before the encode. kExitOk when `meta` has no lyrics= or the file reads; otherwise the exit
// code, with the reason already printed: a usage error for an output that has no name to put a
// file beside ("-"), an input error for a file that is not lyrics.
[[nodiscard]] int check_lyrics_option(const Options& meta, std::string_view out_path);

// After a successful encode: writes `<stream>.lrc`. kExitOk when it was written or left as it was,
// an output error when it could not be written.
[[nodiscard]] int place_lyrics_option(const Options& meta, std::string_view out_path);

// `forge lyrics-to-lrc <in> <out.lrc>`: the same conversion as lyrics=, with no stream and no
// encode, to a path of the caller's choosing, so that a lyrics file in a code page (or an .srt, an
// .ass, a .kar) already beside a stream a player has can be made the UTF-8 LRC every player reads.
// `lyrics-encoding=` names the page; the file written is never the file read, and one that is
// already there is replaced only with `overwrite=1`. kExitOk, or a usage error for standard input
// or output (there is no name to read or write), an input error for a file that is not lyrics and
// an output error for a file that could not be written or is there.
[[nodiscard]] int run_lyrics_convert(std::string_view in_path, std::string_view out_path,
                                     const Options& meta);

}  // namespace forge_cli::commands
