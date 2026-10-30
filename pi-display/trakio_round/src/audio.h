// Speaker: ES8311 codec (I2C 0x18) + I2S, amplifier switched by TCA9554 pin 7.
// Tones play on their own task so the UI never waits for audio.
#pragma once
#include <Arduino.h>

bool audio_init();               // false if the codec doesn't answer
bool audio_ok();
void audio_chime(uint8_t kind);  // 0 = test/confirm, 1 = turn soon, 2 = turn now,
                                 // 3 = crash countdown tick, 4 = crash alarm (both full volume)
void audio_set_volume(uint8_t pct);
void audio_poll();               // loop task: switches the amplifier off when idle
