#pragma once

// Stand-in for the export header libs/base's CMake generates: this project compiles the library's
// two crypto sources straight into the bench, so nothing is exported or imported.
#define ICLFORGE_BASE_EXPORT
