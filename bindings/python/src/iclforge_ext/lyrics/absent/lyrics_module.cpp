#include "optional_modules.hpp"

// The variant of the `iclforge.lyrics` submodule compiled when this configure did not build
// iclforge::lyrics (the minimum-footprint profiles leave it out) - see optional_modules.hpp for
// the pair, and bindings/python/CMakeLists.txt for the selection.
//
// Registers nothing, deliberately: `iclforge.lyrics` is simply absent from the module, so a caller
// reaching for it gets Python's own AttributeError, as for the other optional submodules.

namespace iclforge::python {

void register_lyrics(pybind11::module_& /*m*/) {}

}  // namespace iclforge::python
