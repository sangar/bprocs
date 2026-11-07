#pragma once

#include "mc/core/arena.h"
#include "mc/core/error.h"
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
// theme_parse reads the YAML of a theme file: each colour of Theme by its
// name, as "#rrggbb", with a list of three for a gradient, eight for accents
// and sixteen for ansi. Every one is needed; name is left as it is.
[[nodiscard]] Error theme_parse(Arena *arena, String text, Theme *theme, Err *err);
// theme_load reads the theme file at path, naming the theme after it.
[[nodiscard]] Error theme_load(Arena *arena, String path, Theme *theme, Err *err);
