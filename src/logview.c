#include "logview.h"

#include "mc/text/utf8.h"
#include "width.h"

enum { ESC = 0x1b, TAB_STOP = 8, SGR_PARAMS = 16 };

typedef struct {
    Rgb fg;
    Rgb bg;
    bool has_bg;
    bool reverse;
    uint8_t attrs;
} Style;

static Style default_style(const Theme *theme)
{
    return (Style) { .fg = theme->text, .bg = theme->background };
}

typedef struct {
    int values[SGR_PARAMS];
    size_t count;
} SgrParams;

static SgrParams parse_sgr(String body)
{
    SgrParams params = { .count = 1 };
    for (size_t i = 0; i < body.len; i++) {
        char c = body.data[i];
        if ((c == ';' || c == ':') && params.count < SGR_PARAMS) {
            params.values[params.count++] = 0;
        } else if (c >= '0' && c <= '9') {
            int *value = &params.values[params.count - 1];
            *value = *value < 10000 ? *value * 10 + (c - '0') : *value;
        }
    }
    return params;
}

// extended_color reads 5;n or 2;r;g;b after a 38 or 48 at params[*at] and
// moves *at to the last parameter it used.
static bool extended_color(SgrParams params, size_t *at, Rgb *color)
{
    size_t i = *at;
    if (i + 2 < params.count && params.values[i + 1] == 5) {
        *color = xterm_to_rgb((uint8_t)params.values[i + 2]);
        *at = i + 2;
        return true;
    }
    if (i + 4 < params.count && params.values[i + 1] == 2) {
        *color = (Rgb) { (uint8_t)params.values[i + 2], (uint8_t)params.values[i + 3], (uint8_t)params.values[i + 4] };
        *at = i + 4;
        return true;
    }
    *at = params.count;
    return false;
}

static Rgb palette(const Theme *theme, int index)
{
    return theme->ansi[index & 15];
}

static void apply_sgr(Style *style, String body, const Theme *theme)
{
    SgrParams params = parse_sgr(body);
    for (size_t i = 0; i < params.count; i++) {
        int p = params.values[i];
        if (p == 0) {
            *style = default_style(theme);
        } else if (p == 1) {
            style->attrs |= ATTR_BOLD;
        } else if (p == 2) {
            style->attrs |= ATTR_DIM;
        } else if (p == 3) {
            style->attrs |= ATTR_ITALIC;
        } else if (p == 4) {
            style->attrs |= ATTR_UNDERLINE;
        } else if (p == 7) {
            style->reverse = true;
        } else if (p == 22) {
            style->attrs &= (uint8_t)~(ATTR_BOLD | ATTR_DIM);
        } else if (p == 23) {
            style->attrs &= (uint8_t)~ATTR_ITALIC;
        } else if (p == 24) {
            style->attrs &= (uint8_t)~ATTR_UNDERLINE;
        } else if (p == 27) {
            style->reverse = false;
        } else if (p >= 30 && p <= 37) {
            style->fg = palette(theme, p - 30);
        } else if (p >= 90 && p <= 97) {
            style->fg = palette(theme, p - 90 + 8);
        } else if (p == 39) {
            style->fg = theme->text;
        } else if (p >= 40 && p <= 47) {
            style->bg = palette(theme, p - 40);
            style->has_bg = true;
        } else if (p >= 100 && p <= 107) {
            style->bg = palette(theme, p - 100 + 8);
            style->has_bg = true;
        } else if (p == 49) {
            style->bg = theme->background;
            style->has_bg = false;
        } else if (p == 38) {
            extended_color(params, &i, &style->fg);
        } else if (p == 48) {
            style->has_bg = extended_color(params, &i, &style->bg) || style->has_bg;
        }
    }
}

// Layout walks a line and places each glyph, wrapping at width. With no
// screen it only counts rows.
typedef struct {
    Screen *screen;
    int x;
    int y;
    int width;
    int skip_rows;
    int max_rows;
    const Theme *theme;
    int row;    // of the whole line
    int column; // within the row
    int line_column;
} Layout;

static bool row_visible(const Layout *layout)
{
    return layout->screen != nullptr && layout->row >= layout->skip_rows &&
           layout->row < layout->skip_rows + layout->max_rows;
}

static void place(Layout *layout, uint32_t glyph, int width, Style style)
{
    if (layout->column + width > layout->width && layout->column > 0) {
        layout->row++;
        layout->column = 0;
    }
    if (row_visible(layout)) {
        Rgb fg = style.reverse ? style.bg : style.fg;
        Rgb bg = style.reverse ? style.fg : style.bg;
        if ((style.attrs & ATTR_DIM) != 0) {
            fg = rgb_mix(fg, layout->theme->background, 0.45f);
        }
        int screen_x = layout->x + layout->column;
        int screen_y = layout->y + layout->row - layout->skip_rows;
        screen_glyph(layout->screen, screen_x, screen_y, glyph, fg, (uint8_t)(style.attrs & ~ATTR_DIM));
        if (style.has_bg || style.reverse) {
            for (int i = 0; i < width; i++) {
                Cell *cell = screen_cell(layout->screen, screen_x + i, screen_y);
                if (cell != nullptr) {
                    cell->bg = bg;
                }
            }
        }
    }
    layout->column += width;
    layout->line_column += width;
}

static int lay_out(Layout *layout, String line)
{
    Style style = default_style(layout->theme);
    size_t position = 0;
    while (position < line.len) {
        if ((unsigned char)line.data[position] == ESC) {
            size_t start = position + 2;
            size_t end = start;
            while (end < line.len && !((unsigned char)line.data[end] >= 0x40 && (unsigned char)line.data[end] <= 0x7e)) {
                end++;
            }
            if (end < line.len && line.data[end] == 'm') {
                apply_sgr(&style, str_slice(line, start, end), layout->theme);
            }
            position = end + 1;
            continue;
        }
        uint32_t glyph;
        utf8_decode(line, &position, &glyph);
        if (glyph == '\t') {
            int spaces = TAB_STOP - layout->line_column % TAB_STOP;
            for (int i = 0; i < spaces; i++) {
                place(layout, ' ', 1, style);
            }
            continue;
        }
        int width = glyph_width(glyph);
        if (width > 0) {
            place(layout, glyph, width, style);
        }
    }
    return layout->row + 1;
}

int log_line_rows(String line, int width)
{
    Theme theme = { 0 };
    Layout layout = { .width = width > 0 ? width : 1, .theme = &theme };
    return lay_out(&layout, line);
}

void log_line_end(String line, int width, int *row, int *column)
{
    Theme theme = { 0 };
    Layout layout = { .width = width > 0 ? width : 1, .theme = &theme };
    lay_out(&layout, line);
    bool full = layout.column >= layout.width;
    *row = layout.row + (full ? 1 : 0);
    *column = full ? 0 : layout.column;
}

int log_line_draw(Screen *screen, int x, int y, int width, int skip_rows, int max_rows, String line,
                  const Theme *theme)
{
    Layout layout = {
        .screen = screen,
        .x = x,
        .y = y,
        .width = width > 0 ? width : 1,
        .skip_rows = skip_rows,
        .max_rows = max_rows,
        .theme = theme,
    };
    int rows = lay_out(&layout, line) - skip_rows;
    return rows < 0 ? 0 : rows > max_rows ? max_rows : rows;
}
