#include "client.h"

#include "mc/concurrency/queue.h"
#include "mc/platform/frame.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "protocol.h"

enum {
    START_TIMEOUT_MS = 5000,
    START_POLL_MS = 20,
};

struct Client {
    Arena *arena;
    int fd;
    int64_t next_id;
    ClientInfo info;
    // Once listening, the reader thread owns receiving.
    Thread reader;
    bool listening;
    Queue *messages;
    void (*wake)(void *context);
    void *context;
};

static bool daemon_answers(String socket)
{
    int fd;
    if (net_connect_unix(socket, &fd, nullptr) != ERR_OK) {
        return false;
    }
    net_close(fd);
    return true;
}

// last_log_line is the last line the daemon wrote to its log, to say why it did not start.
static String last_log_line(Arena *scratch, String log)
{
    String text;
    if (file_read_all(scratch, log, &text, nullptr) != ERR_OK) {
        return S("no log");
    }
    StringList lines = str_split(scratch, str_trim(text), '\n');
    return lines.count > 0 ? lines.items[lines.count - 1] : S("no log");
}

[[nodiscard]] static Error start_daemon(Arena *scratch, String config_path, RuntimePaths paths, Err *err)
{
    String executable;
    Error e = process_executable_path(scratch, &executable, err);
    if (e != ERR_OK) {
        return e;
    }
    StringList argv = { 0 };
    strlist_push(scratch, &argv, executable);
    strlist_push(scratch, &argv, S("-f"));
    strlist_push(scratch, &argv, config_path);
    strlist_push(scratch, &argv, S("daemon"));
    int pid;
    e = process_spawn_detached(scratch, argv, paths.log, &pid, err);
    if (e != ERR_OK) {
        return e;
    }
    int64_t deadline = clock_monotonic_ns() + (int64_t)START_TIMEOUT_MS * NS_PER_MILLISECOND;
    while (clock_monotonic_ns() < deadline) {
        if (daemon_answers(paths.socket)) {
            return ERR_OK;
        }
        if (!process_alive(pid)) {
            break;
        }
        clock_sleep_ms(START_POLL_MS);
    }
    String said = last_log_line(scratch, paths.log);
    return err_set(err, ERR_IO, "the daemon did not start: %.*s", (int)said.len, said.data);
}

Error client_ensure_daemon(Arena *scratch, String config_path, bool *started, Err *err)
{
    *started = false;
    RuntimePaths paths;
    Error e = runtime_paths(scratch, config_path, &paths, err);
    if (e != ERR_OK || daemon_answers(paths.socket)) {
        return e;
    }
    // Clients that find no daemon at once take turns, so only one starts it.
    int lock;
    e = file_lock(paths.start, &lock, err);
    if (e != ERR_OK) {
        return e;
    }
    if (!daemon_answers(paths.socket)) {
        e = start_daemon(scratch, config_path, paths, err);
        *started = e == ERR_OK;
    }
    file_close(lock);
    return e;
}

[[nodiscard]] static Error say_hello(Client *client, Err *err)
{
    Arena *arena = client->arena;
    Node *hello = message(arena, "hello");
    message_int(arena, hello, "version", PROTOCOL_VERSION);
    Node *reply;
    String body;
    Err detail = { 0 };
    Error e = client_request(client, arena, hello, S(""), &reply, &body, &detail);
    if (e != ERR_OK) {
        return err_set(err, e == ERR_IO ? ERR_UNSUPPORTED : e, "%s", detail.msg);
    }
    client->info = (ClientInfo) {
        .config_path = node_get_string(reply, S("config"), S("")),
        .theme = node_get_string(reply, S("theme"), S("")),
        .scrollback = (size_t)node_get_int(reply, S("scrollback"), DEFAULT_SCROLLBACK),
        .pid = (int)node_get_int(reply, S("pid"), 0),
    };
    client->info.specs = specs_from_node(arena, node_get(reply, S("procs")), &client->info.count);
    return ERR_OK;
}

Error client_connect_socket(String socket, Client **client, Err *err)
{
    int fd;
    if (net_connect_unix(socket, &fd, nullptr) != ERR_OK) {
        return err_set(err, ERR_NOT_FOUND, "nothing answers on %.*s", (int)socket.len, socket.data);
    }
    Arena *arena = arena_create(0);
    Client *c = arena_push(arena, sizeof *c);
    *c = (Client) { .arena = arena, .fd = fd, .next_id = 1 };
    Error e = say_hello(c, err);
    if (e != ERR_OK) {
        net_close(fd);
        arena_destroy(arena);
        return e;
    }
    *client = c;
    return ERR_OK;
}

Error client_connect(String config_path, Client **client, Err *err)
{
    Arena *scratch = arena_create(0);
    RuntimePaths paths;
    Error e = runtime_paths(scratch, config_path, &paths, err);
    if (e == ERR_OK) {
        e = client_connect_socket(paths.socket, client, err);
    }
    arena_destroy(scratch);
    if (e == ERR_NOT_FOUND) {
        return err_set(err, ERR_NOT_FOUND, "no daemon runs %.*s", (int)config_path.len, config_path.data);
    }
    return e;
}

void client_close(Client *client)
{
    net_shutdown(client->fd);
    if (client->listening) {
        thread_join(&client->reader);
        ClientMessage message;
        while (client_next(client, &message)) {
            client_message_release(&message);
        }
        queue_destroy(client->messages);
    }
    net_close(client->fd);
    arena_destroy(client->arena);
}

const ClientInfo *client_info(const Client *client)
{
    return &client->info;
}

bool client_proc_index(const Client *client, String name, size_t *index)
{
    for (size_t i = 0; i < client->info.count; i++) {
        if (str_equal(client->info.specs[i].name, name)) {
            *index = i;
            return true;
        }
    }
    return false;
}

Error client_send(Client *client, Node *request, String body, Err *err)
{
    Arena *scratch = arena_create(0);
    message_int(scratch, request, "id", client->next_id++);
    Error e = frame_send(client->fd, scratch, message_encode(scratch, request), body, err);
    arena_destroy(scratch);
    return e;
}

Error client_request(Client *client, Arena *arena, Node *request, String body, Node **reply, String *reply_body,
                     Err *err)
{
    int64_t id = client->next_id;
    Error e = client_send(client, request, body, err);
    if (e != ERR_OK) {
        return e;
    }
    for (;;) {
        ArenaMark mark = arena_mark(arena);
        Frame frame;
        e = frame_receive(client->fd, arena, PROTOCOL_FRAME_LIMIT, &frame, err);
        if (e == ERR_CANCELLED) {
            return err_set(err, ERR_NETWORK, "the daemon went away");
        }
        if (e != ERR_OK) {
            return e;
        }
        Node *header;
        e = message_parse(arena, frame.header, &header, err);
        if (e != ERR_OK) {
            return e;
        }
        if (!str_equal(message_type(header), S("reply")) || node_get_int(header, S("id"), 0) != id) {
            arena_release(mark);
            continue;
        }
        String error = message_error(header);
        if (error.len > 0) {
            return err_set(err, ERR_IO, "%.*s", (int)error.len, error.data);
        }
        *reply = header;
        if (reply_body != nullptr) {
            *reply_body = frame.body;
        }
        return ERR_OK;
    }
}

void client_wait_closed(Client *client)
{
    for (;;) {
        Arena *scratch = arena_create(0);
        Frame frame;
        Error e = frame_receive(client->fd, scratch, PROTOCOL_FRAME_LIMIT, &frame, nullptr);
        arena_destroy(scratch);
        if (e != ERR_OK) {
            return;
        }
    }
}

static void *read_messages(void *argument)
{
    Client *client = argument;
    for (;;) {
        Arena *arena = arena_create(0);
        ClientMessage *message = arena_push(arena, sizeof *message);
        message->arena = arena;
        Frame frame;
        bool closed = frame_receive(client->fd, arena, PROTOCOL_FRAME_LIMIT, &frame, nullptr) != ERR_OK ||
                      message_parse(arena, frame.header, &message->header, nullptr) != ERR_OK;
        if (closed) {
            message->header = nullptr;
        } else {
            message->body = frame.body;
        }
        if (!queue_push(client->messages, message)) {
            arena_destroy(arena);
            return nullptr;
        }
        client->wake(client->context);
        if (closed) {
            return nullptr;
        }
    }
}

Error client_listen(Client *client, void (*wake)(void *context), void *context, Err *err)
{
    client->messages = queue_create();
    client->wake = wake;
    client->context = context;
    Error e = thread_start(&client->reader, read_messages, client, err);
    if (e != ERR_OK) {
        queue_destroy(client->messages);
        return e;
    }
    client->listening = true;
    return ERR_OK;
}

bool client_next(Client *client, ClientMessage *message)
{
    void *item;
    if (!queue_pop_timeout(client->messages, 0, &item)) {
        return false;
    }
    *message = *(ClientMessage *)item;
    return true;
}

void client_message_release(ClientMessage *message)
{
    arena_destroy(message->arena);
}
