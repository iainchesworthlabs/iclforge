#pragma once

#include "byte_source.hpp"

// Where an item's bytes are read from when the item does not hold all of them in memory
// (planning/truehd-parity-hearth-player.md, row 5). The interface is the shared media code's
// (apps/shared/media/src/byte_source.hpp), so that the loader's container reader and the session's
// TrueHD cursor speak the same one; the engine has no file I/O of its own, and the application
// supplies the file-backed source (apps/hearth/ui/src/item_loader.cpp).

namespace iclforge::hearth {

using ByteSource = iclforge::apps::ByteSource;
using MemoryByteSource = iclforge::apps::MemoryByteSource;

}  // namespace iclforge::hearth
