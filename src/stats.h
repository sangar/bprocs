#pragma once

#include "history.h"
#include "mc/platform/sysinfo.h"

// SystemStats is the history of the machine, one sample a second.
// SystemStats stats = { 0 }; has seen nothing yet.
typedef struct SystemStats {
    SystemSample previous;
    int64_t previous_ns;
    bool has_previous;
    History cpu; // busy share of every core, 0 to 1
    size_t core_count;
    float cores[SYSINFO_MAX_CORES];
    History memory; // share in use, 0 to 1
    uint64_t memory_used;
    uint64_t memory_total;
    uint64_t swap_used;
    uint64_t swap_total;
    History download; // bytes per second
    History upload;
    double load[3];
    int64_t uptime_seconds;
} SystemStats;

void system_stats_sample(SystemStats *stats, Arena *scratch, int64_t now_ns);

// ProcStats is the history of one supervised process group.
typedef struct ProcStats {
    History cpu;    // share of one core: 1 is 100%, several cores make more
    History memory; // resident bytes
    float cpu_now;
    uint64_t memory_now;
    size_t processes;
    size_t threads;
    int pgid;
    uint64_t cpu_ns;
    int64_t sampled_ns;
} ProcStats;

// proc_stats_sample samples the groups pgids[i] into stats[i], where 0 is a
// process that does not run, in one pass over the system's processes.
void proc_stats_sample(ProcStats *stats, const int *pgids, size_t count, Arena *scratch, int64_t now_ns);
