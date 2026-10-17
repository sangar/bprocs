#include "proc.h"

#include <string.h>

#include "format.h"
#include "mc/log/logfile.h"
#include "mc/platform/platform.h"

enum {
    READ_CHUNK = 16384,
    READ_TIMEOUT_MS = 100,
    DRAIN_TIMEOUT_MS = 50,
    DRAIN_LIMIT_MS = 500,
    INPUT_TIMEOUT_MS = 50,
    RESTART_DELAY_MS = 1000,
    RESTART_DELAY_MAX_MS = 30000,
    STABLE_RUN_MS = 10000,
    RESTART_LIMIT = 10, // automatic restarts in a row without a stable run
    LOG_BYTES = 8 << 20,
    LOG_FILE_LIMIT = 10 << 20,
};

static const PtySize DEFAULT_SIZE = { .columns = 120, .rows = 30 };

static const char *const STATE_NAMES[PROC_STATE_COUNT] = {
    "idle", "waiting", "blocked", "starting", "running", "stopping", "stopped", "exited", "crashed", "failed",
};

static const char *const HEALTH_NAMES[] = { "unknown", "healthy", "unhealthy" };

struct Proc {
    Arena *arena;
    Arena *scratch; // for the owner's thread
    const ProcSpec *spec;
    ProcHooks hooks;

    Mutex lock;
    LogBuffer *log;
    LogFile *file; // nullptr when the spec has no log file
    bool file_failed;
    ProcStatus status;
    bool stop_requested;
    bool fail_requested; // the stop is for a failed check, so the run ends as crashed
    bool reader_done;

    // Only the owner's thread touches these. While the reader runs it reads
    // and reaps pty; the owner writes to pty.fd and signals the status pid.
    Pty pty;
    Thread reader;
    bool reader_running;
    bool restart_after_stop;
    int64_t stop_deadline_ns;
    int64_t restart_delay_ms;
    unsigned quick_restarts;
    PtySize size;
};

bool proc_state_active(ProcState state)
{
    return state == PROC_STARTING || state == PROC_RUNNING || state == PROC_STOPPING;
}

bool proc_status_failed(ProcStatus status)
{
    bool failed = status.state == PROC_CRASHED || status.state == PROC_FAILED || status.state == PROC_BLOCKED;
    return failed && status.restart_at_ns == 0;
}

const char *proc_state_name(ProcState state)
{
    return STATE_NAMES[state];
}

bool proc_state_parse(String name, ProcState *state)
{
    for (size_t i = 0; i < countof(STATE_NAMES); i++) {
        if (str_equal(name, S(STATE_NAMES[i]))) {
            *state = (ProcState)i;
            return true;
        }
    }
    return false;
}

const char *proc_health_name(ProcHealth health)
{
    return HEALTH_NAMES[health];
}

bool proc_health_parse(String name, ProcHealth *health)
{
    for (size_t i = 0; i < countof(HEALTH_NAMES); i++) {
        if (str_equal(name, S(HEALTH_NAMES[i]))) {
            *health = (ProcHealth)i;
            return true;
        }
    }
    return false;
}

Proc *proc_create(const ProcSpec *spec, size_t scrollback, ProcHooks hooks)
{
    Arena *arena = arena_create(0);
    Proc *proc = arena_push(arena, sizeof *proc);
    proc->arena = arena;
    proc->scratch = arena_create(0);
    proc->spec = spec;
    proc->hooks = hooks;
    mutex_init(&proc->lock);
    proc->log = logbuf_create(scrollback, LOG_BYTES);
    proc->file = spec->log_path.len > 0 ? logfile_create(spec->log_path, LOG_FILE_LIMIT) : nullptr;
    proc->pty = (Pty) { .fd = -1 };
    proc->size = DEFAULT_SIZE;
    return proc;
}

void proc_destroy(Proc *proc)
{
    if (proc->reader_running) {
        proc_kill(proc);
        thread_join(&proc->reader);
        pty_close(&proc->pty);
    }
    if (proc->file != nullptr) {
        logfile_destroy(proc->file);
    }
    logbuf_destroy(proc->log);
    mutex_destroy(&proc->lock);
    arena_destroy(proc->scratch);
    arena_destroy(proc->arena);
}

const ProcSpec *proc_spec(const Proc *proc)
{
    return proc->spec;
}

void proc_rehook(Proc *proc, ProcHooks hooks)
{
    mutex_lock(&proc->lock);
    proc->hooks = hooks;
    mutex_unlock(&proc->lock);
}

ProcStatus proc_status(Proc *proc)
{
    mutex_lock(&proc->lock);
    ProcStatus status = proc->status;
    mutex_unlock(&proc->lock);
    return status;
}

void proc_lock(Proc *proc)
{
    mutex_lock(&proc->lock);
}

void proc_unlock(Proc *proc)
{
    mutex_unlock(&proc->lock);
}

const LogBuffer *proc_log(Proc *proc)
{
    return proc->log;
}

void proc_clear_log(Proc *proc)
{
    mutex_lock(&proc->lock);
    logbuf_clear(proc->log);
    mutex_unlock(&proc->lock);
}

static void set_problem(ProcStatus *status, String problem)
{
    size_t len = min_size(problem.len, sizeof status->problem - 1);
    memcpy(status->problem, problem.data, len);
    status->problem[len] = '\0';
}

// write_file adds a line to the log file; the lock must be held. The first
// failure is noted in the log and ends the writing.
static void write_file(Proc *proc, Arena *scratch, String line)
{
    if (proc->file == nullptr || proc->file_failed) {
        return;
    }
    ArenaMark mark = arena_mark(scratch);
    Err err = { 0 };
    if (logfile_write(proc->file, scratch, logbuf_plain(scratch, line), clock_wall_ns(), &err) != ERR_OK) {
        proc->file_failed = true;
        logbuf_note(proc->log, str_format(scratch, "\x1b[31m✖ the log file stopped:\x1b[0m %s", err.msg));
    }
    arena_release(mark);
}

// note adds a line of bprocs's own to the log; the lock must be held.
[[gnu::format(printf, 3, 4)]] static void note(Proc *proc, Arena *scratch, const char *format, ...)
{
    ArenaMark mark = arena_mark(scratch);
    va_list args;
    va_start(args, format);
    String text = str_format_va(scratch, format, args);
    va_end(args);
    String clock = format_clock(scratch, clock_wall_ns());
    String line = str_format(scratch, "\x1b[90m──\x1b[0m %.*s \x1b[90m· %.*s\x1b[0m", (int)text.len, text.data,
                             (int)clock.len, clock.data);
    logbuf_note(proc->log, line);
    if (proc->hooks.note != nullptr) {
        proc->hooks.note(proc->hooks.context, proc->hooks.index, line);
    }
    write_file(proc, scratch, line);
    arena_release(mark);
}

static bool line_is_ready(Arena *scratch, String line, String pattern)
{
    ArenaMark mark = arena_mark(scratch);
    bool ready = str_contains(logbuf_plain(scratch, line), pattern);
    arena_release(mark);
    return ready;
}

// saw_ready reports whether the lines from number from on, or the line
// still being written, hold the ready text; the lock must be held.
static bool saw_ready(Proc *proc, Arena *scratch, uint64_t from)
{
    String pattern = proc->spec->ready.text;
    uint64_t first = logbuf_first(proc->log);
    for (uint64_t n = from > first ? from : first; n < logbuf_end(proc->log); n++) {
        if (line_is_ready(scratch, logbuf_line(proc->log, n), pattern)) {
            return true;
        }
    }
    return line_is_ready(scratch, logbuf_partial(proc->log), pattern);
}

// become_ready marks a starting proc as running; the lock must be held.
static void become_ready(Proc *proc, Arena *scratch, int64_t now_ns)
{
    if (proc->status.state != PROC_STARTING) {
        return;
    }
    proc->status.state = PROC_RUNNING;
    proc->status.ready_ns = now_ns;
    String took = format_elapsed(scratch, now_ns - proc->status.started_ns);
    note(proc, scratch, "\x1b[32m✔ ready\x1b[0m \x1b[90mafter %.*s\x1b[0m", (int)took.len, took.data);
}

static void take_output(Proc *proc, Arena *scratch, String output)
{
    mutex_lock(&proc->lock);
    uint64_t from = logbuf_end(proc->log);
    logbuf_write(proc->log, output);
    if (proc->hooks.output != nullptr) {
        proc->hooks.output(proc->hooks.context, proc->hooks.index, output);
    }
    if (proc->file != nullptr) {
        uint64_t first = logbuf_first(proc->log);
        for (uint64_t n = from > first ? from : first; n < logbuf_end(proc->log); n++) {
            write_file(proc, scratch, logbuf_line(proc->log, n));
        }
    }
    if (proc->status.state == PROC_STARTING && proc->spec->ready.kind == CHECK_LOG && saw_ready(proc, scratch, from)) {
        become_ready(proc, scratch, clock_monotonic_ns());
    }
    ProcHooks hooks = proc->hooks;
    mutex_unlock(&proc->lock);
    hooks.wake(hooks.context);
}

// note_end describes how a run ended; the lock must be held.
static void note_end(Proc *proc, Arena *scratch)
{
    ProcStatus *status = &proc->status;
    String ran = format_elapsed(scratch, status->ended_ns - status->started_ns);
    int ran_len = (int)ran.len;
    if (status->state == PROC_STOPPED) {
        note(proc, scratch, "\x1b[33m■ stopped\x1b[0m \x1b[90mafter %.*s\x1b[0m", ran_len, ran.data);
    } else if (status->state == PROC_EXITED) {
        note(proc, scratch, "\x1b[32m✔ exited\x1b[0m \x1b[90mafter %.*s\x1b[0m", ran_len, ran.data);
    } else if (status->signal != 0) {
        String name = format_signal(scratch, status->signal);
        note(proc, scratch, "\x1b[31m✖ ended by %.*s\x1b[0m \x1b[90mafter %.*s\x1b[0m", (int)name.len, name.data,
             ran_len, ran.data);
    } else {
        note(proc, scratch, "\x1b[31m✖ exited with code %d\x1b[0m \x1b[90mafter %.*s\x1b[0m", status->exit_code,
             ran_len, ran.data);
    }
}

static void finish(Proc *proc, Arena *scratch, int exit_code, int signal)
{
    mutex_lock(&proc->lock);
    ProcStatus *status = &proc->status;
    status->ended_ns = clock_monotonic_ns();
    status->exit_code = exit_code;
    status->signal = signal;
    status->pid = 0;
    status->state = proc->fail_requested                  ? PROC_CRASHED
                    : proc->stop_requested                ? PROC_STOPPED
                    : exit_code == 0 && signal == 0 ? PROC_EXITED
                                                    : PROC_CRASHED;
    note_end(proc, scratch);
    proc->reader_done = true;
    ProcHooks hooks = proc->hooks;
    mutex_unlock(&proc->lock);
    hooks.wake(hooks.context);
}

// read_output runs on the reader thread for one run of the program.
static void *read_output(void *argument)
{
    Proc *proc = argument;
    Arena *scratch = arena_create(0);
    char *buffer = arena_push(scratch, READ_CHUNK);
    int exit_code = -1;
    int signal = 0;
    bool hung_up = false;
    // A program can end while something it started keeps the terminal open,
    // and keep running after closing it, so reaping is checked either way.
    while (!pty_reap(&proc->pty, &exit_code, &signal)) {
        if (hung_up) {
            clock_sleep_ms(READ_TIMEOUT_MS);
            continue;
        }
        size_t got;
        PtyEvent event = pty_read(&proc->pty, READ_TIMEOUT_MS, buffer, READ_CHUNK, &got);
        if (event == PTY_DATA) {
            take_output(proc, scratch, (String) { buffer, got });
        }
        hung_up = event == PTY_HANGUP;
    }
    // The pid is free for reuse from here on: nothing may signal it any more.
    mutex_lock(&proc->lock);
    proc->status.pid = 0;
    mutex_unlock(&proc->lock);
    // What the program wrote just before it ended can still be on its way. A
    // process it left behind may go on writing, so the wait has a limit.
    int64_t drain_until = clock_monotonic_ns() + (int64_t)DRAIN_LIMIT_MS * NS_PER_MILLISECOND;
    while (!hung_up && clock_monotonic_ns() < drain_until) {
        size_t got;
        if (pty_read(&proc->pty, DRAIN_TIMEOUT_MS, buffer, READ_CHUNK, &got) != PTY_DATA) {
            break;
        }
        take_output(proc, scratch, (String) { buffer, got });
    }
    finish(proc, scratch, exit_code, signal);
    arena_destroy(scratch);
    return nullptr;
}

static StringList child_environment(Arena *arena, const ProcSpec *spec)
{
    const char *const terminal[] = { "TERM=xterm-256color", "COLORTERM=truecolor" };
    StringList env = strlist_of(arena, countof(terminal), terminal);
    strlist_push_all(arena, &env, spec->env);
    return env;
}

static void fail_start(Proc *proc, int64_t now_ns, const char *reason)
{
    mutex_lock(&proc->lock);
    proc->status.state = PROC_FAILED;
    proc->status.started_ns = now_ns;
    proc->status.ended_ns = now_ns;
    set_problem(&proc->status, S(reason));
    note(proc, proc->scratch, "\x1b[31m✖ could not start:\x1b[0m %s", reason);
    mutex_unlock(&proc->lock);
}

void proc_start(Proc *proc, int64_t now_ns)
{
    if (proc->reader_running) {
        return;
    }
    proc->restart_after_stop = false;
    ArenaMark mark = arena_mark(proc->scratch);
    Err err = { 0 };
    Error spawned = pty_spawn(proc->scratch, proc->spec->argv, proc->spec->cwd,
                              child_environment(proc->scratch, proc->spec), proc->size, &proc->pty, &err);
    mutex_lock(&proc->lock);
    proc->status.restart_at_ns = 0;
    mutex_unlock(&proc->lock);
    if (spawned != ERR_OK) {
        fail_start(proc, now_ns, err.msg);
        arena_release(mark);
        return;
    }

    mutex_lock(&proc->lock);
    bool waits_for_ready = proc->spec->ready.kind != CHECK_NONE;
    proc->status = (ProcStatus) {
        .state = waits_for_ready ? PROC_STARTING : PROC_RUNNING,
        .pid = proc->pty.pid,
        .started_ns = now_ns,
        .ready_ns = waits_for_ready ? 0 : now_ns,
        .restarts = proc->status.restarts,
    };
    proc->stop_requested = false;
    proc->fail_requested = false;
    proc->reader_done = false;
    note(proc, proc->scratch, "\x1b[32m▶\x1b[0m \x1b[1m%.*s\x1b[0m", (int)proc->spec->command.len,
         proc->spec->command.data);
    mutex_unlock(&proc->lock);

    if (thread_start(&proc->reader, read_output, proc, &err) != ERR_OK) {
        Error killed = pty_signal(&proc->pty, PTY_KILL, nullptr);
        unused(killed);
        pty_close(&proc->pty);
        fail_start(proc, now_ns, err.msg);
    } else {
        proc->reader_running = true;
    }
    arena_release(mark);
}

// signal_running sends signal to the program when it runs, marking it as
// stopping, and as failing when failing is set; the result says whether it did.
static bool signal_running(Proc *proc, PtySignal signal, bool failing)
{
    mutex_lock(&proc->lock);
    bool active = proc_state_active(proc->status.state) && proc->status.pid > 0;
    Pty target = { .fd = proc->pty.fd, .pid = proc->status.pid };
    if (active) {
        proc->stop_requested = true;
        proc->fail_requested = proc->fail_requested || failing;
        proc->status.state = PROC_STOPPING;
    } else {
        proc->status.restart_at_ns = 0;
    }
    mutex_unlock(&proc->lock);
    if (active) {
        Error sent = pty_signal(&target, signal, nullptr);
        unused(sent);
    }
    return active;
}

// stop_waiting puts a proc that waits to start back where it was before.
static void stop_waiting(Proc *proc)
{
    mutex_lock(&proc->lock);
    ProcStatus *status = &proc->status;
    if (status->state == PROC_WAITING || status->state == PROC_BLOCKED) {
        status->state = status->started_ns == 0 ? PROC_IDLE : PROC_STOPPED;
        status->problem[0] = '\0';
    }
    mutex_unlock(&proc->lock);
}

void proc_stop(Proc *proc, int64_t now_ns)
{
    proc->restart_after_stop = false;
    stop_waiting(proc);
    ProcState state = proc_status(proc).state;
    if (state == PROC_STOPPING) {
        return;
    }
    if (signal_running(proc, proc->spec->stop, false)) {
        proc->stop_deadline_ns = now_ns + proc->spec->stop_timeout_ns;
    }
}

void proc_kill(Proc *proc)
{
    stop_waiting(proc);
    signal_running(proc, PTY_KILL, false);
    proc->stop_deadline_ns = INT64_MAX;
}

void proc_fail(Proc *proc, int64_t now_ns, String why)
{
    mutex_lock(&proc->lock);
    bool active = proc_state_active(proc->status.state) && proc->status.state != PROC_STOPPING;
    if (active) {
        set_problem(&proc->status, why);
        note(proc, proc->scratch, "\x1b[31m✖ %.*s\x1b[0m \x1b[90m· stopping it\x1b[0m", (int)why.len, why.data);
    }
    mutex_unlock(&proc->lock);
    if (active && signal_running(proc, proc->spec->stop, true)) {
        proc->restart_after_stop = false;
        proc->stop_deadline_ns = now_ns + proc->spec->stop_timeout_ns;
    }
}

void proc_restart(Proc *proc, int64_t now_ns)
{
    stop_waiting(proc);
    proc->quick_restarts = 0;
    mutex_lock(&proc->lock);
    proc->status.restarts++;
    bool active = proc_state_active(proc->status.state);
    mutex_unlock(&proc->lock);
    if (active) {
        proc_stop(proc, now_ns);
        proc->restart_after_stop = true;
    } else if (proc->reader_running) {
        proc->restart_after_stop = true;
    } else {
        proc_start(proc, now_ns);
    }
}

void proc_refuse(Proc *proc, int64_t now_ns, String why)
{
    if (proc->reader_running) {
        return;
    }
    fail_start(proc, now_ns, str_cstr(proc->scratch, why));
}

void proc_wait(Proc *proc)
{
    if (proc->reader_running) {
        return;
    }
    mutex_lock(&proc->lock);
    if (!proc_state_active(proc->status.state)) {
        proc->status.state = PROC_WAITING;
        proc->status.restart_at_ns = 0;
        proc->status.problem[0] = '\0';
    }
    mutex_unlock(&proc->lock);
}

void proc_block(Proc *proc, String why)
{
    mutex_lock(&proc->lock);
    if (proc->status.state == PROC_WAITING) {
        proc->status.state = PROC_BLOCKED;
        set_problem(&proc->status, why);
        note(proc, proc->scratch, "\x1b[31m⊘ not started:\x1b[0m %.*s", (int)why.len, why.data);
    }
    mutex_unlock(&proc->lock);
}

void proc_mark_ready(Proc *proc, int64_t now_ns)
{
    mutex_lock(&proc->lock);
    become_ready(proc, proc->scratch, now_ns);
    mutex_unlock(&proc->lock);
}

void proc_set_health(Proc *proc, ProcHealth health, String problem)
{
    mutex_lock(&proc->lock);
    ProcHealth was = proc->status.health;
    proc->status.health = health;
    set_problem(&proc->status, problem);
    if (health == HEALTH_UNHEALTHY && was != HEALTH_UNHEALTHY) {
        note(proc, proc->scratch, "\x1b[31m✖ unhealthy:\x1b[0m %.*s", (int)problem.len, problem.data);
    } else if (health == HEALTH_HEALTHY && was == HEALTH_UNHEALTHY) {
        note(proc, proc->scratch, "\x1b[32m✔ healthy again\x1b[0m");
    }
    mutex_unlock(&proc->lock);
}

void proc_send(Proc *proc, String input)
{
    if (proc->reader_running && proc_state_active(proc_status(proc).state)) {
        // A program that has stopped reading must not hold up the owner.
        Error sent = pty_write(&proc->pty, input, INPUT_TIMEOUT_MS, nullptr);
        unused(sent);
    }
}

void proc_resize(Proc *proc, PtySize size)
{
    if (size.columns == proc->size.columns && size.rows == proc->size.rows) {
        return;
    }
    proc->size = size;
    if (proc->reader_running) {
        Error resized = pty_resize(&proc->pty, size, nullptr);
        unused(resized);
    }
}

static bool restarts_after(RestartPolicy restart, ProcState ended)
{
    return (restart == RESTART_ALWAYS && ended == PROC_EXITED) || (restart != RESTART_NO && ended == PROC_CRASHED);
}

// schedule_restart sets when the next automatic restart is due, pausing
// longer after each quick failure, and gives up after RESTART_LIMIT of them.
static void schedule_restart(Proc *proc, int64_t now_ns)
{
    mutex_lock(&proc->lock);
    int64_t ran_ms = (proc->status.ended_ns - proc->status.started_ns) / NS_PER_MILLISECOND;
    proc->quick_restarts = ran_ms > STABLE_RUN_MS ? 0 : proc->quick_restarts + 1;
    if (proc->quick_restarts > RESTART_LIMIT) {
        note(proc, proc->scratch, "\x1b[31m✖ gave up after %d restarts in a row\x1b[0m", RESTART_LIMIT);
        mutex_unlock(&proc->lock);
        return;
    }
    int64_t doubled = proc->restart_delay_ms * 2;
    proc->restart_delay_ms = ran_ms > STABLE_RUN_MS          ? RESTART_DELAY_MS
                             : doubled < RESTART_DELAY_MS     ? RESTART_DELAY_MS
                             : doubled > RESTART_DELAY_MAX_MS ? RESTART_DELAY_MAX_MS
                                                              : doubled;
    proc->status.restart_at_ns = now_ns + proc->restart_delay_ms * NS_PER_MILLISECOND;
    mutex_unlock(&proc->lock);
}

bool proc_tick(Proc *proc, int64_t now_ns)
{
    mutex_lock(&proc->lock);
    bool done = proc->reader_done;
    ProcState state = proc->status.state;
    int64_t restart_at = proc->status.restart_at_ns;
    mutex_unlock(&proc->lock);

    if (proc->reader_running && done) {
        thread_join(&proc->reader);
        proc->reader_running = false;
        pty_close(&proc->pty);
        if (proc->restart_after_stop) {
            proc_start(proc, now_ns);
        } else if (restarts_after(proc->spec->restart, state)) {
            schedule_restart(proc, now_ns);
        }
        return true;
    }
    if (state == PROC_STOPPING && now_ns >= proc->stop_deadline_ns) {
        proc_kill(proc);
        return true;
    }
    if (restart_at != 0 && now_ns >= restart_at && !proc->reader_running) {
        mutex_lock(&proc->lock);
        proc->status.restarts++;
        mutex_unlock(&proc->lock);
        proc_start(proc, now_ns);
        return true;
    }
    return false;
}
