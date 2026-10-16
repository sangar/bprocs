#include "daemon.h"

#include <stdio.h>

#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "protocol.h"
#include "server.h"

enum { STEP_MS = 100 };

// say writes a line to the daemon's log, which is its standard error.
[[gnu::format(printf, 2, 3)]] static void say(Arena *scratch, const char *format, ...)
{
    ArenaMark mark = arena_mark(scratch);
    int64_t now = clock_wall_ns();
    String time = fmt_rfc3339(scratch, now, clock_local_offset(now / NS_PER_SECOND), true);
    va_list args;
    va_start(args, format);
    String text = str_format_va(scratch, format, args);
    va_end(args);
    fprintf(stderr, "%.*s %.*s\n", (int)time.len, time.data, (int)text.len, text.data);
    fflush(stderr);
    arena_release(mark);
}

// on_signal brings everything down on SIGINT or SIGTERM, and reads the
// config again on SIGHUP, as daemons do.
static void on_signal(void *context, Signal signal)
{
    if (signal == SIGNAL_RELOAD) {
        server_request_reload(context);
    } else {
        server_request_down(context);
    }
}

int daemon_main(const Config *config)
{
    Arena *scratch = arena_create(0);
    Err err = { 0 };
    RuntimePaths paths;
    Server *server;
    if (runtime_paths(scratch, config->path, &paths, &err) != ERR_OK ||
        server_create(config, &paths, &server, &err) != ERR_OK) {
        say(scratch, "bprocs: %s", err.msg);
        arena_destroy(scratch);
        return 1;
    }
    server_allow_reload(server);
    say(scratch, "bprocs %d serves %.*s on %.*s", process_id(), (int)config->path.len, config->path.data,
        (int)paths.socket.len, paths.socket.data);
    SignalWatch *watch = nullptr;
    if (signal_watch_start(on_signal, server, &watch, &err) != ERR_OK) {
        say(scratch, "bprocs: signals are not watched: %s", err.msg);
    }
    while (!server_finished(server)) {
        server_step(server, STEP_MS);
    }
    if (watch != nullptr) {
        signal_watch_stop(watch);
    }
    server_destroy(server);
    say(scratch, "bprocs %d is down", process_id());
    arena_destroy(scratch);
    return 0;
}
