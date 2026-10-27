#include "check.h"

#include "mc/term/logbuf.h"
#include "mc/term/ansi.h"
#include "mc/platform/platform.h"

enum {
    RESPONSE_LIMIT = 1024,
    OUTPUT_LIMIT = 4096,
    READ_TIMEOUT_MS = 50,
};

static const PtySize CHECK_SIZE = { .columns = 120, .rows = 24 };

String check_describe(Arena *arena, const Check *check)
{
    switch (check->kind) {
    case CHECK_LOG:
        return str_format(arena, "output holding \"%.*s\"", (int)check->text.len, check->text.data);
    case CHECK_PORT:
        return str_format(arena, "%.*s:%d", (int)check->host.len, check->host.data, check->port);
    case CHECK_HTTP:
        return str_format(arena, "http://%.*s:%d%.*s", (int)check->host.len, check->host.data, check->port,
                          (int)check->path.len, check->path.data);
    case CHECK_CMD:
        return str_join(arena, check->argv, S(" "));
    case CHECK_NONE:
        break;
    }
    return S("");
}

static int timeout_ms(int64_t timeout_ns)
{
    int64_t ms = timeout_ns / NS_PER_MILLISECOND;
    return ms < 1 ? 1 : ms > 600000 ? 600000 : (int)ms;
}

static bool check_port(Arena *scratch, const Check *check, int64_t timeout_ns, String *problem)
{
    Err err = { 0 };
    int fd;
    if (net_connect(check->host, str_format(scratch, "%d", check->port), timeout_ms(timeout_ns), &fd, &err) !=
        ERR_OK) {
        *problem = str_format(scratch, "nothing answers on %.*s:%d", (int)check->host.len, check->host.data,
                              check->port);
        return false;
    }
    net_close(fd);
    return true;
}

// read_status reads the response until its status line is complete and returns the status code, or 0.
static int read_status(int fd, char *buffer)
{
    size_t len = 0;
    size_t end;
    while (len < RESPONSE_LIMIT && !str_find((String) { buffer, len }, S("\r\n"), &end)) {
        size_t got;
        if (net_receive(fd, buffer + len, RESPONSE_LIMIT - len, &got, nullptr) != ERR_OK || got == 0) {
            break;
        }
        len += got;
    }
    String line = str_first_line((String) { buffer, len });
    String version;
    String rest;
    uint64_t code;
    if (!str_cut(line, ' ', &version, &rest) || !str_starts_with(version, S("HTTP/")) || rest.len < 3 ||
        !str_parse_u64(str_slice(rest, 0, 3), &code)) {
        return 0;
    }
    return (int)code;
}

static bool check_http(Arena *scratch, const Check *check, int64_t timeout_ns, String *problem)
{
    String url = check_describe(scratch, check);
    Err err = { 0 };
    int fd;
    if (net_connect(check->host, str_format(scratch, "%d", check->port), timeout_ms(timeout_ns), &fd, &err) !=
        ERR_OK) {
        *problem = str_format(scratch, "GET %.*s: nothing answers", (int)url.len, url.data);
        return false;
    }
    String request = str_format(scratch,
                                "GET %.*s HTTP/1.0\r\nHost: %.*s:%d\r\nUser-Agent: bprocs\r\nConnection: close\r\n\r\n",
                                (int)check->path.len, check->path.data, (int)check->host.len, check->host.data,
                                check->port);
    int status = 0;
    if (net_set_timeouts(fd, timeout_ms(timeout_ns), nullptr) == ERR_OK && net_send(fd, request, nullptr) == ERR_OK) {
        status = read_status(fd, arena_push(scratch, RESPONSE_LIMIT));
    }
    net_close(fd);
    if (status >= 200 && status < 400) {
        return true;
    }
    *problem = status == 0 ? str_format(scratch, "GET %.*s: no answer in time", (int)url.len, url.data)
                           : str_format(scratch, "GET %.*s: %d", (int)url.len, url.data, status);
    return false;
}

// last_line is the last line of output with text, without its escape sequences.
static String last_line(Arena *scratch, String output)
{
    StringList lines = str_split(scratch, output, '\n');
    for (size_t i = lines.count; i > 0; i--) {
        String line = str_trim(ansi_strip(scratch, lines.items[i - 1]));
        if (line.len > 0) {
            return line;
        }
    }
    return S("");
}

static bool check_cmd(Arena *scratch, const Check *check, const ProcSpec *spec, int64_t timeout_ns, String *problem)
{
    String command = check_describe(scratch, check);
    Pty pty;
    Err err = { 0 };
    if (pty_spawn(scratch, check->argv, spec->cwd, spec->env, CHECK_SIZE, &pty, &err) != ERR_OK) {
        *problem = str_format(scratch, "%.*s: %s", (int)command.len, command.data, err.msg);
        return false;
    }
    StringBuilder output = str_builder_create(scratch, OUTPUT_LIMIT);
    char *buffer = arena_push(scratch, OUTPUT_LIMIT);
    int64_t deadline = clock_monotonic_ns() + timeout_ns;
    bool killed = false;
    int exit_code = -1;
    int signal = 0;
    while (!pty_reap(&pty, &exit_code, &signal)) {
        if (!killed && clock_monotonic_ns() >= deadline) {
            Error sent = pty_signal(&pty, PTY_KILL, nullptr);
            unused(sent);
            killed = true;
        }
        size_t got;
        PtyEvent event = pty_read(&pty, READ_TIMEOUT_MS, buffer, OUTPUT_LIMIT, &got);
        if (event == PTY_DATA && output.len + got <= OUTPUT_LIMIT) {
            str_builder_append(&output, (String) { buffer, got });
        } else if (event == PTY_HANGUP) {
            clock_sleep_ms(10);
        }
    }
    pty_close(&pty);
    if (exit_code == 0 && signal == 0 && !killed) {
        return true;
    }
    String said = last_line(scratch, (String) { output.data, output.len });
    String how = killed        ? S("took too long")
                 : signal != 0 ? S("ended by a signal")
                               : str_format(scratch, "exited with code %d", exit_code);
    *problem = str_format(scratch, "%.*s %.*s%s%.*s", (int)command.len, command.data, (int)how.len, how.data,
                          said.len > 0 ? ": " : "", (int)said.len, said.data);
    return false;
}

bool check_run(Arena *scratch, const Check *check, const ProcSpec *spec, int64_t timeout_ns, String *problem)
{
    *problem = S("");
    switch (check->kind) {
    case CHECK_PORT:
        return check_port(scratch, check, timeout_ns, problem);
    case CHECK_HTTP:
        return check_http(scratch, check, timeout_ns, problem);
    case CHECK_CMD:
        return check_cmd(scratch, check, spec, timeout_ns, problem);
    case CHECK_LOG:
    case CHECK_NONE:
        break;
    }
    return true;
}
