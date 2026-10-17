#include "test.h"

#include <stdatomic.h>

#include "client.h"
#include "config.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "protocol.h"
#include "server.h"

enum { DEADLINE_MS = 8000 };

static const char CONFIG[] = "procs:\n"
                             "  db:\n"
                             "    run: echo hello from db; sleep 30\n"
                             "    ready: { log: hello from db }\n"
                             "  web:\n"
                             "    run: while :; do echo tick; sleep 0.1; done\n"
                             "    needs: [db]\n"
                             "  console:\n"
                             "    run: read line; echo \"got $line\"; sleep 30\n"
                             "    autostart: false\n";

// Serving runs a server on a thread of its own until it has gone down.
typedef struct Serving {
    Server *server;
    Thread thread;
} Serving;

static void *serve(void *argument)
{
    Serving *serving = argument;
    int64_t deadline = clock_monotonic_ns() + 3 * (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (!server_finished(serving->server) && clock_monotonic_ns() < deadline) {
        server_step(serving->server, 20);
    }
    return nullptr;
}

static Node *named(Arena *arena, const char *type, const char *name)
{
    Node *request = message(arena, type);
    Node *names = message_list(arena);
    message_append_text(arena, names, S(name));
    message_put(arena, request, "names", names);
    return request;
}

// next_of_type waits for a message of the given type from a listening client.
static bool next_of_type(Client *client, const char *type, ClientMessage *found)
{
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (clock_monotonic_ns() < deadline) {
        ClientMessage message;
        if (!client_next(client, &message)) {
            clock_sleep_ms(10);
            continue;
        }
        if (message.header != nullptr && str_equal(message_type(message.header), S(type))) {
            *found = message;
            return true;
        }
        client_message_release(&message);
    }
    return false;
}

static void ignore(void *context)
{
    unused(context);
}

static void a_client_drives_the_server(Test *test, Client *client)
{
    Arena *a = test->arena;
    Err err = { 0 };
    Node *reply;
    String body;
    test_check(test, client_info(client)->count == 3, "hello lists the procs");
    test_check(test, client_info(client)->specs[1].need_count == 1, "with their needs");
    test_check(test, client_request(client, a, message(a, "up"), S(""), &reply, nullptr, &err) == ERR_OK, err.msg);
    Node *wait = named(a, "wait", "web");
    message_int(a, wait, "timeout_ms", 5000);
    test_check(test, client_request(client, a, wait, S(""), &reply, nullptr, &err) == ERR_OK, "web comes up");
    test_check(test, client_request(client, a, named(a, "logs", "db"), S(""), &reply, &body, &err) == ERR_OK,
               "logs");
    test_check(test, str_contains(body, S("hello from db")), "hold the output");
    test_check(test, client_request(client, a, named(a, "start", "nope"), S(""), &reply, nullptr, &err) == ERR_IO,
               "an unknown name is an error");
    test_check_str(test, S(err.msg), S("no proc called nope"), "that says so");
    wait = named(a, "wait", "console");
    message_int(a, wait, "timeout_ms", 200);
    test_check(test, client_request(client, a, wait, S(""), &reply, nullptr, &err) == ERR_IO, "an idle proc");
    test_check_str(test, S(err.msg), S("console is idle"), "cannot come up");
}

static void a_subscriber_hears_output_and_states(Test *test, String socket)
{
    Arena *a = test->arena;
    Err err = { 0 };
    Client *client;
    test_check(test, client_connect_socket(socket, &client, &err) == ERR_OK, err.msg);
    if (client == nullptr) {
        return;
    }
    test_check(test, client_listen(client, ignore, nullptr, &err) == ERR_OK, "listen");
    Error sent = client_send(client, message(a, "subscribe"), S(""), &err);
    test_check(test, sent == ERR_OK, "subscribe");
    ClientMessage got;
    if (next_of_type(client, "snapshot", &got)) {
        test_check(test, node_get_int(got.header, S("proc"), -1) == 0, "a snapshot of each proc, in order");
        test_check(test, str_contains(got.body, S("hello from db")), "with its log");
        client_message_release(&got);
    } else {
        test_check(test, false, "a snapshot");
    }
    test_check(test, client_send(client, named(a, "start", "console"), S(""), &err) == ERR_OK, "start console");
    bool heard = false;
    int64_t deadline = clock_monotonic_ns() + (int64_t)DEADLINE_MS * NS_PER_MILLISECOND;
    while (!heard && next_of_type(client, "state", &got) && clock_monotonic_ns() < deadline) {
        heard = node_get_int(got.header, S("proc"), -1) == 2 &&
                str_equal(node_get_string(node_get(got.header, S("status")), S("state"), S("")), S("running"));
        client_message_release(&got);
    }
    test_check(test, heard, "the state of a proc that starts");
    Node *input = named(a, "input", "console");
    message_bool(a, input, "reply", false);
    test_check(test, client_send(client, input, S("ping\r"), &err) == ERR_OK, "type into it");
    heard = false;
    while (!heard && next_of_type(client, "output", &got)) {
        heard = node_get_int(got.header, S("proc"), -1) == 2 && str_contains(got.body, S("got ping"));
        client_message_release(&got);
    }
    test_check(test, heard, "its output streams");
    client_close(client);
}

static void server_serves_clients_until_down(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("bprocs-test-"), &dir, &err) == ERR_OK, "a temp dir");
    Config config;
    test_check(test, config_parse(a, S(CONFIG), path_join(a, dir, S("bprocs.yml")), 0, &config, &err) == ERR_OK,
               err.msg);
    RuntimePaths paths = {
        .socket = path_join(a, dir, S("bprocs.sock")),
        .lock = path_join(a, dir, S("bprocs.lock")),
    };
    String socket = paths.socket;
    Serving serving = { 0 };
    if (server_create(&config, &paths, &serving.server, &err) != ERR_OK) {
        test_check(test, false, err.msg);
        return;
    }
    Server *second;
    test_check(test, server_create(&config, &paths, &second, &err) == ERR_IO, "a second server is refused");
    test_check(test, file_exists(socket), "and leaves the first one's socket alone");
    test_check(test, thread_start(&serving.thread, serve, &serving, &err) == ERR_OK, "serve");
    Client *client;
    if (client_connect_socket(socket, &client, &err) == ERR_OK) {
        a_client_drives_the_server(test, client);
        a_subscriber_hears_output_and_states(test, socket);
        Node *reply;
        test_check(test, client_request(client, a, message(a, "down"), S(""), &reply, nullptr, &err) == ERR_OK,
                   "down answers once everything stopped");
        client_close(client);
    } else {
        test_check(test, false, err.msg);
        server_request_down(serving.server);
    }
    thread_join(&serving.thread);
    test_check(test, server_finished(serving.server), "the server is finished");
    server_destroy(serving.server);
    test_check(test, !file_exists(socket), "and its socket gone");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

const TestCase SERVER_TESTS[] = {
    { "server_serves_clients_until_down", server_serves_clients_until_down },
    { nullptr, nullptr },
};
