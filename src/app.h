#pragma once

#include <stdatomic.h>

#include "client.h"
#include "group.h"
#include "mc/term/input.h"
#include "layout.h"
#include "mc/term/logbuf.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"
#include "proc.h"
#include "mc/term/screen.h"
#include "stats.h"
#include "userconfig.h"

typedef enum Focus {
    FOCUS_PROCS,
    FOCUS_LOG,
    FOCUS_INPUT, // keys go to the selected process
} Focus;

// ProcView is what the UI keeps about one process, as the daemon reports it.
typedef struct ProcView {
    ProcSpec spec;
    ProcStatus status;
    LogBuffer *log;
    ProcStats stats;
    // The log shows its newest lines while following, otherwise line anchor at the bottom.
    bool following;
    uint64_t anchor;
    ProcState seen_state;
    int64_t state_changed_ns;
    uint64_t merged; // the first of its lines not yet in the merged log
} ProcView;

enum { TOAST_LIMIT = 160, INPUT_LIMIT = 8192, SEARCH_LIMIT = 120 };

// Daemon is a daemon the project switcher offers.
typedef struct Daemon {
    String config_path;
    size_t running;
    size_t total;
} Daemon;

typedef struct App {
    Arena *arena;
    Arena *frame; // reset every frame
    Arena *views_arena; // the views, made again when the daemon is
    Client *client; // nullptr while the daemon has gone
    String config_path;
    StringList config_files; // what the config was read from, in views_arena
    ProcView *views;
    size_t view_count;
    // all shows every proc's lines, each after its name, since the TUI opened.
    ProcView all;
    bool show_all;
    // The procs of each app of include: show under a header. A group's view
    // shows its procs together: their stats added up, their lines since the
    // TUI opened, and the status of the one most in need of attention.
    Group *groups;
    ProcView *group_views;
    size_t group_count;
    Row *rows; // what the list shows
    size_t row_count;
    size_t selected;    // a row
    size_t list_offset; // the first row the list shows
    Focus focus;
    size_t theme_index;

    Terminal *terminal;
    Screen *screen;
    ColorDepth color_depth;
    Layout layout;
    unsigned shown_boxes; // the LayoutBox bits the user has not hidden
    String user_config_path; // where they are remembered
    PtySize pty_size; // what the procs were last told their terminal is
    SystemStats system;

    int64_t now_ns;
    int64_t started_ns;
    int64_t next_sample_ns;
    int64_t last_frame_ns;
    bool dirty;
    bool show_help;
    bool going_down; // asked the daemon to stop everything; quits when it has
    int64_t down_started_ns;
    size_t down_total;
    bool daemon_gone;
    bool done;

    // A search marks the lines of the log that hold it; filtering hides the rest.
    char search[SEARCH_LIMIT];
    size_t search_len;
    bool typing_search;
    bool filtering;

    // The project switcher lists the daemons running on this machine.
    bool switching;
    Arena *daemons_arena;
    Daemon *daemons;
    size_t daemon_count;
    size_t daemon_selected;

    int64_t config_mtime_ns; // when a config file was last changed, to notice an edit
    bool reloading; // the daemon reloads its config; reconnect when it lets go

    char toast[TOAST_LIMIT];
    size_t toast_len;
    int64_t toast_ns;

    // The signal thread and the client's reader wake the UI thread through
    // the terminal, which only lives while not closing.
    Mutex wake_lock;
    bool closing;
    atomic_bool stop_signal;

    unsigned char input[INPUT_LIMIT];
    size_t input_len;
    bool pasting; // the rest of a paste longer than the input buffer is still arriving
} App;

// app_create takes over client, which app_destroy closes.
App *app_create(Client *client, size_t theme_index, ColorDepth color_depth, const UserConfig *user_config);
void app_destroy(App *app);
// app_run takes over the terminal until the user quits or the daemon has
// brought everything down at the user's request.
[[nodiscard]] Error app_run(App *app, Err *err);

// app_accent is the colour of process index in the current theme.
Rgb app_accent(const App *app, size_t index);
// app_waiting_for names the needs of process index that are not up yet, in the frame arena.
String app_waiting_for(App *app, size_t index);
// app_selected_view is the view of the selected row: a proc's, or a group's.
ProcView *app_selected_view(App *app);
// app_log_view is the view the log pane shows: the selected row's, or every proc's.
ProcView *app_log_view(App *app);
// app_group_worst is the proc of group most in need of attention.
size_t app_group_worst(const App *app, size_t group);
// app_search is the text searched for, empty when none.
String app_search(const App *app);
// app_matches is whether a log line holds the search, ignoring case.
bool app_matches(App *app, String line);
