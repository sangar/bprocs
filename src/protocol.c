#include "protocol.h"

#include <string.h>

#include "check.h"
#include "mc/crypto/sha256.h"
#include "mc/encoding/json.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

enum { ID_DIGITS = 12 };

static const char *const RESTART_NAMES[] = { "no", "on-failure", "always" };
static const char *const CHECK_NAMES[] = { "none", "log", "port", "http", "cmd" };

Error runtime_directory(Arena *arena, String *directory, Err *err)
{
    return dir_runtime(arena, S("bprocs"), directory, err);
}

Error runtime_paths(Arena *arena, String config_path, RuntimePaths *paths, Err *err)
{
    String directory;
    Error e = runtime_directory(arena, &directory, err);
    if (e != ERR_OK) {
        return e;
    }
    uint8_t digest[SHA256_LEN];
    sha256(config_path, digest);
    String id = str_slice(hex_encode(arena, digest, SHA256_LEN), 0, ID_DIGITS);
    String base = path_join(arena, directory, id);
    *paths = (RuntimePaths) {
        .directory = directory,
        .id = id,
        .socket = str_concat(arena, base, S(".sock")),
        .lock = str_concat(arena, base, S(".lock")),
        .start = str_concat(arena, base, S(".start")),
        .log = str_concat(arena, base, S(".log")),
    };
    return ERR_OK;
}

// ---- messages ----

static Node *int_node(Arena *arena, int64_t value)
{
    Node *node = node_scalar(arena, NODE_INT, str_format(arena, "%lld", (long long)value));
    node->integer = value;
    return node;
}

static Node *float_node(Arena *arena, double value)
{
    Node *node = node_create(arena, NODE_FLOAT);
    node->number = value;
    return node;
}

static Node *bool_node(Arena *arena, bool value)
{
    Node *node = node_scalar(arena, NODE_BOOL, value ? S("true") : S("false"));
    node->boolean = value;
    return node;
}

Node *message(Arena *arena, const char *type)
{
    Node *node = node_create(arena, NODE_MAPPING);
    message_text(arena, node, "type", S(type));
    return node;
}

void message_int(Arena *arena, Node *message, const char *key, int64_t value)
{
    node_put(arena, message, S(key), int_node(arena, value));
}

void message_text(Arena *arena, Node *message, const char *key, String value)
{
    node_put(arena, message, S(key), node_scalar(arena, NODE_STRING, value));
}

void message_bool(Arena *arena, Node *message, const char *key, bool value)
{
    node_put(arena, message, S(key), bool_node(arena, value));
}

void message_put(Arena *arena, Node *message, const char *key, Node *value)
{
    node_put(arena, message, S(key), value);
}

Node *message_list(Arena *arena)
{
    return node_create(arena, NODE_SEQUENCE);
}

void message_append_text(Arena *arena, Node *list, String value)
{
    node_append(arena, list, node_scalar(arena, NODE_STRING, value));
}

void message_append_int(Arena *arena, Node *list, int64_t value)
{
    node_append(arena, list, int_node(arena, value));
}

String message_encode(Arena *arena, const Node *message)
{
    return json_encode(arena, message);
}

Error message_parse(Arena *arena, String header, Node **message, Err *err)
{
    Error e = json_parse(arena, header, message, err);
    if (e != ERR_OK) {
        return e;
    }
    if ((*message)->kind != NODE_MAPPING || message_type(*message).len == 0) {
        return err_set(err, ERR_PARSE, "a message without a type");
    }
    return ERR_OK;
}

String message_type(const Node *message)
{
    return node_get_string(message, S("type"), S(""));
}

String message_error(const Node *message)
{
    return node_get_string(message, S("error"), S(""));
}

// ---- specs ----

static size_t find_name(const char *const *names, size_t count, String name)
{
    for (size_t i = 0; i < count; i++) {
        if (str_equal(name, S(names[i]))) {
            return i;
        }
    }
    return 0;
}

static Node *check_to_node(Arena *arena, const Check *check)
{
    Node *node = node_create(arena, NODE_MAPPING);
    message_text(arena, node, "kind", S(CHECK_NAMES[check->kind]));
    message_text(arena, node, "what", check_describe(arena, check));
    return node;
}

static Check check_from_node(const Node *node)
{
    return (Check) {
        .kind = (CheckKind)find_name(CHECK_NAMES, countof(CHECK_NAMES), node_get_string(node, S("kind"), S(""))),
        .text = node_get_string(node, S("what"), S("")),
    };
}

Node *spec_to_node(Arena *arena, const ProcSpec *spec, const Config *config)
{
    Node *node = node_create(arena, NODE_MAPPING);
    message_text(arena, node, "name", spec->name);
    message_text(arena, node, "command", spec->command);
    message_text(arena, node, "cwd", spec->cwd);
    message_int(arena, node, "port", spec->port);
    message_bool(arena, node, "autostart", spec->autostart);
    message_bool(arena, node, "job", spec->job);
    message_text(arena, node, "restart", S(RESTART_NAMES[spec->restart]));
    message_text(arena, node, "log", spec->log_path);
    if (spec->has_color) {
        message_text(arena, node, "color",
                     str_format(arena, "#%02x%02x%02x", spec->color.r, spec->color.g, spec->color.b));
    }
    Node *needs = message_list(arena);
    for (size_t i = 0; i < spec->need_count; i++) {
        message_append_text(arena, needs, config->procs[spec->needs[i]].name);
    }
    message_put(arena, node, "needs", needs);
    message_put(arena, node, "ready", check_to_node(arena, &spec->ready));
    message_put(arena, node, "health", check_to_node(arena, &spec->health));
    return node;
}

static ProcSpec spec_from_node(const Node *node)
{
    ProcSpec spec = {
        .name = node_get_string(node, S("name"), S("?")),
        .command = node_get_string(node, S("command"), S("")),
        .cwd = node_get_string(node, S("cwd"), S("")),
        .port = (int)node_get_int(node, S("port"), 0),
        .autostart = node_get_bool(node, S("autostart"), false),
        .job = node_get_bool(node, S("job"), false),
        .restart = (RestartPolicy)find_name(RESTART_NAMES, countof(RESTART_NAMES),
                                            node_get_string(node, S("restart"), S(""))),
        .log_path = node_get_string(node, S("log"), S("")),
        .ready = check_from_node(node_get(node, S("ready"))),
        .health = check_from_node(node_get(node, S("health"))),
    };
    String color = node_get_string(node, S("color"), S(""));
    spec.has_color = color.len > 0 && rgb_parse(color, &spec.color);
    return spec;
}

ProcSpec *specs_from_node(Arena *arena, const Node *list, size_t *count)
{
    *count = list != nullptr && list->kind == NODE_SEQUENCE ? list->count : 0;
    ProcSpec *specs = arena_push(arena, arena_size_mul(*count, sizeof *specs));
    for (size_t i = 0; i < *count; i++) {
        specs[i] = spec_from_node(list->items[i]);
    }
    for (size_t i = 0; i < *count; i++) {
        const Node *needs = node_get(list->items[i], S("needs"));
        size_t need_count = needs != nullptr && needs->kind == NODE_SEQUENCE ? needs->count : 0;
        specs[i].needs = arena_push(arena, arena_size_mul(need_count, sizeof *specs[i].needs));
        for (size_t n = 0; n < need_count; n++) {
            for (size_t j = 0; j < *count; j++) {
                if (str_equal(specs[j].name, needs->items[n]->text)) {
                    specs[i].needs[specs[i].need_count++] = j;
                }
            }
        }
    }
    return specs;
}

// ---- statuses ----

Node *status_to_node(Arena *arena, ProcStatus status)
{
    Node *node = node_create(arena, NODE_MAPPING);
    message_text(arena, node, "state", S(proc_state_name(status.state)));
    message_text(arena, node, "health", S(proc_health_name(status.health)));
    message_int(arena, node, "pid", status.pid);
    message_int(arena, node, "exit_code", status.exit_code);
    message_int(arena, node, "signal", status.signal);
    message_int(arena, node, "started_ns", status.started_ns);
    message_int(arena, node, "ready_ns", status.ready_ns);
    message_int(arena, node, "ended_ns", status.ended_ns);
    message_int(arena, node, "restart_at_ns", status.restart_at_ns);
    message_int(arena, node, "restarts", status.restarts);
    message_text(arena, node, "problem", S(status.problem));
    return node;
}

ProcStatus status_from_node(const Node *node)
{
    ProcStatus status = {
        .pid = (int)node_get_int(node, S("pid"), 0),
        .exit_code = (int)node_get_int(node, S("exit_code"), 0),
        .signal = (int)node_get_int(node, S("signal"), 0),
        .started_ns = node_get_int(node, S("started_ns"), 0),
        .ready_ns = node_get_int(node, S("ready_ns"), 0),
        .ended_ns = node_get_int(node, S("ended_ns"), 0),
        .restart_at_ns = node_get_int(node, S("restart_at_ns"), 0),
        .restarts = (unsigned)node_get_int(node, S("restarts"), 0),
    };
    bool known = proc_state_parse(node_get_string(node, S("state"), S("")), &status.state);
    unused(known);
    known = proc_health_parse(node_get_string(node, S("health"), S("")), &status.health);
    unused(known);
    String problem = node_get_string(node, S("problem"), S(""));
    size_t len = min_size(problem.len, sizeof status.problem - 1);
    memcpy(status.problem, problem.data, len);
    status.problem[len] = '\0';
    return status;
}

// ---- histories ----

Node *history_to_node(Arena *arena, const History *history)
{
    Node *list = message_list(arena);
    for (size_t age = history->count; age > 0; age--) {
        node_append(arena, list, float_node(arena, (double)history_at(history, age - 1)));
    }
    return list;
}

void history_from_node(const Node *node, History *history)
{
    if (node == nullptr || node->kind != NODE_SEQUENCE) {
        return;
    }
    for (size_t i = 0; i < node->count; i++) {
        const Node *item = node->items[i];
        double value = item->kind == NODE_FLOAT ? item->number : item->kind == NODE_INT ? (double)item->integer : 0.0;
        history_push(history, (float)value);
    }
}
