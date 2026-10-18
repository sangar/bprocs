#include "test.h"

#include "draw.h"
#include "group.h"
#include "layout.h"
#include "screen.h"

static const Rgb WHITE = { 255, 255, 255 };
static const Rgb BLACK = { 0, 0, 0 };

static String render(Test *test, Screen *screen)
{
    StringBuilder out = str_builder_create(test->arena, 256);
    screen_render(screen, &out);
    return str_builder_finish(&out);
}

static void draw_text(Screen *screen, const char *text)
{
    screen_paint(screen, (Rect) { 0, 0, screen_width(screen), screen_height(screen) }, BLACK);
    screen_text(screen, 0, 0, screen_width(screen), S(text), WHITE, 0);
}

static void screen_writes_only_what_changed(Test *test)
{
    Screen *screen = screen_create(COLOR_TRUE);
    screen_resize(screen, 4, 2);
    draw_text(screen, "ab");
    String first = render(test, screen);
    test_check(test, str_contains(first, S("ab")) && str_contains(first, S("38;2;255;255;255")),
               "the first frame paints everything in true colour");
    draw_text(screen, "ac");
    String second = render(test, screen);
    test_check(test, str_starts_with(second, S("\x1b[1;2H")) && str_ends_with(second, S("c")) &&
                         !str_contains(second, S("a")),
               "the next frame moves to the changed cell and writes it");
    draw_text(screen, "ac");
    test_check(test, render(test, screen).len == 0, "an unchanged frame writes nothing");
    screen_destroy(screen);

    screen = screen_create(COLOR_256);
    screen_resize(screen, 2, 1);
    draw_text(screen, "x");
    test_check(test, str_contains(render(test, screen), S("38;5;231")), "the 256-colour palette");
    screen_destroy(screen);
}

static void screen_keeps_wide_glyphs_whole(Test *test)
{
    Screen *screen = screen_create(COLOR_TRUE);
    screen_resize(screen, 3, 1);
    test_check(test, screen_text(screen, 0, 0, 3, S("界"), WHITE, 0) == 2, "a wide glyph takes two columns");
    test_check(test, screen_cell(screen, 1, 0)->glyph == 0, "the second column is its tail");
    screen_glyph(screen, 1, 0, 'x', WHITE, 0);
    test_check(test, screen_cell(screen, 0, 0)->glyph == ' ', "overwriting the tail blanks the head");
    test_check(test, screen_text(screen, 2, 0, 1, S("界"), WHITE, 0) == 0, "a wide glyph does not fit in one column");
    screen_destroy(screen);
}

static void graphs_plot_in_braille(Test *test)
{
    Screen *screen = screen_create(COLOR_TRUE);
    screen_resize(screen, 1, 2);
    Gradient gradient = { BLACK, BLACK, WHITE };
    History history = { 0 };
    history_push(&history, 0.25f);
    history_push(&history, 1.0f);
    draw_graph(screen, (Rect) { 0, 0, 1, 2 }, &history, 1.0f, gradient, false);
    // The newest sample fills the right column, the one before it a quarter of the left.
    test_check(test, screen_cell(screen, 0, 1)->glyph == 0x2800 + (0xB8 | 0x44), "the bottom cell");
    test_check(test, screen_cell(screen, 0, 0)->glyph == 0x2800 + 0xB8, "the top cell");
    test_check(test, rgb_equal(screen_cell(screen, 0, 0)->fg, gradient_at(gradient, 0.75f)), "coloured by height");
    screen_paint(screen, (Rect) { 0, 0, 1, 2 }, BLACK);
    draw_graph(screen, (Rect) { 0, 0, 1, 2 }, &history, 4.0f, gradient, true);
    test_check(test, screen_cell(screen, 0, 0)->glyph == 0x2800 + (0x08 | 0x10 | 0x01), "a downward graph hangs");
    screen_destroy(screen);
}

static void colours_blend_and_map_to_the_palette(Test *test)
{
    Gradient gradient = { { 0, 0, 0 }, { 100, 100, 100 }, { 200, 0, 0 } };
    test_check(test, rgb_equal(gradient_at(gradient, 0.5f), (Rgb) { 100, 100, 100 }), "the middle stop");
    test_check(test, rgb_equal(gradient_at(gradient, 2.0f), (Rgb) { 200, 0, 0 }), "clamped past the end");
    test_check(test, rgb_to_xterm(xterm_to_rgb(196)) == 196 && rgb_to_xterm(xterm_to_rgb(244)) == 244,
               "palette colours map to themselves");
    Rgb parsed;
    test_check(test, rgb_parse(S("#7aA2f7"), &parsed) && rgb_equal(parsed, (Rgb) { 0x7a, 0xa2, 0xf7 }), "hex");
    test_check(test, !rgb_parse(S("7aa2f7"), &parsed), "hex needs its #");
}

static bool inside(Rect rect, int width, int height)
{
    return rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= width && rect.y + rect.height <= height;
}

static bool overlap(Rect a, Rect b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static void layout_fits_every_size(Test *test)
{
    const int sizes[][3] = { { 220, 60, 12 }, { 150, 40, 6 }, { 100, 30, 30 }, { 80, 24, 6 }, { 60, 16, 3 } };
    for (size_t i = 0; i < countof(sizes); i++) {
        int width = sizes[i][0];
        int height = sizes[i][1];
        Layout layout = layout_compute(width, height, (size_t)sizes[i][2]);
        bool fits = inside(layout.procs, width, height) && inside(layout.log, width, height) &&
                    inside(layout.footer, width, height) && !overlap(layout.procs, layout.log) &&
                    layout.footer.y == height - 1 && layout.log.height >= 5;
        fits = fits && (!layout.show_detail || (inside(layout.detail, width, height) &&
                                                 !overlap(layout.detail, layout.procs) &&
                                                 !overlap(layout.detail, layout.log)));
        fits = fits && (!layout.show_system || (inside(layout.cpu, width, height) && !overlap(layout.cpu, layout.procs)));
        test_check(test, fits, "the boxes fit side by side");
    }
    test_check(test, !layout_compute(80, 24, 6).show_system, "a small terminal leaves out the system boxes");
}

static void groups_list_an_apps_procs_under_a_header(Test *test)
{
    static const char *const NAMES[] = { "db", "shop:setup", "shop:web", "admin:web" };
    StringList names = strlist_of(test->arena, countof(NAMES), NAMES);
    Group groups[countof(NAMES)];
    size_t group_count = group_find(names, groups);
    test_check(test, group_count == 2 && str_equal(groups[0].name, S("shop")) && groups[0].first == 1 &&
                         groups[0].count == 2 && groups[1].first == 3,
               "a group for each app, after the top-level procs");
    Row rows[countof(NAMES) + countof(NAMES)];
    size_t count = group_rows(groups, group_count, names.count, rows);
    test_check(test, count == 3 && !rows[0].grouped && rows[1].header && rows[2].header && rows[2].proc == 3,
               "collapsed, a group shows as its header");
    groups[0].expanded = true;
    count = group_rows(groups, group_count, names.count, rows);
    test_check(test, count == 5 && rows[1].header && !rows[3].header && rows[3].grouped && rows[3].group == 0 &&
                         rows[3].proc == 2 && rows[4].header,
               "expanded, its procs follow the header");
    test_check_str(test, group_short_name(S("shop:web")), S("web"), "a proc under a header shows by its short name");
}

static void groups_show_the_proc_most_in_need_of_attention(Test *test)
{
    ProcStatus crashed = { .state = PROC_CRASHED };
    ProcStatus restarting = { .state = PROC_CRASHED, .restart_at_ns = 1 };
    ProcStatus unhealthy = { .state = PROC_RUNNING, .health = HEALTH_UNHEALTHY };
    ProcStatus running = { .state = PROC_RUNNING };
    ProcStatus stopped = { .state = PROC_STOPPED };
    ProcStatus done = { .state = PROC_EXITED };
    test_check(test, group_rank(crashed) > group_rank(unhealthy) && group_rank(unhealthy) > group_rank(restarting) &&
                         group_rank(restarting) > group_rank(running),
               "a failure, then a failing check, then a restart coming, before running");
    test_check(test, group_rank(running) > group_rank(stopped) && group_rank(stopped) > group_rank(done),
               "a stopped proc only shows when none runs, and a finished setup job never");
}

const TestCase SCREEN_TESTS[] = {
    { "screen_writes_only_what_changed", screen_writes_only_what_changed },
    { "screen_keeps_wide_glyphs_whole", screen_keeps_wide_glyphs_whole },
    { "graphs_plot_in_braille", graphs_plot_in_braille },
    { "colours_blend_and_map_to_the_palette", colours_blend_and_map_to_the_palette },
    { "layout_fits_every_size", layout_fits_every_size },
    { "groups_list_an_apps_procs_under_a_header", groups_list_an_apps_procs_under_a_header },
    { "groups_show_the_proc_most_in_need_of_attention", groups_show_the_proc_most_in_need_of_attention },
    { nullptr, nullptr },
};
