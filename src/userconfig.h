#pragma once

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

// UserConfig is the user's own settings, the same for every project. The TUI
// changes them as the user does, as btop does with its config.
typedef struct UserConfig {
    String path;
    unsigned boxes; // the LayoutBox bits shown
} UserConfig;

// user_config_path is $XDG_CONFIG_HOME/bprocs/config.yml, else ~/.config/bprocs/config.yml.
String user_config_path(Arena *arena);
// user_config_load reads the config at path; a missing one is every default.
[[nodiscard]] Error user_config_load(Arena *arena, String path, UserConfig *config, Err *err);
// user_config_save_boxes writes the boxes: line of the config at path and
// keeps the rest of it, making the file and its directories when missing.
[[nodiscard]] Error user_config_save_boxes(Arena *arena, String path, unsigned boxes, Err *err);
