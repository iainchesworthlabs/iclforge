#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../support.hpp"

// `forge project`: encodes what a Forge project file (.forgeproj) says, by running the commands
// the file stands for - the same line the Forge GUI's command bar shows for the same draft. The
// file's reader and the translation to commands are apps/shared/project, which the window uses
// too; this is the command around them.
namespace forge_cli::commands {

// Runs one command given as argv (everything after `forge`), as if typed: main.cpp supplies
// run_main over a vector of strings, so that the commands a project stands for are parsed,
// refused and run by exactly the code a person's line goes through.
using RunCommand = std::function<int(const std::vector<std::string>& argv)>;

// `out` is where the result goes (empty: beside the project, named for it). `print` shows the
// commands and runs none of them.
int run_project(std::string_view project_path, std::string_view out, bool print,
                const RunCommand& run);

}  // namespace forge_cli::commands
