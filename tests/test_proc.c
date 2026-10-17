#include "test.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "proc.h"

enum { DEADLINE_MS = 5000 };

static void ignore(void *context)
{
    unused(context);
}

static ProcSpec shell_spec(Arena *arena, const char *script)
{
    const char *const argv[] = { "/bin/sh", "-c", script };
    return (ProcSpec) {
        .name = S("test"),
        .argv = strlist_of(arena, countof(argv), argv),
        .command = S(script),
        .stop = PTY_TERMINATE,
        .stop_timeout_ns = (int64_t)5 * NS_PER_SECOND,
    };
}

// wait_for ticks proc until its state is wanted and returns that status.
static ProcStatus wait_for(Proc *proc, ProcState wanted)
{
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    ProcStatus status = proc_status(proc);
    while (status.state != wanted && clock_monotonic_ns() < deadline) {
        clock_sleep_ms(10);
        proc_tick(proc, clock_monotonic_ns());
        status = proc_status(proc);
    }
    return status;
}

static bool log_contains(Test *test, Proc *proc, const char *text)
{
    bool found = false;
    proc_lock(proc);
    const LogBuffer *log = proc_log(proc);
    for (uint64_t n = logbuf_first(log); n < logbuf_end(log) && !found; n++) {
        found = str_contains(logbuf_plain(test->arena, logbuf_line(log, n)), S(text));
    }
    proc_unlock(proc);
    return found;
}

static void proc_reports_how_a_run_ended(Test *test)
{
    ProcSpec spec = shell_spec(test->arena, "printf '\\033[32mhello\\033[0m\\n'; exit 3");
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    ProcStatus status = wait_for(proc, PROC_CRASHED);
    test_check(test, status.state == PROC_CRASHED && status.exit_code == 3, "a non-zero exit is a crash");
    test_check(test, log_contains(test, proc, "hello"), "the output is in the log");
    test_check(test, log_contains(test, proc, "exited with code 3"), "and so is how it ended");
    proc_destroy(proc);

    spec = shell_spec(test->arena, "exit 0");
    proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    test_check(test, wait_for(proc, PROC_EXITED).state == PROC_EXITED, "status 0 is a clean exit");
    proc_destroy(proc);

    spec = shell_spec(test->arena, "true");
    spec.argv.items[0] = S("/no/such/shell");
    proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    test_check(test, proc_status(proc).state == PROC_FAILED, "a program that cannot start fails");
    test_check(test, log_contains(test, proc, "could not start"), "and says why");
    proc_destroy(proc);
}

static void proc_waits_for_ready_and_stops(Test *test)
{
    ProcSpec spec = shell_spec(test->arena, "echo booting; sleep 0.2; echo 'Listening on 3000'; sleep 30");
    spec.ready = (Check) { .kind = CHECK_LOG, .text = S("Listening on") };
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    test_check(test, proc_status(proc).state == PROC_STARTING, "starting until the ready output");
    ProcStatus status = wait_for(proc, PROC_RUNNING);
    test_check(test, status.state == PROC_RUNNING && status.ready_ns > status.started_ns, "then running");
    proc_stop(proc, clock_monotonic_ns());
    test_check(test, proc_status(proc).state == PROC_STOPPING, "stopping");
    test_check(test, wait_for(proc, PROC_STOPPED).state == PROC_STOPPED, "a stop it asked for is not a crash");
    proc_destroy(proc);
}

static void proc_stop_cancels_a_pending_restart(Test *test)
{
    ProcSpec spec = shell_spec(test->arena, "sleep 30");
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    proc_restart(proc, clock_monotonic_ns());
    proc_stop(proc, clock_monotonic_ns());
    test_check(test, wait_for(proc, PROC_STOPPED).state == PROC_STOPPED, "the restart ends in a stop");
    for (int i = 0; i < 20; i++) {
        proc_tick(proc, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, proc_status(proc).state == PROC_STOPPED, "and does not start it again");
    proc_destroy(proc);
}

static void proc_finishes_while_a_stray_process_writes(Test *test)
{
    // The stray leaves the group and keeps writing to the terminal. Linux, unlike
    // macOS, leaves the terminal writable after the program ends, so the run
    // must end without waiting for the stray to stop.
    ProcSpec spec = shell_spec(test->arena, "perl -MPOSIX -e 'POSIX::setsid(); $| = 1; "
                                            "while (print \"x\\n\") { select(undef, undef, undef, 0.01) }' & "
                                            "sleep 0.3; exit 0");
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    ProcStatus status = wait_for(proc, PROC_EXITED);
    test_check(test, status.state == PROC_EXITED && status.pid == 0, "the run ends when the program does");
    proc_destroy(proc);
}

static void proc_restarts_what_crashes(Test *test)
{
    ProcSpec spec = shell_spec(test->arena, "echo run; exit 1");
    spec.restart = RESTART_ON_FAILURE;
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    wait_for(proc, PROC_CRASHED);
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (proc_status(proc).restart_at_ns == 0 && clock_monotonic_ns() < deadline) {
        proc_tick(proc, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, proc_status(proc).restart_at_ns != 0, "a crash schedules a restart");
    while (proc_status(proc).restarts == 0 && clock_monotonic_ns() < deadline) {
        proc_tick(proc, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, proc_status(proc).restarts == 1, "which comes after a pause");
    proc_stop(proc, clock_monotonic_ns());
    proc_destroy(proc);
}

static void proc_passes_input_to_the_program(Test *test)
{
    ProcSpec spec = shell_spec(test->arena, "read line; echo \"got $line\"");
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    proc_send(proc, S("ping\r"));
    wait_for(proc, PROC_EXITED);
    test_check(test, log_contains(test, proc, "got ping"), "the program read what was sent");
    proc_destroy(proc);
}

static void proc_writes_its_log_file(Test *test)
{
    Arena *a = test->arena;
    String dir;
    test_check(test, dir_create_temp(a, S("bprocs-test-"), &dir, nullptr) == ERR_OK, "a temp dir");
    ProcSpec spec = shell_spec(a, "printf '\\033[31mred\\033[0m line\\n'");
    spec.log_path = path_join(a, dir, S("logs/test.log"));
    Proc *proc = proc_create(&spec, 100, (ProcHooks) { .wake = ignore });
    proc_start(proc, clock_monotonic_ns());
    wait_for(proc, PROC_EXITED);
    proc_destroy(proc);
    String text = S("");
    test_check(test, file_read_all(a, spec.log_path, &text, nullptr) == ERR_OK, "the file and its directory are made");
    test_check(test, str_contains(text, S(" red line\n")), "lines without their escape sequences");
    test_check(test, str_contains(text, S("exited")), "and bprocs's own notes");
    test_check(test, !str_contains(text, S("\x1b")), "no escape sequences at all");
    test_check(test, dir_remove_all(dir, nullptr) == ERR_OK, "clean up");
}

static void proc_status_failed_only_without_a_restart_coming(Test *test)
{
    test_check(test, proc_status_failed((ProcStatus) { .state = PROC_CRASHED }), "a crash is a failure");
    test_check(test, proc_status_failed((ProcStatus) { .state = PROC_BLOCKED }), "so is being blocked");
    test_check(test, !proc_status_failed((ProcStatus) { .state = PROC_CRASHED, .restart_at_ns = 1 }),
               "a crash with a restart coming is not");
    test_check(test, !proc_status_failed((ProcStatus) { .state = PROC_EXITED }), "nor is exiting with 0");
}

const TestCase PROC_TESTS[] = {
    { "proc_reports_how_a_run_ended", proc_reports_how_a_run_ended },
    { "proc_waits_for_ready_and_stops", proc_waits_for_ready_and_stops },
    { "proc_stop_cancels_a_pending_restart", proc_stop_cancels_a_pending_restart },
    { "proc_finishes_while_a_stray_process_writes", proc_finishes_while_a_stray_process_writes },
    { "proc_restarts_what_crashes", proc_restarts_what_crashes },
    { "proc_passes_input_to_the_program", proc_passes_input_to_the_program },
    { "proc_writes_its_log_file", proc_writes_its_log_file },
    { "proc_status_failed_only_without_a_restart_coming", proc_status_failed_only_without_a_restart_coming },
    { nullptr, nullptr },
};
