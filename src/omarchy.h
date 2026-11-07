#pragma once

#include "mc/core/arena.h"
#include "mc/core/error.h"

// omarchy_setup themes bprocs with Omarchy, as Omarchy themes btop: it puts a
// template in ~/.config/omarchy/themed, from which Omarchy makes
// bprocs-theme.yml on every theme change, links the theme file omarchy to it,
// chooses that theme in the user config, and has Omarchy make it now.
[[nodiscard]] Error omarchy_setup(Arena *arena, Err *err);
