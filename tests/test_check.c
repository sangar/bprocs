#include "test.h"

#include <stdatomic.h>

#include "check.h"
#include "mc/platform/platform.h"

// Responder answers every connection on its listener with one HTTP response.
typedef struct Responder {
    int listener;
    const char *response;
    atomic_bool stop;
} Responder;

static void *respond(void *argument)
{
    Responder *responder = argument;
    while (!atomic_load(&responder->stop)) {
        int client;
        if (!net_accept(responder->listener, 50, &client)) {
            continue;
        }
        char request[1024];
        size_t got;
        Error received = net_receive(client, request, sizeof request, &got, nullptr);
        unused(received);
        Error sent = net_send(client, S(responder->response), nullptr);
        unused(sent);
        net_close(client);
    }
    return nullptr;
}

static int listen_anywhere(Test *test, int *listener)
{
    String address = S("");
    test_check(test, net_listen(S("127.0.0.1:0"), listener, nullptr) == ERR_OK, "listen");
    test_check(test, net_local_address(test->arena, *listener, &address, nullptr) == ERR_OK, "address");
    String host;
    String port;
    uint64_t number = 0;
    if (str_cut(address, ':', &host, &port)) {
        bool parsed = str_parse_u64(port, &number);
        unused(parsed);
    }
    return (int)number;
}

static void port_and_http_checks_connect(Test *test)
{
    Responder responder = { .response = "HTTP/1.1 204 No Content\r\n\r\n" };
    atomic_init(&responder.stop, false);
    int port = listen_anywhere(test, &responder.listener);
    Thread thread;
    test_check(test, thread_start(&thread, respond, &responder, nullptr) == ERR_OK, "start a responder");
    ProcSpec spec = { .name = S("web") };
    String problem;
    int64_t second = NS_PER_SECOND;
    Check check = { .kind = CHECK_PORT, .host = S("127.0.0.1"), .port = port };
    test_check(test, check_run(test->arena, &check, &spec, second, &problem), "a port that answers");
    check = (Check) { .kind = CHECK_HTTP, .host = S("127.0.0.1"), .port = port, .path = S("/up") };
    test_check(test, check_run(test->arena, &check, &spec, second, &problem), "a 204 passes");
    responder.response = "HTTP/1.0 503 Service Unavailable\r\n\r\n";
    test_check(test, !check_run(test->arena, &check, &spec, second, &problem), "a 503 fails");
    test_check(test, str_starts_with(problem, S("GET http://127.0.0.1:")), "and says what it asked");
    test_check(test, str_ends_with(problem, S("/up: 503")), "and what came back");
    atomic_store(&responder.stop, true);
    thread_join(&thread);
    net_close(responder.listener);
    check = (Check) { .kind = CHECK_PORT, .host = S("127.0.0.1"), .port = port };
    test_check(test, !check_run(test->arena, &check, &spec, second, &problem), "nothing listens any more");
}

static void cmd_checks_exit_in_time(Test *test)
{
    ProcSpec spec = { .name = S("db"), .cwd = S("/") };
    String problem;
    const char *const passing[] = { "/bin/sh", "-c", "pwd | grep -q '^/$'" };
    Check check = { .kind = CHECK_CMD, .argv = strlist_of(test->arena, countof(passing), passing) };
    test_check(test, check_run(test->arena, &check, &spec, NS_PER_SECOND, &problem), "runs in the proc's directory");
    const char *const failing[] = { "/bin/sh", "-c", "echo not yet; exit 2" };
    check.argv = strlist_of(test->arena, countof(failing), failing);
    test_check(test, !check_run(test->arena, &check, &spec, NS_PER_SECOND, &problem), "a non-zero exit fails");
    test_check(test, str_ends_with(problem, S("exited with code 2: not yet")), "with its last words");
    const char *const slow[] = { "sleep", "5" };
    check.argv = strlist_of(test->arena, countof(slow), slow);
    int64_t started = clock_monotonic_ns();
    test_check(test, !check_run(test->arena, &check, &spec, 200 * NS_PER_MILLISECOND, &problem), "too slow fails");
    test_check(test, clock_monotonic_ns() - started < 2 * (int64_t)NS_PER_SECOND, "without waiting it out");
    test_check(test, str_ends_with(problem, S("took too long")), "and says so");
}

const TestCase CHECK_TESTS[] = {
    { "port_and_http_checks_connect", port_and_http_checks_connect },
    { "cmd_checks_exit_in_time", cmd_checks_exit_in_time },
    { nullptr, nullptr },
};
