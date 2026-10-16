#pragma once

#include "mc/text/str.h"

typedef struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Rgb;

// Gradient runs from start at 0 through middle at 0.5 to end at 1.
typedef struct Gradient {
    Rgb start;
    Rgb middle;
    Rgb end;
} Gradient;

Rgb rgb_hex(uint32_t hex);
bool rgb_equal(Rgb a, Rgb b);
// rgb_mix is a at 0 and b at 1; t is clamped to that range.
Rgb rgb_mix(Rgb a, Rgb b, float t);
// rgb_scale multiplies each channel by factor, clamped to 0..255.
Rgb rgb_scale(Rgb color, float factor);
Rgb gradient_at(Gradient gradient, float t);
// rgb_parse reads "#rrggbb".
bool rgb_parse(String text, Rgb *color);

// The xterm 256-colour palette: 16 system colours, a 6x6x6 cube, 24 greys.
Rgb xterm_to_rgb(uint8_t index);
// rgb_to_xterm is the nearest colour of the cube or the grey ramp.
uint8_t rgb_to_xterm(Rgb color);
