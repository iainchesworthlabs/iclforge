#pragma once

#include "iclforge/base/version.hpp"

// The family's version is iclforge::base's (iclforge/base/version.hpp, since
// planning/consolidation.md's C6); these names are kept for a release.

namespace iclforge::ac3 {

using base::build_target;
using base::git_branch;
using base::git_commit;
using base::git_commit_full;
using base::git_commits_since_tag;
using base::git_describe;
using base::git_dirty;
using base::version_details;
using base::version_full;
using base::version_major;
using base::version_minor;
using base::version_patch;
using base::version_string;

}  // namespace iclforge::ac3
