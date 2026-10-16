#pragma once

#include "config.h"

// check_run tries a port, http or cmd check once for the proc spec, taking at
// most about timeout_ns, and returns whether it passed. When it did not,
// problem says why, in scratch. A cmd check runs in the proc's directory with
// its environment.
bool check_run(Arena *scratch, const Check *check, const ProcSpec *spec, int64_t timeout_ns, String *problem);

// check_describe is what a check looks at, such as "http://localhost:3000/up".
String check_describe(Arena *arena, const Check *check);
