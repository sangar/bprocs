#include "cli.h"

#include <stdatomic.h>
#include <stdio.h>

#include "app.h"
#include "client.h"
#include "config.h"
#include "daemon.h"
#include "format.h"
#include "logbuf.h"
#include "mc/concurrency/waker.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
#include "mc/text/table.h"
#include "protocol.h"
#include "runner.h"
#include "theme.h"

enum {
    EXIT_OK = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
    EXIT_NO_DAEMON = 3,
    STANDARD_OUTPUT = 1,
    FOLLOW_WAIT_MS = 1000,
    LOG_BYTES = 8 << 20,
    CTRL_A = 0x01,
};

static const char USAGE[] =
    "usage: bprocs [options] [command]\n"
    "\n"
    "Runs the processes of a bprocs.yml or a Procfile in a daemon of their own,\n"
    "and shows them in a terminal UI. The config is found in this directory or\n"
    "the nearest one above it.\n"
    "\n"
    "commands:\n"
    "  (none)                  open the TUI, starting the daemon when none runs\n"
    "  up [NAME…]              start the daemon and the autostart procs, or the named ones\n"
    "  down                    stop every proc, then the daemon\n"
    "  start|stop|restart|kill NAME…\n"
    "  ps [--json]             what each proc is doing\n"
    "  logs NAME [-n N] [-f]   a proc's output; -f follows it\n"
    "  attach NAME             type into a proc on this terminal; ctrl-a d detaches\n"
    "  wait NAME… [--timeout D]  until each is up, or a job has succeeded\n"
    "  run [NAME…] [--wait NAME] [--exit-on-failure] [--ready-timeout D] [--plain]\n"
    "                          run in the foreground without a daemon, for CI\n"
    "  reload                  read the config again; restart only the procs that changed\n"
    "  ls                      every running daemon on this machine\n"
    "\n"
    "options, before the command:\n"
    "  -C DIR         look for the config from DIR\n"
    "  -f FILE        use this config\n"
    "  --theme NAME   tokyo-night, dracula, nord, gruvbox or catppuccin\n"
    "  --256          use the 256-colour palette instead of true colour\n"
    "  --truecolor    use true colour even where the terminal does not say it can\n"
    "  -h, --help     show this help\n"
    "\n"
    "Durations are written as 500ms, 30s or 2m.\n";

typedef struct Options {
    String directory;
    String config_path;
    String theme;
    bool force_256;
    bool force_true;
    bool help;
    String command;
    StringList args;
} Options;

[[gnu::format(printf, 1, 2)]] static int fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("bprocs: ", stderr);
    vfprintf(stderr, format, args);
    fputs("\n", stderr);
    va_end(args);
    return EXIT_FAILED;
}

static bool parse_options(Arena *arena, int argc, char **argv, Options *options)
{
    int i = 1;
    for (; i < argc; i++) {
        String arg = S(argv[i]);
        if (str_equal(arg, S("-h")) || str_equal(arg, S("--help"))) {
            options->help = true;
        } else if (str_equal(arg, S("--256"))) {
            options->force_256 = true;
        } else if (str_equal(arg, S("--truecolor"))) {
            options->force_true = true;
        } else if (str_equal(arg, S("--theme")) && i + 1 < argc) {
            options->theme = S(argv[++i]);
        } else if (str_equal(arg, S("-C")) && i + 1 < argc) {
            options->directory = S(argv[++i]);
        } else if (str_equal(arg, S("-f")) && i + 1 < argc) {
            options->config_path = S(argv[++i]);
        } else if (!str_starts_with(arg, S("-"))) {
            break;
        } else {
            fprintf(stderr, "bprocs: unknown option %s\n\n", argv[i]);
            return false;
        }
    }
    if (i < argc) {
        options->command = S(argv[i++]);
    }
    for (; i < argc; i++) {
        strlist_push(arena, &options->args, S(argv[i]));
    }
    return true;
}

// ---- finding the config ----

static String base_directory(Arena *arena, const Options *options)
{
    return path_absolute(arena, options->directory.len > 0 ? options->directory : S("."));
}

// find_config_path is the config's absolute path, or empty after saying why there is none.
static String find_config_path(Arena *arena, const Options *options)
{
    String base = base_directory(arena, options);
    if (options->config_path.len > 0) {
        return path_is_absolute(options->config_path) ? path_clean(arena, options->config_path)
                                                      : path_clean(arena, path_join(arena, base, options->config_path));
    }
    String path;
    if (!config_find(arena, base, &path)) {
        fprintf(stderr, "bprocs: no bprocs.yml, bprocs.yaml, Procfile.dev or Procfile here or above\n");
        return S("");
    }
    return path;
}

static bool load_config(Arena *arena, const Options *options, Config *config)
{
    String path = find_config_path(arena, options);
    if (path.len == 0) {
        return false;
    }
    Err err = { 0 };
    if (config_load(arena, path, config, &err) != ERR_OK) {
        fprintf(stderr, "bprocs: %s\n", err.msg);
        return false;
    }
    return true;
}

// connect_daemon connects to the daemon of the config, starting it when start is set.
static int connect_daemon(Arena *arena, const Options *options, bool start, Client **client, bool *started)
{
    *started = false;
    String path = find_config_path(arena, options);
    if (path.len == 0) {
        return EXIT_USAGE;
    }
    Err err = { 0 };
    if (start) {
        // A config with a mistake is reported here rather than in the daemon's log.
        Config config;
        if (!load_config(arena, options, &config)) {
            return EXIT_USAGE;
        }
        if (client_ensure_daemon(arena, path, started, &err) != ERR_OK) {
            return fail("%s", err.msg);
        }
    }
    Error e = client_connect(path, client, &err);
    if (e == ERR_NOT_FOUND) {
        fprintf(stderr, "bprocs: no daemon runs here · bprocs up starts one\n");
        return EXIT_NO_DAEMON;
    }
    return e == ERR_OK ? EXIT_OK : fail("%s", err.msg);
}

static Node *named_request(Arena *arena, const char *type, StringList names)
{
    Node *request = message(arena, type);
    if (names.count > 0) {
        Node *list = message_list(arena);
        for (size_t i = 0; i < names.count; i++) {
            message_append_text(arena, list, names.items[i]);
        }
        message_put(arena, request, "names", list);
    }
    return request;
}

static int request(Client *client, Arena *arena, Node *message, Node **reply, String *body)
{
    Err err = { 0 };
    return client_request(client, arena, message, S(""), reply, body, &err) == ERR_OK ? EXIT_OK : fail("%s", err.msg);
}

// ---- the TUI ----

static ColorMode terminal_color_mode(Arena *arena)
{
    String value;
    if (env_get(arena, S("COLORTERM"), &value) && (str_equal(value, S("truecolor")) || str_equal(value, S("24bit")))) {
        return COLOR_TRUE;
    }
    if (env_get(arena, S("TERM_PROGRAM"), &value) && str_equal(value, S("Apple_Terminal"))) {
        return COLOR_256;
    }
    static const char *const TRUE_COLOR_TERMS[] = { "kitty", "ghostty", "wezterm", "alacritty", "foot", "direct" };
    if (env_get(arena, S("TERM"), &value)) {
        for (size_t i = 0; i < countof(TRUE_COLOR_TERMS); i++) {
            if (str_contains(value, S(TRUE_COLOR_TERMS[i]))) {
                return COLOR_TRUE;
            }
        }
    }
    return COLOR_256;
}

static bool choose_theme(String name, size_t *index)
{
    *index = 0;
    if (name.len == 0 || theme_find(name, index)) {
        return true;
    }
    fprintf(stderr, "bprocs: no theme called %.*s; there are", (int)name.len, name.data);
    for (size_t i = 0; i < theme_count(); i++) {
        fprintf(stderr, "%s %s", i == 0 ? "" : ",", theme_at(i)->name);
    }
    fprintf(stderr, "\n");
    return false;
}

static int run_tui(Arena *arena, const Options *options)
{
    if (!terminal_is_terminal(STANDARD_OUTPUT)) {
        return fail("the TUI needs a terminal · bprocs up starts the procs without one, bprocs run runs them here");
    }
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, true, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    if (started) {
        Node *reply;
        status = request(client, arena, message(arena, "up"), &reply, nullptr);
    }
    size_t theme_index;
    if (status != EXIT_OK ||
        !choose_theme(options->theme.len > 0 ? options->theme : client_info(client)->theme, &theme_index)) {
        client_close(client);
        return status != EXIT_OK ? status : EXIT_USAGE;
    }
    ColorMode mode = options->force_256    ? COLOR_256
                     : options->force_true ? COLOR_TRUE
                                           : terminal_color_mode(arena);
    App *app = app_create(client, theme_index, mode);
    Err err = { 0 };
    Error ran = app_run(app, &err);
    app_destroy(app);
    return ran == ERR_OK ? EXIT_OK : fail("%s", err.msg);
}

// ---- up and down ----

static int run_up(Arena *arena, const Options *options)
{
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, true, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    Node *reply;
    status = request(client, arena, named_request(arena, "up", options->args), &reply, nullptr);
    if (status == EXIT_OK) {
        const ClientInfo *info = client_info(client);
        printf("%s %.*s · bprocs opens the TUI, bprocs down stops it all\n", started ? "▶ up" : "▶ already up",
               (int)info->config_path.len, info->config_path.data);
    }
    client_close(client);
    return status;
}

static int run_down(Arena *arena, const Options *options)
{
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status == EXIT_NO_DAEMON) {
        return EXIT_OK;
    }
    if (status != EXIT_OK) {
        return status;
    }
    Node *reply;
    status = request(client, arena, message(arena, "down"), &reply, nullptr);
    if (status == EXIT_OK) {
        client_wait_closed(client);
        printf("■ down\n");
    }
    client_close(client);
    return status;
}

static int run_action(Arena *arena, const Options *options, const char *type)
{
    if (options->args.count == 0) {
        return fail("%s needs the name of a proc", type);
    }
    bool starts = str_equal(S(type), S("start")) || str_equal(S(type), S("restart"));
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, starts, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    Node *reply;
    status = request(client, arena, named_request(arena, type, options->args), &reply, nullptr);
    client_close(client);
    return status;
}

static int run_reload(Arena *arena, const Options *options)
{
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    Node *reply;
    status = request(client, arena, message(arena, "reload"), &reply, nullptr);
    if (status == EXIT_OK) {
        String summary = node_get_string(reply, S("summary"), S(""));
        printf("↻ %.*s\n", (int)summary.len, summary.data);
    }
    client_close(client);
    return status;
}

// ---- ps ----

static String state_text(Arena *arena, ProcStatus status)
{
    if (status.state == PROC_CRASHED && status.signal != 0) {
        String name = format_signal(arena, status.signal);
        return str_format(arena, "crashed (%.*s)", (int)name.len, name.data);
    }
    if (status.state == PROC_CRASHED) {
        return str_format(arena, "crashed (%d)", status.exit_code);
    }
    if (status.restart_at_ns != 0) {
        return S("restarting");
    }
    return S(proc_state_name(status.state));
}

static int run_ps(Arena *arena, const Options *options)
{
    bool json = options->args.count > 0 && str_equal(options->args.items[0], S("--json"));
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    Node *reply;
    status = request(client, arena, message(arena, "status"), &reply, nullptr);
    if (status != EXIT_OK) {
        client_close(client);
        return status;
    }
    const ClientInfo *info = client_info(client);
    const Node *procs = node_get(reply, S("procs"));
    if (json) {
        String text = message_encode(arena, procs);
        printf("%.*s\n", (int)text.len, text.data);
        client_close(client);
        return EXIT_OK;
    }
    int64_t now = node_get_int(reply, S("now_ns"), 0);
    Table table = table_create(arena);
    String header[] = { S("NAME"), S("STATE"), S("HEALTH"), S("PID"), S("PORT"), S("CPU"), S("MEM"), S("UP"),
                        S("RESTARTS") };
    table_row(&table, countof(header), header);
    for (size_t i = 0; procs != nullptr && i < procs->count; i++) {
        const Node *entry = procs->items[i];
        ProcStatus proc = status_from_node(entry);
        const Node *stats = node_get(entry, S("stats"));
        bool active = proc_state_active(proc.state);
        String row[] = {
            node_get_string(entry, S("name"), S("?")),
            state_text(arena, proc),
            proc.health == HEALTH_UNKNOWN ? S("-") : S(proc_health_name(proc.health)),
            active ? str_format(arena, "%d", proc.pid) : S("-"),
            i < info->count && info->specs[i].port > 0 ? str_format(arena, "%d", info->specs[i].port) : S("-"),
            active ? str_format(arena, "%.1f%%", (double)node_get_int(stats, S("cpu_permille"), 0) / 10.0) : S("-"),
            active ? format_size(arena, (uint64_t)node_get_int(stats, S("memory"), 0)) : S("-"),
            active ? format_span(arena, now - proc.started_ns) : S("-"),
            str_format(arena, "%u", proc.restarts),
        };
        table_row(&table, countof(row), row);
    }
    String text = table_format(&table);
    fwrite(text.data, 1, text.len, stdout);
    client_close(client);
    return EXIT_OK;
}

// ---- following output ----

// Follower is what the threads around bprocs logs -f share with it.
typedef struct Follower {
    Waker waker;
    atomic_bool stop;
} Follower;

static void follower_wake(void *context)
{
    Follower *follower = context;
    waker_wake(&follower->waker);
}

static void follower_stop(void *context, Signal signal)
{
    Follower *follower = context;
    if (signal == SIGNAL_STOP) {
        atomic_store(&follower->stop, true);
        waker_wake(&follower->waker);
    }
}

static void print_lines(Arena *arena, const LogBuffer *log, uint64_t *printed, bool plain)
{
    uint64_t first = logbuf_first(log);
    for (uint64_t n = *printed > first ? *printed : first; n < logbuf_end(log); n++) {
        ArenaMark mark = arena_mark(arena);
        String line = logbuf_line(log, n);
        line = plain ? logbuf_plain(arena, line) : line;
        printf("%.*s%s\n", (int)line.len, line.data, plain ? "" : "\x1b[0m");
        arena_release(mark);
    }
    *printed = logbuf_end(log);
    fflush(stdout);
}

// follow prints a proc's lines as they come, until a signal or the daemon ends
// it, and closes the client.
static int follow(Arena *arena, Client *client, String name, int64_t lines)
{
    bool plain = !terminal_is_terminal(STANDARD_OUTPUT);
    Follower follower;
    waker_init(&follower.waker);
    atomic_init(&follower.stop, false);
    Err err = { 0 };
    SignalWatch *watch = nullptr;
    Error e = signal_watch_start(follower_stop, &follower, &watch, &err);
    if (e == ERR_OK) {
        e = client_listen(client, follower_wake, &follower, &err);
    }
    if (e == ERR_OK) {
        Node *subscribe = named_request(arena, "subscribe", (StringList) { .items = &name, .count = 1 });
        message_int(arena, subscribe, "history", lines);
        e = client_send(client, subscribe, S(""), &err);
    }
    LogBuffer *log = logbuf_create(client_info(client)->scrollback, LOG_BYTES);
    uint64_t printed = 0;
    bool open = e == ERR_OK;
    while (open && !atomic_load(&follower.stop)) {
        waker_wait(&follower.waker, FOLLOW_WAIT_MS);
        ClientMessage message;
        while (client_next(client, &message)) {
            String type = message.header != nullptr ? message_type(message.header) : S("bye");
            if (str_equal(type, S("snapshot")) || str_equal(type, S("output"))) {
                logbuf_write(log, message.body);
            } else if (str_equal(type, S("note"))) {
                logbuf_note(log, message.body);
            } else if (str_equal(type, S("bye"))) {
                open = false;
            } else if (str_equal(type, S("reply")) && message_error(message.header).len > 0) {
                String error = message_error(message.header);
                e = err_set(&err, ERR_IO, "%.*s", (int)error.len, error.data);
                open = false;
            }
            client_message_release(&message);
        }
        print_lines(arena, log, &printed, plain);
    }
    logbuf_destroy(log);
    if (watch != nullptr) {
        signal_watch_stop(watch);
    }
    // The client's reader may still wake the follower until the client closes.
    client_close(client);
    waker_destroy(&follower.waker);
    return e == ERR_OK ? EXIT_OK : fail("%s", err.msg);
}

static int run_logs(Arena *arena, const Options *options)
{
    String name = S("");
    int64_t lines = -1;
    bool following = false;
    for (size_t i = 0; i < options->args.count; i++) {
        String arg = options->args.items[i];
        uint64_t count;
        if (str_equal(arg, S("-f"))) {
            following = true;
        } else if (str_equal(arg, S("-n")) && i + 1 < options->args.count &&
                   str_parse_u64(options->args.items[i + 1], &count)) {
            lines = (int64_t)count;
            i++;
        } else if (name.len == 0 && !str_starts_with(arg, S("-"))) {
            name = arg;
        } else {
            return fail("logs takes a name, -n N and -f");
        }
    }
    if (name.len == 0) {
        return fail("logs needs the name of a proc");
    }
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    if (following) {
        return follow(arena, client, name, lines);
    }
    Node *logs = named_request(arena, "logs", (StringList) { .items = &name, .count = 1 });
    message_int(arena, logs, "lines", lines);
    Node *reply;
    String body;
    status = request(client, arena, logs, &reply, &body);
    if (status == EXIT_OK) {
        fwrite(body.data, 1, body.len, stdout);
        if (body.len > 0 && body.data[body.len - 1] != '\n') {
            fputc('\n', stdout);
        }
    }
    client_close(client);
    return status;
}

// ---- attach ----

typedef struct Attachment {
    Terminal *terminal;
    atomic_bool stop;
} Attachment;

static void attachment_wake(void *context)
{
    Attachment *attachment = context;
    terminal_wake(attachment->terminal);
}

static void attachment_stop(void *context, Signal signal)
{
    Attachment *attachment = context;
    if (signal != SIGNAL_STOP) {
        return;
    }
    atomic_store(&attachment->stop, true);
    terminal_wake(attachment->terminal);
}

static void send_size(Client *client, Arena *arena, Terminal *terminal, String name)
{
    TerminalSize size = terminal_size(terminal);
    Node *resize = named_request(arena, "resize", (StringList) { .items = &name, .count = 1 });
    message_int(arena, resize, "columns", size.columns);
    message_int(arena, resize, "rows", size.rows);
    Error sent = client_send(client, resize, S(""), nullptr);
    unused(sent);
}

static void write_terminal(Terminal *terminal, String text)
{
    Error written = terminal_write(terminal, text, nullptr);
    unused(written);
}

// forward_keys passes what was typed to the proc, and returns false on ctrl-a d.
static bool forward_keys(Client *client, Arena *arena, Terminal *terminal, String name, bool *after_ctrl_a)
{
    StringBuilder keys = str_builder_create(arena, 256);
    unsigned char byte;
    bool attached = true;
    while (attached && terminal_read_byte(terminal, 0, &byte)) {
        if (*after_ctrl_a) {
            *after_ctrl_a = false;
            attached = byte != 'd';
            if (attached) {
                str_builder_append_char(&keys, (char)CTRL_A);
                if (byte != CTRL_A) {
                    str_builder_append_char(&keys, (char)byte);
                }
            }
        } else if (byte == CTRL_A) {
            *after_ctrl_a = true;
        } else {
            str_builder_append_char(&keys, (char)byte);
        }
    }
    if (keys.len > 0) {
        Node *input = named_request(arena, "input", (StringList) { .items = &name, .count = 1 });
        message_bool(arena, input, "reply", false);
        Error sent = client_send(client, input, (String) { keys.data, keys.len }, nullptr);
        unused(sent);
    }
    return attached;
}

static int attach_terminal(Arena *arena, Client *client, Terminal *terminal, String name)
{
    Attachment attachment = { .terminal = terminal };
    atomic_init(&attachment.stop, false);
    Err err = { 0 };
    SignalWatch *watch = nullptr;
    Error e = signal_watch_start(attachment_stop, &attachment, &watch, &err);
    if (e == ERR_OK) {
        e = client_listen(client, attachment_wake, &attachment, &err);
    }
    if (e == ERR_OK) {
        Node *subscribe = named_request(arena, "subscribe", (StringList) { .items = &name, .count = 1 });
        message_int(arena, subscribe, "history", 200);
        e = client_send(client, subscribe, S(""), &err);
    }
    write_terminal(terminal, str_format(arena, "\x1b[2m── attached to %.*s · ctrl-a d detaches ──\x1b[0m\r\n",
                                        (int)name.len, name.data));
    send_size(client, arena, terminal, name);
    bool attached = e == ERR_OK;
    bool after_ctrl_a = false;
    while (attached && !atomic_load(&attachment.stop)) {
        TerminalReady ready = terminal_wait(terminal, -1);
        ArenaMark mark = arena_mark(arena);
        if (ready.resized) {
            send_size(client, arena, terminal, name);
        }
        if (ready.input) {
            attached = forward_keys(client, arena, terminal, name, &after_ctrl_a);
        }
        ClientMessage message;
        while (client_next(client, &message)) {
            String type = message.header != nullptr ? message_type(message.header) : S("bye");
            if (str_equal(type, S("snapshot"))) {
                write_terminal(terminal, str_replace_all(arena, message.body, S("\n"), S("\r\n")));
            } else if (str_equal(type, S("output"))) {
                write_terminal(terminal, message.body);
            } else if (str_equal(type, S("note"))) {
                write_terminal(terminal, str_format(arena, "\r\n%.*s\r\n", (int)message.body.len, message.body.data));
            } else if (str_equal(type, S("bye"))) {
                write_terminal(terminal, S("\r\n\x1b[2m── the daemon stopped ──\x1b[0m\r\n"));
                attached = false;
            }
            client_message_release(&message);
        }
        arena_release(mark);
    }
    write_terminal(terminal, S("\x1b[0m\r\n\x1b[2m── detached ──\x1b[0m\r\n"));
    if (watch != nullptr) {
        signal_watch_stop(watch);
    }
    return e == ERR_OK ? EXIT_OK : fail("%s", err.msg);
}

static int run_attach(Arena *arena, const Options *options)
{
    if (options->args.count != 1) {
        return fail("attach needs the name of a proc");
    }
    String name = options->args.items[0];
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    size_t index;
    if (!client_proc_index(client, name, &index)) {
        client_close(client);
        return fail("no proc called %.*s", (int)name.len, name.data);
    }
    Terminal *terminal;
    Err err = { 0 };
    if (terminal_open(&terminal, &err) != ERR_OK) {
        client_close(client);
        return fail("%s", err.msg);
    }
    status = attach_terminal(arena, client, terminal, name);
    // The client's reader may wake the terminal until it is closed.
    client_close(client);
    terminal_close(terminal);
    return status;
}

// ---- wait ----

static int run_wait(Arena *arena, const Options *options)
{
    StringList names = { 0 };
    int64_t timeout_ns = 0;
    for (size_t i = 0; i < options->args.count; i++) {
        String arg = options->args.items[i];
        if (str_equal(arg, S("--timeout")) && i + 1 < options->args.count) {
            if (!fmt_parse_duration(options->args.items[++i], &timeout_ns) || timeout_ns <= 0) {
                return fail("--timeout takes a duration such as 30s or 2m");
            }
        } else if (!str_starts_with(arg, S("-"))) {
            strlist_push(arena, &names, arg);
        } else {
            return fail("wait takes names and --timeout");
        }
    }
    if (names.count == 0) {
        return fail("wait needs the name of a proc");
    }
    Client *client;
    bool started;
    int status = connect_daemon(arena, options, false, &client, &started);
    if (status != EXIT_OK) {
        return status;
    }
    Node *wait = named_request(arena, "wait", names);
    message_int(arena, wait, "timeout_ms", timeout_ns / NS_PER_MILLISECOND);
    Node *reply;
    status = request(client, arena, wait, &reply, nullptr);
    client_close(client);
    return status;
}

// ---- run ----

static int run_foreground(Arena *arena, const Options *options)
{
    RunOptions run = { 0 };
    for (size_t i = 0; i < options->args.count; i++) {
        String arg = options->args.items[i];
        bool has_value = i + 1 < options->args.count;
        if (str_equal(arg, S("--wait")) && has_value) {
            run.wait = options->args.items[++i];
        } else if (str_equal(arg, S("--exit-on-failure"))) {
            run.exit_on_failure = true;
        } else if (str_equal(arg, S("--plain"))) {
            run.plain = true;
        } else if (str_equal(arg, S("--ready-timeout")) && has_value) {
            if (!fmt_parse_duration(options->args.items[++i], &run.ready_timeout_ns) || run.ready_timeout_ns <= 0) {
                return fail("--ready-timeout takes a duration such as 30s or 2m");
            }
        } else if (!str_starts_with(arg, S("-"))) {
            strlist_push(arena, &run.names, arg);
        } else {
            return fail("unknown option for run: %.*s", (int)arg.len, arg.data);
        }
    }
    Config config;
    if (!load_config(arena, options, &config)) {
        return EXIT_USAGE;
    }
    return run_main(&config, run);
}

// ---- ls ----

static int run_ls(Arena *arena)
{
    String directory;
    StringList names = { 0 };
    if (runtime_directory(arena, &directory, nullptr) != ERR_OK || dir_list(arena, directory, &names, nullptr) != ERR_OK) {
        names = (StringList) { 0 };
    }
    Table table = table_create(arena);
    String header[] = { S("CONFIG"), S("PID"), S("RUNNING") };
    table_row(&table, countof(header), header);
    size_t found = 0;
    for (size_t i = 0; i < names.count; i++) {
        if (!str_ends_with(names.items[i], S(".sock"))) {
            continue;
        }
        Client *client;
        if (client_connect_socket(path_join(arena, directory, names.items[i]), &client, nullptr) != ERR_OK) {
            continue;
        }
        Node *reply;
        size_t active = 0;
        size_t total = client_info(client)->count;
        if (client_request(client, arena, message(arena, "status"), S(""), &reply, nullptr, nullptr) == ERR_OK) {
            const Node *procs = node_get(reply, S("procs"));
            for (size_t p = 0; procs != nullptr && p < procs->count; p++) {
                active += proc_state_active(status_from_node(procs->items[p]).state);
            }
        }
        String row[] = {
            str_copy(arena, client_info(client)->config_path),
            str_format(arena, "%d", client_info(client)->pid),
            str_format(arena, "%zu/%zu", active, total),
        };
        table_row(&table, countof(row), row);
        client_close(client);
        found++;
    }
    if (found == 0) {
        printf("no daemon runs\n");
        return EXIT_OK;
    }
    String text = table_format(&table);
    fwrite(text.data, 1, text.len, stdout);
    return EXIT_OK;
}

// ---- dispatch ----

static int run_daemon(Arena *arena, const Options *options)
{
    Config config;
    if (!load_config(arena, options, &config)) {
        return EXIT_USAGE;
    }
    return daemon_main(&config);
}

static int dispatch(Arena *arena, const Options *options)
{
    String command = options->command;
    if (command.len == 0) {
        return run_tui(arena, options);
    }
    static const char *const ACTIONS[] = { "start", "stop", "restart", "kill" };
    for (size_t i = 0; i < countof(ACTIONS); i++) {
        if (str_equal(command, S(ACTIONS[i]))) {
            return run_action(arena, options, ACTIONS[i]);
        }
    }
    if (str_equal(command, S("up"))) {
        return run_up(arena, options);
    }
    if (str_equal(command, S("down"))) {
        return run_down(arena, options);
    }
    if (str_equal(command, S("ps"))) {
        return run_ps(arena, options);
    }
    if (str_equal(command, S("logs"))) {
        return run_logs(arena, options);
    }
    if (str_equal(command, S("attach"))) {
        return run_attach(arena, options);
    }
    if (str_equal(command, S("wait"))) {
        return run_wait(arena, options);
    }
    if (str_equal(command, S("run"))) {
        return run_foreground(arena, options);
    }
    if (str_equal(command, S("ls"))) {
        return run_ls(arena);
    }
    if (str_equal(command, S("reload"))) {
        return run_reload(arena, options);
    }
    if (str_equal(command, S("daemon"))) {
        return run_daemon(arena, options);
    }
    fprintf(stderr, "bprocs: no command called %.*s\n\n%s", (int)command.len, command.data, USAGE);
    return EXIT_USAGE;
}

int cli_main(int argc, char **argv)
{
    Arena *arena = arena_create(0);
    Options options = { 0 };
    int status;
    if (!parse_options(arena, argc, argv, &options)) {
        fputs(USAGE, stderr);
        status = EXIT_USAGE;
    } else if (options.help || str_equal(options.command, S("help"))) {
        fputs(USAGE, stdout);
        status = EXIT_OK;
    } else {
        status = dispatch(arena, &options);
    }
    arena_destroy(arena);
    return status;
}
