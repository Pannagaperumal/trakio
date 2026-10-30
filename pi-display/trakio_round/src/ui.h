// Round LVGL UI: Home / Nav / Ride pages (swipe), call screen + pill,
// crash countdown and arrival screen. Matches the Trakio Round preview.
#pragma once
#include <Arduino.h>

void ui_init();
void ui_update();                    // every loop while the screen is on
void ui_on_events(uint32_t ev);      // EV_* from state.h
void ui_set_link(bool connected);
void ui_set_battery(int pct, bool usb);
void ui_set_lean(float deg);
void ui_crash_show(int secondsLeft); // < 0 hides
void ui_crash_sos_sent();
bool ui_overlay_active();            // call / crash / arrival on screen
void ui_note_wake();                 // restart the home ring's breathing

// Provided by the app (main.cpp)
void app_call_button(bool accept);
void app_crash_ok();
