#pragma once

#include <string>
#include <vector>

#include "session.hpp"

// The application's ItemLoader (apps/hearth/engine/src/session.hpp: "the path is
// turned into bytes by an ItemLoader, which the application supplies").
//
// It reads a raw `.ac3`/`.ec3`/`.ac4` elementary stream from disk, or an MP4,
// Matroska or MPEG-TS file, whose first AC-3, E-AC-3 or AC-4 track it demuxes
// through apps/shared/media/src/container_input.hpp. A container is told by its
// first bytes and not by its name. A TrueHD track is passed over, so a file
// with TrueHD first and AC-3 or E-AC-3 after it plays the second; a file whose
// audio is TrueHD alone (in a container, or a bare stream named for another
// codec) comes back as the error "TrueHD is not played by Hearth yet". An MP4
// edit list's priming and padding come back as
// LoadedItem::skip_samples/play_samples, and what the container said about the
// track as LoadedItem::container. A path this loader does not read -
// another extension, a file that cannot be opened, a container with no such
// track - is not a crash: it comes back as an error, which Session::open()
// turns into the item's unplayable reason, so the queue lists the file and says
// why rather than leaving it out.

namespace iclforge::hearth::ui {

[[nodiscard]] iclforge::hearth::ItemLoader make_file_item_loader();

// Every media file under `folder`, found by walking it (and every
// subfolder) and sorted for a stable queue order. "Media file" is the set
// make_file_item_loader() opens: `.ac3`, `.ec3`, `.ac4`, `.mp4`, `.mkv` and
// `.ts`, matched by extension without regard to case. This lists a path and
// does not judge it: a file that turns out not to hold an AC-3, E-AC-3 or
// AC-4 track gets its unplayable reason when the queue reaches it.
// A folder that does not exist, or cannot be read, comes back empty rather
// than throwing.
[[nodiscard]] std::vector<std::string> list_folder_items(const std::string& folder);

}  // namespace iclforge::hearth::ui
