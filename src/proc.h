#pragma once

#include "config.h"
#include "mc/term/logbuf.h"

// Proc is one supervised program: it starts the program on a terminal of its
// own, collects its output on a reader thread, stops it, restarts it, and
// remembers how it ended. Everything but the reader runs on the thread that
// owns the Proc.

typedef enum ProcState {
    PROC_IDLE,     // never started
    PROC_WAITING,  // asked to start, waiting for what it needs
    PROC_BLOCKED,  // asked to start, but something it needs cannot come up
    PROC_STARTING, // running, not ready yet
    PROC_RUNNING,
    PROC_STOPPING, // asked to stop, not gone yet
    PROC_STOPPED,  // ended after being asked to
    PROC_EXITED,   // ended on its own with status 0
    PROC_CRASHED,  // ended on its own otherwise, or was stopped for failing a check
    PROC_FAILED,   // could not start
} ProcState;

enum { PROC_STATE_COUNT = PROC_FAILED + 1 };

// proc_state_active is whether the program runs: starting, running or stopping.
bool proc_state_active(ProcState state);
// proc_state_name is the state as one lowercase word, such as "running".
const char *proc_state_name(ProcState state);
bool proc_state_parse(String name, ProcState *state);

typedef enum ProcHealth {
    HEALTH_UNKNOWN, // no health check, or none has finished in this run
    HEALTH_HEALTHY,
    HEALTH_UNHEALTHY,
} ProcHealth;

const char *proc_health_name(ProcHealth health);
bool proc_health_parse(String name, ProcHealth *health);

enum { PROC_PROBLEM_LIMIT = 160 };

typedef struct ProcStatus {
    ProcState state;
    ProcHealth health;
    int pid;
    int exit_code; // -1 when a signal ended it
    int signal;
    int64_t started_ns; // monotonic
    int64_t ready_ns;
    int64_t ended_ns;
    int64_t restart_at_ns; // when an automatic restart is due, 0 when none is
    unsigned restarts;
    // problem says what is wrong now: a failed check, or why it cannot
    // start. Empty when nothing is.
    char problem[PROC_PROBLEM_LIMIT];
} ProcStatus;

// proc_status_failed is whether a proc crashed, could not start or is blocked,
// with no restart coming.
bool proc_status_failed(ProcStatus status);

// ProcHooks lets the owner hear what a Proc does. wake is called on the
// reader thread when output or an exit arrives. output and note, when set,
// are called with the Proc's lock held, so they see every byte and note in
// the order the log takes them; they must not call back into the Proc.
typedef struct ProcHooks {
    void (*wake)(void *context);
    void (*output)(void *context, size_t index, String output);
    void (*note)(void *context, size_t index, String text);
    void *context;
    size_t index; // passed back to output and note
} ProcHooks;

typedef struct Proc Proc;

// proc_create keeps spec, which must outlive the Proc.
Proc *proc_create(const ProcSpec *spec, size_t scrollback, ProcHooks hooks);
// proc_destroy kills what still runs and waits for the reader.
void proc_destroy(Proc *proc);

const ProcSpec *proc_spec(const Proc *proc);
// proc_rehook changes how the owner hears of the Proc, as when it moves to
// another index or is retired without anyone listening.
void proc_rehook(Proc *proc, ProcHooks hooks);
ProcStatus proc_status(Proc *proc);

// proc_start starts the program unless it runs; one that cannot start is
// PROC_FAILED with the reason in its log. It waits for the ready check when
// the spec has one: the log check by itself, any other through proc_mark_ready.
void proc_start(Proc *proc, int64_t now_ns);
// proc_stop sends the spec's stop signal, and SIGKILL if that has not ended
// it within the spec's timeout. It cancels a restart that was waiting for the
// stop. A proc that waits to start goes back to stopped.
void proc_stop(Proc *proc, int64_t now_ns);
void proc_kill(Proc *proc);
void proc_restart(Proc *proc, int64_t now_ns);
// proc_fail stops a running program because it failed a check, noting why.
// It ends as crashed, so a restart policy treats it as a failure.
void proc_fail(Proc *proc, int64_t now_ns, String why);

// proc_refuse marks a proc that does not run as failed to start, noting why.
void proc_refuse(Proc *proc, int64_t now_ns, String why);

// proc_wait marks a proc that does not run as waiting to start.
void proc_wait(Proc *proc);
// proc_block marks a waiting proc as blocked, noting why; proc_wait undoes it.
void proc_block(Proc *proc, String why);
// proc_mark_ready makes a starting proc running.
void proc_mark_ready(Proc *proc, int64_t now_ns);
// proc_set_health records the latest health check; problem is empty when it passed.
void proc_set_health(Proc *proc, ProcHealth health, String problem);

// proc_send types input to the program.
void proc_send(Proc *proc, String input);
// proc_resize sets the terminal size for this and later runs.
void proc_resize(Proc *proc, PtySize size);
void proc_clear_log(Proc *proc);

// proc_tick does what is due: collecting a finished run, escalating a stop,
// an automatic restart. It returns whether the state changed.
bool proc_tick(Proc *proc, int64_t now_ns);

// The log may be read only between proc_lock and proc_unlock.
void proc_lock(Proc *proc);
void proc_unlock(Proc *proc);
const LogBuffer *proc_log(Proc *proc);
