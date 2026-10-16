#pragma once

#include "screen.h"
#include "theme.h"

// A log line holds text and SGR sequences, as LogBuffer keeps it. Long lines
// wrap at the width of the view; tabs stop every eight columns.

// log_line_rows is how many rows line takes at width.
int log_line_rows(String line, int width);

// log_line_draw draws the rows of line from skip_rows on, at most max_rows
// of them, from (x, y), and returns how many it drew. The 16 basic colours
// are the theme's.
int log_line_draw(Screen *screen, int x, int y, int width, int skip_rows, int max_rows, String line,
                  const Theme *theme);

// log_line_end stores the row and column just after the last glyph of line
// at width, where a cursor at the end of the line would be.
void log_line_end(String line, int width, int *row, int *column);
