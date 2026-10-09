#pragma once

// objects' names were iclforge::oba and iclforge::emdf until planning/consolidation.md's C6
// (decision 19); they are iclforge::objects::oba and iclforge::objects::emdf, and the old names
// stay as aliases for a release. Every header of the library includes this one.

namespace iclforge::objects::oba {}
namespace iclforge::objects::emdf {}

namespace iclforge {

namespace oba = objects::oba;
namespace emdf = objects::emdf;

}  // namespace iclforge
