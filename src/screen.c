#include "screen.h"

#include <string.h>

#include "mc/text/utf8.h"
#include "width.h"

struct Screen {
    Arena *arena;   // the Screen itself
    Arena *buffers; // the cells, for the current size
    ColorMode mode;
    int width;
    int height;
    Cell *cells;
    Cell *shown; // what the terminal shows, as of the last render
    bool repaint;
};

static const Cell BLANK = { .glyph = ' ' };

Screen *screen_create(ColorMode mode)
{
    Arena *arena = arena_create(0);
    Screen *screen = arena_push(arena, sizeof *screen);
    screen->arena = arena;
    screen->buffers = arena_create(0);
    screen->mode = mode;
    screen->repaint = true;
    return screen;
}

void screen_destroy(Screen *screen)
{
    arena_destroy(screen->buffers);
    arena_destroy(screen->arena);
}

void screen_resize(Screen *screen, int width, int height)
{
    arena_reset(screen->buffers);
    screen->width = width > 0 ? width : 0;
    screen->height = height > 0 ? height : 0;
    size_t count = arena_size_mul((size_t)screen->width, (size_t)screen->height);
    screen->cells = arena_push(screen->buffers, arena_size_mul(count, sizeof(Cell)));
    screen->shown = arena_push(screen->buffers, arena_size_mul(count, sizeof(Cell)));
    for (size_t i = 0; i < count; i++) {
        screen->cells[i] = BLANK;
    }
    screen->repaint = true;
}

int screen_width(const Screen *screen)
{
    return screen->width;
}

int screen_height(const Screen *screen)
{
    return screen->height;
}

Cell *screen_cell(Screen *screen, int x, int y)
{
    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height) {
        return nullptr;
    }
    return &screen->cells[(size_t)y * (size_t)screen->width + (size_t)x];
}

void screen_paint(Screen *screen, Rect rect, Rgb bg)
{
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                *cell = (Cell) { .glyph = ' ', .fg = bg, .bg = bg };
            }
        }
    }
}

// set_cell writes one column and keeps wide glyphs whole: a half that loses
// its other half becomes a blank.
static void set_cell(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    Cell *cell = screen_cell(screen, x, y);
    if (cell->glyph == 0) {
        Cell *head = screen_cell(screen, x - 1, y);
        if (head != nullptr) {
            head->glyph = ' ';
        }
    }
    Cell *next = screen_cell(screen, x + 1, y);
    if (next != nullptr && next->glyph == 0) {
        next->glyph = ' ';
    }
    cell->glyph = glyph;
    cell->fg = fg;
    cell->attrs = attrs;
}

static int put(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    int width = glyph_width(glyph);
    if (width == 0 || screen_cell(screen, x, y) == nullptr) {
        return 0;
    }
    if (glyph < 0x20 || glyph == 0x7f) {
        glyph = ' ';
    }
    if (width == 2 && screen_cell(screen, x + 1, y) == nullptr) {
        set_cell(screen, x, y, ' ', fg, attrs);
        return 1;
    }
    set_cell(screen, x, y, glyph, fg, attrs);
    if (width == 2) {
        set_cell(screen, x + 1, y, 0, fg, attrs);
    }
    return width;
}

void screen_glyph(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    put(screen, x, y, glyph, fg, attrs);
}

int screen_text(Screen *screen, int x, int y, int max_columns, String text, Rgb fg, uint8_t attrs)
{
    int used = 0;
    size_t position = 0;
    while (position < text.len) {
        uint32_t codepoint;
        utf8_decode(text, &position, &codepoint);
        int width = glyph_width(codepoint);
        if (used + width > max_columns) {
            break;
        }
        put(screen, x + used, y, codepoint, fg, attrs);
        used += width;
    }
    return used;
}

void screen_shade(Screen *screen, Rect rect, float factor)
{
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                cell->fg = rgb_scale(cell->fg, factor);
                cell->bg = rgb_scale(cell->bg, factor);
            }
        }
    }
}

void screen_invalidate(Screen *screen)
{
    screen->repaint = true;
}

static bool cell_equal(const Cell *a, const Cell *b)
{
    return a->glyph == b->glyph && a->attrs == b->attrs && rgb_equal(a->fg, b->fg) && rgb_equal(a->bg, b->bg);
}

// Pen is the style the terminal writes with.
typedef struct {
    Rgb fg;
    Rgb bg;
    uint8_t attrs;
    bool known;
} Pen;

static void append_color(StringBuilder *out, ColorMode mode, int selector, Rgb color)
{
    if (mode == COLOR_TRUE) {
        str_builder_append_format(out, "%d;2;%u;%u;%u", selector, color.r, color.g, color.b);
    } else {
        str_builder_append_format(out, "%d;5;%u", selector, rgb_to_xterm(color));
    }
}

static void use_pen(StringBuilder *out, ColorMode mode, Pen *pen, const Cell *cell)
{
    if (!pen->known || pen->attrs != cell->attrs) {
        str_builder_append(out, S("\x1b[0"));
        static const char *const CODES[] = { ";1", ";2", ";3", ";4" };
        for (size_t i = 0; i < countof(CODES); i++) {
            if ((cell->attrs & (1u << i)) != 0) {
                str_builder_append(out, S(CODES[i]));
            }
        }
        str_builder_append_char(out, ';');
        append_color(out, mode, 38, cell->fg);
        str_builder_append_char(out, ';');
        append_color(out, mode, 48, cell->bg);
        str_builder_append_char(out, 'm');
        *pen = (Pen) { .fg = cell->fg, .bg = cell->bg, .attrs = cell->attrs, .known = true };
        return;
    }
    if (!rgb_equal(pen->fg, cell->fg)) {
        str_builder_append(out, S("\x1b["));
        append_color(out, mode, 38, cell->fg);
        str_builder_append_char(out, 'm');
        pen->fg = cell->fg;
    }
    if (!rgb_equal(pen->bg, cell->bg)) {
        str_builder_append(out, S("\x1b["));
        append_color(out, mode, 48, cell->bg);
        str_builder_append_char(out, 'm');
        pen->bg = cell->bg;
    }
}

void screen_render(Screen *screen, StringBuilder *out)
{
    Pen pen = { 0 };
    int cursor_x = -1;
    int cursor_y = -1;
    for (int y = 0; y < screen->height; y++) {
        for (int x = 0; x < screen->width; x++) {
            size_t index = (size_t)y * (size_t)screen->width + (size_t)x;
            const Cell *cell = &screen->cells[index];
            if (cell->glyph == 0 || (!screen->repaint && cell_equal(cell, &screen->shown[index]))) {
                continue;
            }
            if (cursor_x != x || cursor_y != y) {
                str_builder_append_format(out, "\x1b[%d;%dH", y + 1, x + 1);
            }
            use_pen(out, screen->mode, &pen, cell);
            char bytes[UTF8_MAX_BYTES];
            str_builder_append(out, (String) { bytes, utf8_encode(cell->glyph, bytes) });
            cursor_x = x + glyph_width(cell->glyph);
            cursor_y = y;
            if (cursor_x >= screen->width) {
                cursor_x = -1;
            }
        }
    }
    size_t count = (size_t)screen->width * (size_t)screen->height;
    if (count > 0) {
        memcpy(screen->shown, screen->cells, count * sizeof(Cell));
    }
    screen->repaint = false;
}

int text_columns(String text)
{
    int columns = 0;
    size_t position = 0;
    while (position < text.len) {
        uint32_t codepoint;
        utf8_decode(text, &position, &codepoint);
        columns += glyph_width(codepoint);
    }
    return columns;
}

String text_fit(String text, int max_columns)
{
    int columns = 0;
    size_t position = 0;
    while (position < text.len) {
        size_t start = position;
        uint32_t codepoint;
        utf8_decode(text, &position, &codepoint);
        columns += glyph_width(codepoint);
        if (columns > max_columns) {
            return str_slice(text, 0, start);
        }
    }
    return text;
}
