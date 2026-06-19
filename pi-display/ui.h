// LVGL UI for the Trakio Navigator display.
// Call ui_init() once (after LVGL + your display driver are initialised),
// then ui_update() every loop to refresh from the decoded nav state.
#pragma once

void ui_init(void);
void ui_update(void);
