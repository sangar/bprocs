#pragma once

#include "mc/core/base.h"

// glyph_width is the number of terminal columns a code point takes: 0 for
// combining marks and other zero-width characters, 2 for wide East Asian
// characters and emoji, otherwise 1.
int glyph_width(uint32_t codepoint);
