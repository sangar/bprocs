#pragma once

#include "config.h"

// daemon_main serves config on its socket until a down has finished, and
// returns the process's exit status. The procs start when a client asks.
int daemon_main(const Config *config);
