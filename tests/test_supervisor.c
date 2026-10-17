#include "test.h"

#include "config.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "supervisor.h"

enum { DEADLINE_MS = 8000 };

static void ignore(void *context)
{
    unused(context);
}

// Fixture is a supervisor for a config written in a temporary directory.
typedef struct Fixture {
    String dir;
    Config config;
    Supervisor *supervisor;
} Fixture;

static bool setup(Test *test, Fixture *fixture, const char *yaml)
{
    Err err = { 0 };
    *fixture = (Fixture) { 0 };
    test_check(test, dir_create_temp(test->arena, S("bprocs-test-"), &fixture->dir, &err) == ERR_OK, "a temp dir");
    String path = path_join(test->arena, fixture->dir, S("bprocs.yml"));
    String text = str_replace_all(test->arena, S(yaml), S("$DIR"), fixture->dir);
    bool parsed = config_parse(test->arena, text, path, 0, &fixture->config, &err) == ERR_OK;
    test_check(test, parsed, err.msg);
    bool created = parsed && supervisor_create(&fixture->config, (ProcHooks) { .wake = ignore }, &fixture->supervisor,
                                               &err) == ERR_OK;
    test_check(test, !parsed || created, err.msg);
    return created;
}

static void teardown(Test *test, Fixture *fixture)
{
    supervisor_destroy(fixture->supervisor);
    test_check(test, dir_remove_all(fixture->dir, nullptr) == ERR_OK, "clean up");
}

static ProcStatus status_of(Fixture *fixture, size_t index)
{
    return proc_status(supervisor_proc(fixture->supervisor, index));
}

// tick_until ticks until proc index is in state, or the deadline passes, and returns its status.
static ProcStatus tick_until(Fixture *fixture, size_t index, ProcState state)
{
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (status_of(fixture, index).state != state && clock_monotonic_ns() < deadline) {
        supervisor_tick(fixture->supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    return status_of(fixture, index);
}

static void needs_start_in_order(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  db:\n"
               "    run: sleep 0.3; echo ready to go; sleep 30\n"
               "    ready: { log: ready to go }\n"
               "  migrate:\n"
               "    run: sleep 0.2\n"
               "    job: true\n"
               "    needs: [db]\n"
               "  web:\n"
               "    run: sleep 30\n"
               "    needs: [db, migrate]\n"
               "    autostart: false\n")) {
        return;
    }
    supervisor_start(f.supervisor, 2, clock_monotonic_ns());
    test_check(test, status_of(&f, 0).state == PROC_STARTING, "starting web starts db");
    test_check(test, status_of(&f, 1).state == PROC_WAITING, "migrate waits for db");
    test_check(test, status_of(&f, 2).state == PROC_WAITING, "web waits for both");
    ProcStatus web = tick_until(&f, 2, PROC_RUNNING);
    ProcStatus db = status_of(&f, 0);
    ProcStatus migrate = status_of(&f, 1);
    test_check(test, web.state == PROC_RUNNING, "web comes up");
    test_check(test, migrate.state == PROC_EXITED && migrate.started_ns >= db.ready_ns, "migrate ran once db was ready");
    test_check(test, web.started_ns >= migrate.ended_ns, "web started once migrate had succeeded");
    supervisor_down(f.supervisor, clock_monotonic_ns());
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (!supervisor_settled(f.supervisor) && clock_monotonic_ns() < deadline) {
        supervisor_tick(f.supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, supervisor_settled(f.supervisor), "down settles");
    teardown(test, &f);
}

static void a_failed_job_blocks_what_needs_it(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  migrate: { run: sleep 0.2; exit 3, job: true }\n"
               "  web: { run: sleep 30, needs: [migrate] }\n")) {
        return;
    }
    supervisor_up(f.supervisor, clock_monotonic_ns());
    ProcStatus web = tick_until(&f, 1, PROC_BLOCKED);
    test_check(test, web.state == PROC_BLOCKED, "web is blocked");
    test_check_str(test, S(web.problem), S("migrate exited with code 3"), "and says why");
    supervisor_start(f.supervisor, 0, clock_monotonic_ns());
    supervisor_tick(f.supervisor, clock_monotonic_ns());
    test_check(test, status_of(&f, 1).state == PROC_WAITING, "running the job again lets web wait again");
    teardown(test, &f);
}

static void down_stops_dependents_first(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  db:\n"
               "    run: trap 'echo db >> $DIR/order; exit 0' TERM; while :; do sleep 0.05; done\n"
               "  web:\n"
               "    run: trap 'sleep 0.3; echo web >> $DIR/order; exit 0' TERM; while :; do sleep 0.05; done\n"
               "    needs: [db]\n")) {
        return;
    }
    supervisor_up(f.supervisor, clock_monotonic_ns());
    tick_until(&f, 1, PROC_RUNNING);
    clock_sleep_ms(200); // the shells set their traps
    supervisor_down(f.supervisor, clock_monotonic_ns());
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (!supervisor_settled(f.supervisor) && clock_monotonic_ns() < deadline) {
        supervisor_tick(f.supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    String order = S("");
    Error read = file_read_all(test->arena, path_join(test->arena, f.dir, S("order")), &order, nullptr);
    test_check(test, read == ERR_OK, "both wrote down when they stopped");
    test_check_str(test, order, S("web\ndb\n"), "web stopped before db");
    teardown(test, &f);
}

static void checks_decide_ready_and_health(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  db:\n"
               "    run: sleep 0.3; touch $DIR/up; sleep 30\n"
               "    ready: { cmd: test -f $DIR/up, every: 100ms }\n"
               "  web:\n"
               "    run: sleep 30\n"
               "    autostart: false\n"
               "    ready: { cmd: \"false\", every: 100ms, timeout: 600ms }\n"
               "  worker:\n"
               "    run: sleep 30\n"
               "    autostart: false\n"
               "    restart: on-failure\n"
               "    health: { cmd: \"false\", every: 100ms, failures: 2 }\n")) {
        return;
    }
    supervisor_up(f.supervisor, clock_monotonic_ns());
    test_check(test, status_of(&f, 0).state == PROC_STARTING, "starting until the check passes");
    ProcStatus db = tick_until(&f, 0, PROC_RUNNING);
    test_check(test, db.state == PROC_RUNNING && db.ready_ns - db.started_ns >= 300 * NS_PER_MILLISECOND,
               "ready once the command passed");

    supervisor_start(f.supervisor, 1, clock_monotonic_ns());
    ProcStatus web = tick_until(&f, 1, PROC_CRASHED);
    test_check(test, web.state == PROC_CRASHED, "a start that is not ready in time fails");
    test_check_str(test, S(web.problem), S("not ready within 600ms"), "and says why");

    supervisor_start(f.supervisor, 2, clock_monotonic_ns());
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (status_of(&f, 2).restart_at_ns == 0 && status_of(&f, 2).restarts == 0 &&
           clock_monotonic_ns() < deadline) {
        supervisor_tick(f.supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    ProcStatus worker = status_of(&f, 2);
    test_check(test, worker.restart_at_ns != 0 || worker.restarts > 0, "failing health restarts it");
    test_check(test, worker.health == HEALTH_UNHEALTHY || worker.restarts > 0, "after it was unhealthy");
    teardown(test, &f);
}

static void restart_policies_decide_what_comes_back(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  once: { run: exit 0, restart: on-failure }\n"
               "  again: { run: exit 0, restart: always }\n")) {
        return;
    }
    supervisor_up(f.supervisor, clock_monotonic_ns());
    tick_until(&f, 0, PROC_EXITED);
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (status_of(&f, 1).restart_at_ns == 0 && clock_monotonic_ns() < deadline) {
        supervisor_tick(f.supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, status_of(&f, 0).restart_at_ns == 0, "on-failure leaves a clean exit alone");
    test_check(test, status_of(&f, 1).restart_at_ns != 0, "always starts it again");
    teardown(test, &f);
}

static void reload_keeps_what_runs_the_same(Test *test)
{
    Fixture f;
    if (!setup(test, &f,
               "procs:\n"
               "  same: { run: sleep 30 }\n"
               "  changed: { run: sleep 30 }\n"
               "  gone: { run: sleep 30 }\n")) {
        return;
    }
    supervisor_up(f.supervisor, clock_monotonic_ns());
    tick_until(&f, 2, PROC_RUNNING);
    int same_pid = status_of(&f, 0).pid;
    Config next;
    Err err = { 0 };
    String text = S("procs:\n"
                    "  added: { run: sleep 30 }\n"
                    "  same: { run: sleep 30, color: \"#ff0000\" }\n"
                    "  changed: { run: sleep 31 }\n");
    test_check(test, config_parse(test->arena, text, f.config.path, 0, &next, &err) == ERR_OK, err.msg);
    String summary = supervisor_reload(f.supervisor, test->arena, &next, clock_monotonic_ns());
    test_check_str(test, summary, S("added added · changed changed · removed gone"), "what changed");
    test_check(test, supervisor_count(f.supervisor) == 3, "the new procs, in the new order");
    test_check(test, status_of(&f, 1).pid == same_pid, "a proc that runs the same keeps running");
    ProcStatus changed = tick_until(&f, 2, PROC_RUNNING);
    test_check(test, changed.state == PROC_RUNNING && changed.pid != same_pid, "a changed one starts again");
    test_check(test, tick_until(&f, 0, PROC_RUNNING).state == PROC_RUNNING, "an added autostart proc starts");
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    supervisor_down(f.supervisor, clock_monotonic_ns());
    while (!supervisor_settled(f.supervisor) && clock_monotonic_ns() < deadline) {
        supervisor_tick(f.supervisor, clock_monotonic_ns());
        clock_sleep_ms(10);
    }
    test_check(test, supervisor_settled(f.supervisor), "the gone and the old ones end too");
    teardown(test, &f);
}

const TestCase SUPERVISOR_TESTS[] = {
    { "needs_start_in_order", needs_start_in_order },
    { "a_failed_job_blocks_what_needs_it", a_failed_job_blocks_what_needs_it },
    { "down_stops_dependents_first", down_stops_dependents_first },
    { "checks_decide_ready_and_health", checks_decide_ready_and_health },
    { "restart_policies_decide_what_comes_back", restart_policies_decide_what_comes_back },
    { "reload_keeps_what_runs_the_same", reload_keeps_what_runs_the_same },
    { nullptr, nullptr },
};
