#pragma once

#include "color.h"
#include "mc/core/error.h"
#include "mc/platform/pty.h"

typedef enum CheckKind {
    CHECK_NONE,
    CHECK_LOG,  // a line of output holds text
    CHECK_PORT, // a TCP connection to host:port succeeds
    CHECK_HTTP, // a GET of path on host:port answers 2xx or 3xx
    CHECK_CMD,  // argv exits with 0
} CheckKind;

// Check is a ready: or health: setting.
typedef struct Check {
    CheckKind kind;
    String text; // CHECK_LOG
    String host; // CHECK_PORT, CHECK_HTTP
    int port;
    String path;     // CHECK_HTTP, starting with /
    StringList argv; // CHECK_CMD
    int64_t every_ns;
    int64_t timeout_ns; // ready: only; 0 is no limit
    unsigned failures;  // health: only; failures in a row before it counts
} Check;

typedef enum RestartPolicy {
    RESTART_NO,
    RESTART_ON_FAILURE,
    RESTART_ALWAYS,
} RestartPolicy;

// ProcSpec is one entry of procs: in the config.
typedef struct ProcSpec {
    String name;
    StringList argv;
    String command; // as written, for showing
    String cwd;     // absolute
    StringList env; // NAME=value, or a bare NAME to remove it; a later entry wins
    int port;       // 0 when unknown
    bool autostart;
    bool job;
    size_t *needs; // indexes into Config.procs
    size_t need_count;
    Check ready; // CHECK_NONE when it is ready once it runs
    Check health;
    RestartPolicy restart;
    PtySignal stop;
    int64_t stop_timeout_ns;
    String log_path; // absolute; empty when the output is not written to a file
    bool has_color;
    Rgb color;
} ProcSpec;

typedef struct Config {
    String path; // absolute
    String directory;
    String theme; // empty for the default
    size_t scrollback;
    ProcSpec *procs;
    size_t proc_count;
} Config;

enum { DEFAULT_SCROLLBACK = 10000 };

// config_parse reads the text of the config at path, an absolute path that
// relative paths in it start from. A Procfile, or Procfile.dev or any other
// Procfile.*, is read as foreman reads one, with the .env beside it; any other
// name is a bprocs.yml. inherited_port is the PORT bprocs runs with, 0 when
// there is none: a Procfile's ports start from it when its .env sets no PORT.
// A config that does not say what to run, or says it in a way bprocs does not
// know, is ERR_PARSE with the line in err. The env_file: files it names are
// read.
[[nodiscard]] Error config_parse(Arena *arena, String text, String path, int inherited_port, Config *config,
                                 Err *err);
// config_load reads the config at path with the PORT of this process's environment.
[[nodiscard]] Error config_load(Arena *arena, String path, Config *config, Err *err);
// config_find looks for bprocs.yml, bprocs.yaml, Procfile.dev or Procfile,
// in that order, in directory and then in each directory above it.
bool config_find(Arena *arena, String directory, String *path);
// config_spec_runs_same is whether two specs run the same program the same
// way, so a running proc can stay when its config is read again. What only
// changes how it is shown or when it starts, such as color, autostart and
// needs, does not count.
bool config_spec_runs_same(const ProcSpec *a, const ProcSpec *b);
// config_proc_index finds the proc called name.
bool config_proc_index(const Config *config, String name, size_t *index);
