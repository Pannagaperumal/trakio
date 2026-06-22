// Trakio Navigator — CrowPanel ESP32 2.8" sketch.
//
// The implementation lives in the sibling files compiled as their own
// translation units (the normal Arduino way), NOT #include-d into this .ino:
//   esp.cpp  — BLE receiver + display/LVGL init  (defines setup()/loop())
//   ui.cpp   — LVGL UI (HOME / NAV / DONE + call toast)
//   nav_state.h / ui.h / crowpanel_tft.h — shared declarations + board config
//
// This file is intentionally empty: setup() and loop() come from esp.cpp.
