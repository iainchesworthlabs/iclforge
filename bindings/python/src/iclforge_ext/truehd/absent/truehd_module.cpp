#include "optional_modules.hpp"

// The variant of the `iclforge.truehd` submodule compiled when this configure did not build
// iclforge::truehd - see optional_modules.hpp for the pair, and bindings/python/CMakeLists.txt for
// the selection.
//
// Registers nothing, deliberately, on the same reasoning as the ac4 and lyrics absent/ variants
// beside it. `iclforge.truehd` is simply absent from the module, so a caller reaching for it gets
// Python's own AttributeError.

namespace iclforge::python {

void register_truehd(pybind11::module_& /*m*/) {}

}  // namespace iclforge::python
