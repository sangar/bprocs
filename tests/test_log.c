#include "test.h"

#include "logbuf.h"
#include "logview.h"
#include "screen.h"
#include "theme.h"

static void logbuf_splits_terminal_output_into_lines(Test *test)
{
    LogBuffer *log = logbuf_create(100, 1 << 20);
    logbuf_write(log, S("one\r\ntw"));
    logbuf_write(log, S("o\r"));
    logbuf_write(log, S("\nthree"));
    test_check(test, logbuf_end(log) == 2, "two finished lines");
    test_check_str(test, logbuf_line(log, 0), S("one"), "CR LF ends a line");
    test_check_str(test, logbuf_line(log, 1), S("two"), "even when split between writes");
    test_check_str(test, logbuf_partial(log), S("three"), "an unfinished line waits");
    logbuf_destroy(log);
}

static void logbuf_keeps_colour_and_drops_other_control(Test *test)
{
    LogBuffer *log = logbuf_create(100, 1 << 20);
    logbuf_write(log, S("\x1b[31mred\x1b[0m\x1b[2K\x1b]0;title\x07 ok\n"));
    logbuf_write(log, S("downloading 10%\rdownloading 99%\rdone\n"));
    logbuf_write(log, S("\x1b[1G\x1b[0K⠋ spin\x1b[1G\x1b[0K⠙ spun\n"));
    test_check_str(test, logbuf_line(log, 0), S("\x1b[31mred\x1b[0m ok"), "SGR stays, erase and titles go");
    test_check_str(test, logbuf_line(log, 1), S("done"), "a carriage return rewrites the line");
    test_check_str(test, logbuf_line(log, 2), S("⠙ spun"), "so does moving to the first column");
    test_check_str(test, logbuf_plain(test->arena, logbuf_line(log, 0)), S("red ok"), "plain text has no escapes");
    logbuf_destroy(log);
}

static void logbuf_forgets_the_oldest_lines(Test *test)
{
    LogBuffer *log = logbuf_create(3, 1 << 20);
    logbuf_write(log, S("a\nb\nc\nd\n"));
    test_check(test, logbuf_first(log) == 1 && logbuf_end(log) == 4, "only max_lines are kept");
    test_check_str(test, logbuf_line(log, 1), S("b"), "lines keep their numbers");
    logbuf_destroy(log);

    log = logbuf_create(1000, 64);
    for (int i = 0; i < 100; i++) {
        logbuf_write(log, S("0123456789\n"));
    }
    logbuf_note(log, S("newest"));
    test_check(test, logbuf_end(log) - logbuf_first(log) < 10, "the byte budget drops old lines too");
    test_check_str(test, logbuf_line(log, logbuf_end(log) - 1), S("newest"), "and keeps the newest");
    logbuf_destroy(log);
}

static void log_lines_wrap_at_the_view_width(Test *test)
{
    test_check(test, log_line_rows(S("abcdef"), 3) == 2, "six columns in rows of three");
    test_check(test, log_line_rows(S("\x1b[1mabc\x1b[0m"), 3) == 1, "escapes take no room");
    test_check(test, log_line_rows(S("a\tb"), 8) == 2, "a tab moves to the next stop");
    test_check(test, log_line_rows(S("界界"), 3) == 2, "a wide character does not split");
    int row;
    int column;
    log_line_end(S("irb> "), 80, &row, &column);
    test_check(test, row == 0 && column == 5, "the cursor goes after a prompt");
}

static void log_lines_draw_in_theme_colours(Test *test)
{
    const Theme *theme = theme_at(0);
    Screen *screen = screen_create(COLOR_TRUE);
    screen_resize(screen, 10, 2);
    log_line_draw(screen, 0, 0, 10, 0, 2, S("\x1b[31mx\x1b[0my\x1b[38;2;1;2;3mz"), theme);
    test_check(test, rgb_equal(screen_cell(screen, 0, 0)->fg, theme->ansi[1]), "red is the theme's red");
    test_check(test, rgb_equal(screen_cell(screen, 1, 0)->fg, theme->text), "reset is the theme's text");
    test_check(test, rgb_equal(screen_cell(screen, 2, 0)->fg, (Rgb) { 1, 2, 3 }), "true colour passes through");
    screen_destroy(screen);
}

const TestCase LOG_TESTS[] = {
    { "logbuf_splits_terminal_output_into_lines", logbuf_splits_terminal_output_into_lines },
    { "logbuf_keeps_colour_and_drops_other_control", logbuf_keeps_colour_and_drops_other_control },
    { "logbuf_forgets_the_oldest_lines", logbuf_forgets_the_oldest_lines },
    { "log_lines_wrap_at_the_view_width", log_lines_wrap_at_the_view_width },
    { "log_lines_draw_in_theme_colours", log_lines_draw_in_theme_colours },
    { nullptr, nullptr },
};
