#include "format.h"

#include "mc/platform/platform.h"

enum { SECONDS_PER_DAY = 86400 };

String format_clock(Arena *arena, int64_t wall_ns)
{
    int64_t seconds = wall_ns / NS_PER_SECOND;
    int64_t local = seconds + clock_local_offset(seconds);
    int64_t of_day = ((local % SECONDS_PER_DAY) + SECONDS_PER_DAY) % SECONDS_PER_DAY;
    return str_format(arena, "%02d:%02d:%02d", (int)(of_day / 3600), (int)(of_day / 60 % 60), (int)(of_day % 60));
}

String format_span(Arena *arena, int64_t ns)
{
    int64_t seconds = ns > 0 ? ns / NS_PER_SECOND : 0;
    if (seconds < 60) {
        return str_format(arena, "%ds", (int)seconds);
    }
    if (seconds < 3600) {
        return str_format(arena, "%dm%02ds", (int)(seconds / 60), (int)(seconds % 60));
    }
    if (seconds < SECONDS_PER_DAY) {
        return str_format(arena, "%dh%02dm", (int)(seconds / 3600), (int)(seconds / 60 % 60));
    }
    return str_format(arena, "%dd%02dh", (int)(seconds / SECONDS_PER_DAY), (int)(seconds / 3600 % 24));
}

String format_elapsed(Arena *arena, int64_t ns)
{
    if (ns >= 0 && ns < NS_PER_SECOND) {
        return str_format(arena, "%dms", (int)(ns / NS_PER_MILLISECOND));
    }
    return format_span(arena, ns);
}

String format_size(Arena *arena, uint64_t bytes)
{
    static const char UNITS[] = { 'B', 'K', 'M', 'G', 'T', 'P' };
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < sizeof UNITS) {
        value /= 1024.0;
        unit++;
    }
    if (unit > 0 && value < 9.95) {
        return str_format(arena, "%.1f%c", value, UNITS[unit]);
    }
    return str_format(arena, "%d%c", (int)(value + 0.5), UNITS[unit]);
}

String format_rate(Arena *arena, double bytes_per_second)
{
    static const char *const UNITS[] = { "B/s", "KB/s", "MB/s", "GB/s" };
    double value = bytes_per_second > 0.0 ? bytes_per_second : 0.0;
    size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < countof(UNITS)) {
        value /= 1024.0;
        unit++;
    }
    return str_format(arena, unit == 0 || value >= 100.0 ? "%.0f %s" : "%.1f %s", value, UNITS[unit]);
}

String format_signal(Arena *arena, int signal)
{
    // The numbers that macOS and Linux share.
    static const struct {
        int number;
        const char *name;
    } NAMES[] = {
        { 1, "SIGHUP" }, { 2, "SIGINT" }, { 3, "SIGQUIT" }, { 4, "SIGILL" }, { 6, "SIGABRT" },
        { 8, "SIGFPE" }, { 9, "SIGKILL" }, { 11, "SIGSEGV" }, { 13, "SIGPIPE" }, { 14, "SIGALRM" }, { 15, "SIGTERM" },
    };
    for (size_t i = 0; i < countof(NAMES); i++) {
        if (NAMES[i].number == signal) {
            return S(NAMES[i].name);
        }
    }
    return str_format(arena, "signal %d", signal);
}
