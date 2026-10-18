#include "group.h"

size_t group_find(StringList names, Group *groups)
{
    size_t count = 0;
    for (size_t i = 0; i < names.count; i++) {
        String app;
        String rest;
        if (!str_cut(names.items[i], ':', &app, &rest)) {
            continue;
        }
        Group *last = count > 0 ? &groups[count - 1] : nullptr;
        if (last != nullptr && str_equal(last->name, app) && last->first + last->count == i) {
            last->count++;
            continue;
        }
        groups[count++] = (Group) { .name = app, .first = i, .count = 1 };
    }
    return count;
}

size_t group_rows(const Group *groups, size_t group_count, size_t proc_count, Row *rows)
{
    size_t count = 0;
    size_t group = 0;
    for (size_t i = 0; i < proc_count;) {
        if (group == group_count || groups[group].first != i) {
            rows[count++] = (Row) { .proc = i++ };
            continue;
        }
        const Group *g = &groups[group];
        rows[count++] = (Row) { .header = true, .grouped = true, .group = group, .proc = i };
        for (size_t j = 0; g->expanded && j < g->count; j++) {
            rows[count++] = (Row) { .grouped = true, .group = group, .proc = i + j };
        }
        i += g->count;
        group++;
    }
    return count;
}

String group_short_name(String name)
{
    String app;
    String rest;
    return str_cut(name, ':', &app, &rest) ? rest : name;
}

GroupRank group_rank(ProcStatus status)
{
    if (status.restart_at_ns != 0) {
        return RANK_RESTARTING;
    }
    switch (status.state) {
    case PROC_CRASHED:
    case PROC_FAILED:
    case PROC_BLOCKED:
        return RANK_FAILED;
    case PROC_RUNNING:
        return status.health == HEALTH_UNHEALTHY ? RANK_UNHEALTHY : RANK_RUNNING;
    case PROC_STARTING:
    case PROC_STOPPING:
    case PROC_WAITING:
        return RANK_CHANGING;
    case PROC_STOPPED:
        return RANK_STOPPED;
    case PROC_IDLE:
        return RANK_IDLE;
    case PROC_EXITED:
        return RANK_DONE;
    }
    return RANK_DONE;
}
