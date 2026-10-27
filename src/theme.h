#pragma once

#include "mc/term/color.h"

enum { THEME_ACCENTS = 8 };

typedef struct Theme {
    const char *name;
    Rgb background;
    Rgb surface;   // overlays and the footer
    Rgb border;
    Rgb focus;     // the border and marks of what has the keyboard
    Rgb text;
    Rgb muted;
    Rgb faint;     // the empty part of meters
    Rgb highlight; // key names in hints
    Rgb selection;
    Rgb running;
    Rgb starting;
    Rgb stopped;
    Rgb failed;
    Gradient cpu;
    Gradient memory;
    Gradient download;
    Gradient upload;
    Rgb accents[THEME_ACCENTS]; // process names, in turn
    Rgb ansi[16];               // what the processes' 16 terminal colours become
} Theme;

size_t theme_count(void);
const Theme *theme_at(size_t index);
// theme_find looks a theme up by name.
bool theme_find(String name, size_t *index);
