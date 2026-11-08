// `calx clean`, `calx uninstall` and `calx completions`, for managing Calx itself (CLX-85), and
// `calx new`, for starting a program (CLX-86).
#pragma once

#include <string>
#include <vector>

namespace calx {

// `calx clean`: removes the build cache, and says how much it freed. Returns the exit status.
int run_clean(const std::vector<std::string>& args);

// `calx uninstall [--yes]`: removes the installation this calx is in, its build cache, and its
// bin/ from PATH, as the install scripts put it there, after asking unless given `--yes`. A calx
// built from source refuses. Returns the exit status.
int run_uninstall(const std::vector<std::string>& args);

// `calx completions SHELL`: prints a completion script for bash, zsh, fish or PowerShell.
int run_completions(const std::vector<std::string>& args);

// `calx new NAME`: makes the folder NAME, or uses it, with a program NAME.calx in it and a
// .gitignore for what `calx build` makes. `calx new NAME.calx` makes one script. Never overwrites
// a program; returns the exit status.
int run_new(const std::vector<std::string>& args);

}  // namespace calx
