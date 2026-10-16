#pragma once

#include "config.h"
#include "proc.h"
#include "stats.h"

// Supervisor runs the procs of one config: it starts them in the order their
// needs say, runs their ready and health checks, samples what they cost, and
// brings them down in reverse order. Everything but the checks runs on the
// thread that owns it; the checks run on a pool of their own and report back
// through supervisor_tick, waking the owner with hooks.wake.
typedef struct Supervisor Supervisor;

// supervisor_create keeps config, which must outlive the Supervisor. Each
// proc gets hooks with its own index.
[[nodiscard]] Error supervisor_create(const Config *config, ProcHooks hooks, Supervisor **supervisor, Err *err);
// supervisor_destroy kills what still runs.
void supervisor_destroy(Supervisor *supervisor);

const Config *supervisor_config(const Supervisor *supervisor);
size_t supervisor_count(const Supervisor *supervisor);
Proc *supervisor_proc(Supervisor *supervisor, size_t index);
const ProcStats *supervisor_stats(const Supervisor *supervisor, size_t index);

// supervisor_up starts the autostart procs, and supervisor_start one proc,
// with what they need first: a proc whose needs are not met waits for them.
void supervisor_up(Supervisor *supervisor, int64_t now_ns);
void supervisor_start(Supervisor *supervisor, size_t index, int64_t now_ns);
void supervisor_stop(Supervisor *supervisor, size_t index, int64_t now_ns);
void supervisor_restart(Supervisor *supervisor, size_t index, int64_t now_ns);
void supervisor_kill(Supervisor *supervisor, size_t index);

// supervisor_down stops every proc, each once nothing running needs it.
// Called again while that goes on, it kills them all.
void supervisor_down(Supervisor *supervisor, int64_t now_ns);
bool supervisor_going_down(const Supervisor *supervisor);
// supervisor_settled is whether nothing runs, waits or is due to restart.
bool supervisor_settled(Supervisor *supervisor);

// supervisor_need_met is whether proc index counts as up for a proc that
// needs it: a job once it succeeded, anything else once it is running.
bool supervisor_need_met(Supervisor *supervisor, size_t index);
// supervisor_need_lost says, in scratch, why proc index cannot come up
// without being started again, or returns false while it still may.
bool supervisor_need_lost(Supervisor *supervisor, Arena *scratch, size_t index, String *why);

// supervisor_reload runs next from now on, and returns what changed, in
// arena. A proc that runs the same way stays as it is; one that changed is
// stopped and its successor starts once it has ended, if it ran or waited to;
// one that is gone is stopped; a new one starts if it is autostart. next
// must outlive the Supervisor, as the config before it must, since a stopping
// proc still uses it.
String supervisor_reload(Supervisor *supervisor, Arena *arena, const Config *next, int64_t now_ns);

// supervisor_tick does what is due and returns whether anything changed.
bool supervisor_tick(Supervisor *supervisor, int64_t now_ns);
// supervisor_sample adds a sample of each proc's CPU and memory to its stats.
void supervisor_sample(Supervisor *supervisor, Arena *scratch, int64_t now_ns);
