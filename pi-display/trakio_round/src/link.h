// BLE link to the phone: Nordic UART Service, newline-delimited JSON.
//   RX 6E400002 (phone → device, write / write-no-response)
//   TX 6E400003 (device → phone, notify) — the device says hello with its
//   capabilities when the phone subscribes, so a v2 phone knows it may send
//   map areas and themes.
#pragma once
#include <Arduino.h>

void link_init();
void link_poll();                  // loop task: decode received frames, housekeeping
bool link_connected();
bool link_subscribed();
void link_send(const String &json);
void link_set_riding(bool riding); // fast connection interval while navigating
void link_stop();                  // before deep sleep
