#include "audio.h"
#include <Wire.h>
#include <ESP_I2S.h>
#include <math.h>
#include "board.h"
#include "hal.h"

// I2S pins from Waveshare's board config for this board (C6_AMOLED_1_43).
#define PIN_I2S_MCLK  19
#define PIN_I2S_BCLK  21
#define PIN_I2S_WS    22
#define PIN_I2S_DOUT  23
#define PIN_I2S_DIN   20
#define I2C_ADDR_CODEC 0x18
#define RATE          16000   // MCLK = 256 × fs = 4.096 MHz

static I2SClass i2s;
static QueueHandle_t q = nullptr;
static volatile bool playing = false;
static bool ok = false;
static bool ampOn = false;
static uint32_t ampOffAtMs = 0;
static float gain = 0.5f;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_CODEC);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int rd(uint8_t reg) {
  Wire.beginTransmission(I2C_ADDR_CODEC);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_CODEC, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

// ES8311 as I2S slave, DAC only, 16-bit, MCLK from the pin at 256·fs
// (clock coefficients from Espressif's es8311 driver for 4.096 MHz / 16 kHz).
static bool codecInit() {
  static const uint8_t seq[][2] = {
      {0x00, 0x1F}, {0x45, 0x00}, {0x00, 0x00},             // reset, then release
      {0x01, 0x3F},                                          // all clocks on, MCLK from pin
      {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10}, {0x04, 0x10},  // pre-div 1, OSR
      {0x07, 0x00}, {0x08, 0xFF}, {0x06, 0x03},              // LRCK = MCLK/256, BCLK = MCLK/4
      {0x09, 0x0C}, {0x0A, 0x0C},                            // I2S, 16-bit in/out
      {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
      {0x00, 0x80},                                          // power on, slave
      {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00},              // analog + DAC up
      {0x13, 0x10}, {0x1C, 0x6A}, {0x37, 0x08},              // output driver, DAC ramp
      {0x32, 0xBF}, {0x31, 0x00},                            // DAC 0 dB, unmute
  };
  for (auto &r : seq) {
    if (!wr(r[0], r[1])) return false;
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  return true;
}

struct Note { uint16_t hz, ms; };

static void playNotes(const Note *n, int count) {
  static int16_t buf[256 * 2];
  for (int k = 0; k < count; k++) {
    int total = RATE * n[k].ms / 1000;
    int fade = RATE / 200;                       // 5 ms attack/release: no clicks
    float ph = 0, step = 2.0f * (float)M_PI * n[k].hz / RATE;
    for (int i = 0; i < total;) {
      int len = min(256, total - i);
      for (int j = 0; j < len; j++, i++) {
        float env = min(1.0f, min(i, total - i) / (float)fade);
        int16_t v = (int16_t)(sinf(ph) * 30000.0f * gain * env);
        ph += step;
        if (ph > 2 * (float)M_PI) ph -= 2 * (float)M_PI;
        buf[2 * j] = buf[2 * j + 1] = v;
      }
      i2s.write((uint8_t *)buf, len * 4);
    }
  }
  memset(buf, 0, sizeof buf);                    // flush silence so the DMA tail is quiet
  i2s.write((uint8_t *)buf, sizeof buf);
}

static void audioTask(void *) {
  uint8_t kind;
  for (;;) {
    if (xQueueReceive(q, &kind, portMAX_DELAY) != pdTRUE) continue;
    playing = true;
    switch (kind) {
      case 1: { static const Note n[] = {{988, 140}, {0, 60}, {988, 140}}; playNotes(n, 3); break; }
      case 2: { static const Note n[] = {{784, 110}, {988, 110}, {1319, 220}}; playNotes(n, 3); break; }
      default: { static const Note n[] = {{880, 120}, {1319, 180}}; playNotes(n, 2); break; }
    }
    playing = false;
  }
}

bool audio_init() {
  int id1 = rd(0xFD), id2 = rd(0xFE);
  if (id1 != 0x83 || id2 != 0x11) {
    Serial.printf("[audio] ES8311 not found (id %02X %02X)\n", id1 & 0xFF, id2 & 0xFF);
    return false;
  }
  // I2S first: the codec needs MCLK running when it powers up.
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] I2S start failed");
    return false;
  }
  if (!codecInit()) {
    Serial.println("[audio] codec init failed");
    return false;
  }
  q = xQueueCreate(4, sizeof(uint8_t));
  xTaskCreate(audioTask, "audio", 3072, nullptr, 2, nullptr);
  ok = true;
  Serial.printf("[audio] ES8311 ready (DAC vol reg %02X)\n", rd(0x32) & 0xFF);
  return true;
}

bool audio_ok() { return ok; }

void audio_set_volume(uint8_t pct) { gain = constrain(pct, 0, 100) / 100.0f; }

// Loop task only (the amplifier sits on the shared I2C expander).
void audio_chime(uint8_t kind) {
  if (!ok) return;
  if (!ampOn) {
    exio_write(EXIO_PA_EN, true);
    ampOn = true;
  }
  ampOffAtMs = millis() + 1500;
  xQueueSend(q, &kind, 0);
}

// Switch the amplifier off once nothing has played for a moment (it idles at
// several mA). Call from the loop task.
void audio_poll() {
  if (ampOn && !playing && uxQueueMessagesWaiting(q) == 0 && (int32_t)(millis() - ampOffAtMs) > 0) {
    exio_write(EXIO_PA_EN, false);
    ampOn = false;
  }
}
