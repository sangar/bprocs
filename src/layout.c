#include "layout.h"

enum {
    SYSTEM_MIN_HEIGHT = 30,
    SYSTEM_MIN_WIDTH = 70,
    NETWORK_MIN_WIDTH = 110,
    SIDE_BY_SIDE_MIN_WIDTH = 96,
    DETAIL_MIN_HEIGHT = 8,
    LIST_CHROME = 3, // two borders and the column header
};

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

Layout layout_compute(int width, int height, size_t proc_count, unsigned shown)
{
    Layout layout = { .footer = { 0, height - 1, width, 1 } };
    int top = 0;
    int body = height - 1;
    if (height >= SYSTEM_MIN_HEIGHT && width >= SYSTEM_MIN_WIDTH) {
        layout.show_cpu = shown & BOX_CPU;
        layout.show_memory = shown & BOX_MEMORY;
        layout.show_network = (shown & BOX_NETWORK) && width >= NETWORK_MIN_WIDTH;
    }
    int beside_cpu = (int)layout.show_memory + (int)layout.show_network;
    if (layout.show_cpu || beside_cpu > 0) {
        int band = clamp(height / 4, 7, 11);
        int cpu_width = !layout.show_cpu ? 0 : beside_cpu == 0 ? width : width * (beside_cpu == 2 ? 44 : 56) / 100;
        int memory_width = !layout.show_memory ? 0 : layout.show_network ? (width - cpu_width) / 2 : width - cpu_width;
        layout.cpu = (Rect) { 0, 0, cpu_width, band };
        layout.memory = (Rect) { cpu_width, 0, memory_width, band };
        layout.network = (Rect) { cpu_width + memory_width, 0, width - cpu_width - memory_width, band };
        top = band;
        body -= band;
    }
    int list_rows = (int)proc_count + LIST_CHROME;
    if (width >= SIDE_BY_SIDE_MIN_WIDTH) {
        int side = clamp(width * 36 / 100, 46, 72);
        int list_height = clamp(list_rows, 5, body);
        int detail_height = body - list_height;
        bool wants_detail = shown & BOX_DETAIL;
        if (wants_detail && detail_height < DETAIL_MIN_HEIGHT && body >= list_rows / 2 + DETAIL_MIN_HEIGHT + 4) {
            list_height = body - DETAIL_MIN_HEIGHT;
            detail_height = DETAIL_MIN_HEIGHT;
        }
        layout.show_detail = wants_detail && detail_height >= DETAIL_MIN_HEIGHT;
        layout.procs = (Rect) { 0, top, side, layout.show_detail ? list_height : body };
        layout.detail = (Rect) { 0, top + list_height, side, detail_height };
        layout.log = (Rect) { side, top, width - side, body };
        return layout;
    }
    int list_height = clamp(list_rows, 5, body / 2);
    layout.procs = (Rect) { 0, top, width, list_height };
    layout.log = (Rect) { 0, top + list_height, width, body - list_height };
    return layout;
}
