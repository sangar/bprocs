#pragma once

#include "screen.h"

// Layout places the boxes on a screen of the given size. Space goes first to
// the log; the system boxes along the top and the process details appear
// when there is room for them.
typedef struct Layout {
    bool show_system;
    bool show_network;
    bool show_detail;
    Rect cpu;
    Rect memory;
    Rect network;
    Rect procs;
    Rect detail;
    Rect log;
    Rect footer;
} Layout;

Layout layout_compute(int width, int height, size_t proc_count);

// rect_inner is rect without its border.
Rect rect_inner(Rect rect);
bool rect_contains(Rect rect, int x, int y);
