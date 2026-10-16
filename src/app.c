#include "app.h"

#include <string.h>

#include "mc/text/path.h"
#include "protocol.h"
#include "theme.h"
#include "ui.h"

enum {
    FRAME_MIN_MS = 16,
    ANIMATION_FRAME_MS = 50,
    SAMPLE_MS = 1000,
    ESCAPE_WAIT_MS = 30,
    WHEEL_LINES = 3,
    LOG_PADDING = 2, // a blank column each side inside the log border
    OUTPUT_CAPACITY = 1 << 16,
    LOG_BYTES = 8 << 20,
};

// Alternate screen, hidden cursor, no autowrap, mouse reports, bracketed paste.
static const char ENTER_SCREEN[] = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[?2004h";
static const char LEAVE_SCREEN[] = "\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
static const char SYNC_BEGIN[] = "\x1b[?2026h";
static const char SYNC_END[] = "\x1b[?2026l";

static int64_t ms(int64_t milliseconds)
{
    return milliseconds * NS_PER_MILLISECOND;
}

// wake is called from other threads; the terminal only exists until closing.
static void wake(void *context)
{
    App *app = context;
    mutex_lock(&app->wake_lock);
    if (!app->closing && app->terminal != nullptr) {
        terminal_wake(app->terminal);
    }
    mutex_unlock(&app->wake_lock);
}

// on_stop_signal turns SIGINT and SIGTERM from outside into leaving the TUI.
static void on_stop_signal(void *context, Signal signal)
{
    App *app = context;
    if (signal != SIGNAL_STOP) {
        return;
    }
    atomic_store(&app->stop_signal, true);
    wake(app);
}

static void destroy_views(App *app)
{
    for (size_t i = 0; i < app->view_count; i++) {
        logbuf_destroy(app->views[i].log);
    }
    arena_reset(app->views_arena);
    app->views = nullptr;
    app->view_count = 0;
}

// build_views makes a view for each proc the daemon runs.
static void build_views(App *app)
{
    destroy_views(app);
    const ClientInfo *info = client_info(app->client);
    app->config_path = str_copy(app->arena, info->config_path);
    app->view_count = info->count;
    app->views = arena_push(app->views_arena, arena_size_mul(info->count, sizeof(ProcView)));
    for (size_t i = 0; i < info->count; i++) {
        ProcSpec spec = info->specs[i];
        spec.name = str_copy(app->views_arena, spec.name);
        spec.command = str_copy(app->views_arena, spec.command);
        spec.cwd = str_copy(app->views_arena, spec.cwd);
        spec.log_path = str_copy(app->views_arena, spec.log_path);
        spec.ready.text = str_copy(app->views_arena, spec.ready.text);
        spec.health.text = str_copy(app->views_arena, spec.health.text);
        size_t *needs = arena_push(app->views_arena, arena_size_mul(spec.need_count, sizeof *needs));
        if (spec.need_count > 0) {
            memcpy(needs, spec.needs, spec.need_count * sizeof *needs);
        }
        spec.needs = needs;
        app->views[i] = (ProcView) {
            .spec = spec,
            .log = logbuf_create(info->scrollback, LOG_BYTES),
            .following = true,
        };
    }
    app->selected = app->selected < app->view_count ? app->selected : 0;
    app->list_offset = 0;
    logbuf_clear(app->all.log);
    app->all.following = true;
}

App *app_create(Client *client, size_t theme_index, ColorMode color_mode)
{
    Arena *arena = arena_create(0);
    App *app = arena_push(arena, sizeof *app);
    app->arena = arena;
    app->frame = arena_create(0);
    app->views_arena = arena_create(0);
    app->client = client;
    app->theme_index = theme_index;
    app->color_mode = color_mode;
    app->daemons_arena = arena_create(0);
    mutex_init(&app->wake_lock);
    atomic_init(&app->stop_signal, false);
    app->all = (ProcView) {
        .spec = { .name = S("all") },
        .log = logbuf_create(client_info(client)->scrollback, LOG_BYTES),
        .following = true,
    };
    build_views(app);
    return app;
}

void app_destroy(App *app)
{
    if (app->client != nullptr) {
        client_close(app->client);
    }
    destroy_views(app);
    logbuf_destroy(app->all.log);
    mutex_destroy(&app->wake_lock);
    arena_destroy(app->daemons_arena);
    arena_destroy(app->views_arena);
    arena_destroy(app->frame);
    arena_destroy(app->arena);
}

static void write_out(App *app, String text)
{
    if (terminal_write(app->terminal, text, nullptr) != ERR_OK) {
        // The terminal has gone: the daemon keeps the processes.
        app->done = true;
    }
}

[[gnu::format(printf, 2, 3)]] static void toast(App *app, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    String text = str_format_va(app->frame, format, args);
    va_end(args);
    app->toast_len = min_size(text.len, TOAST_LIMIT);
    memcpy(app->toast, text.data, app->toast_len);
    app->toast_ns = app->now_ns;
}

static ProcView *selected_view(App *app)
{
    return &app->views[app->selected];
}

static String selected_name(App *app)
{
    return selected_view(app)->spec.name;
}

static bool selected_active(App *app)
{
    return app->view_count > 0 && proc_state_active(selected_view(app)->status.state);
}

String app_waiting_for(App *app, size_t index)
{
    const ProcSpec *spec = &app->views[index].spec;
    StringList names = { 0 };
    for (size_t i = 0; i < spec->need_count; i++) {
        const ProcView *need = &app->views[spec->needs[i]];
        ProcState state = need->status.state;
        bool met = need->spec.job ? state == PROC_EXITED : state == PROC_RUNNING;
        if (!met) {
            strlist_push(app->frame, &names, need->spec.name);
        }
    }
    return str_join(app->frame, names, S(", "));
}

ProcView *app_log_view(App *app)
{
    return app->show_all ? &app->all : &app->views[app->selected];
}

String app_search(const App *app)
{
    return (String) { app->search, app->search_len };
}

bool app_matches(App *app, String line)
{
    if (app->search_len == 0) {
        return false;
    }
    ArenaMark mark = arena_mark(app->frame);
    bool found = str_contains(str_lower_ascii(app->frame, logbuf_plain(app->frame, line)),
                              str_lower_ascii(app->frame, app_search(app)));
    arena_release(mark);
    return found;
}

// merge_lines copies the new complete lines of view index into the merged
// log, each after the proc's name in its colour.
static void merge_lines(App *app, size_t index)
{
    ProcView *view = &app->views[index];
    int width = 0;
    for (size_t i = 0; i < app->view_count; i++) {
        width = (int)app->views[i].spec.name.len > width ? (int)app->views[i].spec.name.len : width;
    }
    Rgb color = app_accent(app, index);
    uint64_t first = logbuf_first(view->log);
    for (uint64_t n = view->merged > first ? view->merged : first; n < logbuf_end(view->log); n++) {
        ArenaMark mark = arena_mark(app->frame);
        String line = logbuf_line(view->log, n);
        logbuf_note(app->all.log, str_format(app->frame, "\x1b[38;2;%d;%d;%dm%-*.*s │\x1b[0m %.*s", color.r, color.g,
                                              color.b, width, (int)view->spec.name.len, view->spec.name.data,
                                              (int)line.len, line.data));
        arena_release(mark);
    }
    view->merged = logbuf_end(view->log);
}

// ---- requests to the daemon ----

// send_request sends a request about the proc called name, or about every
// proc when name is empty; the reply comes back as a message.
static void send_request(App *app, Node *request, String name, String body)
{
    if (app->client == nullptr) {
        toast(app, "the daemon has stopped · u starts it again");
        return;
    }
    if (name.len > 0) {
        Node *names = message_list(app->frame);
        message_append_text(app->frame, names, name);
        message_put(app->frame, request, "names", names);
    }
    Err err = { 0 };
    if (client_send(app->client, request, body, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
    }
}

static void send_simple(App *app, const char *type, String name)
{
    send_request(app, message(app->frame, type), name, S(""));
}

static void send_input(App *app, String bytes)
{
    Node *request = message(app->frame, "input");
    message_bool(app->frame, request, "reply", false);
    send_request(app, request, selected_name(app), bytes);
}

static void send_size(App *app)
{
    Rect log = rect_inner(app->layout.log);
    PtySize size = { .columns = log.width - LOG_PADDING, .rows = log.height };
    size.columns = size.columns < 20 ? 20 : size.columns;
    size.rows = size.rows < 5 ? 5 : size.rows;
    if (app->client == nullptr || (size.columns == app->pty_size.columns && size.rows == app->pty_size.rows)) {
        return;
    }
    app->pty_size = size;
    Node *request = message(app->frame, "resize");
    message_int(app->frame, request, "columns", size.columns);
    message_int(app->frame, request, "rows", size.rows);
    send_request(app, request, S(""), S(""));
}

// subscribe asks for everything the daemon reports.
[[nodiscard]] static Error subscribe(App *app, Err *err)
{
    Error e = client_listen(app->client, wake, app, err);
    if (e != ERR_OK) {
        return e;
    }
    Node *request = message(app->frame, "subscribe");
    message_int(app->frame, request, "history", (int64_t)client_info(app->client)->scrollback);
    e = client_send(app->client, request, S(""), err);
    app->pty_size = (PtySize) { 0 };
    return e;
}

static void reconnect(App *app, const char *done)
{
    Err err = { 0 };
    bool started;
    Client *client;
    if (client_ensure_daemon(app->frame, app->config_path, &started, &err) != ERR_OK ||
        client_connect(app->config_path, &client, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
        return;
    }
    app->client = client;
    build_views(app);
    app->layout = layout_compute(screen_width(app->screen), screen_height(app->screen), app->view_count);
    if (subscribe(app, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
        return;
    }
    if (started) {
        send_simple(app, "up", S(""));
    }
    app->daemon_gone = false;
    send_size(app);
    toast(app, "%s", done);
}

static void reconnect(App *app, const char *done);

static void daemon_left(App *app)
{
    if (app->going_down) {
        app->done = true;
        return;
    }
    if (app->reloading) {
        app->reloading = false;
        client_close(app->client);
        app->client = nullptr;
        reconnect(app, "↻ reloaded the config");
        return;
    }
    client_close(app->client);
    app->client = nullptr;
    app->daemon_gone = true;
    app->focus = FOCUS_PROCS;
    toast(app, "the daemon stopped · u starts it again");
}

// ---- what the daemon reports ----

static ProcView *message_view(App *app, const Node *header)
{
    int64_t index = node_get_int(header, S("proc"), -1);
    return index >= 0 && (size_t)index < app->view_count ? &app->views[index] : nullptr;
}

static void take_stats(ProcStats *stats, const Node *node)
{
    stats->cpu_now = (float)node_get_int(node, S("cpu_permille"), 0) / 1000.0f;
    stats->memory_now = (uint64_t)node_get_int(node, S("memory"), 0);
    stats->processes = (size_t)node_get_int(node, S("processes"), 0);
    stats->threads = (size_t)node_get_int(node, S("threads"), 0);
}

static void take_snapshot(ProcView *view, const Node *header, String body)
{
    logbuf_clear(view->log);
    logbuf_write(view->log, body);
    view->following = true;
    view->status = status_from_node(node_get(header, S("status")));
    view->seen_state = view->status.state;
    view->stats = (ProcStats) { 0 };
    history_from_node(node_get(header, S("cpu")), &view->stats.cpu);
    history_from_node(node_get(header, S("memory")), &view->stats.memory);
    take_stats(&view->stats, node_get(header, S("stats")));
}

static void take_all_stats(App *app, const Node *header)
{
    const Node *procs = node_get(header, S("procs"));
    size_t count = procs != nullptr && procs->kind == NODE_SEQUENCE ? procs->count : 0;
    for (size_t i = 0; i < count && i < app->view_count; i++) {
        ProcStats *stats = &app->views[i].stats;
        take_stats(stats, procs->items[i]);
        history_push(&stats->cpu, stats->cpu_now);
        history_push(&stats->memory, (float)stats->memory_now);
    }
}

static void take_message(App *app, ClientMessage *message)
{
    const Node *header = message->header;
    if (header == nullptr) {
        daemon_left(app);
        return;
    }
    String type = message_type(header);
    ProcView *view = message_view(app, header);
    if (str_equal(type, S("output")) && view != nullptr) {
        logbuf_write(view->log, message->body);
        merge_lines(app, (size_t)(view - app->views));
    } else if (str_equal(type, S("note")) && view != nullptr) {
        logbuf_note(view->log, message->body);
        merge_lines(app, (size_t)(view - app->views));
    } else if (str_equal(type, S("reload"))) {
        app->reloading = true;
    } else if (str_equal(type, S("state")) && view != nullptr) {
        view->status = status_from_node(node_get(header, S("status")));
    } else if (str_equal(type, S("snapshot")) && view != nullptr) {
        take_snapshot(view, header, message->body);
        view->merged = logbuf_end(view->log);
    } else if (str_equal(type, S("cleared")) && view != nullptr) {
        logbuf_clear(view->log);
        view->following = true;
        view->merged = logbuf_end(view->log);
    } else if (str_equal(type, S("stats"))) {
        take_all_stats(app, header);
    } else if (str_equal(type, S("reply")) && message_error(header).len > 0) {
        String error = message_error(header);
        toast(app, "%.*s", (int)error.len, error.data);
    } else if (str_equal(type, S("bye"))) {
        daemon_left(app);
    }
}

static void take_messages(App *app)
{
    ClientMessage message;
    while (app->client != nullptr && client_next(app->client, &message)) {
        take_message(app, &message);
        client_message_release(&message);
        app->dirty = true;
    }
}

static void resize(App *app)
{
    TerminalSize size = terminal_size(app->terminal);
    screen_resize(app->screen, size.columns, size.rows);
    app->layout = layout_compute(size.columns, size.rows, app->view_count);
    send_size(app);
    app->dirty = true;
}

// ---- actions ----

static void select_proc(App *app, size_t index)
{
    if (index < app->view_count && index != app->selected) {
        app->selected = index;
        if (app->focus == FOCUS_INPUT) {
            app->focus = FOCUS_PROCS;
        }
    }
}

static void move_selection(App *app, int delta)
{
    if (delta < 0 && app->selected > 0) {
        select_proc(app, app->selected - 1);
    } else if (delta > 0) {
        select_proc(app, app->selected + 1);
    }
}

static int log_page(App *app)
{
    int rows = app->layout.log.height - 3;
    return rows > 1 ? rows : 1;
}

// scroll_log moves the bottom of the log view by delta lines, negative
// toward older output; reaching the newest line follows it again.
static void scroll_log(App *app, int64_t delta)
{
    ProcView *view = app_log_view(app);
    int64_t first = (int64_t)logbuf_first(view->log);
    int64_t newest = (int64_t)logbuf_end(view->log) - 1;
    if (newest < first) {
        return;
    }
    int64_t bottom = (view->following ? newest : (int64_t)view->anchor) + delta;
    if (bottom >= newest) {
        view->following = true;
        return;
    }
    view->following = false;
    view->anchor = (uint64_t)(bottom < first ? first : bottom);
}

static void attach(App *app)
{
    if (!selected_active(app)) {
        String name = selected_name(app);
        toast(app, "%.*s is not running · s starts it", (int)name.len, name.data);
        return;
    }
    app->focus = FOCUS_INPUT;
    app->show_all = false;
    selected_view(app)->following = true;
}

static void start_selected(App *app)
{
    String name = selected_name(app);
    if (selected_active(app)) {
        toast(app, "%.*s is already running", (int)name.len, name.data);
        return;
    }
    send_simple(app, "start", name);
    toast(app, "▶ starting %.*s", (int)name.len, name.data);
}

static void stop_selected(App *app)
{
    String name = selected_name(app);
    send_simple(app, "stop", name);
    toast(app, "■ stopping %.*s", (int)name.len, name.data);
}

static void restart_selected(App *app)
{
    String name = selected_name(app);
    send_simple(app, "restart", name);
    selected_view(app)->following = true;
    toast(app, "↻ restarting %.*s", (int)name.len, name.data);
}

static size_t count_active(App *app)
{
    size_t active = 0;
    for (size_t i = 0; i < app->view_count; i++) {
        active += proc_state_active(app->views[i].status.state);
    }
    return active;
}

static void start_all(App *app)
{
    for (size_t i = 0; i < app->view_count; i++) {
        if (!proc_state_active(app->views[i].status.state)) {
            send_simple(app, "start", app->views[i].spec.name);
        }
    }
    toast(app, "▶ starting everything");
}

static void stop_all(App *app)
{
    for (size_t i = 0; i < app->view_count; i++) {
        send_simple(app, "stop", app->views[i].spec.name);
    }
    toast(app, "■ stopping everything");
}

// down asks the daemon to stop everything and end; asked again, it kills them.
static void down(App *app)
{
    if (app->client == nullptr) {
        app->done = true;
        return;
    }
    send_simple(app, "down", S(""));
    if (app->going_down) {
        return;
    }
    app->going_down = true;
    app->down_started_ns = app->now_ns;
    app->down_total = count_active(app);
    app->focus = FOCUS_PROCS;
    app->show_help = false;
}

// jump_to_match scrolls the log to the next line holding the search, older or newer.
static void jump_to_match(App *app, bool older)
{
    ProcView *view = app_log_view(app);
    uint64_t first = logbuf_first(view->log);
    uint64_t end = logbuf_end(view->log);
    if (app->search_len == 0 || end == first) {
        toast(app, "/ starts a search");
        return;
    }
    uint64_t bottom = view->following ? end - 1 : view->anchor;
    uint64_t n = bottom;
    for (;;) {
        if (older ? n == first : n + 1 >= end) {
            toast(app, "no %s line holds \"%.*s\"", older ? "older" : "newer", (int)app->search_len, app->search);
            return;
        }
        n = older ? n - 1 : n + 1;
        if (app_matches(app, logbuf_line(view->log, n))) {
            break;
        }
    }
    view->following = n + 1 >= end;
    view->anchor = n;
}

static void toggle_filter(App *app)
{
    if (app->search_len == 0) {
        toast(app, "/ starts a search, & then shows only its lines");
        return;
    }
    app->filtering = !app->filtering;
    toast(app, app->filtering ? "showing only lines with \"%.*s\"" : "showing every line", (int)app->search_len,
          app->search);
}

// take_search_key edits the search while it is being typed.
static void take_search_key(App *app, Event event)
{
    if (event.key == KEY_ENTER) {
        app->typing_search = false;
        if (app->search_len > 0) {
            jump_to_match(app, true);
        }
    } else if (event.key == KEY_ESCAPE || (event.key == KEY_CHAR && event.ctrl && event.codepoint == 'c')) {
        app->typing_search = false;
        app->search_len = 0;
        app->filtering = false;
    } else if (event.key == KEY_BACKSPACE) {
        // Back over one whole UTF-8 character.
        while (app->search_len > 0 && ((unsigned char)app->search[--app->search_len] & 0xc0) == 0x80) {
        }
    } else if (event.key == KEY_CHAR && !event.ctrl && app->search_len + event.raw.len < SEARCH_LIMIT) {
        memcpy(app->search + app->search_len, event.raw.data, event.raw.len);
        app->search_len += event.raw.len;
    }
}

static void open_port(App *app)
{
    const ProcSpec *spec = &selected_view(app)->spec;
    if (spec->port == 0) {
        toast(app, "%.*s has no port · set port: in the config", (int)spec->name.len, spec->name.data);
        return;
    }
    String url = str_format(app->frame, "http://localhost:%d", spec->port);
    Err err = { 0 };
    if (process_open_default(url, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
        return;
    }
    toast(app, "↗ %.*s", (int)url.len, url.data);
}

// ---- noticing an edited config ----

static int64_t config_mtime(App *app)
{
    FileInfo info = { 0 };
    return file_info_follow(app->config_path, &info, nullptr) == ERR_OK ? info.mtime_ns : 0;
}

// notice_config_change says so when the config has changed since it was last looked at.
static void notice_config_change(App *app)
{
    int64_t mtime = config_mtime(app);
    if (mtime != 0 && app->config_mtime_ns != 0 && mtime != app->config_mtime_ns) {
        String base = path_base(app->config_path);
        toast(app, "%.*s changed · R reloads it", (int)base.len, base.data);
    }
    app->config_mtime_ns = mtime;
}

// ---- the project switcher ----

static void open_switcher(App *app)
{
    arena_reset(app->daemons_arena);
    Arena *arena = app->daemons_arena;
    String directory;
    StringList names = { 0 };
    if (runtime_directory(arena, &directory, nullptr) != ERR_OK || dir_list(arena, directory, &names, nullptr) != ERR_OK) {
        names = (StringList) { 0 };
    }
    app->daemons = arena_push(arena, arena_size_mul(names.count + 1, sizeof *app->daemons));
    app->daemon_count = 0;
    app->daemon_selected = 0;
    for (size_t i = 0; i < names.count; i++) {
        Client *client;
        if (!str_ends_with(names.items[i], S(".sock")) ||
            client_connect_socket(path_join(arena, directory, names.items[i]), &client, nullptr) != ERR_OK) {
            continue;
        }
        Daemon *daemon = &app->daemons[app->daemon_count++];
        *daemon = (Daemon) {
            .config_path = str_copy(arena, client_info(client)->config_path),
            .total = client_info(client)->count,
        };
        Node *reply;
        if (client_request(client, arena, message(arena, "status"), S(""), &reply, nullptr, nullptr) == ERR_OK) {
            const Node *procs = node_get(reply, S("procs"));
            for (size_t p = 0; procs != nullptr && p < procs->count; p++) {
                daemon->running += proc_state_active(status_from_node(procs->items[p]).state);
            }
        }
        if (str_equal(daemon->config_path, app->config_path)) {
            app->daemon_selected = app->daemon_count - 1;
        }
        client_close(client);
    }
    app->switching = true;
}

static void switch_project(App *app)
{
    app->switching = false;
    if (app->daemon_selected >= app->daemon_count) {
        return;
    }
    String path = app->daemons[app->daemon_selected].config_path;
    if (str_equal(path, app->config_path) && app->client != nullptr) {
        return;
    }
    Client *client;
    Err err = { 0 };
    if (client_connect(path, &client, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
        return;
    }
    if (app->client != nullptr) {
        client_close(app->client);
    }
    app->client = client;
    app->selected = 0;
    app->show_all = false;
    app->daemon_gone = false;
    app->focus = FOCUS_PROCS;
    build_views(app);
    app->layout = layout_compute(screen_width(app->screen), screen_height(app->screen), app->view_count);
    if (subscribe(app, &err) != ERR_OK) {
        toast(app, "%s", err.msg);
        return;
    }
    send_size(app);
    app->config_mtime_ns = config_mtime(app);
    toast(app, "⇄ %.*s", (int)app->config_path.len, app->config_path.data);
}

static void take_switcher_key(App *app, Event event)
{
    if (event.key == KEY_UP || (event.key == KEY_CHAR && event.codepoint == 'k')) {
        app->daemon_selected -= app->daemon_selected > 0;
    } else if (event.key == KEY_DOWN || (event.key == KEY_CHAR && event.codepoint == 'j')) {
        app->daemon_selected += app->daemon_selected + 1 < app->daemon_count;
    } else if (event.key == KEY_ENTER) {
        switch_project(app);
    } else if (event.key == KEY_ESCAPE || (event.key == KEY_CHAR && (event.codepoint == 'p' || event.codepoint == 'q'))) {
        app->switching = false;
    }
}

static void next_theme(App *app)
{
    app->theme_index = (app->theme_index + 1) % theme_count();
    toast(app, "theme · %s", theme_at(app->theme_index)->name);
}

// ---- input ----

static void handle_char(App *app, uint32_t c)
{
    switch (c) {
    case 'q':
        app->done = true;
        return;
    case 'D':
        down(app);
        return;
    case 'u':
        if (app->daemon_gone) {
            reconnect(app, "▶ the daemon is back");
        }
        return;
    case '?':
        app->show_help = true;
        return;
    case 'j':
        app->focus == FOCUS_LOG ? scroll_log(app, 1) : move_selection(app, 1);
        return;
    case 'k':
        app->focus == FOCUS_LOG ? scroll_log(app, -1) : move_selection(app, -1);
        return;
    case 'g':
        scroll_log(app, INT32_MIN);
        return;
    case 'G':
        app_log_view(app)->following = true;
        return;
    case 'f':
        app_log_view(app)->following ? scroll_log(app, -1) : (void)(app_log_view(app)->following = true);
        return;
    case '/':
        app->typing_search = true;
        app->search_len = 0;
        return;
    case 'n':
        jump_to_match(app, true);
        return;
    case 'N':
        jump_to_match(app, false);
        return;
    case '&':
        toggle_filter(app);
        return;
    case 'a':
        app->show_all = !app->show_all;
        toast(app, app->show_all ? "log · every proc" : "log · %.*s", (int)selected_name(app).len,
              selected_name(app).data);
        return;
    case 'o':
        open_port(app);
        return;
    case 'R':
        send_simple(app, "reload", S(""));
        toast(app, "↻ reloading the config");
        return;
    case 'p':
        open_switcher(app);
        return;
    case 's':
        start_selected(app);
        return;
    case 'x':
        stop_selected(app);
        return;
    case 'r':
        restart_selected(app);
        return;
    case 'K':
        send_simple(app, "kill", selected_name(app));
        return;
    case 'S':
        start_all(app);
        return;
    case 'X':
        stop_all(app);
        return;
    case 'i':
        attach(app);
        return;
    case 'c':
        send_simple(app, "clear", selected_name(app));
        return;
    case 't':
        next_theme(app);
        return;
    default:
        if (c >= '1' && c <= '9') {
            select_proc(app, (size_t)(c - '1'));
        }
        return;
    }
}

static void handle_control(App *app, uint32_t c)
{
    switch (c) {
    case 'c':
        app->done = true;
        return;
    case 'a':
        attach(app);
        return;
    case 'u':
        scroll_log(app, -log_page(app) / 2);
        return;
    case 'd':
        scroll_log(app, log_page(app) / 2);
        return;
    case 'l':
        screen_invalidate(app->screen);
        return;
    default:
        return;
    }
}

static void handle_key(App *app, Event event)
{
    bool log_focused = app->focus == FOCUS_LOG;
    switch (event.key) {
    case KEY_CHAR:
        event.ctrl ? handle_control(app, event.codepoint) : handle_char(app, event.codepoint);
        return;
    case KEY_UP:
        log_focused ? scroll_log(app, -1) : move_selection(app, -1);
        return;
    case KEY_DOWN:
        log_focused ? scroll_log(app, 1) : move_selection(app, 1);
        return;
    case KEY_PAGE_UP:
        scroll_log(app, -log_page(app));
        return;
    case KEY_PAGE_DOWN:
        scroll_log(app, log_page(app));
        return;
    case KEY_HOME:
        log_focused ? scroll_log(app, INT32_MIN) : select_proc(app, 0);
        return;
    case KEY_END:
        log_focused ? (void)(app_log_view(app)->following = true) : select_proc(app, app->view_count - 1);
        return;
    case KEY_TAB:
    case KEY_BACKTAB:
        app->focus = log_focused ? FOCUS_PROCS : FOCUS_LOG;
        return;
    case KEY_ENTER:
        attach(app);
        return;
    case KEY_ESCAPE:
        app->focus = FOCUS_PROCS;
        return;
    default:
        return;
    }
}

static void handle_mouse(App *app, Event event)
{
    const Layout *layout = &app->layout;
    bool wheel = event.mouse == MOUSE_WHEEL_UP || event.mouse == MOUSE_WHEEL_DOWN;
    int direction = event.mouse == MOUSE_WHEEL_UP ? -1 : 1;
    if (rect_contains(layout->log, event.x, event.y)) {
        if (wheel) {
            scroll_log(app, direction * WHEEL_LINES);
        } else if (event.mouse == MOUSE_PRESS && app->focus != FOCUS_INPUT) {
            app->focus = FOCUS_LOG;
        }
        return;
    }
    if (!rect_contains(layout->procs, event.x, event.y)) {
        return;
    }
    if (wheel) {
        move_selection(app, direction);
        return;
    }
    int row = event.y - layout->procs.y - 2;
    if (event.mouse == MOUSE_PRESS && row >= 0) {
        select_proc(app, app->list_offset + (size_t)row);
        app->focus = FOCUS_PROCS;
    }
}

static void handle_event(App *app, Event event)
{
    if (app->going_down) {
        if (event.key == KEY_CHAR && !event.ctrl && event.codepoint == 'D') {
            down(app);
        } else if (event.key == KEY_CHAR && (event.codepoint == 'q' || (event.ctrl && event.codepoint == 'c'))) {
            app->done = true;
        }
        return;
    }
    if (app->show_help && event.key != KEY_MOUSE) {
        app->show_help = false;
        return;
    }
    if (app->switching && event.key != KEY_MOUSE) {
        take_switcher_key(app, event);
        return;
    }
    if (app->typing_search && event.key != KEY_MOUSE) {
        take_search_key(app, event);
        return;
    }
    if (event.key == KEY_MOUSE) {
        if (app->view_count > 0) {
            handle_mouse(app, event);
        }
        return;
    }
    if (app->focus == FOCUS_INPUT) {
        if (event.key == KEY_CHAR && event.ctrl && event.codepoint == 'a') {
            app->focus = FOCUS_PROCS;
            return;
        }
        // The program did not ask for paste markers.
        send_input(app, event.key == KEY_PASTE ? input_paste_text(event.raw) : event.raw);
        selected_view(app)->following = true;
        return;
    }
    if (app->view_count == 0) {
        if (event.key == KEY_CHAR && (event.codepoint == 'q' || event.codepoint == 'u' || event.codepoint == 'D' ||
                                      event.codepoint == 'p' || (event.ctrl && event.codepoint == 'c'))) {
            handle_key(app, event);
        }
        return;
    }
    handle_key(app, event);
}

// read_available appends the bytes waiting on the terminal to the input,
// after waiting up to wait_ms for the first; it reports whether any came.
static bool read_available(App *app, int wait_ms)
{
    unsigned char byte;
    if (app->input_len == INPUT_LIMIT || !terminal_read_byte(app->terminal, wait_ms, &byte)) {
        return false;
    }
    app->input[app->input_len++] = byte;
    while (app->input_len < INPUT_LIMIT && terminal_read_byte(app->terminal, 0, &byte)) {
        app->input[app->input_len++] = byte;
    }
    return true;
}

// take_paste passes on more of a paste that did not fit the input buffer,
// to the attached process only, and returns the bytes it took.
static size_t take_paste(App *app, String bytes)
{
    String text;
    bool ended;
    size_t used = input_paste_rest(bytes, &text, &ended);
    if (app->focus == FOCUS_INPUT && text.len > 0) {
        send_input(app, text);
    }
    app->pasting = !ended;
    return used;
}

static void read_input(App *app)
{
    read_available(app, 0);
    size_t at = 0;
    while (at < app->input_len) {
        String rest = { (const char *)app->input + at, app->input_len - at };
        if (app->pasting) {
            size_t used = take_paste(app, rest);
            if (used == 0) {
                break; // what is left may be the start of the end marker
            }
            at += used;
            continue;
        }
        Event event;
        size_t used;
        if (!input_decode(rest, false, &event, &used)) {
            // The start of an escape sequence: the rest is usually on its way.
            if (read_available(app, ESCAPE_WAIT_MS)) {
                continue;
            }
            bool decoded = input_decode(rest, true, &event, &used);
            unused(decoded);
        }
        app->pasting = input_paste_continues(event);
        handle_event(app, event);
        at += used;
    }
    app->input_len -= at;
    memmove(app->input, app->input + at, app->input_len);
    app->dirty = true;
}

// ---- the loop ----

static void tick(App *app)
{
    for (size_t i = 0; i < app->view_count; i++) {
        ProcView *view = &app->views[i];
        if (view->status.state == view->seen_state) {
            continue;
        }
        view->seen_state = view->status.state;
        view->state_changed_ns = app->now_ns;
        app->dirty = true;
        String name = view->spec.name;
        if (view->status.state == PROC_CRASHED && !app->going_down) {
            toast(app, "✖ %.*s crashed", (int)name.len, name.data);
        } else if (view->status.state == PROC_BLOCKED) {
            toast(app, "⊘ %.*s cannot start: %s", (int)name.len, name.data, view->status.problem);
        }
    }
    if (app->focus == FOCUS_INPUT && !selected_active(app)) {
        app->focus = FOCUS_PROCS;
    }
}

static void keep_selection_visible(App *app)
{
    size_t rows = (size_t)ui_list_rows(app);
    if (rows == 0) {
        return;
    }
    if (app->selected < app->list_offset) {
        app->list_offset = app->selected;
    }
    if (app->selected >= app->list_offset + rows) {
        app->list_offset = app->selected - rows + 1;
    }
    if (app->list_offset + rows > app->view_count) {
        app->list_offset = app->view_count > rows ? app->view_count - rows : 0;
    }
}

static void draw(App *app)
{
    keep_selection_visible(app);
    ui_draw(app);
    StringBuilder out = str_builder_create(app->frame, OUTPUT_CAPACITY);
    str_builder_append(&out, S(SYNC_BEGIN));
    screen_render(app->screen, &out);
    str_builder_append(&out, S(SYNC_END));
    write_out(app, (String) { out.data, out.len });
    app->last_frame_ns = app->now_ns;
    app->dirty = false;
}

static int wait_ms(App *app)
{
    int64_t due = app->next_sample_ns;
    if (app->dirty) {
        due = app->last_frame_ns + ms(FRAME_MIN_MS) < due ? app->last_frame_ns + ms(FRAME_MIN_MS) : due;
    } else if (ui_animating(app)) {
        due = app->last_frame_ns + ms(ANIMATION_FRAME_MS) < due ? app->last_frame_ns + ms(ANIMATION_FRAME_MS) : due;
    }
    int64_t wait = (due - app->now_ns) / NS_PER_MILLISECOND;
    return wait < 0 ? 0 : wait > SAMPLE_MS ? SAMPLE_MS : (int)wait;
}

static bool frame_due(App *app)
{
    int64_t since = app->now_ns - app->last_frame_ns;
    return since >= ms(FRAME_MIN_MS) && (app->dirty || (since >= ms(ANIMATION_FRAME_MS) && ui_animating(app)));
}

static void step(App *app)
{
    app->now_ns = clock_monotonic_ns();
    TerminalReady ready = terminal_wait(app->terminal, wait_ms(app));
    arena_reset(app->frame);
    app->now_ns = clock_monotonic_ns();
    if (ready.resized) {
        resize(app);
    }
    if (ready.input) {
        read_input(app);
    }
    if (ready.woken) {
        app->dirty = true;
    }
    if (atomic_exchange(&app->stop_signal, false)) {
        app->done = true;
    }
    take_messages(app);
    if (app->now_ns >= app->next_sample_ns) {
        system_stats_sample(&app->system, app->frame, app->now_ns);
        notice_config_change(app);
        app->dirty = true;
        app->next_sample_ns += ms(SAMPLE_MS);
        if (app->next_sample_ns <= app->now_ns) {
            app->next_sample_ns = app->now_ns + ms(SAMPLE_MS);
        }
    }
    tick(app);
    if (frame_due(app)) {
        draw(app);
    }
}

Error app_run(App *app, Err *err)
{
    Error opened = terminal_open(&app->terminal, err);
    if (opened != ERR_OK) {
        return opened;
    }
    SignalWatch *watch;
    Error watching = signal_watch_start(on_stop_signal, app, &watch, err);
    if (watching != ERR_OK) {
        terminal_close(app->terminal);
        return watching;
    }
    write_out(app, S(ENTER_SCREEN));
    app->screen = screen_create(app->color_mode);
    app->now_ns = clock_monotonic_ns();
    app->started_ns = app->now_ns;
    app->next_sample_ns = app->now_ns;
    Error listening = subscribe(app, err);
    if (listening == ERR_OK) {
        resize(app);
        while (!app->done) {
            step(app);
        }
    }
    write_out(app, S(LEAVE_SCREEN));
    screen_destroy(app->screen);
    signal_watch_stop(watch);
    mutex_lock(&app->wake_lock);
    app->closing = true;
    mutex_unlock(&app->wake_lock);
    terminal_close(app->terminal);
    return listening;
}
