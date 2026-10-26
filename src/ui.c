#include "ui.h"

#include <math.h>

#include "draw.h"
#include "format.h"
#include "logview.h"
#include "theme.h"

static const float INTRO_MS = 700.0f;
static const float TOAST_MS = 2600.0f;
static const float TOAST_FADE_IN_MS = 150.0f;
static const float TOAST_FADE_OUT_MS = 400.0f;
static const float FLASH_MS = 1500.0f;

enum {
    PULSE_MS = 2400,
    BLINK_MS = 1000,
    CORE_COLUMN = 16, // "C12 ■■■■■■ 100%" and a gap
    NAME_MIN = 12,
};

enum {
    GLYPH_DOT = 0x25CF,       // ●
    GLYPH_CIRCLE = 0x25CB,    // ○
    GLYPH_SQUARE = 0x25A0,    // ■
    GLYPH_CHECK = 0x2714,     // ✔
    GLYPH_CROSS = 0x2716,     // ✖
    GLYPH_RESTART = 0x21BB,   // ↻
    GLYPH_WAITING = 0x25CC,   // ◌
    GLYPH_BLOCKED = 0x2298,   // ⊘
    GLYPH_SELECTED = 0x258E,  // ▎
    GLYPH_DIVIDER = 0x2502,   // │
    GLYPH_THUMB = 0x2503,     // ┃
    GLYPH_EXPANDED = 0x25BE,  // ▾
    GLYPH_COLLAPSED = 0x25B8, // ▸
};

enum { GROUP_INDENT = 2 }; // where a proc under a header starts, past the header's arrow

static const float TAU = 6.2831853f;

static const Theme *current_theme(const App *app)
{
    return theme_at(app->theme_index);
}

static float elapsed_ms(const App *app, int64_t since_ns)
{
    return (float)(app->now_ns - since_ns) / (float)NS_PER_MILLISECOND;
}

static float ease_out(float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float rest = 1.0f - t;
    return 1.0f - rest * rest * rest;
}

// wave runs between 0 and 1 once a period, shifted by phase periods.
static float wave(const App *app, int period_ms, float phase)
{
    float t = (float)(app->now_ns / NS_PER_MILLISECOND % period_ms) / (float)period_ms;
    return 0.5f + 0.5f * sinf((t - phase) * TAU);
}

static Box box_style(const App *app, String title, Rgb title_color, String info, Rgb info_color, bool focused)
{
    const Theme *theme = current_theme(app);
    return (Box) {
        .title = title,
        .title_color = title_color,
        .info = info,
        .info_color = info_color,
        .border = focused ? rgb_mix(theme->focus, theme->border, 0.55f) : theme->border,
        .glow = focused ? theme->focus : rgb_mix(theme->border, theme->focus, 0.4f),
    };
}

Rgb app_accent(const App *app, size_t index)
{
    const ProcSpec *spec = &app->views[index].spec;
    return spec->has_color ? spec->color : current_theme(app)->accents[index % THEME_ACCENTS];
}

// ---- system ----

static void draw_cores(App *app, Rect area, int columns)
{
    const Theme *theme = current_theme(app);
    const SystemStats *stats = &app->system;
    size_t rows = (size_t)area.height;
    size_t shown = min_size(stats->core_count, (size_t)columns * rows);
    for (size_t i = 0; i < shown; i++) {
        int x = area.x + (int)(i / rows) * CORE_COLUMN;
        int y = area.y + (int)(i % rows);
        float share = stats->cores[i];
        screen_text(app->screen, x, y, 3, str_format(app->frame, "C%zu", i), theme->muted, 0);
        draw_meter(app->screen, x + 4, y, 6, share, theme->cpu, theme->faint);
        String percent = str_format(app->frame, "%3.0f%%", (double)share * 100.0);
        screen_text(app->screen, x + 11, y, 4, percent, gradient_at(theme->cpu, share), 0);
    }
}

static void draw_cpu(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    const SystemStats *stats = &app->system;
    String info = str_format(app->frame, "load %.2f %.2f %.2f", stats->load[0], stats->load[1], stats->load[2]);
    draw_box(app->screen, rect, box_style(app, S("cpu"), theme->text, info, theme->muted, false));
    Rect inner = rect_inner(rect);
    if (inner.width < 12 || inner.height < 1) {
        return;
    }
    int columns = 0;
    if (stats->core_count > 0 && inner.width >= 48) {
        size_t needed = (stats->core_count + (size_t)inner.height - 1) / (size_t)inner.height;
        int room = inner.width / 2 / CORE_COLUMN;
        columns = (int)needed < room ? (int)needed : room;
    }
    int panel = columns * CORE_COLUMN;
    Rect graph = { inner.x, inner.y, inner.width - panel, inner.height };
    draw_graph(app->screen, graph, &stats->cpu, 1.0f, theme->cpu, false);
    float now = history_newest(&stats->cpu);
    int x = graph.x + 1;
    x += screen_text(app->screen, x, graph.y, 6, S("total "), theme->muted, 0);
    String percent = str_format(app->frame, "%.0f%%", (double)now * 100.0);
    x += screen_text(app->screen, x, graph.y, 6, percent, gradient_at(theme->cpu, now), ATTR_BOLD);
    String span = format_span(app->frame, stats->uptime_seconds * NS_PER_SECOND);
    String uptime = str_format(app->frame, "up %.*s", (int)span.len, span.data);
    if (inner.height > 1) {
        screen_text(app->screen, graph.x + 1, graph.y + graph.height - 1, graph.width - 2, uptime, theme->muted, 0);
    }
    if (columns > 0) {
        draw_cores(app, (Rect) { graph.x + graph.width, inner.y, panel, inner.height }, columns);
    }
}

static void draw_usage_line(App *app, int x, int y, int width, String label, uint64_t used, uint64_t total)
{
    const Theme *theme = current_theme(app);
    screen_text(app->screen, x, y, width, label, theme->muted, 0);
    String used_text = format_size(app->frame, used);
    String total_text = format_size(app->frame, total);
    String amount = str_format(app->frame, "%.*s / %.*s", (int)used_text.len, used_text.data, (int)total_text.len,
                               total_text.data);
    draw_text_right(app->screen, x + width, y, amount, theme->text, 0);
}

static void draw_memory(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    const SystemStats *stats = &app->system;
    float share = history_newest(&stats->memory);
    String info = str_format(app->frame, "%.0f%%", (double)share * 100.0);
    draw_box(app->screen, rect, box_style(app, S("mem"), theme->text, info, gradient_at(theme->memory, share), false));
    Rect inner = rect_inner(rect);
    if (inner.width < 12 || inner.height < 2) {
        return;
    }
    int y = inner.y;
    draw_usage_line(app, inner.x, y++, inner.width, S("used"), stats->memory_used, stats->memory_total);
    draw_meter(app->screen, inner.x, y++, inner.width, share, theme->memory, theme->faint);
    if (stats->swap_total > 0 && inner.height >= 6) {
        float swap = (float)((double)stats->swap_used / (double)stats->swap_total);
        draw_usage_line(app, inner.x, y++, inner.width, S("swap"), stats->swap_used, stats->swap_total);
        draw_meter(app->screen, inner.x, y++, inner.width, swap, theme->memory, theme->faint);
    }
    Rect graph = { inner.x, y, inner.width, inner.y + inner.height - y };
    if (graph.height > 0) {
        draw_graph(app->screen, graph, &stats->memory, 1.0f, theme->memory, false);
    }
}

static void draw_network(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    const SystemStats *stats = &app->system;
    draw_box(app->screen, rect, box_style(app, S("net"), theme->text, S(""), theme->muted, false));
    Rect inner = rect_inner(rect);
    if (inner.width < 8 || inner.height < 2) {
        return;
    }
    size_t span = (size_t)inner.width * 2;
    float peak = fmaxf(history_peak(&stats->download, span), history_peak(&stats->upload, span));
    float scale = fmaxf(peak * 1.1f, 1024.0f);
    int top = (inner.height + 1) / 2;
    Rect down = { inner.x, inner.y, inner.width, top };
    Rect up = { inner.x, inner.y + top, inner.width, inner.height - top };
    draw_graph(app->screen, down, &stats->download, scale, theme->download, false);
    draw_graph(app->screen, up, &stats->upload, scale, theme->upload, true);
    String received = format_rate(app->frame, history_newest(&stats->download));
    String sent = format_rate(app->frame, history_newest(&stats->upload));
    int x = inner.x + 1;
    screen_text(app->screen, x, inner.y, inner.width - 2,
                str_format(app->frame, "▼ %.*s", (int)received.len, received.data), theme->download.end, ATTR_BOLD);
    screen_text(app->screen, x, inner.y + inner.height - 1, inner.width - 2,
                str_format(app->frame, "▲ %.*s", (int)sent.len, sent.data), theme->upload.middle, ATTR_BOLD);
    String peak_text = format_rate(app->frame, peak);
    draw_text_right(app->screen, inner.x + inner.width - 1, inner.y, str_format(app->frame, "max %.*s",
                                                                                 (int)peak_text.len, peak_text.data),
                    theme->muted, 0);
}

// ---- process list ----

typedef struct {
    int name_x;
    int name_width;
    int port_x;
    int cpu_x;
    int spark_x;
    int memory_x;
    int status_x;
    bool port;
    bool cpu;
    bool spark;
    bool memory;
    bool status;
} Columns;

enum { PORT_WIDTH = 6, CPU_WIDTH = 5, SPARK_WIDTH = 8, MEMORY_WIDTH = 5, STATUS_WIDTH = 8 };

// list_columns chooses the columns that fit, most useful first, and places
// them from the right edge.
static Columns list_columns(Rect inner)
{
    Columns columns = { .name_x = inner.x + 3 };
    int room = inner.width - 4 - NAME_MIN;
    struct {
        bool *shown;
        int width;
    } wanted[] = {
        { &columns.cpu, CPU_WIDTH },   { &columns.memory, MEMORY_WIDTH }, { &columns.status, STATUS_WIDTH },
        { &columns.spark, SPARK_WIDTH }, { &columns.port, PORT_WIDTH },
    };
    for (size_t i = 0; i < countof(wanted); i++) {
        if (room >= wanted[i].width + 1) {
            *wanted[i].shown = true;
            room -= wanted[i].width + 1;
        }
    }
    int x = inner.x + inner.width - 1;
    if (columns.status) {
        x -= STATUS_WIDTH;
        columns.status_x = x--;
    }
    if (columns.memory) {
        x -= MEMORY_WIDTH;
        columns.memory_x = x--;
    }
    if (columns.spark) {
        x -= SPARK_WIDTH;
        columns.spark_x = x--;
    }
    if (columns.cpu) {
        x -= CPU_WIDTH;
        columns.cpu_x = x--;
    }
    if (columns.port) {
        x -= PORT_WIDTH;
        columns.port_x = x--;
    }
    columns.name_width = x - columns.name_x;
    return columns;
}

static void status_mark(App *app, size_t index, ProcStatus status, uint32_t *glyph, Rgb *color)
{
    const Theme *theme = current_theme(app);
    *glyph = GLYPH_CIRCLE;
    *color = theme->muted;
    if (status.restart_at_ns != 0) {
        *glyph = GLYPH_RESTART;
        *color = theme->starting;
        return;
    }
    switch (status.state) {
    case PROC_RUNNING: {
        float pulse = wave(app, PULSE_MS, (float)index * 0.12f);
        *glyph = GLYPH_DOT;
        *color = rgb_mix(theme->running, theme->background, 0.45f * (1.0f - pulse));
        return;
    }
    case PROC_STARTING:
        *glyph = spinner_glyph(app->now_ns);
        *color = theme->starting;
        return;
    case PROC_STOPPING:
        *glyph = spinner_glyph(app->now_ns);
        *color = theme->failed;
        return;
    case PROC_EXITED:
        *glyph = GLYPH_CHECK;
        *color = rgb_mix(theme->running, theme->muted, 0.4f);
        return;
    case PROC_CRASHED:
    case PROC_FAILED:
        *glyph = GLYPH_CROSS;
        *color = theme->failed;
        return;
    case PROC_STOPPED:
        *glyph = GLYPH_SQUARE;
        *color = theme->muted;
        return;
    case PROC_WAITING:
        *glyph = GLYPH_WAITING;
        *color = rgb_mix(theme->starting, theme->muted, 0.5f);
        return;
    case PROC_BLOCKED:
        *glyph = GLYPH_BLOCKED;
        *color = theme->failed;
        return;
    case PROC_IDLE:
        return;
    }
}

// status_word says how a process is doing in a few characters.
static String status_word(App *app, ProcStatus status, Rgb *color)
{
    const Theme *theme = current_theme(app);
    *color = theme->muted;
    if (status.restart_at_ns != 0) {
        *color = theme->starting;
        String wait = format_span(app->frame, status.restart_at_ns - app->now_ns + NS_PER_SECOND);
        return str_format(app->frame, "↻ %.*s", (int)wait.len, wait.data);
    }
    switch (status.state) {
    case PROC_RUNNING:
        *color = status.health == HEALTH_UNHEALTHY ? theme->failed : theme->text;
        return format_span(app->frame, app->now_ns - status.started_ns);
    case PROC_WAITING:
        return S("waiting");
    case PROC_BLOCKED:
        *color = theme->failed;
        return S("blocked");
    case PROC_STARTING:
        *color = theme->starting;
        return S("starting");
    case PROC_STOPPING:
        *color = theme->starting;
        return S("stopping");
    case PROC_STOPPED:
        return S("stopped");
    case PROC_EXITED:
        return S("done");
    case PROC_CRASHED:
        *color = theme->failed;
        return status.signal != 0 ? format_signal(app->frame, status.signal)
                                  : str_format(app->frame, "exit %d", status.exit_code);
    case PROC_FAILED:
        *color = theme->failed;
        return S("failed");
    case PROC_IDLE:
        return S("idle");
    }
    return S("");
}

static void paint_row(App *app, Rect inner, int y, size_t row, const ProcView *view)
{
    const Theme *theme = current_theme(app);
    bool selected = row == app->selected;
    ProcStatus status = view->status;
    float flash_ms = elapsed_ms(app, view->state_changed_ns);
    bool flashing = (status.state == PROC_CRASHED || status.state == PROC_FAILED) && flash_ms < FLASH_MS;
    for (int x = inner.x; x < inner.x + inner.width; x++) {
        Cell *cell = screen_cell(app->screen, x, y);
        if (cell == nullptr) {
            continue;
        }
        float along = (float)(x - inner.x) / (float)inner.width;
        Rgb bg = selected ? rgb_mix(theme->selection, theme->background, along * along * 0.85f) : theme->background;
        if (flashing) {
            float strength = (1.0f - flash_ms / FLASH_MS) * (1.0f - along * 0.6f);
            bg = rgb_mix(bg, theme->failed, strength * 0.45f);
        }
        cell->bg = bg;
    }
    if (selected) {
        Rgb marker = app->focus == FOCUS_PROCS ? theme->focus : theme->muted;
        screen_glyph(app->screen, inner.x, y, GLYPH_SELECTED, marker, 0);
    }
}

// draw_usage fills the cpu, sparkline and memory columns of a row.
static void draw_usage(App *app, Columns columns, int y, const ProcStats *stats, bool active)
{
    const Theme *theme = current_theme(app);
    if (columns.cpu) {
        String cpu = active ? str_format(app->frame, "%.0f%%", (double)stats->cpu_now * 100.0) : S("·");
        draw_text_right(app->screen, columns.cpu_x + CPU_WIDTH, y, cpu,
                        active ? gradient_at(theme->cpu, stats->cpu_now) : theme->muted, 0);
    }
    if (columns.spark) {
        float scale = fmaxf(1.0f, history_peak(&stats->cpu, SPARK_WIDTH));
        draw_sparkline(app->screen, columns.spark_x, y, SPARK_WIDTH, &stats->cpu, scale, theme->cpu, theme->faint);
    }
    if (columns.memory) {
        String memory = active && stats->memory_now > 0 ? format_size(app->frame, stats->memory_now) : S("·");
        draw_text_right(app->screen, columns.memory_x + MEMORY_WIDTH, y, memory, active ? theme->text : theme->muted,
                        0);
    }
}

// draw_proc_row draws a proc; under a header, indented and by its short name.
static void draw_proc_row(App *app, Rect inner, Columns columns, int y, size_t row)
{
    const Theme *theme = current_theme(app);
    size_t index = app->rows[row].proc;
    const ProcView *view = &app->views[index];
    const ProcSpec *spec = &view->spec;
    ProcStatus status = view->status;
    bool selected = row == app->selected;
    bool active = proc_state_active(status.state);
    int indent = app->rows[row].grouped ? GROUP_INDENT : 0;
    paint_row(app, inner, y, row, view);

    uint32_t glyph;
    Rgb glyph_color;
    status_mark(app, index, status, &glyph, &glyph_color);
    screen_glyph(app->screen, inner.x + 1 + indent, y, glyph, glyph_color, 0);

    Rgb name_color = app_accent(app, index);
    if (!active && status.state != PROC_CRASHED) {
        name_color = rgb_mix(name_color, theme->muted, 0.55f);
    }
    String name = indent > 0 ? group_short_name(spec->name) : spec->name;
    int name_width = columns.name_width - indent;
    screen_text(app->screen, columns.name_x + indent, y, name_width, text_fit(name, name_width), name_color,
                selected ? ATTR_BOLD : 0);
    if (columns.port && spec->port > 0) {
        screen_text(app->screen, columns.port_x, y, PORT_WIDTH, str_format(app->frame, ":%d", spec->port),
                    theme->muted, 0);
    }
    draw_usage(app, columns, y, &view->stats, active);
    if (columns.status) {
        Rgb color;
        String word = text_fit(status_word(app, status, &color), STATUS_WIDTH);
        draw_text_right(app->screen, columns.status_x + STATUS_WIDTH, y, word, color, 0);
    }
}

// is_up is whether a proc is as it should be once started: running, or a job that succeeded.
static bool is_up(ProcState state)
{
    return state == PROC_RUNNING || state == PROC_EXITED;
}

// count_group counts the procs of a group that are up and that run.
static void count_group(const App *app, size_t group, size_t *up, size_t *active)
{
    const Group *g = &app->groups[group];
    *up = 0;
    *active = 0;
    for (size_t i = g->first; i < g->first + g->count; i++) {
        *up += is_up(app->views[i].status.state);
        *active += proc_state_active(app->views[i].status.state);
    }
}

// group_word says how a group is doing: how many of its procs are up while
// they are fine, else how its proc most in need of attention is.
static String group_word(App *app, size_t group, Rgb *color)
{
    const Theme *theme = current_theme(app);
    ProcStatus worst = app->group_views[group].status;
    size_t up;
    size_t active;
    count_group(app, group, &up, &active);
    if (active == 0 || group_rank(worst) > RANK_RUNNING) {
        return status_word(app, worst, color);
    }
    *color = up == app->groups[group].count ? theme->text : theme->muted;
    return str_format(app->frame, "%zu/%zu", up, app->groups[group].count);
}

// draw_header draws a group: the status of its proc most in need of
// attention, an arrow that says whether its procs show, and their usage added up.
static void draw_header(App *app, Rect inner, Columns columns, int y, size_t row)
{
    const Theme *theme = current_theme(app);
    size_t group = app->rows[row].group;
    const Group *g = &app->groups[group];
    const ProcView *view = &app->group_views[group];
    size_t up;
    size_t active;
    count_group(app, group, &up, &active);
    paint_row(app, inner, y, row, view);

    uint32_t glyph;
    Rgb glyph_color;
    status_mark(app, app_group_worst(app, group), view->status, &glyph, &glyph_color);
    screen_glyph(app->screen, inner.x + 1, y, glyph, glyph_color, 0);
    screen_glyph(app->screen, columns.name_x, y, g->expanded ? GLYPH_EXPANDED : GLYPH_COLLAPSED, theme->muted, 0);

    Rgb name_color = app_accent(app, g->first);
    if (active == 0 && view->status.state != PROC_CRASHED) {
        name_color = rgb_mix(name_color, theme->muted, 0.55f);
    }
    int name_width = columns.name_width - GROUP_INDENT;
    screen_text(app->screen, columns.name_x + GROUP_INDENT, y, name_width, text_fit(g->name, name_width), name_color,
                ATTR_BOLD);
    draw_usage(app, columns, y, &view->stats, active > 0);
    if (columns.status) {
        Rgb color;
        String word = text_fit(group_word(app, group, &color), STATUS_WIDTH);
        draw_text_right(app->screen, columns.status_x + STATUS_WIDTH, y, word, color, 0);
    }
}

static void draw_list_header(App *app, Rect inner, Columns columns)
{
    const Theme *theme = current_theme(app);
    Rgb color = rgb_mix(theme->muted, theme->text, 0.25f);
    int y = inner.y;
    screen_text(app->screen, columns.name_x, y, columns.name_width, S("name"), color, ATTR_BOLD);
    if (columns.port) {
        screen_text(app->screen, columns.port_x, y, PORT_WIDTH, S("port"), color, ATTR_BOLD);
    }
    if (columns.cpu) {
        draw_text_right(app->screen, columns.cpu_x + CPU_WIDTH, y, S("cpu"), color, ATTR_BOLD);
    }
    if (columns.memory) {
        draw_text_right(app->screen, columns.memory_x + MEMORY_WIDTH, y, S("mem"), color, ATTR_BOLD);
    }
    if (columns.status) {
        draw_text_right(app->screen, columns.status_x + STATUS_WIDTH, y, S("up"), color, ATTR_BOLD);
    }
}

// draw_scrollbar puts a thumb on the right border of rect for a view of
// visible items out of total, starting at first.
static void draw_scrollbar(App *app, Rect rect, size_t first, size_t visible, size_t total, Rgb color)
{
    int track = rect.height - 2;
    if (total <= visible || track < 2) {
        return;
    }
    int thumb = (int)((size_t)track * visible / total);
    thumb = thumb < 1 ? 1 : thumb;
    int start = (int)((size_t)(track - thumb) * first / (total - visible));
    for (int i = 0; i < thumb; i++) {
        screen_glyph(app->screen, rect.x + rect.width - 1, rect.y + 1 + start + i, GLYPH_THUMB, color, 0);
    }
}

int ui_list_rows(const App *app)
{
    int rows = app->layout.procs.height - 3;
    return rows > 0 ? rows : 0;
}

static void draw_procs(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    size_t running = 0;
    for (size_t i = 0; i < app->view_count; i++) {
        running += proc_state_active(app->views[i].status.state);
    }
    String info = str_format(app->frame, "%zu/%zu up", running, app->view_count);
    bool focused = app->focus == FOCUS_PROCS;
    draw_box(app->screen, rect, box_style(app, S("procs"), theme->text, info, theme->running, focused));
    Rect inner = rect_inner(rect);
    if (inner.width < NAME_MIN || inner.height < 2) {
        return;
    }
    Columns columns = list_columns(inner);
    draw_list_header(app, inner, columns);
    size_t rows = (size_t)ui_list_rows(app);
    for (size_t i = 0; i < rows && app->list_offset + i < app->row_count; i++) {
        size_t row = app->list_offset + i;
        int y = inner.y + 1 + (int)i;
        app->rows[row].header ? draw_header(app, inner, columns, y, row) : draw_proc_row(app, inner, columns, y, row);
    }
    draw_scrollbar(app, rect, app->list_offset, rows, app->row_count, theme->focus);
}

// ---- details of the selected process ----

static int draw_fact(App *app, int x, int y, int end, String label, String value)
{
    const Theme *theme = current_theme(app);
    int width = text_columns(label) + 1 + text_columns(value);
    if (x + width > end) {
        return x;
    }
    x += screen_text(app->screen, x, y, end - x, label, theme->muted, 0) + 1;
    x += screen_text(app->screen, x, y, end - x, value, theme->text, 0);
    return x + 2;
}

static String state_label(App *app, size_t index, Rgb *color)
{
    const Theme *theme = current_theme(app);
    ProcStatus status = app->views[index].status;
    switch (status.state) {
    case PROC_RUNNING:
        *color = status.health == HEALTH_UNHEALTHY ? theme->failed : theme->running;
        return status.health == HEALTH_UNHEALTHY ? S("running · unhealthy")
               : status.health == HEALTH_HEALTHY ? S("running · healthy")
                                                 : S("running");
    case PROC_WAITING: {
        *color = theme->starting;
        String names = app_waiting_for(app, index);
        return str_format(app->frame, "waiting for %.*s", (int)names.len, names.data);
    }
    case PROC_BLOCKED:
        *color = theme->failed;
        return S("blocked");
    case PROC_STARTING:
        *color = theme->starting;
        return S("starting");
    case PROC_STOPPING:
        *color = theme->starting;
        return S("stopping");
    case PROC_CRASHED: {
        *color = theme->failed;
        if (status.signal == 0) {
            return str_format(app->frame, "crashed · exit %d", status.exit_code);
        }
        String name = format_signal(app->frame, status.signal);
        return str_format(app->frame, "crashed · %.*s", (int)name.len, name.data);
    }
    case PROC_FAILED:
        *color = theme->failed;
        return S("failed to start");
    case PROC_EXITED:
        *color = theme->muted;
        return S("exited");
    case PROC_STOPPED:
        *color = theme->muted;
        return S("stopped");
    case PROC_IDLE:
        *color = theme->muted;
        return S("not started");
    }
    *color = theme->muted;
    return S("");
}

static void draw_graph_label(App *app, int x, int y, String label, String value, Rgb value_color)
{
    const Theme *theme = current_theme(app);
    x += screen_text(app->screen, x, y, 8, label, theme->muted, 0) + 1;
    screen_text(app->screen, x, y, 12, value, value_color, ATTR_BOLD);
}

// draw_detail_graphs draws the cpu and memory history of stats in the rows
// of inner below its first, above its last.
static void draw_detail_graphs(App *app, Rect inner, const ProcStats *stats)
{
    const Theme *theme = current_theme(app);
    int graph_height = inner.height - 2;
    if (graph_height < 1) {
        return;
    }
    int left_width = (inner.width - 1) / 2;
    Rect cpu = { inner.x, inner.y + 1, left_width, graph_height };
    Rect memory = { inner.x + left_width + 1, inner.y + 1, inner.width - left_width - 1, graph_height };
    size_t span = (size_t)cpu.width * 2;
    // Scaled to the recent peak, so that a mostly idle process still shows its activity.
    float cpu_scale = fmaxf(0.1f, history_peak(&stats->cpu, span) * 1.15f);
    float memory_scale = fmaxf(1024.0f * 1024.0f, history_peak(&stats->memory, span) * 1.25f);
    draw_graph(app->screen, cpu, &stats->cpu, cpu_scale, theme->cpu, false);
    draw_graph(app->screen, memory, &stats->memory, memory_scale, theme->memory, false);
    for (int y = cpu.y; y < cpu.y + cpu.height; y++) {
        screen_glyph(app->screen, cpu.x + cpu.width, y, GLYPH_DIVIDER, theme->faint, 0);
    }
    float cpu_now = stats->cpu_now;
    draw_graph_label(app, cpu.x + 1, cpu.y, S("cpu"), str_format(app->frame, "%.1f%%", (double)cpu_now * 100.0),
                     gradient_at(theme->cpu, cpu_now));
    draw_graph_label(app, memory.x + 1, memory.y, S("mem"), format_size(app->frame, stats->memory_now),
                     theme->memory.middle);
    float cpu_peak = history_peak(&stats->cpu, span);
    if (cpu_peak > 0.0f) {
        String peak = str_format(app->frame, "peak %.0f%%", (double)cpu_peak * 100.0);
        draw_text_right(app->screen, cpu.x + cpu.width - 1, cpu.y, peak, theme->muted, 0);
    }
    String memory_peak = format_size(app->frame, (uint64_t)history_peak(&stats->memory, span));
    draw_text_right(app->screen, memory.x + memory.width - 1, memory.y,
                    str_format(app->frame, "peak %.*s", (int)memory_peak.len, memory_peak.data), theme->muted, 0);
}

// draw_bottom_line puts a problem, or else text, on the last row of inner.
static void draw_bottom_line(App *app, Rect inner, const char *problem, String text)
{
    const Theme *theme = current_theme(app);
    int bottom = inner.y + inner.height - 1;
    if (problem[0] != '\0') {
        String line = str_format(app->frame, "✖ %s", problem);
        screen_text(app->screen, inner.x + 1, bottom, inner.width - 2, text_fit(line, inner.width - 2), theme->failed,
                    0);
        return;
    }
    screen_text(app->screen, inner.x + 1, bottom, inner.width - 2, text_fit(text, inner.width - 2),
                rgb_mix(theme->muted, theme->text, 0.2f), ATTR_ITALIC);
}

static void draw_proc_detail(App *app, Rect rect, size_t index)
{
    const ProcView *view = &app->views[index];
    const ProcSpec *spec = &view->spec;
    ProcStatus status = view->status;
    Rgb state_color;
    String state = state_label(app, index, &state_color);
    draw_box(app->screen, rect, box_style(app, spec->name, app_accent(app, index), state, state_color, false));
    Rect inner = rect_inner(rect);
    if (inner.width < 20 || inner.height < 3) {
        return;
    }
    const ProcStats *stats = &view->stats;
    bool active = proc_state_active(status.state);
    int end = inner.x + inner.width;
    int x = inner.x + 1;
    if (active) {
        x = draw_fact(app, x, inner.y, end, S("pid"), str_format(app->frame, "%d", status.pid));
        x = draw_fact(app, x, inner.y, end, S("up"), format_span(app->frame, app->now_ns - status.started_ns));
        x = draw_fact(app, x, inner.y, end, S("procs"), str_format(app->frame, "%zu", stats->processes));
        x = draw_fact(app, x, inner.y, end, S("threads"), str_format(app->frame, "%zu", stats->threads));
    }
    x = draw_fact(app, x, inner.y, end, S("restarts"), str_format(app->frame, "%u", status.restarts));
    if (spec->need_count > 0) {
        StringList names = { 0 };
        for (size_t i = 0; i < spec->need_count; i++) {
            strlist_push(app->frame, &names, app->views[spec->needs[i]].spec.name);
        }
        draw_fact(app, x, inner.y, end, S("needs"), str_join(app->frame, names, S(" ")));
    }
    draw_bottom_line(app, inner, status.problem,
                     str_format(app->frame, "$ %.*s", (int)spec->command.len, spec->command.data));
    draw_detail_graphs(app, inner, stats);
}

// draw_group_detail shows a group: how its proc most in need of attention
// is, how many are up, and their usage added up.
static void draw_group_detail(App *app, Rect rect, size_t group)
{
    const Theme *theme = current_theme(app);
    const Group *g = &app->groups[group];
    const ProcView *view = &app->group_views[group];
    size_t worst = app_group_worst(app, group);
    Rgb state_color;
    String state = state_label(app, worst, &state_color);
    size_t up;
    size_t active;
    count_group(app, group, &up, &active);
    if (group_rank(view->status) > RANK_RUNNING) {
        String name = group_short_name(app->views[worst].spec.name);
        state = str_format(app->frame, "%.*s · %.*s", (int)name.len, name.data, (int)state.len, state.data);
    } else if (active > 0) {
        state = str_format(app->frame, "%zu/%zu up", up, g->count);
        state_color = up == g->count ? theme->running : theme->muted;
    }
    draw_box(app->screen, rect, box_style(app, g->name, app_accent(app, g->first), state, state_color, false));
    Rect inner = rect_inner(rect);
    if (inner.width < 20 || inner.height < 3) {
        return;
    }
    const ProcStats *stats = &view->stats;
    unsigned restarts = 0;
    StringList names = { 0 };
    for (size_t i = g->first; i < g->first + g->count; i++) {
        restarts += app->views[i].status.restarts;
        strlist_push(app->frame, &names, group_short_name(app->views[i].spec.name));
    }
    int end = inner.x + inner.width;
    int x = inner.x + 1;
    x = draw_fact(app, x, inner.y, end, S("procs"), str_format(app->frame, "%zu", stats->processes));
    x = draw_fact(app, x, inner.y, end, S("threads"), str_format(app->frame, "%zu", stats->threads));
    draw_fact(app, x, inner.y, end, S("restarts"), str_format(app->frame, "%u", restarts));
    draw_bottom_line(app, inner, view->status.problem, str_join(app->frame, names, S(" · ")));
    draw_detail_graphs(app, inner, stats);
}

static void draw_detail(App *app, Rect rect)
{
    const Row *row = &app->rows[app->selected];
    row->header ? draw_group_detail(app, rect, row->group) : draw_proc_detail(app, rect, row->proc);
}

// ---- log ----

typedef struct {
    App *app;
    Rect area;
    int remaining; // rows still free above what is drawn
} LogCanvas;

static uint32_t lower_glyph(uint32_t glyph)
{
    return glyph >= 'A' && glyph <= 'Z' ? glyph + ('a' - 'A') : glyph;
}

// highlight_matches marks where the search shows in rows y to y + rows of area.
static void highlight_matches(App *app, Rect area, int y, int rows)
{
    String search = app_search(app);
    if (search.len == 0 || search.len > (size_t)area.width) {
        return;
    }
    const Theme *theme = current_theme(app);
    for (int row = y; row < y + rows; row++) {
        for (int x = area.x; x + (int)search.len <= area.x + area.width; x++) {
            bool found = true;
            for (size_t i = 0; i < search.len && found; i++) {
                Cell *cell = screen_cell(app->screen, x + (int)i, row);
                found = cell != nullptr && lower_glyph(cell->glyph) == lower_glyph((unsigned char)search.data[i]);
            }
            for (size_t i = 0; found && i < search.len; i++) {
                Cell *cell = screen_cell(app->screen, x + (int)i, row);
                cell->bg = theme->highlight;
                cell->fg = theme->background;
            }
        }
    }
}

// draw_log_line puts line just above what is drawn so far, or as much of
// its bottom as fits, and returns the row it starts on.
static int draw_log_line(LogCanvas *canvas, String line)
{
    const Theme *theme = current_theme(canvas->app);
    int rows = log_line_rows(line, canvas->area.width);
    int skip = rows > canvas->remaining ? rows - canvas->remaining : 0;
    int shown = rows - skip;
    int y = canvas->area.y + canvas->remaining - shown;
    log_line_draw(canvas->app->screen, canvas->area.x, y, canvas->area.width, skip, shown, line, theme);
    highlight_matches(canvas->app, canvas->area, y, shown);
    canvas->remaining -= shown;
    return y - skip;
}

// shown is whether the log shows line: every line, or only matches while filtering.
static bool shown(App *app, String line)
{
    return !app->filtering || app_matches(app, line);
}

// draw_log_from_top fills area with the lines from first on and returns the
// number of the last line it reached.
static uint64_t draw_log_from_top(App *app, const LogBuffer *log, Rect area, uint64_t first, uint64_t end)
{
    int bottom = area.y + area.height;
    int y = area.y;
    uint64_t last = first;
    for (uint64_t n = first; n < end && y < bottom; n++) {
        String line = logbuf_line(log, n);
        if (!shown(app, line)) {
            continue;
        }
        log_line_draw(app->screen, area.x, y, area.width, 0, bottom - y, line, current_theme(app));
        highlight_matches(app, area, y, log_line_rows(line, area.width));
        y += log_line_rows(line, area.width);
        last = n;
    }
    return last;
}

static void draw_cursor(App *app, Rect area, int line_y, String partial)
{
    const Theme *theme = current_theme(app);
    int row;
    int column;
    log_line_end(partial, area.width, &row, &column);
    Cell *cell = screen_cell(app->screen, area.x + column, line_y + row);
    bool visible = (app->now_ns / NS_PER_MILLISECOND / (BLINK_MS / 2)) % 2 == 0;
    if (cell != nullptr && line_y + row < area.y + area.height && visible) {
        cell->bg = theme->text;
        cell->fg = theme->background;
    }
}

static String placeholder(ProcStatus status)
{
    switch (status.state) {
    case PROC_IDLE:
        return S("not started · press s to start");
    case PROC_WAITING:
        return S("waiting for what it needs…");
    case PROC_STARTING:
    case PROC_RUNNING:
        return S("waiting for output…");
    default:
        return S("no output");
    }
}

static String log_info(App *app, const ProcView *view, uint64_t end, Rgb *color)
{
    const Theme *theme = current_theme(app);
    if (app->focus == FOCUS_INPUT) {
        *color = theme->highlight;
        return S("attached · ctrl-a detaches");
    }
    if (app->filtering) {
        *color = theme->highlight;
        return str_format(app->frame, "only \"%.*s\"", (int)app->search_len, app->search);
    }
    if (view->following) {
        *color = theme->running;
        return S("● live");
    }
    *color = theme->starting;
    return str_format(app->frame, "↓ %llu newer lines", (unsigned long long)(end - 1 - view->anchor));
}

static void draw_log(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    ProcView *view = app_log_view(app);
    const ProcSpec *spec = &view->spec;
    ProcStatus status = view->status;
    bool attached = app->focus == FOCUS_INPUT;
    bool focused = attached || app->focus == FOCUS_LOG;

    const LogBuffer *log = view->log;
    uint64_t first = logbuf_first(log);
    uint64_t end = logbuf_end(log);
    String partial = logbuf_partial(log);
    if (!view->following && (view->anchor < first || view->anchor >= end)) {
        view->following = true;
    }
    Rgb info_color;
    String info = log_info(app, view, end, &info_color);
    String title = str_format(app->frame, "log · %.*s", (int)spec->name.len, spec->name.data);
    Rgb title_color = app->show_all ? theme->text : app_accent(app, app->rows[app->selected].proc);
    Box box = box_style(app, title, title_color, info, info_color, focused);
    if (attached) {
        box.glow = rgb_mix(theme->highlight, theme->focus, wave(app, PULSE_MS, 0.0f));
        box.border = rgb_mix(box.glow, theme->border, 0.5f);
    }
    draw_box(app->screen, rect, box);
    Rect inner = rect_inner(rect);
    if (inner.width < 4 || inner.height < 1) {
        return;
    }
    Rect area = { inner.x + 1, inner.y, inner.width - 2, inner.height };
    LogCanvas canvas = { .app = app, .area = area, .remaining = area.height };
    if (view->following && partial.len > 0 && shown(app, partial)) {
        int y = draw_log_line(&canvas, partial);
        if (attached) {
            draw_cursor(app, area, y, partial);
        }
    }
    uint64_t n = view->following ? end : view->anchor + 1;
    while (canvas.remaining > 0 && n > first) {
        n--;
        if (shown(app, logbuf_line(log, n))) {
            draw_log_line(&canvas, logbuf_line(log, n));
        }
    }
    if (!view->following && canvas.remaining > 0) {
        // Scrolled back to the oldest output, which does not fill the view:
        // show it from the top and the newer lines after it.
        screen_paint(app->screen, area, theme->background);
        view->anchor = draw_log_from_top(app, log, area, first, end);
        view->following = view->anchor + 1 >= end;
    }
    bool empty = end == first && partial.len == 0;

    if (empty) {
        String text = app->show_all                       ? S("lines of every process show here as they come")
                      : app->rows[app->selected].header ? S("lines of its processes show here as they come")
                                                        : placeholder(status);
        int x = area.x + (area.width - text_columns(text)) / 2;
        screen_text(app->screen, x, area.y + area.height / 2, area.width, text, theme->muted, ATTR_ITALIC);
    }
    size_t total = (size_t)(end - first);
    size_t bottom = view->following ? total : (size_t)(view->anchor - first + 1);
    size_t visible = min_size((size_t)area.height, total);
    draw_scrollbar(app, rect, bottom > visible ? bottom - visible : 0, visible, total, theme->focus);
}

// ---- footer, toast and overlays ----

static void draw_brand(App *app, int x, int y)
{
    const Theme *theme = current_theme(app);
    Gradient gradient = { theme->accents[0], theme->accents[1], theme->accents[6] };
    String brand = S("bprocs");
    float shift = (float)(app->now_ns / NS_PER_MILLISECOND % 6000) / 6000.0f;
    for (size_t i = 0; i < brand.len; i++) {
        float t = fmodf((float)i / (float)brand.len * 0.7f + shift * 2.0f, 2.0f);
        t = t > 1.0f ? 2.0f - t : t;
        screen_glyph(app->screen, x + (int)i, y, (uint32_t)brand.data[i], gradient_at(gradient, t), ATTR_BOLD);
    }
}

static int draw_hint(App *app, int x, int y, int end, const char *key, const char *label)
{
    const Theme *theme = current_theme(app);
    int width = text_columns(S(key)) + 1 + text_columns(S(label));
    if (x + width > end) {
        return end;
    }
    x += screen_text(app->screen, x, y, end - x, S(key), theme->highlight, ATTR_BOLD) + 1;
    x += screen_text(app->screen, x, y, end - x, S(label), theme->muted, 0);
    return x + 2;
}

static void draw_footer(App *app, Rect rect)
{
    const Theme *theme = current_theme(app);
    String clock = format_clock(app->frame, clock_wall_ns());
    int right = rect.x + rect.width - 1;
    int clock_x = draw_text_right(app->screen, right, rect.y, clock, theme->muted, 0);
    int brand_x = clock_x - 8;
    draw_brand(app, brand_x, rect.y);
    int end = brand_x - 2;
    int x = rect.x + 1;
    if (app->focus == FOCUS_INPUT) {
        String pill = S(" ATTACHED ");
        int width = text_columns(pill);
        screen_paint(app->screen, (Rect) { x, rect.y, width, 1 }, theme->highlight);
        screen_text(app->screen, x, rect.y, width, pill, theme->background, ATTR_BOLD);
        x += width + 1;
        const ProcSpec *spec = &app_selected_view(app)->spec;
        String text = str_format(app->frame, "keys go to %.*s · ctrl-a detaches", (int)spec->name.len, spec->name.data);
        screen_text(app->screen, x, rect.y, end - x, text_fit(text, end - x), theme->text, 0);
        return;
    }
    if (app->typing_search) {
        x += screen_text(app->screen, x, rect.y, end - x, S("/"), theme->highlight, ATTR_BOLD);
        x += screen_text(app->screen, x, rect.y, end - x, app_search(app), theme->text, 0);
        screen_glyph(app->screen, x, rect.y, ' ', theme->text, 0);
        Cell *cursor = screen_cell(app->screen, x, rect.y);
        if (cursor != nullptr) {
            cursor->bg = theme->text;
        }
        x = draw_hint(app, x + 2, rect.y, end, "⏎", "find");
        draw_hint(app, x, rect.y, end, "esc", "cancel");
        return;
    }
    if (app->daemon_gone) {
        String pill = S(" DAEMON STOPPED ");
        int width = text_columns(pill);
        screen_paint(app->screen, (Rect) { x, rect.y, width, 1 }, theme->failed);
        screen_text(app->screen, x, rect.y, width, pill, theme->background, ATTR_BOLD);
        x += width + 1;
        x = draw_hint(app, x, rect.y, end, "u", "start it again");
        draw_hint(app, x, rect.y, end, "q", "quit");
        return;
    }
    static const struct {
        const char *key;
        const char *label;
    } HINTS[] = {
        { "↑↓", "select" }, { "⏎", "attach" }, { "s", "start" }, { "x", "stop" }, { "r", "restart" },
        { "tab", "log" },   { "?", "help" },   { "q", "quit" },  { "D", "down" },
    };
    for (size_t i = 0; i < countof(HINTS) && x < end; i++) {
        x = draw_hint(app, x, rect.y, end, HINTS[i].key, HINTS[i].label);
    }
}

static void draw_toast(App *app)
{
    float age = elapsed_ms(app, app->toast_ns);
    if (app->toast_len == 0 || age > TOAST_MS) {
        return;
    }
    const Theme *theme = current_theme(app);
    float fade_in = age / TOAST_FADE_IN_MS;
    float fade_out = (TOAST_MS - age) / TOAST_FADE_OUT_MS;
    float alpha = ease_out(fminf(fminf(fade_in, fade_out), 1.0f));
    String text = { app->toast, app->toast_len };
    int width = text_columns(text) + 4;
    int screen_w = screen_width(app->screen);
    width = width > screen_w - 2 ? screen_w - 2 : width;
    Rect rect = { screen_w - width - 1, app->layout.footer.y - 3, width, 3 };
    Rgb background = rgb_mix(theme->background, theme->surface, alpha);
    screen_paint(app->screen, rect, background);
    Rgb border = rgb_mix(theme->background, theme->focus, alpha);
    draw_box(app->screen, rect, (Box) { .border = rgb_mix(background, border, 0.6f), .glow = border });
    screen_text(app->screen, rect.x + 2, rect.y + 1, width - 4, text_fit(text, width - 4),
                rgb_mix(theme->background, theme->text, alpha), 0);
}

static Rect centered(App *app, int width, int height)
{
    int screen_w = screen_width(app->screen);
    int screen_h = screen_height(app->screen);
    width = width > screen_w ? screen_w : width;
    height = height > screen_h ? screen_h : height;
    return (Rect) { (screen_w - width) / 2, (screen_h - height) / 2, width, height };
}

static void draw_help(App *app)
{
    static const struct {
        const char *keys;
        const char *what;
    } KEYS[] = {
        { "↑ ↓  j k", "select a process, or scroll the log" },
        { "enter  i", "attach: type into the process" },
        { "h l  ← →", "hide or show an app's processes; enter too" },
        { "ctrl-a", "detach again" },
        { "tab", "move between the list and the log" },
        { "s  x  r", "start, stop, restart" },
        { "K", "kill at once (SIGKILL)" },
        { "S  X", "start all, stop all" },
        { "D", "down: stop everything, then quit" },
        { "pgup pgdn", "scroll the log a page" },
        { "g  G", "oldest output, newest output" },
        { "f  c", "follow on or off, clear the log" },
        { "/  n N", "search the log, next older, next newer" },
        { "&", "show only the lines found, or all again" },
        { "a", "the log of every process together" },
        { "o", "open http://localhost:port" },
        { "p", "switch to another running project" },
        { "R", "reload the config" },
        { "1 2 3 4", "hide or show cpu, mem, net, details" },
        { "t", "next colour theme" },
        { "q", "quit, leaving everything running" },
        { "mouse", "click to select, wheel to scroll" },
    };
    const Theme *theme = current_theme(app);
    Rect all = { 0, 0, screen_width(app->screen), screen_height(app->screen) };
    screen_shade(app->screen, all, 0.35f);
    Rect rect = centered(app, 58, (int)countof(KEYS) + 4);
    screen_paint(app->screen, rect, theme->surface);
    draw_box(app->screen, rect,
             box_style(app, S("keys"), theme->text, S(current_theme(app)->name), theme->muted, true));
    for (size_t i = 0; i < countof(KEYS); i++) {
        int y = rect.y + 2 + (int)i;
        screen_text(app->screen, rect.x + 3, y, 12, S(KEYS[i].keys), theme->highlight, ATTR_BOLD);
        screen_text(app->screen, rect.x + 16, y, rect.width - 19, S(KEYS[i].what), theme->text, 0);
    }
}

static void draw_switcher(App *app)
{
    const Theme *theme = current_theme(app);
    Rect all = { 0, 0, screen_width(app->screen), screen_height(app->screen) };
    screen_shade(app->screen, all, 0.35f);
    int rows = app->daemon_count > 0 ? (int)app->daemon_count : 1;
    Rect rect = centered(app, 72, rows + 4);
    screen_paint(app->screen, rect, theme->surface);
    draw_box(app->screen, rect, box_style(app, S("projects"), theme->text, S("⏎ switch · esc"), theme->muted, true));
    if (app->daemon_count == 0) {
        screen_text(app->screen, rect.x + 3, rect.y + 2, rect.width - 6, S("no daemon runs"), theme->muted, ATTR_ITALIC);
        return;
    }
    for (size_t i = 0; i < app->daemon_count; i++) {
        const Daemon *daemon = &app->daemons[i];
        int y = rect.y + 2 + (int)i;
        bool selected = i == app->daemon_selected;
        if (selected) {
            screen_paint(app->screen, (Rect) { rect.x + 1, y, rect.width - 2, 1 }, theme->selection);
            screen_glyph(app->screen, rect.x + 1, y, GLYPH_SELECTED, theme->focus, 0);
        }
        String count = str_format(app->frame, "%zu/%zu up", daemon->running, daemon->total);
        int right = draw_text_right(app->screen, rect.x + rect.width - 3, y, count, theme->muted, 0);
        bool current = str_equal(daemon->config_path, app->config_path);
        screen_text(app->screen, rect.x + 3, y, right - rect.x - 5, text_fit(daemon->config_path, right - rect.x - 5),
                    current ? theme->highlight : theme->text, selected ? ATTR_BOLD : 0);
    }
}

static void draw_going_down(App *app)
{
    const Theme *theme = current_theme(app);
    size_t left = 0;
    for (size_t i = 0; i < app->view_count; i++) {
        left += proc_state_active(app->views[i].status.state);
    }
    Rect all = { 0, 0, screen_width(app->screen), screen_height(app->screen) };
    screen_shade(app->screen, all, 0.5f);
    Rect rect = centered(app, 46, 6);
    screen_paint(app->screen, rect, theme->surface);
    draw_box(app->screen, rect, box_style(app, S("down"), theme->text, S(""), theme->muted, true));
    String text = str_format(app->frame, "stopping %zu of %zu processes", left, app->down_total);
    screen_glyph(app->screen, rect.x + 3, rect.y + 2, spinner_glyph(app->now_ns), theme->starting, 0);
    screen_text(app->screen, rect.x + 5, rect.y + 2, rect.width - 8, text, theme->text, 0);
    float done = app->down_total > 0 ? 1.0f - (float)left / (float)app->down_total : 1.0f;
    draw_meter(app->screen, rect.x + 3, rect.y + 3, rect.width - 6, done, theme->cpu, theme->faint);
    screen_text(app->screen, rect.x + 3, rect.y + 4, rect.width - 6, S("D again kills them · q leaves"), theme->muted,
                ATTR_ITALIC);
}

void ui_draw(App *app)
{
    const Theme *theme = current_theme(app);
    Rect all = { 0, 0, screen_width(app->screen), screen_height(app->screen) };
    screen_paint(app->screen, all, theme->background);
    const Layout *layout = &app->layout;
    if (layout->show_cpu) {
        draw_cpu(app, layout->cpu);
    }
    if (layout->show_memory) {
        draw_memory(app, layout->memory);
    }
    if (layout->show_network) {
        draw_network(app, layout->network);
    }
    if (app->view_count > 0) {
        draw_procs(app, layout->procs);
        if (layout->show_detail) {
            draw_detail(app, layout->detail);
        }
        draw_log(app, layout->log);
    }
    draw_footer(app, layout->footer);
    draw_toast(app);
    if (app->show_help) {
        draw_help(app);
    }
    if (app->switching) {
        draw_switcher(app);
    }
    if (app->going_down) {
        draw_going_down(app);
    }
    float intro = elapsed_ms(app, app->started_ns) / INTRO_MS;
    if (intro < 1.0f) {
        screen_shade(app->screen, all, 0.1f + 0.9f * ease_out(intro));
    }
}

bool ui_animating(App *app)
{
    if (app->going_down || app->typing_search || app->focus == FOCUS_INPUT || elapsed_ms(app, app->started_ns) < INTRO_MS ||
        (app->toast_len > 0 && elapsed_ms(app, app->toast_ns) < TOAST_MS)) {
        return true;
    }
    for (size_t i = 0; i < app->view_count; i++) {
        ProcStatus status = app->views[i].status;
        if (status.state == PROC_RUNNING || status.state == PROC_STARTING || status.state == PROC_STOPPING ||
            status.restart_at_ns != 0 || elapsed_ms(app, app->views[i].state_changed_ns) < FLASH_MS) {
            return true;
        }
    }
    return false;
}
