#pragma once

#include "mc/text/str.h"

// Short forms for narrow columns, allocated in arena unless they are constant text.

// format_clock is the local time of day at wall_ns, as 14:05:09.
String format_clock(Arena *arena, int64_t wall_ns);
// format_span is a duration in its two largest units: 45s, 3m12s, 4h05m, 2d07h.
String format_span(Arena *arena, int64_t ns);
// format_elapsed is format_span, but in milliseconds below a second: 120ms.
String format_elapsed(Arena *arena, int64_t ns);
// format_size is a byte count in at most four characters: 512B, 9.8K, 312M, 1.2G.
String format_size(Arena *arena, uint64_t bytes);
// format_rate is bytes per second, as 1.2 MB/s.
String format_rate(Arena *arena, double bytes_per_second);
// format_signal is the name of a signal, as SIGTERM, or its number.
String format_signal(Arena *arena, int signal);
