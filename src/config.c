#include "config.h"

#include "mc/encoding/dotenv.h"
#include "mc/encoding/yaml.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"

static const char *const CONFIG_NAMES[] = { "bprocs.yml", "bprocs.yaml", "Procfile.dev", "Procfile" };

enum {
    READY_EVERY_MS = 1000,
    HEALTH_EVERY_MS = 10000,
    HEALTH_FAILURES = 3,
    STOP_TIMEOUT_MS = 5000,
    PROCFILE_FIRST_PORT = 3000, // 3000 for the first proc, 3001 for the next
    PROCFILE_PORT_STEP = 1,
};

// Reader is what reading one config needs at every level. An app of
// include: is read with a reader of its own.
typedef struct Reader {
    Arena *arena;
    String root;      // the directory of the config bprocs was started with, which errors show paths from
    String directory; // the directory of the config being read
    StringList env;   // what every proc starts from: the top level's env_file: and env:, after an app's .env
    String log_dir;
    int first_port;    // a Procfile's first PORT
    bool included;     // an app of include:, which leaves theme:, scrollback: and include: to the config above
    StringList *files; // every file read, for Config.files
    Err *err;
} Reader;

[[nodiscard]] static Error invalid(Err *err, const Node *node, String where, const char *problem)
{
    // The YAML parser gives an empty value, such as an app of include: with no settings, no line.
    if (node->line == 0) {
        return err_set(err, ERR_PARSE, "%.*s: %s", (int)where.len, where.data, problem);
    }
    return err_set(err, ERR_PARSE, "line %zu: %.*s: %s", node->line, (int)where.len, where.data, problem);
}

static String setting(Arena *arena, String where, String key)
{
    return str_format(arena, "%.*s.%.*s", (int)where.len, where.data, (int)key.len, key.data);
}

static String relative_to(Arena *arena, String directory, String path)
{
    return path_is_absolute(path) ? path_clean(arena, path) : path_clean(arena, path_join(arena, directory, path));
}

// shown is how errors show path: below the root, relative to it.
static String shown(const Reader *reader, String path)
{
    String relative;
    return path_relative(reader->root, path, &relative) ? relative : path;
}

static bool is_scalar(const Node *node)
{
    return node->kind != NODE_SEQUENCE && node->kind != NODE_MAPPING && node->kind != NODE_NULL;
}

[[nodiscard]] static Error read_string(const Node *node, String where, String *value, Err *err)
{
    if (!is_scalar(node)) {
        return invalid(err, node, where, "expected text");
    }
    *value = node->text;
    return ERR_OK;
}

[[nodiscard]] static Error read_bool(const Node *node, String where, bool *value, Err *err)
{
    if (node->kind != NODE_BOOL) {
        return invalid(err, node, where, "expected true or false");
    }
    *value = node->boolean;
    return ERR_OK;
}

[[nodiscard]] static Error read_port(const Node *node, String where, int *port, Err *err)
{
    if (node->kind != NODE_INT || node->integer <= 0 || node->integer > 65535) {
        return invalid(err, node, where, "expected a port number");
    }
    *port = (int)node->integer;
    return ERR_OK;
}

[[nodiscard]] static Error read_duration(const Node *node, String where, int64_t *ns, Err *err)
{
    if (!is_scalar(node) || !fmt_parse_duration(node->text, ns) || *ns <= 0) {
        return invalid(err, node, where, "expected a duration such as 500ms, 10s or 2m");
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_argv(Arena *arena, const Node *node, String where, StringList *argv, Err *err)
{
    if (node->kind != NODE_SEQUENCE || node->count == 0) {
        return invalid(err, node, where, "expected a list of words, such as [\"rails\", \"server\"]");
    }
    for (size_t i = 0; i < node->count; i++) {
        if (!is_scalar(node->items[i])) {
            return invalid(err, node->items[i], where, "expected a word");
        }
        strlist_push(arena, argv, node->items[i]->text);
    }
    return ERR_OK;
}

static StringList shell_argv(Arena *arena, String script)
{
    const char *const shell[] = { "/bin/sh", "-c" };
    StringList argv = strlist_of(arena, countof(shell), shell);
    strlist_push(arena, &argv, script);
    return argv;
}

// read_command reads a command that is a script for sh -c when it is text
// and the words to run when it is a list.
[[nodiscard]] static Error read_command(Arena *arena, const Node *node, String where, StringList *argv, Err *err)
{
    *argv = (StringList) { 0 };
    if (node->kind == NODE_SEQUENCE) {
        return read_argv(arena, node, where, argv, err);
    }
    String script;
    Error e = read_string(node, where, &script, err);
    if (e != ERR_OK) {
        return e;
    }
    if (str_trim(script).len == 0) {
        return invalid(err, node, where, "expected a command");
    }
    *argv = shell_argv(arena, script);
    return ERR_OK;
}

// one_line shows a script by its first line, marking that more follows.
static String one_line(Arena *arena, String script)
{
    String trimmed = str_trim(script);
    String first = str_trim(str_first_line(trimmed));
    if (first.len == trimmed.len) {
        return first;
    }
    return str_format(arena, "%.*s …", (int)first.len, first.data);
}

// command_text is how a command shows: a script by its first line, words joined.
static String command_text(Arena *arena, StringList argv, bool script)
{
    return script ? one_line(arena, argv.items[2]) : str_join(arena, argv, S(" "));
}

[[nodiscard]] static Error read_env(Arena *arena, const Node *node, String where, StringList *env, Err *err)
{
    if (node->kind != NODE_MAPPING) {
        return invalid(err, node, where, "expected NAME: value pairs");
    }
    for (size_t i = 0; i < node->entry_count; i++) {
        const Node *value = node->entries[i].value;
        String name = node->entries[i].key;
        if (value->kind == NODE_NULL) {
            strlist_push(arena, env, name);
            continue;
        }
        if (!is_scalar(value)) {
            return invalid(err, value, where, "expected a value");
        }
        strlist_push(arena, env, str_format(arena, "%.*s=%.*s", (int)name.len, name.data, (int)value->text.len,
                                            value->text.data));
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_env_file(Reader *reader, const Node *path_node, String where, StringList *env)
{
    String path;
    Error e = read_string(path_node, where, &path, reader->err);
    if (e != ERR_OK) {
        return e;
    }
    String absolute = relative_to(reader->arena, reader->directory, path);
    String text;
    e = file_read_all(reader->arena, absolute, &text, reader->err);
    if (e == ERR_OK) {
        e = dotenv_parse(reader->arena, text, env, reader->err);
    }
    if (e != ERR_OK) {
        return err_wrap(reader->err, ERR_PARSE, "line %zu: %.*s", path_node->line, (int)where.len, where.data);
    }
    strlist_push(reader->arena, reader->files, absolute);
    return ERR_OK;
}

// read_env_files reads one env_file: path or a list of them, in order.
[[nodiscard]] static Error read_env_files(Reader *reader, const Node *node, String where, StringList *env)
{
    if (node->kind != NODE_SEQUENCE) {
        return read_env_file(reader, node, where, env);
    }
    for (size_t i = 0; i < node->count; i++) {
        Error e = read_env_file(reader, node->items[i], where, env);
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_signal(const Node *node, String where, PtySignal *signal, Err *err)
{
    static const struct {
        const char *name;
        PtySignal signal;
    } SIGNALS[] = {
        { "SIGINT", PTY_INTERRUPT }, { "SIGTERM", PTY_TERMINATE }, { "SIGHUP", PTY_RELOAD },
        { "SIGQUIT", PTY_QUIT },     { "SIGKILL", PTY_KILL },
    };
    for (size_t i = 0; is_scalar(node) && i < countof(SIGNALS); i++) {
        if (str_equal(node->text, S(SIGNALS[i].name))) {
            *signal = SIGNALS[i].signal;
            return ERR_OK;
        }
    }
    return invalid(err, node, where, "expected SIGINT, SIGTERM, SIGHUP, SIGQUIT or SIGKILL");
}

// read_stop reads a signal, or {signal, timeout}.
[[nodiscard]] static Error read_stop(Arena *arena, const Node *node, String where, ProcSpec *spec, Err *err)
{
    if (node->kind != NODE_MAPPING) {
        return read_signal(node, where, &spec->stop, err);
    }
    for (size_t i = 0; i < node->entry_count; i++) {
        String key = node->entries[i].key;
        const Node *value = node->entries[i].value;
        String at = setting(arena, where, key);
        Error e = str_equal(key, S("signal"))    ? read_signal(value, at, &spec->stop, err)
                  : str_equal(key, S("timeout")) ? read_duration(value, at, &spec->stop_timeout_ns, err)
                                                 : invalid(err, value, at, "unknown setting");
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_restart(const Node *node, String where, RestartPolicy *restart, Err *err)
{
    if (node->kind == NODE_BOOL && !node->boolean) {
        *restart = RESTART_NO;
        return ERR_OK;
    }
    String text = is_scalar(node) ? node->text : S("");
    if (str_equal(text, S("no"))) {
        *restart = RESTART_NO;
    } else if (str_equal(text, S("on-failure"))) {
        *restart = RESTART_ON_FAILURE;
    } else if (str_equal(text, S("always"))) {
        *restart = RESTART_ALWAYS;
    } else {
        return invalid(err, node, where, "expected no, on-failure or always");
    }
    return ERR_OK;
}

// read_url reads http://host[:port][/path]; only plain HTTP is supported.
[[nodiscard]] static Error read_url(const Node *node, String where, Check *check, Err *err)
{
    String rest = node->text;
    if (!str_starts_with(rest, S("http://"))) {
        return invalid(err, node, where, "expected a path such as /up, or a URL such as http://localhost:3000/up");
    }
    rest = str_slice(rest, 7, rest.len);
    size_t slash;
    String authority = str_find_char(rest, '/', &slash) ? str_slice(rest, 0, slash) : rest;
    check->path = authority.len < rest.len ? str_slice(rest, authority.len, rest.len) : S("/");
    String host = authority;
    String port_text;
    uint64_t port = 80;
    if (str_cut(authority, ':', &host, &port_text) && (!str_parse_u64(port_text, &port) || port == 0 || port > 65535)) {
        return invalid(err, node, where, "expected a port number after the host");
    }
    check->host = host;
    check->port = (int)port;
    if (check->host.len == 0) {
        return invalid(err, node, where, "expected a host after http://");
    }
    return ERR_OK;
}

// read_check_kind reads the one key of a check that says what it checks.
[[nodiscard]] static Error read_check_kind(Arena *arena, String key, const Node *value, String where, Check *check,
                                           Err *err)
{
    if (check->kind != CHECK_NONE) {
        return invalid(err, value, where, "a check has one of log, port, http or cmd");
    }
    if (str_equal(key, S("log"))) {
        check->kind = CHECK_LOG;
        return read_string(value, where, &check->text, err);
    }
    if (str_equal(key, S("port"))) {
        check->kind = CHECK_PORT;
        check->host = S("localhost");
        return read_port(value, where, &check->port, err);
    }
    if (str_equal(key, S("http"))) {
        check->kind = CHECK_HTTP;
        // A bare path is resolved against the proc's port once it is known.
        return read_string(value, where, &check->path, err);
    }
    check->kind = CHECK_CMD;
    return read_command(arena, value, where, &check->argv, err);
}

[[nodiscard]] static Error read_check(Arena *arena, const Node *node, String where, bool health, Check *check, Err *err)
{
    *check = (Check) {
        .every_ns = (int64_t)(health ? HEALTH_EVERY_MS : READY_EVERY_MS) * NS_PER_MILLISECOND,
        .failures = health ? HEALTH_FAILURES : 1,
    };
    if (node->kind != NODE_MAPPING) {
        return invalid(err, node, where, "expected a check such as { http: /up } or { port: 5432 }");
    }
    for (size_t i = 0; i < node->entry_count; i++) {
        String key = node->entries[i].key;
        const Node *value = node->entries[i].value;
        String at = setting(arena, where, key);
        Error e;
        if (str_equal(key, S("log")) || str_equal(key, S("port")) || str_equal(key, S("http")) ||
            str_equal(key, S("cmd"))) {
            e = read_check_kind(arena, key, value, at, check, err);
        } else if (str_equal(key, S("every"))) {
            e = read_duration(value, at, &check->every_ns, err);
        } else if (str_equal(key, S("timeout")) && !health) {
            e = read_duration(value, at, &check->timeout_ns, err);
        } else if (str_equal(key, S("failures")) && health) {
            e = value->kind == NODE_INT && value->integer > 0 && value->integer < 1000
                    ? ERR_OK
                    : invalid(err, value, at, "expected a number of failures");
            check->failures = e == ERR_OK ? (unsigned)value->integer : check->failures;
        } else {
            e = invalid(err, value, at, "unknown setting");
        }
        if (e != ERR_OK) {
            return e;
        }
    }
    if (check->kind == CHECK_NONE) {
        return invalid(err, node, where, "a check needs one of log, port, http or cmd");
    }
    if (health && check->kind == CHECK_LOG) {
        return invalid(err, node, where, "health takes port, http or cmd");
    }
    return ERR_OK;
}

// finish_http resolves an http check against the proc's own port.
[[nodiscard]] static Error finish_http(const Node *node, String where, int port, Check *check, Err *err)
{
    if (check->kind != CHECK_HTTP) {
        return ERR_OK;
    }
    const Node *url = node_get(node, S("http"));
    if (!str_starts_with(check->path, S("/"))) {
        return read_url(url, where, check, err);
    }
    if (port == 0) {
        return invalid(err, url, where, "a path needs the proc's port: set port:, or give a whole URL");
    }
    check->host = S("localhost");
    check->port = port;
    return ERR_OK;
}

// port_number is text as a port number, 0 when it is not one.
static int port_number(String text)
{
    uint64_t number;
    return str_parse_u64(text, &number) && number > 0 && number < 65536 ? (int)number : 0;
}

// either is port, or fallback when port is 0.
static int either(int port, int fallback)
{
    return port > 0 ? port : fallback;
}

// port_from_env is the value of the last PORT among the env entries, 0 when there is none.
static int port_from_env(StringList env)
{
    int port = 0;
    for (size_t i = 0; i < env.count; i++) {
        String name;
        String value;
        int number = str_cut(env.items[i], '=', &name, &value) && str_equal(name, S("PORT")) ? port_number(value) : 0;
        port = number > 0 ? number : port;
    }
    return port;
}

static bool is_name(String name)
{
    if (name.len == 0 || name.data[0] == '-' || name.data[0] == '_') {
        return false;
    }
    for (size_t i = 0; i < name.len; i++) {
        char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

// ProcEnv is a proc's env: and env_file: entries, kept apart so the files come first.
typedef struct ProcEnv {
    StringList from_files;
    StringList entries;
} ProcEnv;

[[nodiscard]] static Error read_log(Reader *reader, const Node *value, String where, ProcSpec *spec)
{
    if (value->kind == NODE_BOOL) {
        spec->log_path = value->boolean
                             ? path_join(reader->arena, reader->log_dir,
                                         str_format(reader->arena, "%.*s.log", (int)spec->name.len, spec->name.data))
                             : S("");
        return ERR_OK;
    }
    String path;
    Error e = read_string(value, where, &path, reader->err);
    spec->log_path = e == ERR_OK ? relative_to(reader->arena, reader->directory, path) : S("");
    return e;
}

[[nodiscard]] static Error read_proc_key(Reader *reader, ProcSpec *spec, ProcEnv *env, String key, const Node *value,
                                         String where)
{
    Arena *arena = reader->arena;
    Err *err = reader->err;
    if (str_equal(key, S("run"))) {
        Error e = read_command(arena, value, where, &spec->argv, err);
        spec->command = e == ERR_OK ? command_text(arena, spec->argv, value->kind != NODE_SEQUENCE) : spec->command;
        return e;
    }
    if (str_equal(key, S("cwd"))) {
        String cwd;
        Error e = read_string(value, where, &cwd, err);
        spec->cwd = e == ERR_OK ? relative_to(arena, reader->directory, cwd) : spec->cwd;
        return e;
    }
    if (str_equal(key, S("env"))) {
        return read_env(arena, value, where, &env->entries, err);
    }
    if (str_equal(key, S("env_file"))) {
        return read_env_files(reader, value, where, &env->from_files);
    }
    if (str_equal(key, S("port"))) {
        return read_port(value, where, &spec->port, err);
    }
    if (str_equal(key, S("autostart"))) {
        return read_bool(value, where, &spec->autostart, err);
    }
    if (str_equal(key, S("job"))) {
        return read_bool(value, where, &spec->job, err);
    }
    if (str_equal(key, S("needs"))) {
        // Names are resolved once every proc is known.
        return value->kind == NODE_SEQUENCE ? ERR_OK : invalid(err, value, where, "expected a list of proc names");
    }
    if (str_equal(key, S("ready"))) {
        return read_check(arena, value, where, false, &spec->ready, err);
    }
    if (str_equal(key, S("health"))) {
        return read_check(arena, value, where, true, &spec->health, err);
    }
    if (str_equal(key, S("restart"))) {
        return read_restart(value, where, &spec->restart, err);
    }
    if (str_equal(key, S("stop"))) {
        return read_stop(arena, value, where, spec, err);
    }
    if (str_equal(key, S("log"))) {
        return read_log(reader, value, where, spec);
    }
    if (str_equal(key, S("color"))) {
        spec->has_color = is_scalar(value) && rgb_parse(value->text, &spec->color);
        return spec->has_color ? ERR_OK : invalid(err, value, where, "expected a colour such as \"#7aa2f7\"");
    }
    return invalid(err, value, where, "unknown setting");
}

// finish_proc checks what only the whole proc can tell and puts its environment together.
[[nodiscard]] static Error finish_proc(Reader *reader, const Node *node, String where, ProcEnv *env, ProcSpec *spec)
{
    Arena *arena = reader->arena;
    Err *err = reader->err;
    if (spec->argv.count == 0) {
        return invalid(err, node, where, "needs run: to say what to run");
    }
    // The proc's own port comes first, so an env: or env_file: PORT still wins.
    spec->env = strlist_copy(arena, reader->env);
    if (spec->port > 0) {
        strlist_push(arena, &spec->env, str_format(arena, "PORT=%d", spec->port));
    }
    strlist_push_all(arena, &spec->env, env->from_files);
    strlist_push_all(arena, &spec->env, env->entries);
    if (spec->port == 0) {
        spec->port = port_from_env(spec->env);
    }
    Error e = finish_http(node_get(node, S("ready")), setting(arena, where, S("ready")), spec->port, &spec->ready, err);
    if (e == ERR_OK) {
        e = finish_http(node_get(node, S("health")), setting(arena, where, S("health")), spec->port, &spec->health,
                        err);
    }
    if (e != ERR_OK) {
        return e;
    }
    if (spec->job && spec->ready.kind != CHECK_NONE) {
        return invalid(err, node_get(node, S("ready")), where, "a job is done when it exits, so it takes no ready:");
    }
    if (spec->job && spec->restart == RESTART_ALWAYS) {
        return invalid(err, node_get(node, S("restart")), where, "a job cannot restart: always");
    }
    return ERR_OK;
}

static ProcSpec default_spec(String name, String directory)
{
    return (ProcSpec) {
        .name = name,
        .cwd = directory,
        .autostart = true,
        .stop = PTY_TERMINATE,
        .stop_timeout_ns = (int64_t)STOP_TIMEOUT_MS * NS_PER_MILLISECOND,
    };
}

[[nodiscard]] static Error read_proc(Reader *reader, String name, const Node *node, ProcSpec *spec)
{
    *spec = default_spec(name, reader->directory);
    String where = setting(reader->arena, S("procs"), name);
    if (!is_name(name)) {
        return invalid(reader->err, node, where, "a name is lowercase letters, digits, - and _");
    }
    if (node->kind != NODE_MAPPING) {
        return invalid(reader->err, node, where, "expected settings such as run:");
    }
    ProcEnv env = { 0 };
    for (size_t i = 0; i < node->entry_count; i++) {
        String key = node->entries[i].key;
        Error e = read_proc_key(reader, spec, &env, key, node->entries[i].value, setting(reader->arena, where, key));
        if (e != ERR_OK) {
            return e;
        }
    }
    return finish_proc(reader, node, where, &env, spec);
}

// read_needs turns a list of names into indexes into config's procs; self is
// the name of what needs them.
[[nodiscard]] static Error read_needs(Reader *reader, const Node *needs, String where, const Config *config,
                                      String self, size_t **indexes, size_t *count)
{
    *indexes = arena_push(reader->arena, arena_size_mul(needs->count, sizeof **indexes));
    *count = 0;
    for (size_t i = 0; i < needs->count; i++) {
        const Node *item = needs->items[i];
        size_t index;
        if (!is_scalar(item) || !config_proc_index(config, item->text, &index)) {
            return invalid(reader->err, item, where,
                           str_cstr(reader->arena, str_format(reader->arena, "no proc called %.*s",
                                                              (int)item->text.len, item->text.data)));
        }
        if (str_equal(item->text, self)) {
            return invalid(reader->err, item, where, "a proc cannot need itself");
        }
        (*indexes)[(*count)++] = index;
    }
    return ERR_OK;
}

typedef enum Visit { UNVISITED, VISITING, VISITED } Visit;

// find_cycle walks the needs from proc depth first and reports the first
// loop it meets in path, which holds the procs on the way.
static bool find_cycle(const Config *config, size_t proc, Visit *visits, size_t *path, size_t *depth)
{
    visits[proc] = VISITING;
    path[(*depth)++] = proc;
    const ProcSpec *spec = &config->procs[proc];
    for (size_t i = 0; i < spec->need_count; i++) {
        size_t need = spec->needs[i];
        if (visits[need] == VISITING) {
            path[(*depth)++] = need;
            return true;
        }
        if (visits[need] == UNVISITED && find_cycle(config, need, visits, path, depth)) {
            return true;
        }
    }
    visits[proc] = VISITED;
    (*depth)--;
    return false;
}

[[nodiscard]] static Error check_cycles(Reader *reader, const Node *procs, const Config *config)
{
    Visit *visits = arena_push(reader->arena, arena_size_mul(config->proc_count, sizeof *visits));
    size_t *path = arena_push(reader->arena, arena_size_mul(config->proc_count + 1, sizeof *path));
    for (size_t i = 0; i < config->proc_count; i++) {
        size_t depth = 0;
        if (visits[i] != UNVISITED || !find_cycle(config, i, visits, path, &depth)) {
            continue;
        }
        // The path ends where the loop closes; the loop starts at that proc's first visit.
        size_t start = 0;
        while (path[start] != path[depth - 1]) {
            start++;
        }
        StringList names = { 0 };
        for (size_t at = start; at < depth; at++) {
            strlist_push(reader->arena, &names, config->procs[path[at]].name);
        }
        String loop = str_join(reader->arena, names, S(" → "));
        const ProcSpec *first = &config->procs[path[start]];
        return invalid(reader->err, node_get(procs->entries[path[start]].value, S("needs")),
                       setting(reader->arena, setting(reader->arena, S("procs"), first->name), S("needs")),
                       str_cstr(reader->arena, str_format(reader->arena, "needs go round in a loop: %.*s",
                                                          (int)loop.len, loop.data)));
    }
    return ERR_OK;
}

[[nodiscard]] static Error check_ports(Reader *reader, const Node *procs, const Config *config)
{
    for (size_t i = 0; i < config->proc_count; i++) {
        for (size_t j = 0; j < i; j++) {
            const ProcSpec *a = &config->procs[j];
            const ProcSpec *b = &config->procs[i];
            if (b->port > 0 && b->port == a->port) {
                return invalid(reader->err, procs->entries[i].value, setting(reader->arena, S("procs"), b->name),
                               str_cstr(reader->arena, str_format(reader->arena, "port %d is also the port of %.*s",
                                                                  b->port, (int)a->name.len, a->name.data)));
            }
        }
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_procs(Reader *reader, const Node *procs, Config *config)
{
    if (procs->kind != NODE_MAPPING || procs->entry_count == 0) {
        return invalid(reader->err, procs, S("procs"), "expected one or more named processes");
    }
    config->procs = arena_push(reader->arena, arena_size_mul(procs->entry_count, sizeof(ProcSpec)));
    for (size_t i = 0; i < procs->entry_count; i++) {
        Error e = read_proc(reader, procs->entries[i].key, procs->entries[i].value, &config->procs[i]);
        if (e != ERR_OK) {
            return e;
        }
        config->proc_count++;
    }
    for (size_t i = 0; i < config->proc_count; i++) {
        ProcSpec *spec = &config->procs[i];
        String where = setting(reader->arena, setting(reader->arena, S("procs"), spec->name), S("needs"));
        const Node *needs = node_get(procs->entries[i].value, S("needs"));
        Error e = needs == nullptr
                      ? ERR_OK
                      : read_needs(reader, needs, where, config, spec->name, &spec->needs, &spec->need_count);
        if (e != ERR_OK) {
            return e;
        }
    }
    Error e = check_cycles(reader, procs, config);
    return e == ERR_OK ? check_ports(reader, procs, config) : e;
}

// Sections is what read_top leaves for later: procs: needs the other
// settings first, and include: the procs.
typedef struct Sections {
    const Node *procs;
    const Node *include;
    const Node *include_defaults;
} Sections;

static bool is_top_only(String key)
{
    return str_equal(key, S("theme")) || str_equal(key, S("scrollback")) || str_equal(key, S("include")) ||
           str_equal(key, S("include_defaults"));
}

// read_top reads every top-level setting but the sections.
[[nodiscard]] static Error read_top(Reader *reader, const Node *root, Config *config, Sections *sections)
{
    String log_dir = S("log/bprocs");
    StringList from_files = { 0 };
    StringList entries = { 0 };
    for (size_t i = 0; i < root->entry_count; i++) {
        String key = root->entries[i].key;
        const Node *value = root->entries[i].value;
        Error e = ERR_OK;
        if (reader->included && is_top_only(key)) {
            e = invalid(reader->err, value, key, "only the config that includes this one sets it");
        } else if (str_equal(key, S("procs"))) {
            sections->procs = value;
        } else if (str_equal(key, S("include"))) {
            sections->include = value;
        } else if (str_equal(key, S("include_defaults"))) {
            sections->include_defaults = value;
        } else if (str_equal(key, S("theme"))) {
            e = read_string(value, key, &config->theme, reader->err);
        } else if (str_equal(key, S("scrollback"))) {
            e = value->kind == NODE_INT && value->integer > 0
                    ? ERR_OK
                    : invalid(reader->err, value, key, "expected a number of lines");
            config->scrollback = e == ERR_OK ? (size_t)value->integer : config->scrollback;
        } else if (str_equal(key, S("env"))) {
            e = read_env(reader->arena, value, key, &entries, reader->err);
        } else if (str_equal(key, S("env_file"))) {
            e = read_env_files(reader, value, key, &from_files);
        } else if (str_equal(key, S("log_dir"))) {
            e = read_string(value, key, &log_dir, reader->err);
        } else {
            e = invalid(reader->err, value, key, "unknown setting");
        }
        if (e != ERR_OK) {
            return e;
        }
    }
    reader->log_dir = relative_to(reader->arena, reader->directory, log_dir);
    strlist_push_all(reader->arena, &reader->env, from_files);
    strlist_push_all(reader->arena, &reader->env, entries);
    return ERR_OK;
}

// is_procfile_name is foreman's rule for a name, which allows capitals.
static bool is_procfile_name(String name)
{
    for (size_t i = 0; i < name.len; i++) {
        char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return name.len > 0;
}

// read_dotenv reads the .env beside the config, which may not be there.
[[nodiscard]] static Error read_dotenv(Reader *reader, StringList *env)
{
    String path = path_join(reader->arena, reader->directory, S(".env"));
    if (!file_exists(path)) {
        return ERR_OK;
    }
    String text;
    Error e = file_read_all(reader->arena, path, &text, reader->err);
    if (e == ERR_OK) {
        e = dotenv_parse(reader->arena, text, env, reader->err);
    }
    if (e != ERR_OK) {
        String at = shown(reader, path);
        return err_wrap(reader->err, ERR_PARSE, "%.*s", (int)at.len, at.data);
    }
    strlist_push(reader->arena, reader->files, path);
    return ERR_OK;
}

// read_procfile reads a Procfile as foreman does: name: command lines run in
// sh -c, each with the reader's env and PORT in steps of 1 from its
// first_port. That PORT is not the proc's port, as bprocs cannot tell whether
// the program listens on it.
[[nodiscard]] static Error read_procfile(Reader *reader, String text, Config *config)
{
    Arena *arena = reader->arena;
    int port = reader->first_port;
    StringList lines = str_split(arena, text, '\n');
    config->procs = arena_push(arena, arena_size_mul(lines.count, sizeof(ProcSpec)));
    for (size_t i = 0; i < lines.count; i++) {
        String line = str_trim(lines.items[i]);
        if (line.len == 0 || line.data[0] == '#') {
            continue;
        }
        String name;
        String command;
        if (!str_cut(line, ':', &name, &command) || !is_procfile_name(name) || str_trim(command).len == 0) {
            return err_set(reader->err, ERR_PARSE, "line %zu: expected name: command, such as web: bin/rails server",
                           i + 1);
        }
        size_t index;
        if (config_proc_index(config, name, &index)) {
            return err_set(reader->err, ERR_PARSE, "line %zu: %.*s: there is already a proc called %.*s", i + 1,
                           (int)name.len, name.data, (int)name.len, name.data);
        }
        ProcSpec *spec = &config->procs[config->proc_count++];
        *spec = default_spec(name, reader->directory);
        spec->command = str_trim(command);
        spec->argv = shell_argv(arena, spec->command);
        spec->env = strlist_copy(arena, reader->env);
        strlist_push(arena, &spec->env, str_format(arena, "PORT=%d", port));
        port += PROCFILE_PORT_STEP;
    }
    if (config->proc_count == 0) {
        return err_set(reader->err, ERR_PARSE, "no lines such as web: bin/rails server say what to run");
    }
    return ERR_OK;
}

static bool is_procfile(String path)
{
    String base = path_base(path);
    return str_equal(base, S("Procfile")) || str_starts_with(base, S("Procfile."));
}

// read_yaml reads a bprocs.yml into config, all but its include:, which it leaves in sections.
[[nodiscard]] static Error read_yaml(Reader *reader, String text, Config *config, Sections *sections)
{
    Node *root;
    Error parsed = yaml_parse(reader->arena, text, &root, reader->err);
    if (parsed != ERR_OK) {
        return parsed;
    }
    if (root->kind != NODE_MAPPING) {
        return err_set(reader->err, ERR_PARSE, "expected settings with a procs: section");
    }
    Error e = read_top(reader, root, config, sections);
    if (e != ERR_OK) {
        return e;
    }
    if (sections->procs == nullptr && sections->include == nullptr) {
        return err_set(reader->err, ERR_PARSE, "no procs: section says what to run");
    }
    return sections->procs == nullptr ? ERR_OK : read_procs(reader, sections->procs, config);
}

// Include is an app of include:, or the include_defaults: every app starts from.
typedef struct Include {
    String name;
    String directory; // absolute
    const Node *node; // its settings, for errors
    int port;         // its Procfile's first PORT; 0 when not set
    size_t *needs;    // indexes of top-level procs
    size_t need_count;
    StringList setup; // the argv of its setup job; empty when there is none
    String setup_command;
    StringList env;
    StringList wrap;
    bool autostart;
} Include;

// Included is an app once its config has been read.
typedef struct Included {
    Include include;
    Config config;
    StringList env; // what its setup job starts from, as its procs did
    bool procfile;
} Included;

// include_node is the node of an include's setting, or the include's when
// the setting came from include_defaults:, for errors.
static const Node *include_node(const Include *include, String key)
{
    const Node *node = node_get(include->node, key);
    return node != nullptr ? node : include->node;
}

[[nodiscard]] static Error read_include_key(Reader *reader, const Config *top, Include *include, String key,
                                            const Node *value, String where, bool defaults)
{
    Arena *arena = reader->arena;
    Err *err = reader->err;
    if (defaults && (str_equal(key, S("dir")) || str_equal(key, S("port")))) {
        return invalid(err, value, where, "each app sets its own");
    }
    if (str_equal(key, S("dir"))) {
        String dir;
        Error e = read_string(value, where, &dir, err);
        include->directory = e == ERR_OK ? relative_to(arena, reader->directory, dir) : include->directory;
        return e;
    }
    if (str_equal(key, S("port"))) {
        return read_port(value, where, &include->port, err);
    }
    if (str_equal(key, S("needs"))) {
        return value->kind == NODE_SEQUENCE
                   ? read_needs(reader, value, where, top, include->name, &include->needs, &include->need_count)
                   : invalid(err, value, where, "expected a list of proc names");
    }
    if (str_equal(key, S("setup"))) {
        Error e = read_command(arena, value, where, &include->setup, err);
        include->setup_command = e == ERR_OK ? command_text(arena, include->setup, value->kind != NODE_SEQUENCE) : S("");
        return e;
    }
    if (str_equal(key, S("env"))) {
        return read_env(arena, value, where, &include->env, err);
    }
    if (str_equal(key, S("wrap"))) {
        include->wrap = (StringList) { 0 };
        return read_argv(arena, value, where, &include->wrap, err);
    }
    if (str_equal(key, S("autostart"))) {
        return read_bool(value, where, &include->autostart, err);
    }
    return invalid(err, value, where, "unknown setting");
}

// read_include reads the settings of an app, or of include_defaults:, over
// what include holds. Its needs: name procs of top.
[[nodiscard]] static Error read_include(Reader *reader, const Config *top, const Node *node, String where, bool defaults,
                                        Include *include)
{
    if (node->kind == NODE_NULL) {
        return ERR_OK;
    }
    if (node->kind != NODE_MAPPING) {
        return invalid(reader->err, node, where, "expected settings such as port: or setup:");
    }
    for (size_t i = 0; i < node->entry_count; i++) {
        String key = node->entries[i].key;
        Error e = read_include_key(reader, top, include, key, node->entries[i].value,
                                   setting(reader->arena, where, key), defaults);
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

// find_app_config looks for a config in directory alone, in config_find's order.
static bool find_app_config(Arena *arena, String directory, String *path)
{
    for (size_t i = 0; i < countof(CONFIG_NAMES); i++) {
        *path = path_join(arena, directory, S(CONFIG_NAMES[i]));
        if (file_exists(*path)) {
            return true;
        }
    }
    return false;
}

// read_app reads the config in an app's directory as a config of its own.
// Its procs start from the env of the config above, then the app's .env.
[[nodiscard]] static Error read_app(Reader *outer, Included *app)
{
    Arena *arena = outer->arena;
    Err *err = outer->err;
    const Include *include = &app->include;
    String where = setting(arena, S("include"), include->name);
    String path;
    if (!find_app_config(arena, include->directory, &path)) {
        String dir = shown(outer, include->directory);
        return invalid(err, include->node, where,
                       str_cstr(arena, str_format(arena, "no bprocs.yml, bprocs.yaml, Procfile.dev or Procfile in %.*s",
                                                  (int)dir.len, dir.data)));
    }
    app->procfile = is_procfile(path);
    if (include->port > 0 && !app->procfile) {
        return invalid(err, include_node(include, S("port")), setting(arena, where, S("port")),
                       "a bprocs.yml sets the ports of its procs itself");
    }
    app->config = (Config) { .path = path, .directory = include->directory, .scrollback = DEFAULT_SCROLLBACK };
    Reader reader = {
        .arena = arena,
        .root = outer->root,
        .directory = include->directory,
        .env = strlist_copy(arena, outer->env),
        .included = true,
        .files = outer->files,
        .err = err,
    };
    String text;
    Error e = file_read_all(arena, path, &text, err);
    if (e != ERR_OK) {
        return e;
    }
    strlist_push(arena, outer->files, path);
    StringList dotenv = { 0 };
    e = read_dotenv(&reader, &dotenv);
    if (e != ERR_OK) {
        return e;
    }
    strlist_push_all(arena, &reader.env, dotenv);
    reader.first_port = either(include->port, either(port_from_env(dotenv), PROCFILE_FIRST_PORT));
    Sections sections = { 0 };
    e = app->procfile ? read_procfile(&reader, text, &app->config) : read_yaml(&reader, text, &app->config, &sections);
    if (e != ERR_OK) {
        String at = shown(outer, path);
        return err_wrap(err, ERR_PARSE, "%.*s", (int)at.len, at.data);
    }
    app->env = reader.env;
    return ERR_OK;
}

// add_needs moves the needs of spec by offset and adds more after them.
static void add_needs(Arena *arena, ProcSpec *spec, size_t offset, const size_t *more, size_t more_count)
{
    size_t *needs = arena_push(arena, arena_size_mul(spec->need_count + more_count, sizeof *needs));
    for (size_t i = 0; i < spec->need_count; i++) {
        needs[i] = spec->needs[i] + offset;
    }
    for (size_t i = 0; i < more_count; i++) {
        needs[spec->need_count + i] = more[i];
    }
    spec->needs = needs;
    spec->need_count += more_count;
}

// adopt makes a proc of an app one of the including config's: named app:proc,
// wrapped, with the include's env last, and stopped when the app is.
static void adopt(Arena *arena, const Include *include, ProcSpec *spec)
{
    spec->name = str_format(arena, "%.*s:%.*s", (int)include->name.len, include->name.data, (int)spec->name.len,
                            spec->name.data);
    if (include->wrap.count > 0) {
        StringList argv = strlist_copy(arena, include->wrap);
        strlist_push_all(arena, &argv, spec->argv);
        spec->argv = argv;
    }
    strlist_push_all(arena, &spec->env, include->env);
    spec->autostart = spec->autostart && include->autostart;
}

// add_app adds an app's procs after config's: its setup job, which the others
// need, then its own. ports holds the port of each proc in config, which the
// app's must not take: a Procfile's procs by the PORT they are given.
[[nodiscard]] static Error add_app(Reader *reader, const Included *app, Config *config, int *ports)
{
    Arena *arena = reader->arena;
    const Include *include = &app->include;
    String where = setting(arena, S("include"), include->name);
    size_t first = config->proc_count;
    const size_t *app_needs = include->needs;
    size_t app_need_count = include->need_count;
    if (include->setup.count > 0) {
        size_t clash;
        if (config_proc_index(&app->config, S("setup"), &clash)) {
            return invalid(reader->err, include_node(include, S("setup")), setting(arena, where, S("setup")),
                           "the app has a proc called setup already");
        }
        ProcSpec *setup = &config->procs[config->proc_count];
        *setup = default_spec(S("setup"), include->directory);
        setup->job = true;
        setup->argv = include->setup;
        setup->command = include->setup_command;
        setup->env = strlist_copy(arena, app->env);
        add_needs(arena, setup, 0, include->needs, include->need_count);
        adopt(arena, include, setup);
        ports[config->proc_count] = 0;
        app_needs = &first;
        app_need_count = 1;
        config->proc_count++;
    }
    size_t offset = config->proc_count;
    for (size_t i = 0; i < app->config.proc_count; i++) {
        ProcSpec *spec = &config->procs[config->proc_count];
        *spec = app->config.procs[i];
        add_needs(arena, spec, offset, app_needs, app_need_count);
        adopt(arena, include, spec);
        ports[config->proc_count] = app->procfile ? port_from_env(spec->env) : spec->port;
        config->proc_count++;
    }
    for (size_t i = first; i < config->proc_count; i++) {
        for (size_t j = 0; j < first; j++) {
            if (ports[i] > 0 && ports[i] == ports[j]) {
                const ProcSpec *a = &config->procs[j];
                const ProcSpec *b = &config->procs[i];
                return invalid(reader->err, include_node(include, S("port")), where,
                               str_cstr(arena, str_format(arena, "port %d of %.*s is also the port of %.*s", ports[i],
                                                          (int)b->name.len, b->name.data, (int)a->name.len,
                                                          a->name.data)));
            }
        }
    }
    return ERR_OK;
}

// read_includes reads each app of include: and adds its procs after config's.
[[nodiscard]] static Error read_includes(Reader *reader, const Sections *sections, Config *config)
{
    Arena *arena = reader->arena;
    const Node *node = sections->include;
    if (node->kind != NODE_MAPPING || node->entry_count == 0) {
        return invalid(reader->err, node, S("include"), "expected one or more apps, such as make-start: { port: 3001 }");
    }
    Include defaults = { .autostart = true };
    if (sections->include_defaults != nullptr) {
        Error e = read_include(reader, config, sections->include_defaults, S("include_defaults"), true, &defaults);
        if (e != ERR_OK) {
            return e;
        }
    }
    Included *apps = arena_push(arena, arena_size_mul(node->entry_count, sizeof *apps));
    size_t total = config->proc_count;
    for (size_t i = 0; i < node->entry_count; i++) {
        String name = node->entries[i].key;
        const Node *value = node->entries[i].value;
        String where = setting(arena, S("include"), name);
        Include *include = &apps[i].include;
        *include = defaults;
        include->name = name;
        include->directory = relative_to(arena, reader->directory, name);
        include->node = value;
        include->env = strlist_copy(arena, defaults.env);
        size_t clash;
        if (!is_name(name)) {
            return invalid(reader->err, value, where, "a name is lowercase letters, digits, - and _");
        }
        if (config_proc_index(config, name, &clash)) {
            return invalid(reader->err, value, where,
                           str_cstr(arena, str_format(arena, "there is already a proc called %.*s", (int)name.len,
                                                      name.data)));
        }
        Error e = read_include(reader, config, value, where, false, include);
        if (e == ERR_OK) {
            e = read_app(reader, &apps[i]);
        }
        if (e != ERR_OK) {
            return e;
        }
        total += apps[i].config.proc_count + (include->setup.count > 0 ? 1 : 0);
    }
    ProcSpec *procs = arena_push(arena, arena_size_mul(total, sizeof *procs));
    int *ports = arena_push(arena, arena_size_mul(total, sizeof *ports));
    for (size_t i = 0; i < config->proc_count; i++) {
        procs[i] = config->procs[i];
        ports[i] = procs[i].port;
    }
    config->procs = procs;
    for (size_t i = 0; i < node->entry_count; i++) {
        Error e = add_app(reader, &apps[i], config, ports);
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

Error config_parse(Arena *arena, String text, String path, int inherited_port, Config *config, Err *err)
{
    *config = (Config) { .path = path, .directory = path_dir(path), .scrollback = DEFAULT_SCROLLBACK };
    strlist_push(arena, &config->files, path);
    Reader reader = {
        .arena = arena,
        .root = config->directory,
        .directory = config->directory,
        .files = &config->files,
        .err = err,
    };
    if (is_procfile(path)) {
        Error e = read_dotenv(&reader, &reader.env);
        reader.first_port = either(port_from_env(reader.env), either(inherited_port, PROCFILE_FIRST_PORT));
        return e == ERR_OK ? read_procfile(&reader, text, config) : e;
    }
    Sections sections = { 0 };
    Error e = read_yaml(&reader, text, config, &sections);
    if (e != ERR_OK || sections.include == nullptr) {
        return e;
    }
    return read_includes(&reader, &sections, config);
}

Error config_load(Arena *arena, String path, Config *config, Err *err)
{
    String absolute = path_absolute(arena, path);
    String text;
    Error read = file_read_all(arena, absolute, &text, err);
    if (read != ERR_OK) {
        return read;
    }
    String inherited;
    int inherited_port = env_get(arena, S("PORT"), &inherited) ? port_number(inherited) : 0;
    Error parsed = config_parse(arena, text, absolute, inherited_port, config, err);
    if (parsed != ERR_OK) {
        return err_wrap(err, parsed, "%.*s", (int)path.len, path.data);
    }
    return ERR_OK;
}

bool config_find(Arena *arena, String directory, String *path)
{
    return file_find_upward(arena, directory, strlist_of(arena, countof(CONFIG_NAMES), CONFIG_NAMES), path);
}

size_t config_select(const Config *config, String name, size_t *indexes)
{
    size_t count = 0;
    for (size_t i = 0; i < config->proc_count; i++) {
        String proc = config->procs[i].name;
        String app;
        String rest;
        if (str_equal(proc, name) || (str_cut(proc, ':', &app, &rest) && str_equal(app, name))) {
            indexes[count++] = i;
        }
    }
    return count;
}

bool config_proc_index(const Config *config, String name, size_t *index)
{
    for (size_t i = 0; i < config->proc_count; i++) {
        if (str_equal(config->procs[i].name, name)) {
            *index = i;
            return true;
        }
    }
    return false;
}

static bool same_list(StringList a, StringList b)
{
    if (a.count != b.count) {
        return false;
    }
    for (size_t i = 0; i < a.count; i++) {
        if (!str_equal(a.items[i], b.items[i])) {
            return false;
        }
    }
    return true;
}

static bool same_check(const Check *a, const Check *b)
{
    return a->kind == b->kind && str_equal(a->text, b->text) && str_equal(a->host, b->host) && a->port == b->port &&
           str_equal(a->path, b->path) && same_list(a->argv, b->argv) && a->every_ns == b->every_ns &&
           a->timeout_ns == b->timeout_ns && a->failures == b->failures;
}

bool config_spec_runs_same(const ProcSpec *a, const ProcSpec *b)
{
    return str_equal(a->name, b->name) && same_list(a->argv, b->argv) && str_equal(a->cwd, b->cwd) &&
           same_list(a->env, b->env) && a->port == b->port && a->job == b->job && same_check(&a->ready, &b->ready) &&
           same_check(&a->health, &b->health) && a->restart == b->restart && a->stop == b->stop &&
           a->stop_timeout_ns == b->stop_timeout_ns && str_equal(a->log_path, b->log_path);
}
