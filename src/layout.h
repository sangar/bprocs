#pragma once

#include "mc/term/screen.h"

// LayoutBox is a box the user can hide, a bit of the set layout_compute shows.
typedef enum LayoutBox {
    BOX_CPU = 1u << 0,
    BOX_MEMORY = 1u << 1,
    BOX_NETWORK = 1u << 2,
    BOX_DETAIL = 1u << 3,
    BOX_ALL = BOX_CPU | BOX_MEMORY | BOX_NETWORK | BOX_DETAIL,
} LayoutBox;

// Layout places the boxes on a screen of the given size. Space goes first to
// the log; of the shown boxes, the system boxes along the top and the process
// details appear when there is room for them.
typedef struct Layout {
    bool show_cpu;
    bool show_memory;
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

Layout layout_compute(int width, int height, size_t proc_count, unsigned shown);

// rect_inner is rect without its border.
Rect rect_inner(Rect rect);
bool rect_contains(Rect rect, int x, int y);
