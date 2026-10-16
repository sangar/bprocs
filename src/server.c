#include "server.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mc/concurrency/outbox.h"
#include "mc/concurrency/queue.h"
#include "mc/concurrency/waker.h"
#include "mc/platform/frame.h"
#include "mc/platform/platform.h"
#include "protocol.h"

enum {
    ACCEPT_TIMEOUT_MS = 200,
    SAMPLE_MS = 1000,
    OUTBOX_LIMIT = 4 << 20, // a client further behind than this is dropped
    FLUSH_MS = 1000,
};

typedef struct Connection Connection;

struct Connection {
    Arena *arena;
    Server *server;
    int fd;
    Outbox *outbox;
    Thread reader;
    Thread writer;
    bool *streaming; // streaming[i]: proc i's output and notes go to this client; under clients_lock
    bool subscribed; // states, stats and other events go to this client; under clients_lock
    bool writer_joined;
    atomic_bool writer_done;
    Connection *next;
};

// Command is a request from a connection's reader, or word that it is gone.
typedef struct Command {
    Arena *arena;
    Connection *connection;
    Node *message; // nullptr when the connection is gone
    String body;
} Command;

// Wait is a wait request that has not been answered yet.
typedef struct Wait {
    Arena *arena;
    Connection *connection;
    int64_t id;
    size_t *procs;
    size_t count;
    int64_t deadline_ns; // 0 for none
    struct Wait *next;
} Wait;

typedef struct Down {
    Connection *connection; // nullptr once it has gone
    int64_t id;
} Down;

struct Server {
    Arena *arena;
    Arena *frame; // reset every step
    const Config *config;
    Supervisor *supervisor;
    String socket_path;
    int listener;
    Thread listen_thread;
    atomic_bool closing;

    int lock; // held for as long as the server listens
    Waker waker;
    atomic_bool reload_requested;
    atomic_bool down_requested;

    Queue *commands;
    Mutex clients_lock;
    Connection *clients;

    ProcStatus *sent; // the status each client last heard of
    Wait *waits;
    Down *downs;
    size_t down_count;
    size_t down_capacity;
    int64_t next_sample_ns;
    bool finished;
    bool reloadable;
    Arena **configs; // every config read again, which stopping procs may still use
    size_t config_count;
    size_t config_capacity;
};

static void wake(void *context)
{
    Server *server = context;
    waker_wake(&server->waker);
}

static void put_frame(Outbox *outbox, String header, String body)
{
    char prefix[FRAME_PREFIX_LEN];
    frame_prefix(prefix, header.len, body.len);
    String parts[] = { { prefix, FRAME_PREFIX_LEN }, header, body };
    outbox_put_parts(outbox, parts, countof(parts));
}

// stream sends a frame about proc index to every client streaming it.
static void stream(Server *server, size_t index, String header, String body)
{
    mutex_lock(&server->clients_lock);
    for (Connection *c = server->clients; c != nullptr; c = c->next) {
        if (c->streaming[index]) {
            put_frame(c->outbox, header, body);
        }
    }
    mutex_unlock(&server->clients_lock);
}

static void on_output(void *context, size_t index, String output)
{
    char header[64];
    int len = snprintf(header, sizeof header, "{\"type\":\"output\",\"proc\":%zu}", index);
    stream(context, index, (String) { header, (size_t)len }, output);
}

static void on_note(void *context, size_t index, String text)
{
    char header[64];
    int len = snprintf(header, sizeof header, "{\"type\":\"note\",\"proc\":%zu}", index);
    stream(context, index, (String) { header, (size_t)len }, text);
}

// broadcast sends a frame to every client that subscribed.
static void broadcast(Server *server, const Node *header, String body)
{
    String text = message_encode(server->frame, header);
    mutex_lock(&server->clients_lock);
    for (Connection *c = server->clients; c != nullptr; c = c->next) {
        if (c->subscribed) {
            put_frame(c->outbox, text, body);
        }
    }
    mutex_unlock(&server->clients_lock);
}

static void send_to(Server *server, Connection *connection, const Node *header, String body)
{
    String text = message_encode(server->frame, header);
    mutex_lock(&server->clients_lock);
    put_frame(connection->outbox, text, body);
    mutex_unlock(&server->clients_lock);
}

static Node *reply_to(Server *server, int64_t id)
{
    Node *reply = message(server->frame, "reply");
    message_int(server->frame, reply, "id", id);
    return reply;
}

static void reply_error(Server *server, Connection *connection, int64_t id, String error)
{
    Node *reply = reply_to(server, id);
    message_text(server->frame, reply, "error", error);
    send_to(server, connection, reply, S(""));
}

static void reply_ok(Server *server, Connection *connection, int64_t id)
{
    send_to(server, connection, reply_to(server, id), S(""));
}

// ---- connections ----

static void *read_requests(void *argument)
{
    Connection *connection = argument;
    Server *server = connection->server;
    for (;;) {
        Arena *arena = arena_create(0);
        Command *command = arena_push(arena, sizeof *command);
        *command = (Command) { .arena = arena, .connection = connection };
        Frame frame;
        Node *parsed = nullptr;
        bool done = frame_receive(connection->fd, arena, PROTOCOL_FRAME_LIMIT, &frame, nullptr) != ERR_OK ||
                    message_parse(arena, frame.header, &parsed, nullptr) != ERR_OK;
        if (!done) {
            command->message = parsed;
            command->body = frame.body;
        }
        if (!queue_push(server->commands, command)) {
            arena_destroy(arena);
            return nullptr;
        }
        wake(server);
        if (done) {
            return nullptr;
        }
    }
}

static void *write_frames(void *argument)
{
    Connection *connection = argument;
    for (;;) {
        String bytes = outbox_take(connection->outbox);
        if (bytes.len == 0) {
            break;
        }
        if (net_send(connection->fd, bytes, nullptr) != ERR_OK) {
            outbox_close(connection->outbox);
            break;
        }
    }
    // A reader blocked on the socket hears that it is over.
    net_shutdown(connection->fd);
    atomic_store(&connection->writer_done, true);
    return nullptr;
}

static void accept_client(Server *server, int fd)
{
    Arena *arena = arena_create(0);
    Connection *connection = arena_push(arena, sizeof *connection);
    *connection = (Connection) {
        .arena = arena,
        .server = server,
        .fd = fd,
        .outbox = outbox_create(OUTBOX_LIMIT),
        .streaming = arena_push(arena, arena_size_mul(supervisor_count(server->supervisor) + 1, sizeof(bool))),
    };
    atomic_init(&connection->writer_done, false);
    if (thread_start(&connection->writer, write_frames, connection, nullptr) != ERR_OK) {
        outbox_destroy(connection->outbox);
        net_close(fd);
        arena_destroy(arena);
        return;
    }
    if (thread_start(&connection->reader, read_requests, connection, nullptr) != ERR_OK) {
        outbox_close(connection->outbox);
        thread_join(&connection->writer);
        outbox_destroy(connection->outbox);
        net_close(fd);
        arena_destroy(arena);
        return;
    }
    mutex_lock(&server->clients_lock);
    connection->next = server->clients;
    server->clients = connection;
    mutex_unlock(&server->clients_lock);
}

static void *listen_for_clients(void *argument)
{
    Server *server = argument;
    while (!atomic_load(&server->closing)) {
        int fd;
        if (net_accept(server->listener, ACCEPT_TIMEOUT_MS, &fd)) {
            accept_client(server, fd);
        }
    }
    return nullptr;
}

// close_connection ends a connection whose reader has finished or is about to.
static void close_connection(Server *server, Connection *connection)
{
    mutex_lock(&server->clients_lock);
    for (Connection **at = &server->clients; *at != nullptr; at = &(*at)->next) {
        if (*at == connection) {
            *at = connection->next;
            break;
        }
    }
    mutex_unlock(&server->clients_lock);
    for (Wait **at = &server->waits; *at != nullptr;) {
        Wait *wait = *at;
        if (wait->connection == connection) {
            *at = wait->next;
            arena_destroy(wait->arena);
        } else {
            at = &wait->next;
        }
    }
    for (size_t i = 0; i < server->down_count; i++) {
        if (server->downs[i].connection == connection) {
            server->downs[i].connection = nullptr;
        }
    }
    // The writer may be stuck sending to a client that stopped reading.
    outbox_close(connection->outbox);
    net_shutdown(connection->fd);
    if (!connection->writer_joined) {
        thread_join(&connection->writer);
    }
    thread_join(&connection->reader);
    net_close(connection->fd);
    outbox_destroy(connection->outbox);
    arena_destroy(connection->arena);
}

// ---- requests ----

// resolve turns the names of a request into proc indexes; no names is every proc when all is set.
static bool resolve(Server *server, const Node *request, bool all, size_t **procs, size_t *count, String *error)
{
    const Node *names = node_get(request, S("names"));
    size_t total = supervisor_count(server->supervisor);
    size_t wanted = names != nullptr && names->kind == NODE_SEQUENCE ? names->count : 0;
    *procs = arena_push(server->frame, arena_size_mul(wanted > 0 ? wanted : total, sizeof **procs));
    *count = 0;
    if (wanted == 0) {
        if (!all) {
            *error = S("name a proc");
            return false;
        }
        for (size_t i = 0; i < total; i++) {
            (*procs)[(*count)++] = i;
        }
        return true;
    }
    for (size_t i = 0; i < wanted; i++) {
        size_t index;
        String name = names->items[i]->text;
        if (!config_proc_index(server->config, name, &index)) {
            *error = str_format(server->frame, "no proc called %.*s", (int)name.len, name.data);
            return false;
        }
        (*procs)[(*count)++] = index;
    }
    return true;
}

static void handle_hello(Server *server, Connection *connection, int64_t id, const Node *request)
{
    int64_t version = node_get_int(request, S("version"), 0);
    if (version != PROTOCOL_VERSION) {
        reply_error(server, connection, id,
                    str_format(server->frame, "bprocs was updated (protocol %lld, the daemon speaks %d): run "
                                              "`bprocs down` and start it again",
                               (long long)version, PROTOCOL_VERSION));
        return;
    }
    Arena *a = server->frame;
    Node *reply = reply_to(server, id);
    message_int(a, reply, "version", PROTOCOL_VERSION);
    message_int(a, reply, "pid", process_id());
    message_text(a, reply, "config", server->config->path);
    message_text(a, reply, "theme", server->config->theme);
    message_int(a, reply, "scrollback", (int64_t)server->config->scrollback);
    Node *procs = message_list(a);
    for (size_t i = 0; i < server->config->proc_count; i++) {
        node_append(a, procs, spec_to_node(a, &server->config->procs[i], server->config));
    }
    message_put(a, reply, "procs", procs);
    send_to(server, connection, reply, S(""));
}

static Node *stats_node(Arena *arena, const ProcStats *stats)
{
    Node *node = node_create(arena, NODE_MAPPING);
    message_int(arena, node, "cpu_permille", (int64_t)(stats->cpu_now * 1000.0f));
    message_int(arena, node, "memory", (int64_t)stats->memory_now);
    message_int(arena, node, "processes", (int64_t)stats->processes);
    message_int(arena, node, "threads", (int64_t)stats->threads);
    return node;
}

static void handle_status(Server *server, Connection *connection, int64_t id)
{
    Arena *a = server->frame;
    Node *reply = reply_to(server, id);
    Node *procs = message_list(a);
    for (size_t i = 0; i < supervisor_count(server->supervisor); i++) {
        Node *entry = status_to_node(a, proc_status(supervisor_proc(server->supervisor, i)));
        message_text(a, entry, "name", server->config->procs[i].name);
        message_put(a, entry, "stats", stats_node(a, supervisor_stats(server->supervisor, i)));
        node_append(a, procs, entry);
    }
    message_put(a, reply, "procs", procs);
    message_int(a, reply, "now_ns", clock_monotonic_ns());
    send_to(server, connection, reply, S(""));
}

static void handle_action(Server *server, Connection *connection, int64_t id, const Node *request, String type)
{
    size_t *procs;
    size_t count;
    String error;
    bool up = str_equal(type, S("up"));
    if (!resolve(server, request, up, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    int64_t now = clock_monotonic_ns();
    Supervisor *supervisor = server->supervisor;
    if (up && node_get(request, S("names")) == nullptr) {
        supervisor_up(supervisor, now);
        count = 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (up || str_equal(type, S("start"))) {
            supervisor_start(supervisor, procs[i], now);
        } else if (str_equal(type, S("stop"))) {
            supervisor_stop(supervisor, procs[i], now);
        } else if (str_equal(type, S("restart"))) {
            supervisor_restart(supervisor, procs[i], now);
        } else {
            supervisor_kill(supervisor, procs[i]);
        }
    }
    reply_ok(server, connection, id);
}

static void handle_down(Server *server, Connection *connection, int64_t id)
{
    supervisor_down(server->supervisor, clock_monotonic_ns());
    server->downs = arena_grow(server->arena, server->downs, &server->down_capacity, server->down_count,
                               sizeof *server->downs);
    server->downs[server->down_count++] = (Down) { connection, id };
}

// log_text is the last lines of a proc's log, then the line still being
// written; the proc's lock must be held. plain leaves out escape sequences.
static String log_text(Arena *arena, const LogBuffer *log, int64_t lines, bool plain)
{
    uint64_t first = logbuf_first(log);
    uint64_t end = logbuf_end(log);
    if (lines >= 0 && end - first > (uint64_t)lines) {
        first = end - (uint64_t)lines;
    }
    StringBuilder out = str_builder_create(arena, 4096);
    for (uint64_t n = first; n < end; n++) {
        String line = logbuf_line(log, n);
        str_builder_append(&out, plain ? logbuf_plain(arena, line) : line);
        str_builder_append_char(&out, '\n');
    }
    String partial = logbuf_partial(log);
    str_builder_append(&out, plain ? logbuf_plain(arena, partial) : partial);
    return str_builder_finish(&out);
}

static void handle_logs(Server *server, Connection *connection, int64_t id, const Node *request)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, false, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    Proc *proc = supervisor_proc(server->supervisor, procs[0]);
    proc_lock(proc);
    String text = log_text(server->frame, proc_log(proc), node_get_int(request, S("lines"), -1), true);
    proc_unlock(proc);
    send_to(server, connection, reply_to(server, id), text);
}

// handle_subscribe sends each proc's status, stats and log, and from then on
// its output; taking the proc's lock keeps output from slipping in between.
static void handle_subscribe(Server *server, Connection *connection, int64_t id, const Node *request)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, true, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    Arena *a = server->frame;
    int64_t history = node_get_int(request, S("history"), -1);
    for (size_t i = 0; i < count; i++) {
        size_t index = procs[i];
        Proc *proc = supervisor_proc(server->supervisor, index);
        const ProcStats *stats = supervisor_stats(server->supervisor, index);
        Node *snapshot = message(a, "snapshot");
        message_int(a, snapshot, "proc", (int64_t)index);
        message_put(a, snapshot, "status", status_to_node(a, proc_status(proc)));
        message_put(a, snapshot, "stats", stats_node(a, stats));
        message_put(a, snapshot, "cpu", history_to_node(a, &stats->cpu));
        message_put(a, snapshot, "memory", history_to_node(a, &stats->memory));
        String header = message_encode(a, snapshot);
        proc_lock(proc);
        String text = log_text(a, proc_log(proc), history, false);
        mutex_lock(&server->clients_lock);
        put_frame(connection->outbox, header, text);
        connection->streaming[index] = true;
        mutex_unlock(&server->clients_lock);
        proc_unlock(proc);
    }
    mutex_lock(&server->clients_lock);
    connection->subscribed = true;
    mutex_unlock(&server->clients_lock);
    Node *reply = reply_to(server, id);
    message_int(a, reply, "now_ns", clock_monotonic_ns());
    send_to(server, connection, reply, S(""));
}

static void handle_input(Server *server, Connection *connection, int64_t id, const Node *request, String body)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, false, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    proc_send(supervisor_proc(server->supervisor, procs[0]), body);
    if (node_get_bool(request, S("reply"), true)) {
        reply_ok(server, connection, id);
    }
}

static void handle_resize(Server *server, Connection *connection, int64_t id, const Node *request)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, true, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    PtySize size = {
        .columns = (int)node_get_int(request, S("columns"), 80),
        .rows = (int)node_get_int(request, S("rows"), 24),
    };
    size.columns = size.columns < 20 ? 20 : size.columns > 1000 ? 1000 : size.columns;
    size.rows = size.rows < 5 ? 5 : size.rows > 1000 ? 1000 : size.rows;
    for (size_t i = 0; i < count; i++) {
        proc_resize(supervisor_proc(server->supervisor, procs[i]), size);
    }
    reply_ok(server, connection, id);
}

static void handle_clear(Server *server, Connection *connection, int64_t id, const Node *request)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, false, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    proc_clear_log(supervisor_proc(server->supervisor, procs[0]));
    Node *event = message(server->frame, "cleared");
    message_int(server->frame, event, "proc", (int64_t)procs[0]);
    broadcast(server, event, S(""));
    reply_ok(server, connection, id);
}

static void handle_wait(Server *server, Connection *connection, int64_t id, const Node *request)
{
    size_t *procs;
    size_t count;
    String error;
    if (!resolve(server, request, false, &procs, &count, &error)) {
        reply_error(server, connection, id, error);
        return;
    }
    Arena *arena = arena_create(0);
    Wait *wait = arena_push(arena, sizeof *wait);
    *wait = (Wait) {
        .arena = arena,
        .connection = connection,
        .id = id,
        .procs = arena_push(arena, arena_size_mul(count, sizeof(size_t))),
        .count = count,
        .next = server->waits,
    };
    memcpy(wait->procs, procs, count * sizeof(size_t));
    int64_t timeout_ms = node_get_int(request, S("timeout_ms"), 0);
    wait->deadline_ns = timeout_ms > 0 ? clock_monotonic_ns() + timeout_ms * NS_PER_MILLISECOND : 0;
    server->waits = wait;
}

// cancel_waits answers every wait with an error, as when the procs it named may have moved.
static void cancel_waits(Server *server, String why)
{
    while (server->waits != nullptr) {
        Wait *wait = server->waits;
        reply_error(server, wait->connection, wait->id, why);
        server->waits = wait->next;
        arena_destroy(wait->arena);
    }
}

// reload reads the config again and runs it, and says what changed or why
// it could not. Subscribed clients hear "reload" and are let go, to connect
// again for the new procs.
static bool reload(Server *server, String *said)
{
    if (!server->reloadable) {
        *said = S("bprocs run does not reload; start it again");
        return false;
    }
    Arena *arena = arena_create(0);
    Config *next = arena_push(arena, sizeof *next);
    Err err = { 0 };
    if (config_load(arena, server->config->path, next, &err) != ERR_OK) {
        *said = str_copy(server->frame, S(err.msg));
        arena_destroy(arena);
        return false;
    }
    server->configs = arena_grow(server->arena, server->configs, &server->config_capacity, server->config_count,
                                 sizeof *server->configs);
    server->configs[server->config_count++] = arena;
    cancel_waits(server, S("the config was reloaded"));
    *said = supervisor_reload(server->supervisor, server->frame, next, clock_monotonic_ns());
    server->config = next;
    server->sent = arena_push(server->arena, arena_size_mul(next->proc_count, sizeof *server->sent));
    return true;
}

static void let_subscribers_go(Server *server)
{
    String event = message_encode(server->frame, message(server->frame, "reload"));
    mutex_lock(&server->clients_lock);
    for (Connection *c = server->clients; c != nullptr; c = c->next) {
        c->streaming = arena_push(c->arena, arena_size_mul(server->config->proc_count + 1, sizeof(bool)));
        if (c->subscribed) {
            c->subscribed = false;
            put_frame(c->outbox, event, S(""));
            outbox_close(c->outbox);
        }
    }
    mutex_unlock(&server->clients_lock);
}

static void handle_reload(Server *server, Connection *connection, int64_t id)
{
    String said;
    if (!reload(server, &said)) {
        reply_error(server, connection, id, said);
        return;
    }
    Node *reply = reply_to(server, id);
    message_text(server->frame, reply, "summary", said);
    send_to(server, connection, reply, S(""));
    let_subscribers_go(server);
}

static void handle(Server *server, Command *command)
{
    Connection *connection = command->connection;
    const Node *request = command->message;
    int64_t id = node_get_int(request, S("id"), 0);
    String type = message_type(request);
    if (str_equal(type, S("hello"))) {
        handle_hello(server, connection, id, request);
    } else if (str_equal(type, S("status"))) {
        handle_status(server, connection, id);
    } else if (str_equal(type, S("up")) || str_equal(type, S("start")) || str_equal(type, S("stop")) ||
               str_equal(type, S("restart")) || str_equal(type, S("kill"))) {
        handle_action(server, connection, id, request, type);
    } else if (str_equal(type, S("down"))) {
        handle_down(server, connection, id);
    } else if (str_equal(type, S("logs"))) {
        handle_logs(server, connection, id, request);
    } else if (str_equal(type, S("subscribe"))) {
        handle_subscribe(server, connection, id, request);
    } else if (str_equal(type, S("input"))) {
        handle_input(server, connection, id, request, command->body);
    } else if (str_equal(type, S("resize"))) {
        handle_resize(server, connection, id, request);
    } else if (str_equal(type, S("clear"))) {
        handle_clear(server, connection, id, request);
    } else if (str_equal(type, S("wait"))) {
        handle_wait(server, connection, id, request);
    } else if (str_equal(type, S("reload"))) {
        handle_reload(server, connection, id);
    } else {
        reply_error(server, connection, id,
                    str_format(server->frame, "unknown request %.*s", (int)type.len, type.data));
    }
}

// ---- events ----

static bool same_status(const ProcStatus *a, const ProcStatus *b)
{
    return a->state == b->state && a->health == b->health && a->pid == b->pid && a->exit_code == b->exit_code &&
           a->signal == b->signal && a->started_ns == b->started_ns && a->ready_ns == b->ready_ns &&
           a->ended_ns == b->ended_ns && a->restart_at_ns == b->restart_at_ns && a->restarts == b->restarts &&
           strcmp(a->problem, b->problem) == 0;
}

static void send_states(Server *server)
{
    for (size_t i = 0; i < supervisor_count(server->supervisor); i++) {
        ProcStatus status = proc_status(supervisor_proc(server->supervisor, i));
        if (same_status(&status, &server->sent[i])) {
            continue;
        }
        server->sent[i] = status;
        Node *event = message(server->frame, "state");
        message_int(server->frame, event, "proc", (int64_t)i);
        message_put(server->frame, event, "status", status_to_node(server->frame, status));
        broadcast(server, event, S(""));
    }
}

static void send_stats(Server *server)
{
    Arena *a = server->frame;
    Node *event = message(a, "stats");
    Node *procs = message_list(a);
    for (size_t i = 0; i < supervisor_count(server->supervisor); i++) {
        node_append(a, procs, stats_node(a, supervisor_stats(server->supervisor, i)));
    }
    message_put(a, event, "procs", procs);
    broadcast(server, event, S(""));
}

// answer_waits replies to each wait whose procs are all up, or one of which
// cannot come up, or whose time is over.
static void answer_waits(Server *server, int64_t now_ns)
{
    for (Wait **at = &server->waits; *at != nullptr;) {
        Wait *wait = *at;
        bool met = true;
        String why = S("");
        bool lost = false;
        for (size_t i = 0; i < wait->count && !lost; i++) {
            met = supervisor_need_met(server->supervisor, wait->procs[i]) && met;
            lost = !supervisor_need_met(server->supervisor, wait->procs[i]) &&
                   supervisor_need_lost(server->supervisor, server->frame, wait->procs[i], &why);
        }
        bool late = wait->deadline_ns != 0 && now_ns >= wait->deadline_ns;
        if (!met && !lost && !late) {
            at = &wait->next;
            continue;
        }
        if (lost) {
            reply_error(server, wait->connection, wait->id, why);
        } else if (!met) {
            reply_error(server, wait->connection, wait->id, S("timed out"));
        } else {
            reply_ok(server, wait->connection, wait->id);
        }
        *at = wait->next;
        arena_destroy(wait->arena);
    }
}

// ---- the server ----

Error server_create(const Config *config, const RuntimePaths *paths, Server **server, Err *err)
{
    int lock;
    bool locked;
    Error e = file_try_lock(paths->lock, &lock, &locked, err);
    if (e != ERR_OK) {
        return e;
    }
    if (!locked) {
        return err_set(err, ERR_IO, "a daemon or bprocs run already serves %.*s", (int)config->path.len,
                       config->path.data);
    }
    // Holding the lock, nothing else serves this config: a socket left here is stale.
    Error removed = file_remove(paths->socket, nullptr);
    unused(removed);
    int listener;
    e = net_listen_unix(paths->socket, 0600, &listener, err);
    if (e != ERR_OK) {
        file_close(lock);
        return e;
    }
    Arena *arena = arena_create(0);
    Server *s = arena_push(arena, sizeof *s);
    s->arena = arena;
    s->frame = arena_create(0);
    s->config = config;
    s->socket_path = str_copy(arena, paths->socket);
    s->listener = listener;
    s->lock = lock;
    atomic_init(&s->closing, false);
    atomic_init(&s->down_requested, false);
    atomic_init(&s->reload_requested, false);
    waker_init(&s->waker);
    mutex_init(&s->clients_lock);
    s->commands = queue_create();
    s->sent = arena_push(arena, arena_size_mul(config->proc_count, sizeof *s->sent));
    ProcHooks hooks = { .wake = wake, .output = on_output, .note = on_note, .context = s };
    e = supervisor_create(config, hooks, &s->supervisor, err);
    if (e == ERR_OK) {
        e = thread_start(&s->listen_thread, listen_for_clients, s, err);
        if (e != ERR_OK) {
            supervisor_destroy(s->supervisor);
        }
    }
    if (e != ERR_OK) {
        net_close(listener);
        Error unlinked = file_remove(paths->socket, nullptr);
        unused(unlinked);
        file_close(lock);
        queue_destroy(s->commands);
        mutex_destroy(&s->clients_lock);
        waker_destroy(&s->waker);
        arena_destroy(s->frame);
        arena_destroy(arena);
        return e;
    }
    s->next_sample_ns = clock_monotonic_ns();
    *server = s;
    return ERR_OK;
}

void server_destroy(Server *server)
{
    atomic_store(&server->closing, true);
    thread_join(&server->listen_thread);
    net_close(server->listener);
    Error removed = file_remove(server->socket_path, nullptr);
    unused(removed);
    arena_reset(server->frame);
    Node *bye = message(server->frame, "bye");
    mutex_lock(&server->clients_lock);
    for (Connection *c = server->clients; c != nullptr; c = c->next) {
        put_frame(c->outbox, message_encode(server->frame, bye), S(""));
        outbox_close(c->outbox);
    }
    mutex_unlock(&server->clients_lock);
    // Each writer sends what is left, bye last, before the connection
    // closes; a client that does not take it in time is cut off.
    int64_t deadline = clock_monotonic_ns() + (int64_t)FLUSH_MS * NS_PER_MILLISECOND;
    for (Connection *c = server->clients; c != nullptr; c = c->next) {
        while (!atomic_load(&c->writer_done) && clock_monotonic_ns() < deadline) {
            clock_sleep_ms(10);
        }
        net_shutdown(c->fd);
        thread_join(&c->writer);
        c->writer_joined = true;
    }
    while (server->clients != nullptr) {
        close_connection(server, server->clients);
    }
    void *item;
    while (queue_pop_timeout(server->commands, 0, &item)) {
        arena_destroy(((Command *)item)->arena);
    }
    while (server->waits != nullptr) {
        Wait *next = server->waits->next;
        arena_destroy(server->waits->arena);
        server->waits = next;
    }
    supervisor_destroy(server->supervisor);
    for (size_t i = 0; i < server->config_count; i++) {
        arena_destroy(server->configs[i]);
    }
    queue_destroy(server->commands);
    mutex_destroy(&server->clients_lock);
    waker_destroy(&server->waker);
    // The lock goes last, once the socket is gone and every proc has stopped.
    file_close(server->lock);
    arena_destroy(server->frame);
    arena_destroy(server->arena);
}

Supervisor *server_supervisor(Server *server)
{
    return server->supervisor;
}

bool server_finished(Server *server)
{
    return server->finished;
}

void server_allow_reload(Server *server)
{
    server->reloadable = true;
}

void server_request_reload(Server *server)
{
    atomic_store(&server->reload_requested, true);
    wake(server);
}

void server_request_down(Server *server)
{
    atomic_store(&server->down_requested, true);
    wake(server);
}

static void take_commands(Server *server)
{
    void *item;
    while (queue_pop_timeout(server->commands, 0, &item)) {
        Command *command = item;
        if (command->message == nullptr) {
            close_connection(server, command->connection);
        } else {
            handle(server, command);
        }
        arena_destroy(command->arena);
    }
}

static void finish_down(Server *server)
{
    for (size_t i = 0; i < server->down_count; i++) {
        if (server->downs[i].connection != nullptr) {
            reply_ok(server, server->downs[i].connection, server->downs[i].id);
        }
    }
    server->down_count = 0;
    server->finished = true;
}

void server_step(Server *server, int wait_ms)
{
    waker_wait(&server->waker, wait_ms);
    arena_reset(server->frame);
    int64_t now = clock_monotonic_ns();
    if (atomic_exchange(&server->down_requested, false)) {
        supervisor_down(server->supervisor, now);
    }
    String said;
    if (atomic_exchange(&server->reload_requested, false)) {
        bool reloaded = reload(server, &said);
        // The daemon's standard error is its log.
        fprintf(stderr, "bprocs: reload: %.*s\n", (int)said.len, said.data);
        fflush(stderr);
        if (reloaded) {
            let_subscribers_go(server);
        }
    }
    take_commands(server);
    supervisor_tick(server->supervisor, now);
    if (now >= server->next_sample_ns) {
        supervisor_sample(server->supervisor, server->frame, now);
        send_stats(server);
        server->next_sample_ns = now + (int64_t)SAMPLE_MS * NS_PER_MILLISECOND;
    }
    send_states(server);
    answer_waits(server, now);
    if (supervisor_going_down(server->supervisor) && supervisor_settled(server->supervisor) && !server->finished) {
        finish_down(server);
    }
}
