#pragma once

#include "app.h"

// ui_draw draws the whole screen for the app's state at app->now_ns.
void ui_draw(App *app);

// ui_animating reports whether the picture changes with time alone, so the
// app keeps drawing frames without new events.
bool ui_animating(App *app);

// ui_list_rows is how many processes the list shows at once.
int ui_list_rows(const App *app);
