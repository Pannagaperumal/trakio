// CO5300 AMOLED over QSPI + LVGL port + FT6146 touch.
#pragma once
#include <Arduino.h>

void display_init();
void display_set_brightness(uint8_t pct);   // 0..100, panel register 0x51
void display_sleep();                        // panel off + sleep-in (~0 mA on the panel)
void display_wake();
bool display_is_asleep();
uint32_t touch_last_ms();                    // millis() of the last touch, 0 if never
bool touch_poll_pressed();                   // cheap read while the UI is asleep
