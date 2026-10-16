#pragma once

#include "config.h"

// RunOptions are the flags of bprocs run.
typedef struct RunOptions {
    StringList names;      // the procs to start, with what they need; none for the autostart ones
    String wait;           // when set, end once this proc has ended, with its exit status
    bool exit_on_failure;  // end, with 1, once any proc fails with no restart coming
    int64_t ready_timeout_ns; // when set, end with 1 if the procs are not all up in time
    bool plain;            // leave out escape sequences
} RunOptions;

// run_main runs config in the foreground, writing every proc's lines to
// standard output with its name in front, until everything has ended or a
// condition of options ends it. It serves the config's socket meanwhile, so
// other commands work. It returns the process's exit status.
int run_main(const Config *config, RunOptions options);
