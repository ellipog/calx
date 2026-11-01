// `calx update`, and the daily check for a newer release.
#pragma once

#include <string>
#include <vector>

namespace calx {

// `calx update ARGS`: runs bin/calx-update beside this calx, which downloads and installs a
// release. A calx built from source says how to update it instead. Returns the exit status.
int run_update(const std::vector<std::string>& args);

// Says on standard error, at most once a day, that a newer Calx is out, from what the daily
// check last found, and starts that check in the background when it's a day old. On the usual
// path that's one file's time and contents read. Does nothing in a build from source, when
// standard error isn't a terminal, in CI, or with CALX_NO_UPDATE_CHECK set.
void update_notice();

}  // namespace calx
