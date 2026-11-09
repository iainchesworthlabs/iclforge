#pragma once

#include "truehd_stream.hpp"

// The index of a TrueHD or MLP stream and the cursor that reads it from a source live in
// apps/shared/media/src/truehd_stream.hpp, where Forge's window uses them too; Hearth's engine
// names them in its own namespace.

namespace iclforge::hearth {

using iclforge::apps::index_truehd;
using iclforge::apps::kTruehdRestartSpacing;
using iclforge::apps::TruehdCursor;
using iclforge::apps::TruehdIndex;
using iclforge::apps::TruehdRestart;

}  // namespace iclforge::hearth
