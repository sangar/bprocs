#include "stats.h"

#include "mc/core/arena.h"

static float busy_share(CpuTicks before, CpuTicks after)
{
    if (after.total <= before.total || after.busy < before.busy) {
        return 0.0f;
    }
    float share = (float)(after.busy - before.busy) / (float)(after.total - before.total);
    return share > 1.0f ? 1.0f : share;
}

static float rate(uint64_t before, uint64_t after, double seconds)
{
    return after >= before && seconds > 0.0 ? (float)((double)(after - before) / seconds) : 0.0f;
}

void system_stats_sample(SystemStats *stats, Arena *scratch, int64_t now_ns)
{
    SystemSample sample;
    if (system_sample(scratch, &sample, nullptr) != ERR_OK) {
        return;
    }
    if (stats->has_previous) {
        double seconds = (double)(now_ns - stats->previous_ns) / (double)NS_PER_SECOND;
        history_push(&stats->cpu, busy_share(stats->previous.cpu, sample.cpu));
        stats->core_count = min_size(sample.core_count, SYSINFO_MAX_CORES);
        for (size_t i = 0; i < stats->core_count; i++) {
            stats->cores[i] = busy_share(stats->previous.cores[i], sample.cores[i]);
        }
        history_push(&stats->download,
                     rate(stats->previous.network_received_bytes, sample.network_received_bytes, seconds));
        history_push(&stats->upload, rate(stats->previous.network_sent_bytes, sample.network_sent_bytes, seconds));
    }
    stats->memory_used = sample.memory_used_bytes;
    stats->memory_total = sample.memory_total_bytes;
    stats->swap_used = sample.swap_used_bytes;
    stats->swap_total = sample.swap_total_bytes;
    float memory_share = sample.memory_total_bytes > 0
                             ? (float)((double)sample.memory_used_bytes / (double)sample.memory_total_bytes)
                             : 0.0f;
    history_push(&stats->memory, memory_share);
    for (size_t i = 0; i < countof(stats->load); i++) {
        stats->load[i] = sample.load_average[i];
    }
    stats->uptime_seconds = sample.uptime_seconds;
    stats->previous = sample;
    stats->previous_ns = now_ns;
    stats->has_previous = true;
}

static void record(ProcStats *stats, GroupSample sample, int pgid, int64_t now_ns)
{
    // A new group starts its CPU time from what it has used so far.
    bool continues = stats->pgid == pgid && stats->sampled_ns > 0 && sample.cpu_ns >= stats->cpu_ns;
    float share = 0.0f;
    if (continues && now_ns > stats->sampled_ns) {
        share = (float)((double)(sample.cpu_ns - stats->cpu_ns) / (double)(now_ns - stats->sampled_ns));
    }
    stats->pgid = pgid;
    stats->cpu_ns = sample.cpu_ns;
    stats->sampled_ns = now_ns;
    stats->cpu_now = share;
    stats->memory_now = sample.resident_bytes;
    stats->processes = sample.process_count;
    stats->threads = sample.thread_count;
    history_push(&stats->cpu, share);
    history_push(&stats->memory, (float)sample.resident_bytes);
}

void proc_stats_sample(ProcStats *stats, const int *pgids, size_t count, Arena *scratch, int64_t now_ns)
{
    ArenaMark mark = arena_mark(scratch);
    int *live = arena_push(scratch, arena_size_mul(count, sizeof *live));
    size_t *owner = arena_push(scratch, arena_size_mul(count, sizeof *owner));
    size_t live_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (pgids[i] > 0) {
            live[live_count] = pgids[i];
            owner[live_count++] = i;
        } else {
            record(&stats[i], (GroupSample) { 0 }, 0, now_ns);
        }
    }
    GroupSample *samples = arena_push(scratch, arena_size_mul(live_count, sizeof *samples));
    if (live_count > 0 && process_groups_sample(scratch, live, live_count, samples, nullptr) == ERR_OK) {
        for (size_t i = 0; i < live_count; i++) {
            record(&stats[owner[i]], samples[i], live[i], now_ns);
        }
    }
    arena_release(mark);
}
