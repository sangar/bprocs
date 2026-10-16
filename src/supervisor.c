#include "supervisor.h"

#include <string.h>

#include "check.h"
#include "format.h"
#include "mc/concurrency/queue.h"
#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"

enum {
    PORT_PROBE_MS = 200,
    CHECK_THREADS = 4,
    CHECK_TIMEOUT_MIN_MS = 1000,
    CHECK_TIMEOUT_MAX_MS = 30000,
};

// CheckJob is one try of a check on the pool, in an arena of its own, reported
// back through results. It names its Proc rather than an index, which a
// reload can change while it runs.
typedef struct CheckJob {
    Arena *arena;
    Task task;
    Supervisor *supervisor;
    Proc *proc;
    const ProcSpec *spec;
    bool health;
    int64_t run_started_ns; // the run it checks; a result for an older run is dropped
    int64_t timeout_ns;
    bool passed;
    char problem[PROC_PROBLEM_LIMIT];
} CheckJob;

typedef struct Entry {
    Proc *proc;
    bool checking; // a check of it is on the pool
    int64_t run_started_ns;
    int64_t next_check_ns;
    unsigned health_failures; // in a row
} Entry;

// Retiring is a proc that a reload took out, waiting to end; its successor,
// when it has one, starts once it has.
typedef struct Retiring {
    Proc *proc;
    bool start_successor;
    String successor;
} Retiring;

struct Supervisor {
    Arena *arena;
    Arena *scratch; // reset every tick
    const Config *config;
    ProcHooks hooks;
    Entry *entries;
    ProcStats *stats; // stats[i] belongs to entries[i]
    size_t count;
    ThreadPool *pool;
    Queue *results;
    bool going_down;
    Retiring *retiring;
    size_t retiring_count;
    size_t retiring_capacity;
};

static const ProcSpec *spec_of(const Supervisor *supervisor, size_t index)
{
    return &supervisor->config->procs[index];
}

static ProcStatus status_of(Supervisor *supervisor, size_t index)
{
    return proc_status(supervisor->entries[index].proc);
}

Error supervisor_create(const Config *config, ProcHooks hooks, Supervisor **supervisor, Err *err)
{
    ThreadPool *pool;
    Error e = threadpool_create(CHECK_THREADS, &pool, err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    Supervisor *s = arena_push(arena, sizeof *s);
    s->arena = arena;
    s->scratch = arena_create(0);
    s->config = config;
    s->hooks = hooks;
    s->pool = pool;
    s->results = queue_create();
    s->count = config->proc_count;
    s->entries = arena_push(arena, arena_size_mul(s->count, sizeof *s->entries));
    s->stats = arena_push(arena, arena_size_mul(s->count, sizeof *s->stats));
    for (size_t i = 0; i < s->count; i++) {
        ProcHooks own = hooks;
        own.index = i;
        s->entries[i].proc = proc_create(&config->procs[i], config->scrollback, own);
    }
    *supervisor = s;
    return ERR_OK;
}

void supervisor_destroy(Supervisor *supervisor)
{
    // The pool finishes the checks it holds before the procs they look at go.
    threadpool_destroy(supervisor->pool);
    void *item;
    while (queue_pop_timeout(supervisor->results, 0, &item)) {
        arena_destroy(((CheckJob *)item)->arena);
    }
    queue_destroy(supervisor->results);
    for (size_t i = 0; i < supervisor->count; i++) {
        proc_destroy(supervisor->entries[i].proc);
    }
    for (size_t i = 0; i < supervisor->retiring_count; i++) {
        proc_destroy(supervisor->retiring[i].proc);
    }
    arena_destroy(supervisor->scratch);
    arena_destroy(supervisor->arena);
}

const Config *supervisor_config(const Supervisor *supervisor)
{
    return supervisor->config;
}

size_t supervisor_count(const Supervisor *supervisor)
{
    return supervisor->count;
}

Proc *supervisor_proc(Supervisor *supervisor, size_t index)
{
    return supervisor->entries[index].proc;
}

const ProcStats *supervisor_stats(const Supervisor *supervisor, size_t index)
{
    return &supervisor->stats[index];
}

bool supervisor_going_down(const Supervisor *supervisor)
{
    return supervisor->going_down;
}

// ---- needs ----

bool supervisor_need_met(Supervisor *supervisor, size_t index)
{
    ProcState state = status_of(supervisor, index).state;
    return spec_of(supervisor, index)->job ? state == PROC_EXITED : state == PROC_RUNNING;
}

bool supervisor_need_lost(Supervisor *supervisor, Arena *scratch, size_t index, String *why)
{
    ProcStatus status = status_of(supervisor, index);
    const ProcSpec *spec = spec_of(supervisor, index);
    String name = spec->name;
    if (status.restart_at_ns != 0) {
        return false;
    }
    switch (status.state) {
    case PROC_CRASHED: {
        String signal = format_signal(scratch, status.signal);
        *why = status.signal != 0
                   ? str_format(scratch, "%.*s was ended by %.*s", (int)name.len, name.data, (int)signal.len,
                                signal.data)
                   : str_format(scratch, "%.*s exited with code %d", (int)name.len, name.data, status.exit_code);
        return true;
    }
    case PROC_EXITED:
        if (spec->job) {
            return false;
        }
        *why = str_format(scratch, "%.*s exited", (int)name.len, name.data);
        return true;
    case PROC_IDLE:
    case PROC_STOPPED:
    case PROC_FAILED:
    case PROC_BLOCKED:
        *why = str_format(scratch, "%.*s is %s", (int)name.len, name.data, proc_state_name(status.state));
        return true;
    case PROC_WAITING:
    case PROC_STARTING:
    case PROC_RUNNING:
    case PROC_STOPPING:
        break;
    }
    return false;
}

static bool needs_met(Supervisor *supervisor, size_t index)
{
    const ProcSpec *spec = spec_of(supervisor, index);
    for (size_t i = 0; i < spec->need_count; i++) {
        if (!supervisor_need_met(supervisor, spec->needs[i])) {
            return false;
        }
    }
    return true;
}

static bool port_taken(Arena *scratch, int port)
{
    int fd;
    ArenaMark mark = arena_mark(scratch);
    bool taken = net_connect(S("localhost"), str_format(scratch, "%d", port), PORT_PROBE_MS, &fd, nullptr) == ERR_OK;
    arena_release(mark);
    if (taken) {
        net_close(fd);
    }
    return taken;
}

// launch starts proc index unless something else already holds its port.
static void launch(Supervisor *supervisor, size_t index, int64_t now_ns)
{
    const ProcSpec *spec = spec_of(supervisor, index);
    Proc *proc = supervisor->entries[index].proc;
    if (spec->port > 0 && port_taken(supervisor->scratch, spec->port)) {
        ArenaMark mark = arena_mark(supervisor->scratch);
        proc_refuse(proc, now_ns, str_format(supervisor->scratch, "port %d is already in use", spec->port));
        arena_release(mark);
        return;
    }
    proc_start(proc, now_ns);
}

// start starts index, or has it wait, after starting what it needs that
// neither is up nor is on its way.
static void start(Supervisor *supervisor, size_t index, int64_t now_ns)
{
    ProcState state = status_of(supervisor, index).state;
    if (proc_state_active(state) || state == PROC_WAITING) {
        return;
    }
    const ProcSpec *spec = spec_of(supervisor, index);
    for (size_t i = 0; i < spec->need_count; i++) {
        size_t need = spec->needs[i];
        if (!supervisor_need_met(supervisor, need)) {
            start(supervisor, need, now_ns);
        }
    }
    if (needs_met(supervisor, index)) {
        launch(supervisor, index, now_ns);
    } else {
        proc_wait(supervisor->entries[index].proc);
    }
}

void supervisor_start(Supervisor *supervisor, size_t index, int64_t now_ns)
{
    supervisor->going_down = false;
    start(supervisor, index, now_ns);
}

void supervisor_up(Supervisor *supervisor, int64_t now_ns)
{
    supervisor->going_down = false;
    for (size_t i = 0; i < supervisor->count; i++) {
        if (spec_of(supervisor, i)->autostart) {
            start(supervisor, i, now_ns);
        }
    }
}

void supervisor_stop(Supervisor *supervisor, size_t index, int64_t now_ns)
{
    proc_stop(supervisor->entries[index].proc, now_ns);
}

void supervisor_restart(Supervisor *supervisor, size_t index, int64_t now_ns)
{
    supervisor->going_down = false;
    if (proc_state_active(status_of(supervisor, index).state)) {
        proc_restart(supervisor->entries[index].proc, now_ns);
    } else {
        start(supervisor, index, now_ns);
    }
}

void supervisor_kill(Supervisor *supervisor, size_t index)
{
    proc_kill(supervisor->entries[index].proc);
}

// start_waiting starts each waiting proc whose needs are met, blocks the ones
// whose needs cannot be met, and lets blocked ones wait again once they can.
static bool start_waiting(Supervisor *supervisor, Arena *scratch, int64_t now_ns)
{
    bool changed = false;
    for (size_t i = 0; i < supervisor->count; i++) {
        ProcState state = status_of(supervisor, i).state;
        if (state != PROC_WAITING && state != PROC_BLOCKED) {
            continue;
        }
        Proc *proc = supervisor->entries[i].proc;
        if (needs_met(supervisor, i)) {
            launch(supervisor, i, now_ns);
            changed = true;
            continue;
        }
        const ProcSpec *spec = spec_of(supervisor, i);
        String why = S("");
        bool lost = false;
        for (size_t n = 0; n < spec->need_count && !lost; n++) {
            lost = supervisor_need_lost(supervisor, scratch, spec->needs[n], &why);
        }
        if (lost && state == PROC_WAITING) {
            proc_block(proc, why);
            changed = true;
        } else if (!lost && state == PROC_BLOCKED) {
            proc_wait(proc);
            changed = true;
        }
    }
    return changed;
}

// ---- going down ----

static bool needed_by_active(Supervisor *supervisor, size_t index)
{
    for (size_t i = 0; i < supervisor->count; i++) {
        const ProcSpec *spec = spec_of(supervisor, i);
        if (!proc_state_active(status_of(supervisor, i).state)) {
            continue;
        }
        for (size_t n = 0; n < spec->need_count; n++) {
            if (spec->needs[n] == index) {
                return true;
            }
        }
    }
    return false;
}

void supervisor_down(Supervisor *supervisor, int64_t now_ns)
{
    if (supervisor->going_down) {
        for (size_t i = 0; i < supervisor->count; i++) {
            proc_kill(supervisor->entries[i].proc);
        }
        return;
    }
    supervisor->going_down = true;
    for (size_t i = 0; i < supervisor->count; i++) {
        // Waiting procs and pending restarts are called off; what runs stops in order.
        if (!proc_state_active(status_of(supervisor, i).state)) {
            proc_stop(supervisor->entries[i].proc, now_ns);
        }
    }
}

static bool step_down(Supervisor *supervisor, int64_t now_ns)
{
    bool changed = false;
    for (size_t i = 0; i < supervisor->count; i++) {
        ProcStatus status = status_of(supervisor, i);
        bool running = status.state == PROC_STARTING || status.state == PROC_RUNNING;
        bool pending = !proc_state_active(status.state) &&
                       (status.restart_at_ns != 0 || status.state == PROC_WAITING || status.state == PROC_BLOCKED);
        if ((running && !needed_by_active(supervisor, i)) || pending) {
            proc_stop(supervisor->entries[i].proc, now_ns);
            changed = true;
        }
    }
    return changed;
}

bool supervisor_settled(Supervisor *supervisor)
{
    if (supervisor->retiring_count > 0) {
        return false;
    }
    for (size_t i = 0; i < supervisor->count; i++) {
        ProcStatus status = status_of(supervisor, i);
        if (proc_state_active(status.state) || status.state == PROC_WAITING || status.restart_at_ns != 0) {
            return false;
        }
    }
    return true;
}

// ---- checks ----

static void run_check(void *argument)
{
    CheckJob *job = argument;
    Supervisor *supervisor = job->supervisor;
    const ProcSpec *spec = job->spec;
    String problem;
    job->passed =
        check_run(job->arena, job->health ? &spec->health : &spec->ready, spec, job->timeout_ns, &problem);
    size_t len = min_size(problem.len, sizeof job->problem - 1);
    memcpy(job->problem, problem.data, len);
    job->problem[len] = '\0';
    if (queue_push(supervisor->results, job)) {
        supervisor->hooks.wake(supervisor->hooks.context);
    } else {
        arena_destroy(job->arena);
    }
}

static int64_t check_timeout(const Check *check)
{
    int64_t ns = check->every_ns;
    int64_t low = (int64_t)CHECK_TIMEOUT_MIN_MS * NS_PER_MILLISECOND;
    int64_t high = (int64_t)CHECK_TIMEOUT_MAX_MS * NS_PER_MILLISECOND;
    return ns < low ? low : ns > high ? high : ns;
}

static void submit_check(Supervisor *supervisor, size_t index, bool health, int64_t now_ns)
{
    Entry *entry = &supervisor->entries[index];
    const ProcSpec *spec = spec_of(supervisor, index);
    const Check *check = health ? &spec->health : &spec->ready;
    Arena *arena = arena_create(0);
    CheckJob *job = arena_push(arena, sizeof *job);
    *job = (CheckJob) {
        .arena = arena,
        .supervisor = supervisor,
        .proc = entry->proc,
        .spec = spec,
        .health = health,
        .run_started_ns = entry->run_started_ns,
        .timeout_ns = check_timeout(check),
    };
    job->task = (Task) { .run = run_check, .argument = job };
    entry->checking = true;
    entry->next_check_ns = now_ns + check->every_ns;
    threadpool_submit(supervisor->pool, &job->task);
}

static bool entry_of(Supervisor *supervisor, const Proc *proc, size_t *index)
{
    for (size_t i = 0; i < supervisor->count; i++) {
        if (supervisor->entries[i].proc == proc) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void take_result(Supervisor *supervisor, CheckJob *job, int64_t now_ns)
{
    size_t index;
    if (!entry_of(supervisor, job->proc, &index)) {
        return; // a reload retired the proc meanwhile
    }
    Entry *entry = &supervisor->entries[index];
    entry->checking = false;
    ProcStatus status = proc_status(entry->proc);
    if (job->run_started_ns != status.started_ns) {
        return;
    }
    String problem = S(job->problem);
    if (!job->health) {
        if (status.state != PROC_STARTING) {
            return;
        }
        if (job->passed) {
            proc_mark_ready(entry->proc, now_ns);
        } else {
            proc_set_health(entry->proc, HEALTH_UNKNOWN, problem);
        }
        return;
    }
    if (status.state != PROC_RUNNING) {
        return;
    }
    const ProcSpec *spec = spec_of(supervisor, index);
    if (job->passed) {
        entry->health_failures = 0;
        proc_set_health(entry->proc, HEALTH_HEALTHY, S(""));
        return;
    }
    entry->health_failures++;
    if (entry->health_failures < spec->health.failures) {
        proc_set_health(entry->proc, status.health, problem);
        return;
    }
    entry->health_failures = 0;
    proc_set_health(entry->proc, HEALTH_UNHEALTHY, problem);
    if (spec->restart != RESTART_NO) {
        proc_fail(entry->proc, now_ns, S("failed its health check"));
    }
}

// run_checks starts the checks that are due and fails a start that took too long.
static bool run_checks(Supervisor *supervisor, Arena *scratch, int64_t now_ns)
{
    bool changed = false;
    for (size_t i = 0; i < supervisor->count; i++) {
        Entry *entry = &supervisor->entries[i];
        const ProcSpec *spec = spec_of(supervisor, i);
        ProcStatus status = proc_status(entry->proc);
        if (status.started_ns != entry->run_started_ns) {
            entry->run_started_ns = status.started_ns;
            entry->next_check_ns = now_ns;
            entry->health_failures = 0;
        }
        if (status.state == PROC_STARTING && spec->ready.timeout_ns > 0 &&
            now_ns - status.started_ns >= spec->ready.timeout_ns) {
            String limit = fmt_duration(scratch, spec->ready.timeout_ns);
            proc_fail(entry->proc, now_ns,
                      str_format(scratch, "not ready within %.*s", (int)limit.len, limit.data));
            changed = true;
            continue;
        }
        if (entry->checking || now_ns < entry->next_check_ns) {
            continue;
        }
        bool ready_due = status.state == PROC_STARTING && spec->ready.kind != CHECK_NONE &&
                         spec->ready.kind != CHECK_LOG;
        bool health_due = status.state == PROC_RUNNING && spec->health.kind != CHECK_NONE;
        if (ready_due || health_due) {
            submit_check(supervisor, i, health_due, now_ns);
        }
    }
    return changed;
}

// ---- reloading ----

static ProcHooks hooks_for(const Supervisor *supervisor, size_t index)
{
    ProcHooks hooks = supervisor->hooks;
    hooks.index = index;
    return hooks;
}

// retire_proc stops a proc a reload took out; nobody hears of it any more.
static void retire_proc(Supervisor *supervisor, Proc *proc, String successor, bool start_successor, int64_t now_ns)
{
    proc_rehook(proc, (ProcHooks) { .wake = supervisor->hooks.wake, .context = supervisor->hooks.context });
    proc_stop(proc, now_ns);
    supervisor->retiring = arena_grow(supervisor->arena, supervisor->retiring, &supervisor->retiring_capacity,
                                      supervisor->retiring_count, sizeof *supervisor->retiring);
    supervisor->retiring[supervisor->retiring_count++] = (Retiring) {
        .proc = proc,
        .start_successor = start_successor,
        .successor = successor,
    };
}

// retire ends the retiring procs that have stopped and starts their successors.
static bool retire(Supervisor *supervisor, int64_t now_ns)
{
    bool changed = false;
    for (size_t i = 0; i < supervisor->retiring_count;) {
        Retiring retiring = supervisor->retiring[i];
        proc_tick(retiring.proc, now_ns);
        if (proc_state_active(proc_status(retiring.proc).state)) {
            i++;
            continue;
        }
        proc_destroy(retiring.proc);
        supervisor->retiring[i] = supervisor->retiring[--supervisor->retiring_count];
        size_t successor;
        if (retiring.start_successor && !supervisor->going_down &&
            config_proc_index(supervisor->config, retiring.successor, &successor)) {
            start(supervisor, successor, now_ns);
        }
        changed = true;
    }
    return changed;
}

static bool old_index(const Supervisor *supervisor, String name, size_t *index)
{
    return config_proc_index(supervisor->config, name, index);
}

String supervisor_reload(Supervisor *supervisor, Arena *arena, const Config *next, int64_t now_ns)
{
    size_t count = next->proc_count;
    Entry *entries = arena_push(supervisor->arena, arena_size_mul(count, sizeof *entries));
    ProcStats *stats = arena_push(supervisor->arena, arena_size_mul(count, sizeof *stats));
    bool *kept = arena_push(arena, arena_size_mul(supervisor->count + 1, sizeof *kept));
    StringList added = { 0 };
    StringList changed = { 0 };
    StringList removed = { 0 };
    StringList start_now = { 0 };
    for (size_t i = 0; i < count; i++) {
        const ProcSpec *spec = &next->procs[i];
        size_t was;
        bool existed = old_index(supervisor, spec->name, &was);
        if (existed && config_spec_runs_same(spec_of(supervisor, was), spec)) {
            entries[i] = supervisor->entries[was];
            stats[i] = supervisor->stats[was];
            kept[was] = true;
            proc_rehook(entries[i].proc, hooks_for(supervisor, i));
            continue;
        }
        entries[i] = (Entry) { .proc = proc_create(spec, next->scrollback, hooks_for(supervisor, i)) };
        stats[i] = (ProcStats) { 0 };
        if (!existed) {
            strlist_push(arena, &added, spec->name);
            if (spec->autostart) {
                strlist_push(arena, &start_now, spec->name);
            }
            continue;
        }
        kept[was] = true;
        strlist_push(arena, &changed, spec->name);
        ProcState state = status_of(supervisor, was).state;
        bool wanted = proc_state_active(state) || state == PROC_WAITING || state == PROC_BLOCKED;
        retire_proc(supervisor, supervisor->entries[was].proc, str_copy(supervisor->arena, spec->name), wanted, now_ns);
    }
    for (size_t i = 0; i < supervisor->count; i++) {
        if (!kept[i]) {
            strlist_push(arena, &removed, spec_of(supervisor, i)->name);
            retire_proc(supervisor, supervisor->entries[i].proc, S(""), false, now_ns);
        }
    }
    supervisor->entries = entries;
    supervisor->stats = stats;
    supervisor->count = count;
    supervisor->config = next;
    for (size_t i = 0; i < start_now.count; i++) {
        size_t index;
        if (config_proc_index(next, start_now.items[i], &index)) {
            start(supervisor, index, now_ns);
        }
    }
    StringList parts = { 0 };
    const struct {
        const char *label;
        StringList names;
    } GROUPS[] = { { "added", added }, { "changed", changed }, { "removed", removed } };
    for (size_t i = 0; i < countof(GROUPS); i++) {
        if (GROUPS[i].names.count > 0) {
            String names = str_join(arena, GROUPS[i].names, S(", "));
            strlist_push(arena, &parts, str_format(arena, "%s %.*s", GROUPS[i].label, (int)names.len, names.data));
        }
    }
    return parts.count > 0 ? str_join(arena, parts, S(" · ")) : S("nothing changed");
}

// ---- the tick ----

bool supervisor_tick(Supervisor *supervisor, int64_t now_ns)
{
    Arena *scratch = supervisor->scratch;
    arena_reset(scratch);
    bool changed = false;
    void *item;
    while (queue_pop_timeout(supervisor->results, 0, &item)) {
        CheckJob *job = item;
        take_result(supervisor, job, now_ns);
        arena_destroy(job->arena);
        changed = true;
    }
    changed = retire(supervisor, now_ns) || changed;
    for (size_t i = 0; i < supervisor->count; i++) {
        changed = proc_tick(supervisor->entries[i].proc, now_ns) || changed;
    }
    changed = run_checks(supervisor, scratch, now_ns) || changed;
    if (supervisor->going_down) {
        changed = step_down(supervisor, now_ns) || changed;
    } else {
        changed = start_waiting(supervisor, scratch, now_ns) || changed;
    }
    return changed;
}

void supervisor_sample(Supervisor *supervisor, Arena *scratch, int64_t now_ns)
{
    int *pgids = arena_push(scratch, arena_size_mul(supervisor->count, sizeof *pgids));
    for (size_t i = 0; i < supervisor->count; i++) {
        ProcStatus status = status_of(supervisor, i);
        pgids[i] = proc_state_active(status.state) ? status.pid : 0;
    }
    proc_stats_sample(supervisor->stats, pgids, supervisor->count, scratch, now_ns);
}
