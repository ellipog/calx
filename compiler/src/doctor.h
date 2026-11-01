// `calx doctor`: checks that Calx can build and run programs, and says how to fix what's wrong.
#pragma once

namespace calx {

// Runs the checks and prints what each found. Returns 0 if everything works, 1 if not.
int run_doctor();

}  // namespace calx
