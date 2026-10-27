#include "runner.h"

#include <stdio.h>

#include "mc/term/logbuf.h"
#include "mc/term/ansi.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"
#include "protocol.h"
#include "server.h"

enum { STEP_MS = 50, STANDARD_OUTPUT = 1 };

static const int NAME_COLORS[] = { 36, 35, 33, 32, 34, 31, 96, 95, 93, 92, 94, 91 };

typedef struct Runner {
    Arena *scratch;
    const Config *config;
    RunOptions options;
    Server *server;
    Supervisor *supervisor;
    uint64_t *printed; // printed[i]: the next line of proc i to write
    int name_width;
    bool color;
} Runner;

static void write_prefixed(Runner *runner, String name, int color, String text)
{
    if (runner->color) {
        printf("\x1b[%dm%-*.*s |\x1b[0m %.*s%s\n", color, runner->name_width, (int)name.len, name.data,
               (int)text.len, text.data, runner->options.plain ? "" : "\x1b[0m");
    } else {
        printf("%-*.*s | %.*s\n", runner->name_width, (int)name.len, name.data, (int)text.len, text.data);
    }
}

[[gnu::format(printf, 2, 3)]] static void say(Runner *runner, const char *format, ...)
{
    ArenaMark mark = arena_mark(runner->scratch);
    va_list args;
    va_start(args, format);
    String text = str_format_va(runner->scratch, format, args);
    va_end(args);
    write_prefixed(runner, S("bprocs"), 1, text);
    fflush(stdout);
    arena_release(mark);
}

static void print_new_lines(Runner *runner)
{
    for (size_t i = 0; i < supervisor_count(runner->supervisor); i++) {
        Proc *proc = supervisor_proc(runner->supervisor, i);
        String name = runner->config->procs[i].name;
        int color = NAME_COLORS[i % countof(NAME_COLORS)];
        proc_lock(proc);
        const LogBuffer *log = proc_log(proc);
        uint64_t first = logbuf_first(log);
        uint64_t end = logbuf_end(log);
        for (uint64_t n = runner->printed[i] > first ? runner->printed[i] : first; n < end; n++) {
            ArenaMark mark = arena_mark(runner->scratch);
            String line = logbuf_line(log, n);
            write_prefixed(runner, name, color, runner->options.plain || !runner->color
                                                    ? ansi_strip(runner->scratch, line)
                                                    : line);
            arena_release(mark);
        }
        runner->printed[i] = end;
        proc_unlock(proc);
    }
    fflush(stdout);
}

// ended is whether a proc is done for good: it ended, or cannot start, and no restart is coming.
static bool ended(ProcStatus status)
{
    bool over = status.state == PROC_EXITED || status.state == PROC_CRASHED || status.state == PROC_FAILED ||
                status.state == PROC_STOPPED || status.state == PROC_BLOCKED;
    return over && status.restart_at_ns == 0;
}

static int exit_status(ProcStatus status)
{
    if (status.state == PROC_EXITED) {
        return 0;
    }
    if (status.state == PROC_CRASHED && status.signal != 0) {
        return 128 + status.signal;
    }
    return status.state == PROC_CRASHED && status.exit_code > 0 ? status.exit_code : 1;
}

static void on_signal(void *context, Signal signal)
{
    if (signal == SIGNAL_STOP) {
        server_request_down(context);
    }
}

// start_targets starts the procs asked for, and returns false when one is unknown.
static bool start_targets(Runner *runner, size_t wait_index, bool waits)
{
    int64_t now = clock_monotonic_ns();
    if (runner->options.names.count == 0) {
        supervisor_up(runner->supervisor, now);
    }
    size_t *indexes = arena_push(runner->scratch, arena_size_mul(runner->config->proc_count, sizeof *indexes));
    for (size_t i = 0; i < runner->options.names.count; i++) {
        String name = runner->options.names.items[i];
        size_t found = config_select(runner->config, name, indexes);
        if (found == 0) {
            fprintf(stderr, "bprocs: no proc called %.*s\n", (int)name.len, name.data);
            return false;
        }
        for (size_t j = 0; j < found; j++) {
            supervisor_start(runner->supervisor, indexes[j], now);
        }
    }
    if (waits) {
        supervisor_start(runner->supervisor, wait_index, now);
    }
    return true;
}

// not_up names the started procs that are not up, in scratch.
static String not_up(Runner *runner)
{
    StringList names = { 0 };
    for (size_t i = 0; i < supervisor_count(runner->supervisor); i++) {
        ProcState state = proc_status(supervisor_proc(runner->supervisor, i)).state;
        if (state != PROC_IDLE && !supervisor_need_met(runner->supervisor, i)) {
            strlist_push(runner->scratch, &names, runner->config->procs[i].name);
        }
    }
    return str_join(runner->scratch, names, S(", "));
}

// failed_proc finds a proc that failed with no restart coming.
static bool failed_proc(Runner *runner, size_t *index)
{
    for (size_t i = 0; i < supervisor_count(runner->supervisor); i++) {
        if (proc_status_failed(proc_status(supervisor_proc(runner->supervisor, i)))) {
            *index = i;
            return true;
        }
    }
    return false;
}

static bool any_failed(Runner *runner)
{
    size_t index;
    return failed_proc(runner, &index);
}

// decide ends the run when its options say so, and returns the exit status then, or -1.
static int decide(Runner *runner, size_t wait_index, bool waits, int64_t *ready_deadline_ns)
{
    int64_t now = clock_monotonic_ns();
    if (waits) {
        ProcStatus status = proc_status(supervisor_proc(runner->supervisor, wait_index));
        if (ended(status)) {
            String name = runner->config->procs[wait_index].name;
            int code = exit_status(status);
            say(runner, "%.*s ended with %d · stopping everything", (int)name.len, name.data, code);
            return code;
        }
    }
    size_t failed;
    if (runner->options.exit_on_failure && failed_proc(runner, &failed)) {
        String name = runner->config->procs[failed].name;
        say(runner, "%.*s failed · stopping everything", (int)name.len, name.data);
        return 1;
    }
    if (*ready_deadline_ns != 0) {
        String late = not_up(runner);
        if (late.len == 0) {
            *ready_deadline_ns = 0;
        } else if (now >= *ready_deadline_ns) {
            say(runner, "not up in time: %.*s · stopping everything", (int)late.len, late.data);
            return 1;
        }
    }
    if (!waits && supervisor_settled(runner->supervisor)) {
        return any_failed(runner) ? 1 : 0;
    }
    return -1;
}

int run_main(const Config *config, RunOptions options)
{
    Arena *scratch = arena_create(0);
    String no_color;
    Runner runner = {
        .scratch = scratch,
        .config = config,
        .options = options,
        .color = terminal_is_terminal(STANDARD_OUTPUT) && !env_get(scratch, S("NO_COLOR"), &no_color),
        .name_width = 6,
    };
    for (size_t i = 0; i < config->proc_count; i++) {
        runner.name_width = (int)config->procs[i].name.len > runner.name_width ? (int)config->procs[i].name.len
                                                                               : runner.name_width;
    }
    size_t wait_index = 0;
    bool waits = options.wait.len > 0;
    if (waits && !config_proc_index(config, options.wait, &wait_index)) {
        fprintf(stderr, "bprocs: no proc called %.*s\n", (int)options.wait.len, options.wait.data);
        arena_destroy(scratch);
        return 2;
    }
    Err err = { 0 };
    RuntimePaths paths;
    if (runtime_paths(scratch, config->path, &paths, &err) != ERR_OK ||
        server_create(config, &paths, &runner.server, &err) != ERR_OK) {
        fprintf(stderr, "bprocs: %s\n", err.msg);
        arena_destroy(scratch);
        return 1;
    }
    runner.supervisor = server_supervisor(runner.server);
    runner.printed = arena_push(scratch, arena_size_mul(config->proc_count, sizeof *runner.printed));
    if (!start_targets(&runner, wait_index, waits)) {
        server_destroy(runner.server);
        arena_destroy(scratch);
        return 2;
    }
    SignalWatch *watch = nullptr;
    if (signal_watch_start(on_signal, runner.server, &watch, &err) != ERR_OK) {
        fprintf(stderr, "bprocs: signals are not watched: %s\n", err.msg);
    }
    int64_t ready_deadline = options.ready_timeout_ns > 0 ? clock_monotonic_ns() + options.ready_timeout_ns : 0;
    int status = -1;
    while (!server_finished(runner.server)) {
        server_step(runner.server, STEP_MS);
        print_new_lines(&runner);
        if (status < 0 && !supervisor_going_down(runner.supervisor)) {
            status = decide(&runner, wait_index, waits, &ready_deadline);
            if (status >= 0) {
                server_request_down(runner.server);
            }
        }
    }
    print_new_lines(&runner);
    if (watch != nullptr) {
        signal_watch_stop(watch);
    }
    server_destroy(runner.server);
    arena_destroy(scratch);
    return status < 0 ? 0 : status;
}
