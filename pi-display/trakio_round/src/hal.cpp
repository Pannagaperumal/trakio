#include "hal.h"
#include <Wire.h>
#include "board.h"

static uint8_t exioOut = 0;      // shadow of the TCA9554 output register
static uint8_t exioCfg = 0xFF;   // 1 = input

static void exio_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_EXIO);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void exio_write(uint8_t pin, bool level) {
  exioOut = level ? (exioOut | (1 << pin)) : (exioOut & ~(1 << pin));
  exio_reg(0x01, exioOut);
  if (exioCfg & (1 << pin)) {
    exioCfg &= ~(1 << pin);
    exio_reg(0x03, exioCfg);
  }
}

uint8_t exio_read_inputs() {
  Wire.beginTransmission(I2C_ADDR_EXIO);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_EXIO, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

void hal_init() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  // Hold power first, so letting go of PWR doesn't switch the board off.
  exioOut = (1 << EXIO_BAT_EN) | (1 << EXIO_LCD_EN);
  exio_reg(0x01, exioOut);
  exioCfg = (uint8_t)~((1 << EXIO_BAT_EN) | (1 << EXIO_LCD_EN) | (1 << EXIO_PA_EN));
  exio_reg(0x03, exioCfg);
  exio_read_inputs();

  pinMode(PIN_PWR_KEY, INPUT_PULLUP);
  pinMode(PIN_BOOT_KEY, INPUT_PULLUP);
  pinMode(PIN_EXIO_INT, INPUT_PULLUP);
  analogReadResolution(12);

  Serial.print("[hal] i2c:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

// ── Battery ───────────────────────────────────────────────
int battery_millivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  return (int)(sum / 8) * 2;   // 200k/200k divider
}

static int mvToPercent(int mv) {
  // 1-cell Li-ion under light load
  static const int16_t mvTab[]  = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  static const int8_t  pctTab[] = {0,    5,    10,   20,   30,   40,   50,   60,   75,   90,   100};
  if (mv <= mvTab[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (mv <= mvTab[i]) return pctTab[i - 1] + (pctTab[i] - pctTab[i - 1]) * (mv - mvTab[i - 1]) / (mvTab[i] - mvTab[i - 1]);
  }
  return 100;
}

static float battEma = -1;
int battery_percent() {
  int pct = mvToPercent(battery_millivolts());
  battEma = battEma < 0 ? pct : battEma * 0.9f + pct * 0.1f;
  return (int)(battEma + 0.5f);
}

bool battery_on_usb() { return battery_millivolts() > 4150; }

// ── PWR key ──────────────────────────────────────────────
KeyEvent pwr_key_poll() {
  static uint32_t downAt = 0;
  static bool longSent = false;
  bool down = digitalRead(PIN_PWR_KEY) == LOW;
  uint32_t now = millis();
  if (down) {
    if (!downAt) { downAt = now; longSent = false; }
    if (!longSent && now - downAt > 1800) { longSent = true; return KEY_LONG; }
    return KEY_NONE;
  }
  KeyEvent ev = KEY_NONE;
  if (downAt && !longSent && now - downAt > 40) ev = KEY_SHORT;
  downAt = 0;
  return ev;
}

void board_power_off() {
  exio_write(EXIO_BAT_EN, false);
  delay(200);
}
